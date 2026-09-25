#include "tests/support/test_check.h"
#include "room.h"
#include "signal_client.h"
#include "webrtc_manager.h"
#include "livekit_rtc.pb.h"
#include "local_audio_track.h"
#include "local_video_track.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace livekit {

// Arrange the installed transport boundary and inspect negotiation ownership;
// all requirements run through the production signaling dispatcher and handler.
class RoomSinglePcTestAccess final {
public:
    static void InitializeCapabilities(
        webrtc::PeerConnectionInterface* publisher, bool single_pc) {
        Room::InitializePeerConnectionCapabilities(publisher, single_pc);
    }

    static uint64_t Install(
        Room& room,
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher,
        std::shared_ptr<SignalClient> signal) {
        std::lock_guard lock(room.room_mutex_);
        const auto generation = ++room.session_generation_;
        room.installed_session_generation_ = generation;
        room.connection_state_ = ConnectionState::Connected;
        room.publisher_pc_ = std::move(publisher);
        room.ResetSubscriptionSessionLocked(signal->options().auto_subscribe);
        room.signal_client_ = std::move(signal);
        return generation;
    }

    static void Requirement(Room& room, uint32_t audio, uint32_t video,
                            uint64_t generation) {
        auto message = std::make_shared<proto::SignalResponse>();
        auto* requirement = message->mutable_media_sections_requirement();
        requirement->set_num_audios(audio);
        requirement->set_num_videos(video);
        room.HandleSignalMessage(std::move(message), generation);
    }

    static void MarkOfferInFlight(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        TEST_CHECK(room.negotiation_state_ == Room::NegotiationState::Idle);
        room.negotiation_state_ = Room::NegotiationState::InProgress;
    }

    static bool PendingRetry(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.negotiation_state_ == Room::NegotiationState::PendingRetry;
    }

    static bool Idle(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.negotiation_state_ == Room::NegotiationState::Idle;
    }

    static std::size_t Waiters(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.negotiation_waiters_.size();
    }

    static asio::awaitable<PublishedSenderBundle> InstallSender(
        Room& room, std::shared_ptr<Track> track, uint64_t generation) {
        return room.AddTrackToPublisherAsync(std::move(track), generation);
    }

    static void RememberSenderBundle(
        Room& room,
        const std::shared_ptr<Track>& track,
        const PublishedSenderBundle& bundle,
        const std::string& sid) {
        std::shared_ptr<LocalParticipant> local;
        {
            std::lock_guard lock(room.room_mutex_);
            if (!room.local_participant_) {
                room.local_participant_ = std::make_shared<LocalParticipant>(
                    "PA_LOCAL", "local", [](const proto::SignalRequest&) {});
            }
            local = room.local_participant_;
            room.published_sender_track_ids_[track.get()] = bundle.track_ids;
        }
        track->set_sid(sid);
        local->add_publication(std::make_shared<TrackPublication>(
            track, sid, track->name()));
    }

    static asio::awaitable<void> RemoveSenderBundle(
        Room& room,
        std::shared_ptr<Track> track,
        uint64_t generation) {
        co_await room.RemoveLocalTrackFromPublisherAsync(
            std::move(track), generation);
    }

};

} // namespace livekit

namespace {

using Access = livekit::RoomSinglePcTestAccess;

struct Observer final : webrtc::PeerConnectionObserver {
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
};

struct Fixture {
    asio::io_context io;
    Observer observer;
    std::vector<std::string> track_bindings;
    std::shared_ptr<livekit::Room> room = livekit::Room::Create(io.get_executor());
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    uint64_t generation = 0;
    bool single_pc_mode = false;

    explicit Fixture(bool single_pc = true, bool auto_subscribe = true) {
        Install(single_pc, auto_subscribe);
    }

    ~Fixture() {
        room->Disconnect();
        Drain();
        publisher = nullptr;
        room.reset();
    }

    void Install(bool single_pc = true, bool auto_subscribe = true) {
        single_pc_mode = single_pc;
        room->SetLogHandler([this](const std::string&, const std::string& tag,
                                   const std::string& message) {
            if (tag == "MID_TRACK_BIND") track_bindings.push_back(message);
        });
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        auto created = livekit::WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(
            config, webrtc::PeerConnectionDependencies(&observer));
        TEST_CHECK(created.ok());
        publisher = created.MoveValue();
        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            Access::InitializeCapabilities(publisher.get(), single_pc);
        });
        // No external transport is opened. These tests cover offer construction,
        // merge decisions and session isolation, not server acceptance or media.
        livekit::SignalOptions options;
        options.auto_subscribe = auto_subscribe;
        auto signal = std::make_shared<livekit::SignalClient>(
            "wss://single-pc.test", "test-token", options, single_pc,
            std::make_shared<livekit::proto::JoinResponse>(),
            livekit::SignalEventHandler{}, io.get_executor());
        generation = Access::Install(*room, publisher, std::move(signal));
    }

    void Drain() {
        io.restart();
        io.poll();
    }

    void Requirement(uint32_t audio, uint32_t video) {
        Access::Requirement(*room, audio, video, generation);
        Drain();
    }

    livekit::PublishedSenderBundle InstallSenderBundle(
        const std::shared_ptr<livekit::Track>& track) {
        bool done = false;
        std::exception_ptr error;
        livekit::PublishedSenderBundle bundle;
        asio::co_spawn(io, Access::InstallSender(*room, track, generation),
            [&](std::exception_ptr failure,
                livekit::PublishedSenderBundle result) {
                error = failure;
                bundle = std::move(result);
                done = true;
            });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            io.restart();
            io.run_one_for(10ms);
        }
        TEST_CHECK(done);
        if (error) std::rethrow_exception(error);
        TEST_CHECK(bundle.primary);
        return bundle;
    }

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> InstallSender(
        const std::shared_ptr<livekit::Track>& track) {
        return InstallSenderBundle(track).primary;
    }

    void RemoveSenderBundle(const std::shared_ptr<livekit::Track>& track) {
        bool done = false;
        std::exception_ptr error;
        asio::co_spawn(io,
            Access::RemoveSenderBundle(*room, track, generation),
            [&](std::exception_ptr failure) {
                error = failure;
                done = true;
            });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            io.restart();
            io.run_one_for(10ms);
        }
        TEST_CHECK(done);
        if (error) std::rethrow_exception(error);
    }

    std::string LocalOffer() const {
        return livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            std::string sdp;
            const auto* description = publisher->local_description();
            if (description) {
                TEST_CHECK(description->GetType() == webrtc::SdpType::kOffer);
                description->ToString(&sdp);
            }
            return sdp;
        });
    }

    void CheckReceivers(std::size_t expected_audio, std::size_t expected_video) const {
        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            std::size_t audio = 0, video = 0, inactive = 0;
            for (const auto& transceiver : publisher->GetTransceivers()) {
                if (transceiver->direction() == webrtc::RtpTransceiverDirection::kInactive) {
                    TEST_CHECK(transceiver->media_type() == webrtc::MediaType::VIDEO);
                    TEST_CHECK(!transceiver->sender()->track());
                    ++inactive;
                    continue;
                }
                TEST_CHECK(transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly);
                if (transceiver->media_type() == webrtc::MediaType::AUDIO) ++audio;
                else if (transceiver->media_type() == webrtc::MediaType::VIDEO) ++video;
            }
            TEST_CHECK(audio == expected_audio);
            TEST_CHECK(video == expected_video);
            TEST_CHECK(inactive == (single_pc_mode ? 1 : 0));
        });
    }
};

void AutoSubscribeFalseDoesNotGateEmptyVideoDemandOffer() {
    Fixture f(true, false);
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        webrtc::DataChannelInit init;
        const auto channel = f.publisher->CreateDataChannelOrError(
            "_reliable", &init);
        TEST_CHECK(channel.ok());
    });
    f.Requirement(0, 0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (f.LocalOffer().empty() && std::chrono::steady_clock::now() < deadline) {
        f.io.restart();
        f.io.run_one_for(10ms);
    }
    const auto offer = f.LocalOffer();
    TEST_CHECK(offer.find("m=application ") != std::string::npos);
    TEST_CHECK(offer.find("m=audio ") == std::string::npos);
    TEST_CHECK(offer.find("m=video ") != std::string::npos);
    TEST_CHECK(offer.find("a=inactive\r\n") != std::string::npos);
    TEST_CHECK(offer.find(" VP8/90000\r\n") != std::string::npos);
    TEST_CHECK(offer.find(" H264/90000\r\n") != std::string::npos);
    TEST_CHECK(offer.find("a=recvonly\r\n") == std::string::npos);
    f.CheckReceivers(0, 0);
}

void ZeroSectionsCreatesOfferForExistingTransceiver() {
    Fixture f;
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        TEST_CHECK(f.publisher->AddTransceiver(webrtc::MediaType::AUDIO, init).ok());
    });
    f.Requirement(0, 0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (f.LocalOffer().empty() && std::chrono::steady_clock::now() < deadline) {
        f.io.restart();
        f.io.run_one_for(10ms);
    }
    const auto offer = f.LocalOffer();
    TEST_CHECK(offer.find("m=audio ") != std::string::npos);
    TEST_CHECK(offer.find("a=recvonly") != std::string::npos);
    f.CheckReceivers(1, 0);
}

void ZeroSectionsWhileOfferInFlightQueuesRetry() {
    Fixture f;
    Access::MarkOfferInFlight(*f.room);
    f.Requirement(0, 0);
    TEST_CHECK(Access::PendingRetry(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 1);
    f.Requirement(0, 0);
    TEST_CHECK(Access::PendingRetry(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 2);
    TEST_CHECK(f.LocalOffer().empty());
    f.CheckReceivers(0, 0);
}

void RepeatedCountsAreAdditionalSections() {
    Fixture f;
    Access::MarkOfferInFlight(*f.room);
    f.Requirement(1, 0);
    f.CheckReceivers(1, 0);
    f.Requirement(1, 0);
    f.CheckReceivers(2, 0);
    f.Requirement(0, 1);
    f.CheckReceivers(2, 1);
    TEST_CHECK(Access::PendingRetry(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 3);
    TEST_CHECK(f.LocalOffer().empty());
}

void DualPcIgnoresClientOfferRequirements() {
    Fixture f(false);
    f.Requirement(1, 1);
    f.Requirement(0, 0);
    f.CheckReceivers(0, 0);
    TEST_CHECK(Access::Idle(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 0);
    TEST_CHECK(f.LocalOffer().empty());
}

void DisconnectAndReplacementRejectOldRequirementAndQueuedNegotiation() {
    Fixture f;
    Access::MarkOfferInFlight(*f.room);
    f.Requirement(0, 0);
    TEST_CHECK(Access::Waiters(*f.room) == 1);
    const auto old_generation = f.generation;

    // Leave a second negotiation coroutine queued at the old generation.
    Access::Requirement(*f.room, 0, 0, old_generation);
    f.room->Disconnect();
    TEST_CHECK(Access::Idle(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 0);
    f.Install(); // Install a new joined transport bundle without a real service.
    TEST_CHECK(f.generation > old_generation);
    f.Drain();
    TEST_CHECK(Access::Idle(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 0);

    Access::Requirement(*f.room, 1, 1, old_generation);
    Access::Requirement(*f.room, 0, 0, old_generation);
    f.Drain();
    f.CheckReceivers(0, 0);
    TEST_CHECK(Access::Idle(*f.room));
    TEST_CHECK(f.LocalOffer().empty());

    Access::MarkOfferInFlight(*f.room);
    f.Requirement(0, 0);
    TEST_CHECK(Access::PendingRetry(*f.room));
    TEST_CHECK(Access::Waiters(*f.room) == 1);
}

void LocalPublicationDoesNotReuseDownstreamTransceivers() {
    Fixture f;
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        webrtc::RtpTransceiverInit receive;
        receive.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        TEST_CHECK(f.publisher->AddTransceiver(webrtc::MediaType::AUDIO, receive).ok());
        TEST_CHECK(f.publisher->AddTransceiver(webrtc::MediaType::VIDEO, receive).ok());
    });

    auto audio = std::make_shared<livekit::LocalAudioTrack>("TR_audio", "simple_audio",
        std::make_shared<livekit::AudioSource>(48000, 1));
    livekit::VideoPublishOptions plain_options;
    plain_options.simulcast = false;
    auto video = livekit::LocalVideoTrack::createLocalVideoTrack("plain_video",
        std::make_shared<livekit::VideoSource>(640, 480),
        livekit::TrackSource::Camera, plain_options);
    auto simulcast = livekit::LocalVideoTrack::createLocalVideoTrack("camera_video",
        std::make_shared<livekit::VideoSource>(1280, 720),
        livekit::TrackSource::Camera,
        livekit::LocalVideoTrack::DefaultVp8SimulcastOptions(1280, 720));
    TEST_CHECK(audio && video && simulcast);
    const std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders{
        f.InstallSender(audio), f.InstallSender(video), f.InstallSender(simulcast)};

    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        const auto transceivers = f.publisher->GetTransceivers();
        TEST_CHECK(transceivers.size() == 6);
        for (std::size_t i = 0; i < transceivers.size(); ++i) {
            if (i == 0) {
                TEST_CHECK(transceivers[i]->direction() ==
                    webrtc::RtpTransceiverDirection::kInactive);
                TEST_CHECK(!transceivers[i]->sender()->track());
            } else if (i < 3) {
                TEST_CHECK(transceivers[i]->direction() ==
                    webrtc::RtpTransceiverDirection::kRecvOnly);
                TEST_CHECK(!transceivers[i]->sender()->track());
            } else {
                TEST_CHECK(transceivers[i]->direction() ==
                    webrtc::RtpTransceiverDirection::kSendOnly);
                TEST_CHECK(transceivers[i]->sender() == senders[i - 3]);
            }
        }
        const auto parameters = senders[2]->GetParameters();
        const auto layers = simulcast->publish_options().layers;
        TEST_CHECK(parameters.encodings.size() == layers.size());
        for (std::size_t i = 0; i < layers.size(); ++i) {
            TEST_CHECK(parameters.encodings[i].rid == layers[i].rid);
            TEST_CHECK(parameters.encodings[i].active);
        }
    });

    f.Requirement(0, 0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (f.track_bindings.size() < senders.size() &&
           std::chrono::steady_clock::now() < deadline) {
        f.io.restart();
        f.io.run_one_for(10ms);
    }
    TEST_CHECK(f.track_bindings.size() == senders.size());
    const auto offer = f.LocalOffer();
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        for (const auto& transceiver : f.publisher->GetTransceivers()) {
            TEST_CHECK(transceiver->mid());
            const auto mid = *transceiver->mid();
            const auto mid_line = "a=mid:" + mid + "\r\n";
            const auto mid_offset = offer.find(mid_line);
            TEST_CHECK(mid_offset != std::string::npos);
            const auto section_end = offer.find("\r\nm=", mid_offset);
            const auto section = offer.substr(mid_offset, section_end - mid_offset);
            if (transceiver->direction() == webrtc::RtpTransceiverDirection::kInactive) {
                TEST_CHECK(section.find("a=inactive") != std::string::npos);
                TEST_CHECK(section.find(" H264/90000\r\n") != std::string::npos);
                continue;
            }
            if (!transceiver->sender()->track()) {
                TEST_CHECK(section.find("a=recvonly") != std::string::npos);
                if (transceiver->media_type() == webrtc::MediaType::VIDEO) {
                    TEST_CHECK(section.find(" H264/90000\r\n") != std::string::npos);
                }
                continue;
            }
            const auto cid = transceiver->sender()->track()->id();
            TEST_CHECK(section.find("a=sendonly") != std::string::npos);
            if (transceiver->media_type() == webrtc::MediaType::VIDEO) {
                TEST_CHECK(section.find(" VP8/90000\r\n") != std::string::npos);
                TEST_CHECK(section.find(" H264/90000\r\n") == std::string::npos);
                TEST_CHECK(section.find(" VP9/90000\r\n") == std::string::npos);
            }
            TEST_CHECK(section.find("a=msid:livekit_stream_local " + cid + "\r\n") !=
                std::string::npos);
            const auto binding = "mid=" + mid + ", cid=" + cid;
            TEST_CHECK(std::find(f.track_bindings.begin(), f.track_bindings.end(), binding) !=
                f.track_bindings.end());
        }
    });
}

void SenderTrackIdOverridesRetainedSdpMsid() {
    Fixture f;
    auto original = std::make_shared<livekit::LocalAudioTrack>("TR_original", "original_audio",
        std::make_shared<livekit::AudioSource>(48000, 1));
    const auto sender = f.InstallSender(original);
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        const auto replacement = livekit::WebRTCManager::Instance().factory()->CreateAudioTrack(
            "current_audio", nullptr);
        TEST_CHECK(sender->SetTrack(replacement.get()));
        // WebRTC keeps the sender ID used in SDP even though its local track
        // binding has changed. The LiveKit publication binding must use the
        // current native track ID, never the retained SDP MSID.
        TEST_CHECK(sender->id() == "original_audio");
        TEST_CHECK(sender->track()->id() == "current_audio");
    });
    f.Requirement(0, 0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (f.track_bindings.empty() && std::chrono::steady_clock::now() < deadline) {
        f.io.restart();
        f.io.run_one_for(10ms);
    }
    TEST_CHECK(f.track_bindings.size() == 1);
    TEST_CHECK(f.LocalOffer().find("a=msid:livekit_stream_local original_audio\r\n") !=
        std::string::npos);
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        const auto transceivers = f.publisher->GetTransceivers();
        TEST_CHECK(transceivers.size() == 2);
        TEST_CHECK(transceivers[1]->mid());
        TEST_CHECK(f.track_bindings[0] ==
            "mid=" + *transceivers[1]->mid() + ", cid=current_audio");
    });
}

void SingleStreamSvcStaysInSenderTransaction() {
    Fixture f;
    livekit::VideoPublishOptions options;
    options.video_codec = "vp9";
    options.simulcast = false;
    options.scalability_mode = "L1T1";
    options.auto_backup_codec = false;
    auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
        "svc_video",
        std::make_shared<livekit::VideoSource>(640, 480),
        livekit::TrackSource::Camera,
        options);
    const auto bundle = f.InstallSenderBundle(track);
    TEST_CHECK(bundle.scalability_modes.size() == 1);
    TEST_CHECK(bundle.scalability_modes[0] == "L1T1");
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        const auto parameters = bundle.primary->GetParameters();
        TEST_CHECK(parameters.encodings.size() == 1);
    });
}

void AudioPublishPolicyClosesSignalAndSenderLoop() {
    livekit::AudioPublishPolicy requested;
    requested.max_bitrate_bps = 64000;
    requested.dtx = false;
    requested.red = true;

    const auto encrypted = livekit::LocalAudioTrack::ResolvePublishPlan(
        requested, {"audio/opus"}, true);
    TEST_CHECK(encrypted.ok());
    TEST_CHECK(encrypted.requested.red);
    TEST_CHECK(!encrypted.effective.red);
    TEST_CHECK(encrypted.red_disabled_for_encryption);
    TEST_CHECK(encrypted.effective.max_bitrate_bps == 64000);

    auto invalid_codec = requested;
    invalid_codec.codec = "aac";
    TEST_CHECK(!livekit::LocalAudioTrack::ResolvePublishPlan(
        invalid_codec, {"opus"}, false).ok());
    auto invalid_bitrate = requested;
    invalid_bitrate.max_bitrate_bps = 1000;
    TEST_CHECK(!livekit::LocalAudioTrack::ResolvePublishPlan(
        invalid_bitrate, {"opus"}, false).ok());

    auto track = livekit::LocalAudioTrack::createLocalAudioTrack(
        "policy_audio", std::make_shared<livekit::AudioSource>(48000, 1),
        requested);
    livekit::LocalParticipant participant(
        "PA_AUDIO", "audio_identity", [](const auto&) {});
    const auto request = participant.BuildTrackPublishRequest(track);
    TEST_CHECK(request.add_track().disable_dtx());
    TEST_CHECK(!request.add_track().disable_red());
    TEST_CHECK(request.add_track().audio_features_size() == 1);
    TEST_CHECK(request.add_track().audio_features(0) ==
        livekit::proto::AudioTrackFeature::TF_NO_DTX);
    TEST_CHECK(track->requested_publish_policy().red);

    Fixture f;
    const auto bundle = f.InstallSenderBundle(track);
    TEST_CHECK(bundle.senders.size() == 1);
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        const auto parameters = bundle.primary->GetParameters();
        TEST_CHECK(parameters.encodings.size() == 1);
        TEST_CHECK(parameters.encodings.front().max_bitrate_bps.has_value());
        TEST_CHECK(*parameters.encodings.front().max_bitrate_bps == 64000);
    });
}

void BackupSenderBundleRollsBackAsOneTransaction() {
    struct PolicyCase {
        livekit::BackupCodecPolicy policy;
        bool primary_active;
        bool backup_active;
        const char* track_name;
    };
    const PolicyCase cases[] = {
        {livekit::BackupCodecPolicy::PreferRegression, true, false,
         "backup_prefer"},
        {livekit::BackupCodecPolicy::Simulcast, true, true,
         "backup_simulcast"},
        {livekit::BackupCodecPolicy::Regression, false, true,
         "backup_regression"},
    };

    for (const auto& policy_case : cases) {
        Fixture f;
        livekit::VideoPublishOptions options;
        options.video_codec = "vp9";
        options.simulcast = true;
        options.backup_codec = "vp8";
        options.backup_codec_policy = policy_case.policy;
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
            policy_case.track_name,
            std::make_shared<livekit::VideoSource>(1280, 720),
            livekit::TrackSource::Camera,
            options);
        const auto bundle = f.InstallSenderBundle(track);
        TEST_CHECK(bundle.senders.size() == 2);
        TEST_CHECK(bundle.track_ids.size() == 2);
        TEST_CHECK(bundle.primary == bundle.senders.front());
        TEST_CHECK(bundle.track_ids[0] == policy_case.track_name);
        TEST_CHECK(bundle.track_ids[1] ==
            std::string(policy_case.track_name) + "_backup");

        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            const auto primary = bundle.senders[0]->GetParameters();
            const auto backup = bundle.senders[1]->GetParameters();
            TEST_CHECK(!primary.encodings.empty());
            TEST_CHECK(!backup.encodings.empty());
            TEST_CHECK(std::all_of(primary.encodings.begin(), primary.encodings.end(),
                [&](const auto& encoding) {
                    return encoding.active == policy_case.primary_active;
                }));
            TEST_CHECK(std::all_of(backup.encodings.begin(), backup.encodings.end(),
                [&](const auto& encoding) {
                    return encoding.active == policy_case.backup_active;
                }));
            TEST_CHECK(bundle.senders[0]->track()->id() == bundle.track_ids[0]);
            TEST_CHECK(bundle.senders[1]->track()->id() == bundle.track_ids[1]);
        });
        const std::string sid = std::string("TR_") + policy_case.track_name;
        Access::RememberSenderBundle(*f.room, track, bundle, sid);
        if (policy_case.policy == livekit::BackupCodecPolicy::PreferRegression) {
            livekit::proto::SignalResponse message;
            auto* quality = message.mutable_subscribed_quality_update();
            quality->set_track_sid(sid);
            for (const auto level : {
                     livekit::proto::VideoQuality::LOW,
                     livekit::proto::VideoQuality::MEDIUM,
                     livekit::proto::VideoQuality::HIGH}) {
                auto* layer = quality->add_subscribed_qualities();
                layer->set_quality(level);
                layer->set_enabled(true);
            }
            f.room->HandleSignalMessageForTesting(message);
            livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
                const auto backup = bundle.senders[1]->GetParameters();
                TEST_CHECK(std::all_of(
                    backup.encodings.begin(), backup.encodings.end(),
                    [](const auto& encoding) { return encoding.active; }));
            });
        }
        f.RemoveSenderBundle(track);
        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            for (const auto& sender : bundle.senders) TEST_CHECK(!sender->track());
        });
    }
}

} // namespace

int main() {
    TEST_CHECK(livekit::WebRTCManager::Instance().Initialize());
    AutoSubscribeFalseDoesNotGateEmptyVideoDemandOffer();
    ZeroSectionsCreatesOfferForExistingTransceiver();
    ZeroSectionsWhileOfferInFlightQueuesRetry();
    RepeatedCountsAreAdditionalSections();
    DualPcIgnoresClientOfferRequirements();
    DisconnectAndReplacementRejectOldRequirementAndQueuedNegotiation();
    LocalPublicationDoesNotReuseDownstreamTransceivers();
    SenderTrackIdOverridesRetainedSdpMsid();
    SingleStreamSvcStaysInSenderTransaction();
    AudioPublishPolicyClosesSignalAndSenderLoop();
    BackupSenderBundleRollsBackAsOneTransaction();
    livekit::WebRTCManager::Instance().Deinitialize();
    std::cout << "Single-PC media-section negotiation and publication: 11 cases PASS\n";
    return 0;
}
