#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <future>
#include <cassert>
#include <sstream>
#include <atomic>
#include <asio.hpp>
#include <openssl/sha.h>
#include "signal_client.h"
#include "room.h"
#include "participant.h"
#include "track.h"
#include "webrtc_manager.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cout << "[ASSERT_FAILED] FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            std::cout.flush(); \
            fflush(stdout); \
            std::exit(1); \
        } \
    } while (0)

namespace {

static std::string Base64Encode(const unsigned char* buffer, size_t length) {
    static const char char_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i < length) {
        bool has_a = (i < length);
        uint32_t octet_a = has_a ? buffer[i++] : 0;
        bool has_b = (i < length);
        uint32_t octet_b = has_b ? buffer[i++] : 0;
        bool has_c = (i < length);
        uint32_t octet_c = has_c ? buffer[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        result.push_back(char_set[(triple >> 3 * 6) & 0x3F]);
        result.push_back(char_set[(triple >> 2 * 6) & 0x3F]);
        result.push_back(has_b ? char_set[(triple >> 1 * 6) & 0x3F] : '=');
        result.push_back(has_c ? char_set[(triple >> 0 * 6) & 0x3F] : '=');
    }
    return result;
}

class StressMockServer : public std::enable_shared_from_this<StressMockServer> {
public:
    StressMockServer(asio::io_context& io_ctx, uint16_t port = 0)
        : io_ctx_(io_ctx), acceptor_(io_ctx, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
        port_ = acceptor_.local_endpoint().port();
    }

    uint16_t port() const { return port_; }

    void SetRejectResume(bool reject) {
        reject_resume_ = reject;
    }

    void StartAccept() {
        auto self = shared_from_this();
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
        acceptor_.async_accept(*socket, [self, socket](std::error_code ec) {
            if (!ec) {
                self->HandleConnection(socket);
            }
            if (self->acceptor_.is_open()) {
                self->StartAccept();
            }
        });
    }

    void Stop() {
        std::error_code ec;
        acceptor_.close(ec);
    }

    void CloseActiveConnections() {
        std::error_code ec;
        std::lock_guard<std::mutex> lock(socket_mutex_);
        for (auto& s : active_sockets_) {
            if (s && s->is_open()) {
                s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                s->close(ec);
            }
        }
        active_sockets_.clear();
    }

private:
    void HandleConnection(std::shared_ptr<asio::ip::tcp::socket> socket) {
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            active_sockets_.push_back(socket);
        }
        auto self = shared_from_this();
        auto buffer = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *buffer, "\r\n\r\n",
            [self, socket, buffer](std::error_code ec, size_t) {
                if (!ec) {
                    std::istream request_stream(buffer.get());
                    std::string request_line;
                    std::getline(request_stream, request_line);
                    std::string header;
                    std::string key;
                    while (std::getline(request_stream, header) && header != "\r") {
                        if (header.find("Sec-WebSocket-Key:") == 0) {
                            key = header.substr(19);
                            if (!key.empty() && key.back() == '\r') key.pop_back();
                            while (!key.empty() && key.front() == ' ') key.erase(0, 1);
                        }
                    }
                    if (!key.empty()) {
                        self->PerformHandshake(socket, key, request_line);
                    }
                }
            });
    }

    void PerformHandshake(std::shared_ptr<asio::ip::tcp::socket> socket, const std::string& key, const std::string& request_line) {
        std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string accept = key + magic;
        unsigned char hash[20];
        SHA1(reinterpret_cast<const unsigned char*>(accept.c_str()), accept.length(), hash);
        std::string accept_b64 = Base64Encode(hash, 20);

        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_b64 + "\r\n\r\n";

        auto self = shared_from_this();
        auto response_buf = std::make_shared<std::string>(std::move(response));
        asio::async_write(*socket, asio::buffer(*response_buf),
            [self, socket, response_buf, request_line](std::error_code ec, size_t) {
                if (!ec) {
                    bool is_reconnect = (request_line.find("reconnect=1") != std::string::npos);
                    if (is_reconnect) {
                        if (self->reject_resume_) {
                            // 拒绝软重连：关闭连接迫使客户端降级为 Full Restart
                            std::error_code sec;
                            socket->close(sec);
                            return;
                        }
                        self->SendReconnectResponse(socket);
                    } else {
                        self->SendJoinResponse(socket);
                    }
                    self->ReadLoop(socket);
                }
            });
    }

    void SendJoinResponse(std::shared_ptr<asio::ip::tcp::socket> socket) {
        livekit::proto::SignalResponse resp;
        auto* join = resp.mutable_join();
        join->set_ping_interval(10);
        join->set_ping_timeout(20);
        auto* room = join->mutable_room();
        room->set_name("stress_room");
        room->set_sid("RM_STRESS_1");

        auto* participant = join->mutable_participant();
        participant->set_sid("PA_STRESS_LOCAL");
        participant->set_identity("stress_user");

        auto* config = join->mutable_client_configuration();
        config->set_resume_connection(livekit::proto::ClientConfigSetting::ENABLED);

        std::string serialized;
        resp.SerializeToString(&serialized);
        SendWsFrame(socket, serialized);
    }

    void SendReconnectResponse(std::shared_ptr<asio::ip::tcp::socket> socket) {
        livekit::proto::SignalResponse resp;
        auto* rec = resp.mutable_reconnect();
        auto* config = rec->mutable_client_configuration();
        config->set_resume_connection(livekit::proto::ClientConfigSetting::ENABLED);

        std::string serialized;
        resp.SerializeToString(&serialized);
        SendWsFrame(socket, serialized);
    }

    void SendWsFrame(std::shared_ptr<asio::ip::tcp::socket> socket, const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(0x82); // Binary frame, FIN=1
        size_t len = payload.length();
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i) {
                frame.push_back((len >> (i * 8)) & 0xFF);
            }
        }
        frame.insert(frame.end(), payload.begin(), payload.end());

        auto frame_buf = std::make_shared<std::vector<uint8_t>>(std::move(frame));
        asio::async_write(*socket, asio::buffer(*frame_buf),
            [frame_buf](std::error_code, size_t) {});
    }

    void ReadLoop(std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto self = shared_from_this();
        auto header_buf = std::make_shared<std::vector<uint8_t>>(2);
        asio::async_read(*socket, asio::buffer(*header_buf),
            [self, socket, header_buf](std::error_code ec, size_t) {
                if (!ec) {
                    uint8_t b2 = (*header_buf)[1];
                    bool masked = (b2 & 0x80) != 0;
                    uint64_t payload_len = b2 & 0x7F;

                    if (payload_len == 126) {
                        auto ext_len = std::make_shared<std::vector<uint8_t>>(2);
                        asio::async_read(*socket, asio::buffer(*ext_len),
                            [self, socket, masked](std::error_code ec, size_t) {
                                if (!ec) self->ReadRemaining(socket, masked, 0);
                            });
                    } else if (payload_len == 127) {
                        auto ext_len = std::make_shared<std::vector<uint8_t>>(8);
                        asio::async_read(*socket, asio::buffer(*ext_len),
                            [self, socket, masked](std::error_code ec, size_t) {
                                if (!ec) self->ReadRemaining(socket, masked, 0);
                            });
                    } else {
                        self->ReadRemaining(socket, masked, payload_len);
                    }
                }
            });
    }

    void ReadRemaining(std::shared_ptr<asio::ip::tcp::socket> socket, bool masked, uint64_t length) {
        auto self = shared_from_this();
        size_t total = (masked ? 4 : 0) + length;
        auto body_buf = std::make_shared<std::vector<uint8_t>>(total);
        asio::async_read(*socket, asio::buffer(*body_buf),
            [self, socket, body_buf, masked, length](std::error_code ec, size_t) {
                if (!ec) {
                    if (length > 0) {
                        std::vector<uint8_t> payload(length);
                        const uint8_t* mask_key = masked ? body_buf->data() : nullptr;
                        const uint8_t* data = body_buf->data() + (masked ? 4 : 0);
                        for (size_t i = 0; i < length; ++i) {
                            payload[i] = masked ? (data[i] ^ mask_key[i % 4]) : data[i];
                        }
                        livekit::proto::SignalRequest req;
                        if (req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                            if (req.has_ping_req()) {
                                livekit::proto::SignalResponse resp;
                                auto* pong = resp.mutable_pong_resp();
                                pong->set_last_ping_timestamp(req.ping_req().timestamp());
                                std::string serialized;
                                resp.SerializeToString(&serialized);
                                self->SendWsFrame(socket, serialized);
                            } else if (req.has_add_track()) {
                                livekit::proto::SignalResponse resp;
                                auto* pub = resp.mutable_track_published();
                                pub->set_cid(req.add_track().cid());
                                pub->mutable_track()->set_sid("TR_" + req.add_track().cid());
                                pub->mutable_track()->set_name(req.add_track().name());
                                pub->mutable_track()->set_type(req.add_track().type());
                                std::string serialized;
                                resp.SerializeToString(&serialized);
                                self->SendWsFrame(socket, serialized);
                            }
                        }
                    }
                    self->ReadLoop(socket);
                }
            });
    }

    asio::io_context& io_ctx_;
    asio::ip::tcp::acceptor acceptor_;
    uint16_t port_ = 0;
    std::mutex socket_mutex_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> active_sockets_;
    bool reject_resume_ = false;
};

static std::vector<std::shared_ptr<StressMockServer>> g_keep_alive_servers;

// 状态与生命周期记录观察者
class StressRoomListener : public livekit::RoomListener {
public:
    std::atomic<int> connected_count{0};
    std::atomic<int> disconnected_count{0};
    std::atomic<int> reconnecting_count{0};
    std::atomic<int> reconnected_count{0};
    std::atomic<int> republished_count{0};

    void OnConnected() override { connected_count.fetch_add(1); }
    void OnDisconnected(const std::string&) override { disconnected_count.fetch_add(1); }
    void OnReconnecting() override { reconnecting_count.fetch_add(1); }
    void OnReconnected() override { reconnected_count.fetch_add(1); }
    void OnLocalTrackRepublished(const std::string&, std::shared_ptr<livekit::TrackPublication>) override {
        republished_count.fetch_add(1);
    }
};

} // namespace

// ============================================================================
// Case 1: 100 轮极速快速进出房与析构竞态压力测试
// ============================================================================
asio::awaitable<void> TestCase1_RapidConnectDisconnect100Cycles(asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 1] Starting Rapid Connect/Disconnect (100 Iterations)..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::milliseconds(100);

    for (int i = 1; i <= 100; ++i) {
        auto room = livekit::Room::Create(executor);
        auto listener = std::make_shared<StressRoomListener>();
        room->AddListener(listener);

        if (i % 3 == 0) {
            // 模式 A：刚发起连接立即断开（测试握手在途取消）
            livekit::safe_co_spawn(executor, [room, url, opts]() -> asio::awaitable<void> {
                try {
                    co_await room->Connect(url, "token-rapid-stress", opts);
                } catch (...) {}
            });
            room->Disconnect();
        } else if (i % 3 == 1) {
            // 模式 B：微小延迟后断开
            livekit::safe_co_spawn(executor, [room, url, opts]() -> asio::awaitable<void> {
                try {
                    co_await room->Connect(url, "token-rapid-stress", opts);
                } catch (...) {}
            });
            asio::steady_timer t(executor, std::chrono::milliseconds(5));
            std::error_code ec;
            co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            room->Disconnect();
        } else {
            // 模式 C：同步等待就绪后立即断开
            try {
                co_await room->Connect(url, "token-rapid-stress", opts);
            } catch (...) {}
            room->Disconnect();
        }

        // 销毁 room 智能指针，验证 RAII 安全性
        room.reset();

        if (i % 25 == 0) {
            std::cout << "  -> Completed " << i << " / 100 rapid connect-disconnect cycles..." << std::endl;
        }
    }

    // 暂停 150ms 允许底层清理完成
    asio::steady_timer flush_timer(executor, std::chrono::milliseconds(150));
    std::error_code ec;
    co_await flush_timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 1: 100 Rapid Connect/Disconnect Cycles Finished with Zero Crashes/Deadlocks!" << std::endl;
}

// ============================================================================
// Case 2: 突发断网与 Soft Reconnect (SyncState) 状态自愈测试
// ============================================================================
asio::awaitable<void> TestCase2_SuddenDropAndSoftReconnect(asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 2] Starting Sudden Drop & Soft Reconnect Test..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server1 = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server1);
    server1->StartAccept();

    uint16_t port = server1->port();
    std::string url = "ws://127.0.0.1:" + std::to_string(port);
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.timeouts.reconnect_attempt = std::chrono::milliseconds(500);
    opts.timeouts.reconnect_total = std::chrono::seconds(5);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<StressRoomListener>();
    room->AddListener(listener);

    bool ok = co_await room->Connect(url, "token-drop-test", opts);
    TEST_ASSERT(ok, "Initial room connection failed");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Room not in connected state");

    // 挂载本地虚拟发布 Track
    auto track = std::make_shared<livekit::Track>("stress_track_1", "camera", livekit::TrackKind::Video);
    auto pub = std::make_shared<livekit::TrackPublication>(track, "TR_STRESS_101", "camera");
    room->local_participant()->add_publication(pub);

    // 模拟突发断网：服务端强行关闭 TCP
    server1->CloseActiveConnections();
    server1->Stop();

    // 等待客户端感知断网并进入 Reconnecting
    asio::steady_timer t(executor, std::chrono::milliseconds(100));
    std::error_code ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Reconnecting, "Room did not transition to Reconnecting");
    TEST_ASSERT(listener->reconnecting_count.load() >= 1, "OnReconnecting not called");

    // 重启服务端支持 Resume
    auto server2 = std::make_shared<StressMockServer>(io_ctx, port);
    g_keep_alive_servers.push_back(server2);
    server2->StartAccept();

    // 等待软重连完成
    t.expires_after(std::chrono::milliseconds(800));
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Room failed to soft reconnect");
    TEST_ASSERT(listener->reconnected_count.load() >= 1, "OnReconnected was not dispatched");
    TEST_ASSERT(room->local_participant()->tracks().size() == 1, "Local publication lost during soft reconnect");

    room->Disconnect();
    server2->CloseActiveConnections();
    server2->Stop();
    std::cout << "[PASS] Test 2: Sudden Drop & Soft Reconnect (SyncState) Successfully Verified!" << std::endl;
}

// ============================================================================
// Case 3: 连续断网扰动与硬重连降级 (Hard Reconnect Fallback)
// ============================================================================
asio::awaitable<void> TestCase3_HardReconnectFallback(asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 3] Starting Hard Reconnect Fallback Test..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server1 = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server1);
    server1->StartAccept();

    uint16_t port = server1->port();
    std::string url = "ws://127.0.0.1:" + std::to_string(port);
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.timeouts.reconnect_attempt = std::chrono::milliseconds(200);
    opts.timeouts.reconnect_total = std::chrono::seconds(10);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<StressRoomListener>();
    room->AddListener(listener);

    bool ok = co_await room->Connect(url, "token-hard-test", opts);
    TEST_ASSERT(ok, "Initial room connection failed");

    // 强行断网
    server1->CloseActiveConnections();
    server1->Stop();

    // 启动服务端，但配置为拒绝 Resume（强迫降级为 Full Restart）
    auto server2 = std::make_shared<StressMockServer>(io_ctx, port);
    g_keep_alive_servers.push_back(server2);
    server2->SetRejectResume(true);
    server2->StartAccept();

    // 等待降级 Full Restart 重新建联
    asio::steady_timer t(executor, std::chrono::milliseconds(100));
    std::error_code ec;

    for (int wait_idx = 0; wait_idx < 30; ++wait_idx) {
        if (room->connection_state() == livekit::ConnectionState::Connected && listener->reconnected_count.load() >= 1) {
            break;
        }
        t.expires_after(std::chrono::milliseconds(100));
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Room failed to full restart reconnect");
    TEST_ASSERT(listener->reconnected_count.load() >= 1, "OnReconnected not triggered after hard reconnect");

    room->Disconnect();
    server2->CloseActiveConnections();
    server2->Stop();
    std::cout << "[PASS] Test 3: Hard Reconnect Fallback Verified!" << std::endl;
}

// ============================================================================
// Case 4: 并发多线程事件密集投递与析构竞态测试
// ============================================================================
asio::awaitable<void> TestCase4_ConcurrentEventAndDestructionRace(asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 4] Starting Concurrent Event & Destruction Race Test..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;

    for (int cycle = 0; cycle < 10; ++cycle) {
        auto room = livekit::Room::Create(executor);
        co_await room->Connect(url, "token-concurrent", opts);

        std::atomic<bool> stress_running{true};
        std::vector<std::thread> workers;

        // 启动 4 个高并发工作线程，密集调用 SDK 数据、音量、日志与属性修改
        for (int w = 0; w < 4; ++w) {
            workers.emplace_back([room, &stress_running, w]() {
                int count = 0;
                while (stress_running.load()) {
                    if (w == 0) {
                        room->SetParticipantVolume("stress_user", 0.5);
                    } else if (w == 1) {
                        room->SetParticipantMuted("stress_user", (count % 2 == 0));
                    } else if (w == 2) {
                        room->Log("TEST", "CONCURRENT", "Worker thread log flood #" + std::to_string(count));
                    } else {
                        std::vector<uint8_t> payload = {1, 2, 3, 4};
                        room->PublishData(payload, false);
                    }
                    count++;
                    std::this_thread::yield();
                }
            });
        }

        // 主线程等待 20ms 后突然发起 Disconnect
        asio::steady_timer t(executor, std::chrono::milliseconds(20));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        room->Disconnect();
        stress_running = false;

        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }

        room.reset();
    }

    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 4: Concurrent Event & Destruction Race Finished Safely!" << std::endl;
}

// ============================================================================
// 主入口
// ============================================================================
int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  LiveKit Native C++ SDK Lifecycle & Reconnect Stress Harness" << std::endl;
    std::cout << "========================================================" << std::endl;

    asio::io_context io_ctx;
    auto work_guard = asio::make_work_guard(io_ctx);

    asio::co_spawn(io_ctx, [&]() -> asio::awaitable<void> {
        try {
            auto executor = co_await asio::this_coro::executor;
            co_await TestCase1_RapidConnectDisconnect100Cycles(executor);
            co_await TestCase2_SuddenDropAndSoftReconnect(executor);
            co_await TestCase3_HardReconnectFallback(executor);
            co_await TestCase4_ConcurrentEventAndDestructionRace(executor);

            std::cout << "\n[ALL STRESS TESTS PASSED SUCCESSFULLY]" << std::endl;
            for (auto& s : g_keep_alive_servers) {
                if (s) {
                    s->CloseActiveConnections();
                    s->Stop();
                }
            }
            g_keep_alive_servers.clear();
            work_guard.reset();
        } catch (const std::exception& e) {
            std::cerr << "[STRESS TEST EXCEPTION] " << e.what() << std::endl;
            std::exit(1);
        } catch (...) {
            std::cerr << "[STRESS TEST UNKNOWN EXCEPTION]" << std::endl;
            std::exit(1);
        }
    }, asio::detached);

    io_ctx.run();
    return 0;
}
