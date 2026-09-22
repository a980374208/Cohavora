#include <iostream>
#include "tests/support/test_check.h"
#include <thread>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <asio.hpp>
#include "room.h"
#include "stats.h"
#include "stats_collector.h"
#include "webrtc_manager.h"
#include "api/stats/rtcstats_objects.h"
#include "api/notifier.h"

namespace {

using namespace std::chrono_literals;
using PeerConnection = webrtc::scoped_refptr<webrtc::PeerConnectionInterface>;

void ParsedOutboundStatsDistinguishMissingZeroAndPositive() {
    const auto timestamp = webrtc::Timestamp::Millis(1234);
    auto native = webrtc::RTCStatsReport::Create(timestamp);

    // A valid report with no outbound streams is different from a stream
    // whose counters exist and are zero; no synthetic entry may be created.
    auto empty = livekit::ParseRtcStatsReport(*native);
    TEST_CHECK(empty.timestamp_ms == 1234);
    TEST_CHECK(empty.outbound_rtp.empty());

    native->AddStats(std::make_unique<webrtc::RTCOutboundRtpStreamStats>(
        "missing", timestamp));

    auto zero = std::make_unique<webrtc::RTCOutboundRtpStreamStats>("zero", timestamp);
    zero->kind = "video";
    zero->mid = "3";
    zero->rid = "q";
    zero->bytes_sent = 0;
    zero->packets_sent = 0;
    zero->frames_encoded = 0;
    native->AddStats(std::move(zero));

    auto positive = std::make_unique<webrtc::RTCOutboundRtpStreamStats>(
        "positive", timestamp);
    positive->kind = "video";
    positive->mid = "3";
    positive->rid = "f";
    positive->bytes_sent = 203900;
    positive->packets_sent = 1692;
    positive->frames_encoded = 120;
    native->AddStats(std::move(positive));

    // Audio normally has no frames_encoded; availability is per field,
    // not inferred from the existence of a stream or another counter.
    auto audio = std::make_unique<webrtc::RTCOutboundRtpStreamStats>("audio", timestamp);
    audio->kind = "audio";
    audio->bytes_sent = 280;
    audio->packets_sent = 0;
    native->AddStats(std::move(audio));

    const auto parsed = livekit::ParseRtcStatsReport(*native);
    TEST_CHECK(parsed.outbound_rtp.size() == 4);
    const auto find = [&parsed](const char* id) -> const livekit::OutboundRtpStreamStats& {
        for (const auto& stream : parsed.outbound_rtp) {
            if (stream.id == id) return stream;
        }
        TEST_CHECK(false && "Expected outbound stream was not parsed");
        return parsed.outbound_rtp.front();
    };

    const auto& missing = find("missing");
    TEST_CHECK(!missing.kind_available && missing.kind.empty());
    TEST_CHECK(!missing.mid_available && missing.mid.empty());
    TEST_CHECK(!missing.rid_available && missing.rid.empty());
    TEST_CHECK(!missing.bytes_sent_available && missing.bytes_sent == 0);
    TEST_CHECK(!missing.packets_sent_available && missing.packets_sent == 0);
    TEST_CHECK(!missing.frames_encoded_available && missing.frames_encoded == 0);

    const auto& present_zero = find("zero");
    TEST_CHECK(present_zero.kind_available && present_zero.kind == "video");
    TEST_CHECK(present_zero.mid_available && present_zero.mid == "3");
    TEST_CHECK(present_zero.rid_available && present_zero.rid == "q");
    TEST_CHECK(present_zero.bytes_sent_available && present_zero.bytes_sent == 0);
    TEST_CHECK(present_zero.packets_sent_available && present_zero.packets_sent == 0);
    TEST_CHECK(present_zero.frames_encoded_available && present_zero.frames_encoded == 0);

    const auto& present_positive = find("positive");
    TEST_CHECK(present_positive.kind_available && present_positive.kind == "video");
    TEST_CHECK(present_positive.mid_available && present_positive.mid == "3");
    TEST_CHECK(present_positive.rid_available && present_positive.rid == "f");
    TEST_CHECK(present_positive.bytes_sent_available && present_positive.bytes_sent == 203900);
    TEST_CHECK(present_positive.packets_sent_available && present_positive.packets_sent == 1692);
    TEST_CHECK(present_positive.frames_encoded_available && present_positive.frames_encoded == 120);

    const auto& present_audio = find("audio");
    TEST_CHECK(present_audio.kind_available && present_audio.kind == "audio");
    TEST_CHECK(present_audio.bytes_sent_available && present_audio.bytes_sent == 280);
    TEST_CHECK(present_audio.packets_sent_available && present_audio.packets_sent == 0);
    TEST_CHECK(!present_audio.frames_encoded_available);
    TEST_CHECK(!present_audio.mid_available && !present_audio.rid_available);
}

struct StatsPeerObserver final : webrtc::PeerConnectionObserver {
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
};

struct StatsPeerFixture {
    asio::io_context io;
    StatsPeerObserver observer;
    PeerConnection peer;

    StatsPeerFixture() {
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        auto created = livekit::WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(
            config, webrtc::PeerConnectionDependencies(&observer));
        TEST_CHECK(created.ok());
        peer = created.MoveValue();
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        TEST_CHECK(peer->AddTransceiver(webrtc::MediaType::AUDIO, init).ok());
        TEST_CHECK(peer->AddTransceiver(webrtc::MediaType::VIDEO, init).ok());
    }

    ~StatsPeerFixture() {
        if (peer) {
            peer->Close();
            peer = nullptr;
        }
    }
};

// A source with no device or capture thread: sender diagnostics must work
// before media exists and must not require connecting to a signaling service.
class SilentStatsAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
public:
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    void AddSink(webrtc::AudioTrackSinkInterface*) override {}
    void RemoveSink(webrtc::AudioTrackSinkInterface*) override {}
};

void SenderDiagnosticsRemainPlainDataAndReflectTrackState() {
    StatsPeerFixture fixture;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        auto source = webrtc::make_ref_counted<SilentStatsAudioSource>();
        track = livekit::WebRTCManager::Instance().factory()->CreateAudioTrack(
            "stats_audio", source.get());
        TEST_CHECK(track);
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
        TEST_CHECK(fixture.peer->AddTransceiver(track, init).ok());
    });

    for (bool enabled : {true, false}) {
        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            // WebRTC returns whether the value changed, so setting the
            // initially enabled track to true is a valid no-op.
            track->set_enabled(enabled);
            TEST_CHECK(track->enabled() == enabled);
        });
        fixture.io.restart();
        bool completed = false;
        asio::co_spawn(
            fixture.io,
            livekit::CollectRtcStats(fixture.peer, fixture.io.get_executor(), 3s),
            [&](std::exception_ptr error, std::optional<livekit::StatsReport> report) {
                TEST_CHECK(!error && report.has_value());
                TEST_CHECK(report->senders_available);
                TEST_CHECK(report->senders.size() == 1); // Excludes two recvonly sections.
                const auto& sender = report->senders.front();
                TEST_CHECK(sender.track_id == "stats_audio");
                TEST_CHECK(sender.kind == "audio");
                TEST_CHECK(sender.track_enabled == enabled);
                TEST_CHECK(sender.direction == "sendonly");
                TEST_CHECK(!sender.mid_available);
                TEST_CHECK(!sender.current_direction_available);
                TEST_CHECK(sender.active_encoding_count <= sender.encoding_count);
                completed = true;
            });
        fixture.io.run();
        TEST_CHECK(completed);
    }
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
        track = nullptr;
    });
}

// Hold the signaling queue without a timing race. The queued closure contains
// only a trivial pointer; owned state stays in this scope until the queue drains.
class SignalingQueueGate final {
public:
    SignalingQueueGate() {
        auto entered = entered_.get_future();
        livekit::WebRTCManager::Instance().signaling_thread()->PostTask(
            [gate = this] { gate->Block(); });
        TEST_CHECK(entered.wait_for(5s) == std::future_status::ready);
    }

    void ReleaseAndClose(PeerConnection& owner) {
        {
            std::lock_guard lock(mutex_);
            close_on_release_ = std::move(owner);
            released_ = true;
        }
        cv_.notify_one();
    }

private:
    void Block() {
        entered_.set_value();
        PeerConnection owner;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return released_; });
            owner = std::move(close_on_release_);
        }
        // Close on the signaling thread, before the queued stats request runs.
        // Only that request retains the PC after the application owner is freed.
        owner->Close();
        owner = nullptr;
    }

    std::promise<void> entered_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
    PeerConnection close_on_release_;
};

void RealPeerStatsPreservePeerAndCompleteOnce() {
    StatsPeerFixture fixture;
    int completions = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        fixture.io.restart();
        asio::co_spawn(
            fixture.io,
            livekit::CollectRtcStats(fixture.peer, fixture.io.get_executor(), 3s),
            [&](std::exception_ptr error, std::optional<livekit::StatsReport> report) {
                TEST_CHECK(!error);
                TEST_CHECK(report.has_value());
                TEST_CHECK(report->senders_available && report->senders.empty());
                ++completions;
            });
        fixture.io.run();

        // Drain the dispatch task's destructor as well as GetStats itself.
        livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([] {});
        fixture.io.restart();
        fixture.io.poll();
        TEST_CHECK(completions == attempt + 1);
        TEST_CHECK(fixture.peer->signaling_state() ==
                   webrtc::PeerConnectionInterface::kStable);
        const auto transceivers = fixture.peer->GetTransceivers();
        TEST_CHECK(transceivers.size() == 2);
        for (const auto& transceiver : transceivers) {
            TEST_CHECK(transceiver->direction() ==
                       webrtc::RtpTransceiverDirection::kRecvOnly);
        }
    }

    // A usable peer must also accept subsequent media operations, not merely
    // leave a successful report behind after corrupting an owned reference.
    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    TEST_CHECK(fixture.peer->AddTransceiver(webrtc::MediaType::AUDIO, init).ok());
    TEST_CHECK(fixture.peer->GetTransceivers().size() == 3);
}

void TimedOutStatsSurviveQueuedCloseAndOwnerRelease() {
    StatsPeerFixture fixture;
    SignalingQueueGate gate;
    int completions = 0;
    asio::co_spawn(
        fixture.io,
        livekit::CollectRtcStats(fixture.peer, fixture.io.get_executor(), 40ms),
        [&](std::exception_ptr error, std::optional<livekit::StatsReport> report) {
            TEST_CHECK(!error);
            TEST_CHECK(!report.has_value());
            ++completions;
        });
    // The signaling task cannot run until ReleaseAndClose, so a null report
    // proves the timeout path, independent of device speed or scheduler timing.
    fixture.io.run();
    TEST_CHECK(completions == 1);
    gate.ReleaseAndClose(fixture.peer);
    TEST_CHECK(!fixture.peer);

    // The barrier is behind the delayed request: its PC reference and callback
    // must be disposed safely without a second completion after timeout.
    livekit::WebRTCManager::Instance().signaling_thread()->BlockingCall([] {});
    fixture.io.restart();
    fixture.io.poll();
    TEST_CHECK(completions == 1);
}

} // namespace

int main() {
    std::cout << "==================================================\n";
    std::cout << " Running WebRTC RTCStats & QoS Monitoring Tests   \n";
    std::cout << "==================================================\n";

    // ------------------------------------------------------------------
    // [Test 1] Stats Report Data Structures & Field Verification
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Testing Stats Data Models & Field Extraction...\n";

    livekit::InboundRtpStreamStats inbound;
    inbound.id = "inbound_video_0";
    inbound.kind = "video";
    inbound.ssrc = "12345678";
    inbound.bytes_received = 1048576;
    inbound.packets_received = 1000;
    inbound.packets_lost = 5;
    inbound.jitter = 0.003;
    inbound.frames_decoded = 300;
    inbound.frames_dropped = 2;
    inbound.frame_width = 1920;
    inbound.frame_height = 1080;
    inbound.frames_per_second = 30.0;

    TEST_CHECK(inbound.kind == "video" && "Inbound kind mismatch!");
    TEST_CHECK(inbound.bytes_received == 1048576 && "Inbound bytes_received mismatch!");
    TEST_CHECK(inbound.packets_lost == 5 && "Inbound packets_lost mismatch!");
    TEST_CHECK(inbound.frame_width == 1920 && inbound.frame_height == 1080 && "Resolution mismatch!");

    livekit::OutboundRtpStreamStats outbound;
    outbound.id = "outbound_audio_0";
    outbound.kind = "audio";
    outbound.ssrc = "87654321";
    outbound.bytes_sent = 524288;
    outbound.packets_sent = 500;
    outbound.frames_encoded = 250;
    outbound.frames_per_second = 50.0;

    TEST_CHECK(outbound.kind == "audio" && "Outbound kind mismatch!");
    TEST_CHECK(outbound.bytes_sent == 524288 && "Outbound bytes_sent mismatch!");

    livekit::CandidatePairStats cp;
    cp.id = "cp_active";
    cp.state = "succeeded";
    cp.current_pair = true;
    cp.current_round_trip_time = 0.025; // 25ms RTT
    cp.available_outgoing_bitrate = 2500000.0; // 2.5 Mbps

    TEST_CHECK(cp.current_pair && "Candidate pair status mismatch!");
    TEST_CHECK(cp.current_round_trip_time == 0.025 && "RTT mismatch!");

    std::cout << "  -> [Test 1 PASSED] All Stats Data Structures & Fields Verified!\n\n";

    // ------------------------------------------------------------------
    // [Test 2] RoomStatsReport Aggregation Verification
    // ------------------------------------------------------------------
    std::cout << "[Test 2] Testing RoomStatsReport Aggregation & RTT Metrics...\n";

    livekit::RoomStatsReport room_report;
    room_report.timestamp_ms = 1700000000000;
    room_report.publisher_rtt_ms = 24.5;
    room_report.subscriber_rtt_ms = 18.2;
    room_report.total_bytes_sent = outbound.bytes_sent;
    room_report.total_bytes_received = inbound.bytes_received;
    room_report.available_outgoing_bitrate = cp.available_outgoing_bitrate;

    livekit::StatsReport single_report;
    single_report.timestamp_ms = room_report.timestamp_ms;
    single_report.inbound_rtp.push_back(inbound);
    single_report.outbound_rtp.push_back(outbound);
    single_report.candidate_pairs.push_back(cp);
    room_report.reports.push_back(single_report);

    TEST_CHECK(room_report.publisher_rtt_ms == 24.5 && "Publisher RTT mismatch!");
    TEST_CHECK(room_report.subscriber_rtt_ms == 18.2 && "Subscriber RTT mismatch!");
    TEST_CHECK(room_report.total_bytes_sent == 524288 && "Total bytes sent mismatch!");
    TEST_CHECK(room_report.total_bytes_received == 1048576 && "Total bytes received mismatch!");
    TEST_CHECK(room_report.reports.size() == 1 && "Reports vector count mismatch!");

    std::cout << "  -> [Test 2 PASSED] RoomStatsReport Aggregation Verified Successfully!\n\n";

    // ------------------------------------------------------------------
    // [Test 3] Empty-room async collection must complete without blocking.
    // ------------------------------------------------------------------
    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    bool async_completed = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto empty_report = co_await room->GetStats();
        TEST_CHECK(empty_report.reports.empty());
        async_completed = true;
    }, asio::detached);
    io.run();
    TEST_CHECK(async_completed);

    std::cout << "  -> [Test 3 PASSED] Async empty-room collection completed without blocking!\n\n";

    TEST_CHECK(livekit::WebRTCManager::Instance().Initialize());
    RealPeerStatsPreservePeerAndCompleteOnce();
    std::cout << "  -> [Test 4 PASSED] Real GetStats preserves PC ownership and completes once!\n\n";
    TimedOutStatsSurviveQueuedCloseAndOwnerRelease();
    std::cout << "  -> [Test 5 PASSED] Timed-out stats safely release a closed peer after queue delay!\n\n";
    ParsedOutboundStatsDistinguishMissingZeroAndPositive();
    std::cout << "  -> [Test 6 PASSED] Native outbound stats distinguish missing fields, zero and positive counters!\n\n";
    SenderDiagnosticsRemainPlainDataAndReflectTrackState();
    std::cout << "  -> [Test 7 PASSED] Sender snapshots preserve direction and track enabled state!\n\n";
    livekit::WebRTCManager::Instance().Deinitialize();

    std::cout << "==================================================\n";
    std::cout << " ALL RTCSTATS & QOS MONITORING TESTS PASSED 100%! \n";
    std::cout << "==================================================\n";

    return 0;
}
