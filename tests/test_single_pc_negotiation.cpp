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
    static uint64_t Install(
        Room& room,
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher,
        std::shared_ptr<SignalClient> signal) {
        std::lock_guard lock(room.room_mutex_);
        const auto generation = ++room.session_generation_;
        room.installed_session_generation_ = generation;
        room.connection_state_ = ConnectionState::Connected;
        room.publisher_pc_ = std::move(publisher);
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

    static asio::awaitable<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> InstallSender(
        Room& room, std::shared_ptr<Track> track, uint64_t generation) {
        return room.AddTrackToPublisherAsync(std::move(track), generation);
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

    explicit Fixture(bool single_pc = true) { Install(single_pc); }

    ~Fixture() {
        room->Disconnect();
        Drain();
        publisher = nullptr;
        room.reset();
    }

    void Install(bool single_pc = true) {
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
        // No external transport is opened. These tests cover offer construction,
        // merge decisions and session isolation, not server acceptance or media.
        auto signal = std::make_shared<livekit::SignalClient>(
            "wss://single-pc.test", "test-token", livekit::SignalOptions{}, single_pc,
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

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> InstallSender(
        const std::shared_ptr<livekit::Track>& track) {
        bool done = false;
        std::exception_ptr error;
        webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
        asio::co_spawn(io, Access::InstallSender(*room, track, generation),
            [&](std::exception_ptr failure,
                webrtc::scoped_refptr<webrtc::RtpSenderInterface> result) {
                error = failure;
                sender = std::move(result);
                done = true;
            });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            io.restart();
            io.run_one_for(10ms);
        }
        TEST_CHECK(done);
        if (error) std::rethrow_exception(error);
        TEST_CHECK(sender);
        return sender;
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
            std::size_t audio = 0, video = 0;
            for (const auto& transceiver : publisher->GetTransceivers()) {
                TEST_CHECK(transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly);
                if (transceiver->media_type() == webrtc::MediaType::AUDIO) ++audio;
                else if (transceiver->media_type() == webrtc::MediaType::VIDEO) ++video;
            }
            TEST_CHECK(audio == expected_audio);
            TEST_CHECK(video == expected_video);
        });
    }
};

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
        TEST_CHECK(transceivers.size() == 5);
        for (std::size_t i = 0; i < transceivers.size(); ++i) {
            if (i < 2) {
                TEST_CHECK(transceivers[i]->direction() ==
                    webrtc::RtpTransceiverDirection::kRecvOnly);
                TEST_CHECK(!transceivers[i]->sender()->track());
            } else {
                TEST_CHECK(transceivers[i]->direction() ==
                    webrtc::RtpTransceiverDirection::kSendOnly);
                TEST_CHECK(transceivers[i]->sender() == senders[i - 2]);
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
            if (!transceiver->sender()->track()) {
                TEST_CHECK(section.find("a=recvonly") != std::string::npos);
                continue;
            }
            const auto cid = transceiver->sender()->track()->id();
            TEST_CHECK(section.find("a=sendonly") != std::string::npos);
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
        TEST_CHECK(transceivers.size() == 1);
        TEST_CHECK(transceivers[0]->mid());
        TEST_CHECK(f.track_bindings[0] ==
            "mid=" + *transceivers[0]->mid() + ", cid=current_audio");
    });
}

} // namespace

int main() {
    TEST_CHECK(livekit::WebRTCManager::Instance().Initialize());
    ZeroSectionsCreatesOfferForExistingTransceiver();
    ZeroSectionsWhileOfferInFlightQueuesRetry();
    RepeatedCountsAreAdditionalSections();
    DualPcIgnoresClientOfferRequirements();
    DisconnectAndReplacementRejectOldRequirementAndQueuedNegotiation();
    LocalPublicationDoesNotReuseDownstreamTransceivers();
    SenderTrackIdOverridesRetainedSdpMsid();
    livekit::WebRTCManager::Instance().Deinitialize();
    std::cout << "Single-PC media-section negotiation and publication: 7 cases PASS\n";
    return 0;
}
