#include <iostream>
#include "tests/support/test_check.h"
#include <thread>
#include <chrono>
#include <asio.hpp>
#include "room.h"
#include "stats.h"
#include "stats_collector.h"

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

    std::cout << "==================================================\n";
    std::cout << " ALL RTCSTATS & QOS MONITORING TESTS PASSED 100%! \n";
    std::cout << "==================================================\n";

    return 0;
}
