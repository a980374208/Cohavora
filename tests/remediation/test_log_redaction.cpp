#include "log_redaction.h"
#include "room.h"
#include "signal_client.h"
#include "livekit_rtc.pb.h"
#include "tests/support/test_check.h"

#include <asio.hpp>

#include <atomic>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

namespace livekit {

class SignalClientIrSec001TestAccess final {
public:
    static void DeliverHeartbeatFailure(SignalClient& client, const std::error_code& error) {
        client.HandleHeartbeatFailure(error);
    }

    static void DeliverClose(SignalClient& client, const std::string& detail) {
        client.HandleClose(detail);
    }
};

class RoomIrSec001TestAccess final {
public:
    static void DeliverClose(Room& room, const SignalEvent& event) {
        uint64_t generation = 0;
        {
            std::lock_guard lock(room.room_mutex_);
            // Model an installed Connected session, including its owner token.
            generation = room.session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            room.installed_session_generation_ = generation;
            room.connection_state_ = ConnectionState::Connected;
            room.reconnect_disabled_ = true;
        }
        room.HandleSignalEvent(event, generation);
    }
};

} // namespace livekit

namespace {

class SensitiveHeartbeatCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "ir_sec_001"; }

    std::string message(int) const override {
        return "access_token=heartbeat-secret Authorization: Bearer heartbeat-credential";
    }
};

class DisconnectCapture final : public livekit::RoomListener {
public:
    void OnDisconnected(livekit::RoomDisconnectReason reason,
                        const std::string& detail) override {
        ++calls;
        last_reason = reason;
        last_detail = detail;
    }

    int calls = 0;
    livekit::RoomDisconnectReason last_reason = livekit::RoomDisconnectReason::Unknown;
    std::string last_detail;
};

void TestEndpointSummary() {
    constexpr const char* kSecret = "synthetic-url-secret";
    const std::string raw =
        "wss://user:credential@example.invalid:8443/rtc/v1?access_token=" +
        std::string(kSecret) + "&join_request=opaque#fragment";
    const auto summary = livekit::secure_log::EndpointSummary(raw);
    TEST_CHECK(summary == "endpoint{scheme=wss,route=rtc_v1,port=8443,query=yes}");
    TEST_CHECK(summary.find(kSecret) == std::string::npos);
    TEST_CHECK(summary.find("example.invalid") == std::string::npos);
    TEST_CHECK(summary.find("credential") == std::string::npos);

    TEST_CHECK(livekit::secure_log::EndpointSummary(
        "ws://[::1]:7880/rtc?access_token=x") ==
        "endpoint{scheme=ws,route=rtc,port=7880,query=yes}");
    TEST_CHECK(livekit::secure_log::EndpointSummary("ws://host:99999/rtc") ==
        "endpoint{invalid}");
    TEST_CHECK(livekit::secure_log::EndpointSummary(
        std::string("ws://host/rtc?") + std::string(1, '\0') + "access_token=x") ==
        "endpoint{invalid}");
}

void TestBoundaryFiltering() {
    constexpr const char* kSecret = "synthetic-boundary-secret";
    const std::vector<std::string> unsafe = {
        std::string("access_token=") + kSecret,
        std::string("Authorization: Bearer ") + kSecret,
        std::string("Proxy-Authorization: Basic ") + kSecret,
        std::string("Set-Cookie: session=") + kSecret,
        std::string("a=ice-pwd:") + kSecret,
        std::string("candidate:1 1 UDP 1 10.0.0.1 999 typ host ") + kSecret,
        std::string("eyJheader.") + kSecret + ".signature"
    };
    for (const auto& value : unsafe) {
        const auto safe = livekit::secure_log::SanitizeForOutput(value);
        TEST_CHECK(safe == "[redacted: sensitive log field]");
        TEST_CHECK(safe.find(kSecret) == std::string::npos);
        TEST_CHECK(livekit::secure_log::SanitizeForOutput(safe) == safe);
    }
    TEST_CHECK(livekit::secure_log::SanitizeForOutput("stage=connect\r\nforged=true") ==
        "stage=connect forged=true");
    TEST_CHECK(livekit::secure_log::SanitizeForOutput(
        std::string(livekit::secure_log::kMaxInputBytes + 1, 'x')) ==
        "[omitted: oversized log field]");
}

void TestTypedSummaries() {
    constexpr const char* kSdpSecret = "synthetic-sdp-secret";
    const std::string sdp =
        "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "m=private 9 X 1\r\na=ice-pwd:" + std::string(kSdpSecret) + "\r\n";
    const auto summary = livekit::secure_log::SdpSummary("remote_answer", sdp);
    TEST_CHECK(summary.find("audio=1") != std::string::npos);
    TEST_CHECK(summary.find("video=1") != std::string::npos);
    TEST_CHECK(summary.find("application=1") != std::string::npos);
    TEST_CHECK(summary.find("other=1") != std::string::npos);
    TEST_CHECK(summary.find(kSdpSecret) == std::string::npos);

    const auto error = livekit::secure_log::ErrorCodeSummary(
        "websocket_upgrade", 401, "websocket_http");
    TEST_CHECK(error.find("stage=websocket_upgrade") != std::string::npos);
    TEST_CHECK(error.find("category=websocket_http") != std::string::npos);
    TEST_CHECK(error.find("code=401") != std::string::npos);
}

void TestRoomBoundary() {
    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    std::string received;
    room->SetLogHandler([&](const std::string&, const std::string&, const std::string& message) {
        received = message;
    });
    room->Log("ERROR", "SYNTHETIC", "access_token=synthetic-room-secret");
    TEST_CHECK(received == "[redacted: sensitive log field]");
    TEST_CHECK(received.find("synthetic-room-secret") == std::string::npos);
}

void TestCloseBusinessAndOutputSeparation() {
    asio::io_context io;
    livekit::SignalEvent captured{};
    int signal_events = 0;
    auto signal = std::make_shared<livekit::SignalClient>(
        "wss://internal.invalid",
        "unused-token",
        livekit::SignalOptions{},
        false,
        std::make_shared<livekit::proto::JoinResponse>(),
        [&](const livekit::SignalEvent& event) {
            captured = event;
            ++signal_events;
        },
        io.get_executor());

    static const SensitiveHeartbeatCategory heartbeat_category;
    const std::error_code heartbeat_error(73, heartbeat_category);
    const std::string expected_heartbeat =
        "heartbeat timer error: " + heartbeat_error.message();
    livekit::SignalClientIrSec001TestAccess::DeliverHeartbeatFailure(
        *signal, asio::error::operation_aborted);
    TEST_CHECK(signal_events == 0);
    livekit::SignalClientIrSec001TestAccess::DeliverHeartbeatFailure(
        *signal, heartbeat_error);
    TEST_CHECK(signal_events == 1);
    TEST_CHECK(captured.type == livekit::SignalEvent::Close);
    TEST_CHECK(captured.close_reason == expected_heartbeat);

    auto room = livekit::Room::Create(io.get_executor());
    auto listener = std::make_shared<DisconnectCapture>();
    room->AddListener(listener);
    std::string logged;
    room->SetLogHandler([&](const std::string&, const std::string&, const std::string& message) {
        logged = message;
    });
    livekit::RoomIrSec001TestAccess::DeliverClose(*room, captured);
    TEST_CHECK(listener->calls == 1);
    TEST_CHECK(listener->last_reason == livekit::RoomDisconnectReason::NetworkError);
    TEST_CHECK(listener->last_detail == expected_heartbeat);

    room->Log("ERROR", "HEARTBEAT_FAILURE", expected_heartbeat);
    TEST_CHECK(logged == "[redacted: sensitive log field]");
    TEST_CHECK(logged.find("heartbeat-secret") == std::string::npos);
    TEST_CHECK(logged.find("heartbeat-credential") == std::string::npos);

    const std::string normal_close = "normal server shutdown";
    livekit::SignalClientIrSec001TestAccess::DeliverClose(*signal, normal_close);
    TEST_CHECK(signal_events == 2);
    TEST_CHECK(captured.close_reason == normal_close);

    const std::string reconnect_detail =
        "Reconnect failed: credential=reconnect-secret";
    livekit::SignalClientIrSec001TestAccess::DeliverClose(*signal, reconnect_detail);
    TEST_CHECK(signal_events == 3);
    TEST_CHECK(captured.close_reason == reconnect_detail);

    auto reconnect_room = livekit::Room::Create(io.get_executor());
    auto reconnect_listener = std::make_shared<DisconnectCapture>();
    reconnect_room->AddListener(reconnect_listener);
    livekit::RoomIrSec001TestAccess::DeliverClose(*reconnect_room, captured);
    TEST_CHECK(reconnect_listener->calls == 1);
    TEST_CHECK(reconnect_listener->last_detail == reconnect_detail);
}

void TestConcurrentDeterminism() {
    constexpr int kThreads = 8;
    constexpr int kIterations = 500;
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const auto result = livekit::secure_log::EndpointSummary(
                    "wss://tenant.invalid/rtc?access_token=synthetic-thread-secret");
                if (result != "endpoint{scheme=wss,route=rtc,port=443,query=yes}") {
                    ok.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    TEST_CHECK(ok.load(std::memory_order_relaxed));
}

} // namespace

int main() {
    TestEndpointSummary();
    TestBoundaryFiltering();
    TestTypedSummaries();
    TestRoomBoundary();
    TestCloseBusinessAndOutputSeparation();
    TestConcurrentDeterminism();
    return 0;
}
