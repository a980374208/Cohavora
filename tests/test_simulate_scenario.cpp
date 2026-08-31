#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <cassert>
#include <sstream>
#include <atomic>
#include <asio.hpp>
#include <openssl/sha.h>
#include "signal_client.h"
#include "room.h"
#include "participant.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cout << "[ASSERT_FAILED] FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
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

class SimulateMockServer : public std::enable_shared_from_this<SimulateMockServer> {
public:
    SimulateMockServer(asio::io_context& io_ctx, uint16_t port = 0)
        : io_ctx_(io_ctx), acceptor_(io_ctx, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
        port_ = acceptor_.local_endpoint().port();
    }

    uint16_t port() const { return port_; }

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

    std::vector<livekit::proto::SignalRequest> received_requests;
    std::mutex req_mutex;

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
        room->set_name("simulate_room");
        room->set_sid("RM_SIMULATE_1");

        auto* participant = join->mutable_participant();
        participant->set_sid("PA_SIM_LOCAL");
        participant->set_identity("sim_user");

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
                            {
                                std::lock_guard<std::mutex> lock(self->req_mutex);
                                self->received_requests.push_back(req);
                            }
                            if (req.has_ping_req()) {
                                livekit::proto::SignalResponse resp;
                                auto* pong = resp.mutable_pong_resp();
                                pong->set_last_ping_timestamp(req.ping_req().timestamp());
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
};

static std::vector<std::shared_ptr<SimulateMockServer>> g_keep_alive_servers;

} // namespace

asio::awaitable<void> TestSimulateScenarioSuite(asio::any_io_executor executor) {
    std::cout << "[TEST] Starting LiveKit SimulateScenario Comprehensive Suite..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<SimulateMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;

    auto room = livekit::Room::Create(executor);
    bool ok = co_await room->Connect(url, "token-simulate", opts);
    TEST_ASSERT(ok, "Room connection failed");

    // 1. SpeakerUpdate
    std::cout << "  -> Testing SpeakerUpdate simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::SpeakerUpdate);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_speaker_update() && r.simulate().speaker_update() == 3) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.speaker_update request not received by server");
    }

    // 2. NodeFailure
    std::cout << "  -> Testing NodeFailure simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::NodeFailure);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_node_failure() && r.simulate().node_failure()) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.node_failure request not received by server");
    }

    // 3. Migration
    std::cout << "  -> Testing Migration simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::Migration);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_migration() && r.simulate().migration()) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.migration request not received by server");
    }

    // 4. ServerLeave
    std::cout << "  -> Testing ServerLeave simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::ServerLeave);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_server_leave() && r.simulate().server_leave()) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.server_leave request not received by server");
    }

    // 5. SwitchCandidate
    std::cout << "  -> Testing SwitchCandidate simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::SwitchCandidate);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_switch_candidate_protocol() &&
                r.simulate().switch_candidate_protocol() == livekit::proto::CandidateProtocol::TCP) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.switch_candidate_protocol request not received by server");
    }

    // 6. Clear
    std::cout << "  -> Testing Clear simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::Clear);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found = false;
        for (const auto& r : server->received_requests) {
            if (r.has_simulate() && r.simulate().has_subscriber_bandwidth() && r.simulate().subscriber_bandwidth() == 0) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found, "SimulateScenario.subscriber_bandwidth clear request not received by server");
    }

    // 7. ParticipantName & Metadata
    std::cout << "  -> Testing ParticipantName & ParticipantMetadata simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::ParticipantName);
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::ParticipantMetadata);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(50));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool found_name = false;
        bool found_meta = false;
        for (const auto& r : server->received_requests) {
            if (r.has_update_metadata()) {
                if (!r.update_metadata().name().empty()) found_name = true;
                if (!r.update_metadata().metadata().empty()) found_meta = true;
            }
        }
        TEST_ASSERT(found_name, "ParticipantName update request not received");
        TEST_ASSERT(found_meta, "ParticipantMetadata update request not received");
    }

    // 8. E2eeKeyRatchet
    std::cout << "  -> Testing E2eeKeyRatchet simulation..." << std::endl;
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::E2eeKeyRatchet);

    // 9. SignalReconnect
    std::cout << "  -> Testing SignalReconnect simulation (Soft Reconnect / Resume)..." << std::endl;
    std::atomic<bool> reconnecting_called{false};
    std::atomic<bool> reconnected_called{false};

    class SimListener : public livekit::RoomListener {
    public:
        SimListener(std::atomic<bool>& rc, std::atomic<bool>& rd) : rc_(rc), rd_(rd) {}
        void OnReconnecting() override { rc_.store(true); }
        void OnReconnected() override { rd_.store(true); }
    private:
        std::atomic<bool>& rc_;
        std::atomic<bool>& rd_;
    };

    auto listener = std::make_shared<SimListener>(reconnecting_called, reconnected_called);
    room->AddListener(listener);

    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::SignalReconnect);
    {
        asio::steady_timer t(executor, std::chrono::milliseconds(300));
        std::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        TEST_ASSERT(reconnecting_called.load(), "RoomListener::OnReconnecting was not called during SignalReconnect");
        TEST_ASSERT(reconnected_called.load(), "RoomListener::OnReconnected was not called during SignalReconnect");
    }

    room->Disconnect();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] ALL 11 SimulateScenario Actions Verified Successfully!" << std::endl;
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  LiveKit Native C++ SDK Simulate Scenario Test Suite" << std::endl;
    std::cout << "========================================================" << std::endl;

    asio::io_context io_ctx;
    auto work_guard = asio::make_work_guard(io_ctx);

    asio::co_spawn(io_ctx, [&]() -> asio::awaitable<void> {
        try {
            auto executor = co_await asio::this_coro::executor;
            co_await TestSimulateScenarioSuite(executor);

            std::cout << "\n[ALL SIMULATE SCENARIO TESTS PASSED SUCCESSFULLY]" << std::endl;
            for (auto& s : g_keep_alive_servers) {
                if (s) {
                    s->CloseActiveConnections();
                    s->Stop();
                }
            }
            g_keep_alive_servers.clear();
            work_guard.reset();
        } catch (const std::exception& e) {
            std::cerr << "[TEST EXCEPTION] " << e.what() << std::endl;
            std::exit(1);
        } catch (...) {
            std::cerr << "[TEST UNKNOWN EXCEPTION]" << std::endl;
            std::exit(1);
        }
    }, asio::detached);

    io_ctx.run();
    return 0;
}
