#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <future>
#include <cassert>
#include <sstream>
#include <asio.hpp>
#include <openssl/sha.h>
#include "signal_client.h"
#include "room.h"
#include "participant.h"
#include "track.h"
#include "webrtc_manager.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

namespace stdext {
    class exception;
}
namespace std {
    void (__cdecl* _Raise_handler)(class stdext::exception const &) = nullptr;
}

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cout << "[ASSERT_FAILED] FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            std::cout.flush(); \
            fflush(stdout); \
            std::exit(1); \
        } \
    } while (0)

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

class MockServer {
public:
    MockServer(asio::io_context& io_ctx, uint16_t port = 0)
        : io_ctx_(io_ctx), acceptor_(io_ctx, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
        port_ = acceptor_.local_endpoint().port();
    }

    uint16_t port() const { return port_; }

    void StartAccept() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
        std::cout << "MockServer: StartAccept called, waiting on port " << port_ << std::endl;
        acceptor_.async_accept(*socket, [this, socket](std::error_code ec) {
            std::cout << "MockServer: async_accept callback triggered, ec=" << ec.message() << std::endl;
            if (!ec) {
                HandleConnection(socket);
            } else {
                std::cout << "MockServer: accept error: " << ec.message() << std::endl;
            }
            if (acceptor_.is_open()) {
                StartAccept();
            }
        });
    }

    void Stop() {
        std::error_code ec;
        acceptor_.close(ec);
    }

    void CloseActiveConnections() {
        std::error_code ec;
        for (auto& s : active_sockets_) {
            if (s && s->is_open()) {
                s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                s->close(ec);
            }
        }
        active_sockets_.clear();
        for (auto& s : keep_alive_sockets_) {
            if (s && s->is_open()) {
                s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                s->close(ec);
            }
        }
        keep_alive_sockets_.clear();
    }

    void SetValidationStatus(int status) { validation_status_ = status; }
    void SetV1Status(int status) { v1_status_ = status; }
    void SetRegionJson(std::string json) { region_json_ = json; }
    void SetMockJoinSids(std::string sid) { mock_sid_ = sid; }
    void SetJoinPublishCodecs(std::vector<std::string> codecs) {
        join_publish_codecs_ = std::move(codecs);
    }
    
    std::vector<livekit::proto::SignalRequest> received_requests;
    std::mutex req_mutex;
    std::atomic<int> v1_requests{0};
    std::atomic<int> v0_requests{0};

    void SendResponse(std::shared_ptr<asio::ip::tcp::socket> socket, const livekit::proto::SignalResponse& resp) {
        std::vector<uint8_t> payload(resp.ByteSizeLong());
        resp.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
        
        std::vector<uint8_t> frame;
        frame.push_back(0x82); // FIN, Binary
        size_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
            }
        }
        frame.insert(frame.end(), payload.begin(), payload.end());
        
        std::error_code ec;
        asio::write(*socket, asio::buffer(frame), ec);
        std::cout << "MockServer: SendResponse: sent WS frame, total bytes=" << frame.size() << ", payload bytes=" << payload.size() << ", ec=" << ec.message() << std::endl;
    }

private:
    void HandleConnection(std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto buf = std::make_shared<asio::streambuf>();
        std::cout << "MockServer: Accepted connection, reading..." << std::endl;
        asio::async_read_until(*socket, *buf, "\r\n\r\n", [this, socket, buf](std::error_code ec, size_t) {
            if (ec) {
                std::cout << "MockServer: Read error: " << ec.message() << std::endl;
                return;
            }
            std::istream is(buf.get());
            std::string req_line;
            std::getline(is, req_line);
            std::cout << "MockServer: Got request: " << req_line << std::endl;
            
            if (req_line.find("GET /rtc/validate") != std::string::npos || req_line.find("GET /rtc/v1/validate") != std::string::npos) {
                std::cout << "MockServer: Handling validate status=" << validation_status_ << std::endl;
                std::stringstream ss;
                ss << "HTTP/1.1 " << validation_status_ << " ";
                if (validation_status_ == 200) ss << "OK\r\n";
                else if (validation_status_ == 401) ss << "Unauthorized\r\n";
                else ss << "Service Unavailable\r\n";
                ss << "Content-Length: 0\r\nConnection: close\r\n\r\n";
                asio::write(*socket, asio::buffer(ss.str()));
            } else if (req_line.find("GET /settings/regions") != std::string::npos) {
                std::cout << "MockServer: Handling settings regions" << std::endl;
                std::stringstream ss;
                ss << "HTTP/1.1 200 OK\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << region_json_.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << region_json_;
                asio::write(*socket, asio::buffer(ss.str()));
            } else if (req_line.find("GET /rtc") != std::string::npos) {
                const bool is_v1 = req_line.find("GET /rtc/v1") != std::string::npos;
                if (is_v1) {
                    v1_requests.fetch_add(1);
                } else {
                    v0_requests.fetch_add(1);
                }
                const int handshake_status = is_v1 ? v1_status_ : validation_status_;
                std::cout << "MockServer: Handling GET /rtc, validation_status=" << validation_status_ << std::endl;
                if (handshake_status != 200) {
                    std::cout << "MockServer: validation failed, writing " << handshake_status << std::endl;
                    std::stringstream ss;
                    ss << "HTTP/1.1 " << handshake_status << " Error\r\n"
                       << "Content-Length: 0\r\n"
                       << "Connection: close\r\n\r\n";
                    asio::write(*socket, asio::buffer(ss.str()));
                    std::cout << "MockServer: validation error response written, returning" << std::endl;
                    return;
                }
                std::string header;
                std::string key;
                while (std::getline(is, header) && header != "\r") {
                    if (header.find("Sec-WebSocket-Key:") != std::string::npos) {
                        key = header.substr(header.find(':') + 1);
                        key.erase(0, key.find_first_not_of(" \t"));
                        key.erase(key.find_last_not_of(" \t\r") + 1);
                    }
                }
                
                std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                std::string key_magic = key + magic;
                unsigned char hash[SHA_DIGEST_LENGTH];
                SHA1(reinterpret_cast<const unsigned char*>(key_magic.data()), key_magic.size(), hash);
                std::string accept_val = Base64Encode(hash, SHA_DIGEST_LENGTH);
                
                std::stringstream ss;
                ss << "HTTP/1.1 101 Switching Protocols\r\n"
                   << "Upgrade: websocket\r\n"
                   << "Connection: Upgrade\r\n"
                   << "Sec-WebSocket-Accept: " << accept_val << "\r\n\r\n";
                asio::write(*socket, asio::buffer(ss.str()));
                std::cout << "MockServer: Switching protocols 101 written" << std::endl;
                active_sockets_.push_back(socket);
                
                if (req_line.find("reconnect=1") != std::string::npos) {
                    if (dummy_reconnect_) {
                        std::cout << "MockServer: Dummy reconnect mode active. Storing socket to keep alive for timeout." << std::endl;
                        keep_alive_sockets_.push_back(socket);
                        return;
                    }
                    livekit::proto::SignalResponse resp;
                    resp.mutable_reconnect();
                    SendResponse(socket, resp);
                } else {
                    livekit::proto::SignalResponse resp;
                    auto* join = resp.mutable_join();
                    join->set_ping_interval(1);
                    join->set_ping_timeout(2);
                    auto* part = join->mutable_participant();
                    part->set_sid(mock_sid_);
                    for (const auto& mime : join_publish_codecs_) {
                        join->add_enabled_publish_codecs()->set_mime(mime);
                    }
                    SendResponse(socket, resp);

                    if (mock_sid_ == "participant_event_ready") {
                        livekit::proto::SignalResponse update_resp;
                        auto* update = update_resp.mutable_update();
                        auto* p = update->add_participants();
                        p->set_sid("ready_alice");
                        p->set_identity("alice");
                        p->set_state(livekit::proto::ParticipantInfo::JOINED);
                        SendResponse(socket, update_resp);
                    }
                }

                ReadWsFrame(socket);
            }
        });
    }

    void ReadWsFrame(std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto header = std::make_shared<std::vector<uint8_t>>(2);
        asio::async_read(*socket, asio::buffer(*header), [this, socket, header](std::error_code ec, size_t) {
            if (ec) {
                std::cout << "MockServer: ReadWsFrame header read error: " << ec.message() << std::endl;
                return;
            }
            uint8_t opcode = (*header)[0] & 0x0F;
            bool masked = ((*header)[1] & 0x80) != 0;
            uint64_t len = (*header)[1] & 0x7F;
            std::cout << "MockServer: ReadWsFrame header read, opcode=" << (int)opcode << " masked=" << masked << " len=" << len << std::endl;
            
            if (len == 126) {
                auto ext_len = std::make_shared<std::vector<uint8_t>>(2);
                asio::async_read(*socket, asio::buffer(*ext_len), [this, socket, opcode, masked, ext_len](std::error_code ec, size_t) {
                    if (ec) return;
                    uint64_t payload_len = (static_cast<uint64_t>((*ext_len)[0]) << 8) | (*ext_len)[1];
                    ReadWsPayload(socket, opcode, masked, payload_len);
                });
            } else if (len == 127) {
                auto ext_len = std::make_shared<std::vector<uint8_t>>(8);
                asio::async_read(*socket, asio::buffer(*ext_len), [this, socket, opcode, masked, ext_len](std::error_code ec, size_t) {
                    if (ec) return;
                    uint64_t payload_len = 0;
                    for (int i = 0; i < 8; ++i) {
                        payload_len = (payload_len << 8) | (*ext_len)[i];
                    }
                    ReadWsPayload(socket, opcode, masked, payload_len);
                });
            } else {
                ReadWsPayload(socket, opcode, masked, len);
            }
        });
    }

    void ReadWsPayload(std::shared_ptr<asio::ip::tcp::socket> socket, uint8_t opcode, bool masked, uint64_t payload_len) {
        auto mask_key = std::make_shared<std::vector<uint8_t>>(masked ? 4 : 0);
        auto read_payload = [this, socket, opcode, masked, mask_key, payload_len]() {
            auto payload = std::make_shared<std::vector<uint8_t>>(payload_len);
            asio::async_read(*socket, asio::buffer(*payload), [this, socket, opcode, masked, mask_key, payload](std::error_code ec, size_t) {
                if (ec) return;
                if (masked) {
                    for (size_t i = 0; i < payload->size(); ++i) {
                        (*payload)[i] ^= (*mask_key)[i % 4];
                    }
                }
                
                if (opcode == 0x8) { 
                    std::error_code err;
                    socket->close(err);
                    return;
                } else if (opcode == 0x9) { 
                    std::vector<uint8_t> frame;
                    frame.push_back(0x8A); 
                    frame.push_back(static_cast<uint8_t>(payload->size()));
                    frame.insert(frame.end(), payload->begin(), payload->end());
                    std::error_code err;
                    asio::write(*socket, asio::buffer(frame), err);
                } else if (opcode == 0x2) { 
                    livekit::proto::SignalRequest req;
                    if (req.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
                        std::lock_guard<std::mutex> lock(req_mutex);
                        received_requests.push_back(req);
                        std::cout << "MockServer: Got SignalRequest, has_ping_req=" << req.has_ping_req() << " mute=" << req.has_mute() << std::endl;
                        
                        if (req.has_ping_req()) {
                            livekit::proto::SignalResponse resp;
                            auto* pong = resp.mutable_pong_resp();
                            pong->set_last_ping_timestamp(req.ping_req().timestamp());
                            SendResponse(socket, resp);
                            std::cout << "MockServer: Sent PongResponse back" << std::endl;
                        }
                    } else {
                        std::cout << "MockServer: Failed to parse SignalRequest from binary payload of size " << payload->size() << std::endl;
                    }
                } else {
                    std::cout << "MockServer: Got non-binary WS frame, opcode=" << (int)opcode << std::endl;
                }
                
                ReadWsFrame(socket);
            });
        };

        if (masked) {
            asio::async_read(*socket, asio::buffer(*mask_key), [read_payload](std::error_code ec, size_t) {
                if (ec) return;
                read_payload();
            });
        } else {
            read_payload();
        }
    }

private:
    asio::io_context& io_ctx_;
    asio::ip::tcp::acceptor acceptor_;
    uint16_t port_ = 0;
    int validation_status_ = 200;
    int v1_status_ = 200;
    std::string region_json_;
    std::string mock_sid_ = "default_sid";
    std::vector<std::string> join_publish_codecs_;
    bool dummy_reconnect_ = false;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> keep_alive_sockets_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> active_sockets_;

public:
    void SetDummyReconnect(bool active) { dummy_reconnect_ = active; }
    void PushResponse(const livekit::proto::SignalResponse& resp) {
        if (!active_sockets_.empty()) {
            SendResponse(active_sockets_.back(), resp);
        }
    }
};

static std::vector<std::shared_ptr<MockServer>> g_keep_alive_servers;

// Test Cases (Coroutines)
asio::awaitable<void> TestConnectAndJoin() {
    std::cout << "Running TestConnectAndJoin..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_123");
    server->StartAccept();
    std::cout << "TestConnectAndJoin: MockServer started on port " << server->port() << std::endl;

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;

    std::cout << "TestConnectAndJoin: calling SignalClient::Connect" << std::endl;
    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    std::cout << "TestConnectAndJoin: SignalClient::Connect returned" << std::endl;
    
    TEST_ASSERT(!res.error, "Connect error: " + res.error.message());
    auto client = res.client;
    TEST_ASSERT(client != nullptr, "Client is null");
    auto join = client->join_response();
    TEST_ASSERT(join != nullptr, "JoinResponse is null");
    TEST_ASSERT(join->participant().sid() == "participant_123", "Incorrect participant SID: " + join->participant().sid());
    
    client->Close();
    server->Stop();
    std::cout << "TestConnectAndJoin PASSED!" << std::endl;
}

asio::awaitable<void> TestValidationFail() {
    std::cout << "Running TestValidationFail..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetValidationStatus(401);
    server->SetV1Status(401);
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = true;

    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    bool failed = (res.error != std::error_code{});
    TEST_ASSERT(failed, "Expected validation failure, but succeeded!");
    TEST_ASSERT(server->v1_requests.load() == 1, "v1 endpoint should be attempted once");
    TEST_ASSERT(server->v0_requests.load() == 0, "non-404 v1 failure must not downgrade to v0");
    server->Stop();
    std::cout << "TestValidationFail PASSED!" << std::endl;
}

asio::awaitable<void> TestV1FallbackOnlyOn404() {
    std::cout << "Running TestV1FallbackOnlyOn404..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetV1Status(404);
    server->SetMockJoinSids("participant_v0_fallback");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = true;

    auto res = co_await livekit::SignalClient::Connect(
        url, "test-token", opts, std::nullopt,
        [server](const livekit::SignalEvent&) {});
    TEST_ASSERT(!res.error, "404 compatibility fallback should connect through v0");
    TEST_ASSERT(res.client && !res.client->is_single_pc_mode_active(),
                "successful 404 fallback must record dual-PC/v0 mode");
    TEST_ASSERT(server->v1_requests.load() == 1, "v1 endpoint should be attempted once");
    TEST_ASSERT(server->v0_requests.load() == 1, "404 v1 response should downgrade exactly once");
    TEST_ASSERT(livekit::IsWebSocketHttpStatus(livekit::MakeWebSocketHttpError(404), 404),
                "HTTP handshake error must preserve status code");

    res.client->Close();
    server->Stop();
    std::cout << "TestV1FallbackOnlyOn404 PASSED!" << std::endl;
}

asio::awaitable<void> TestHeartbeat() {
    std::cout << "Running TestHeartbeat..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_hb");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;

    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    TEST_ASSERT(!res.error, "Connect error: " + res.error.message());
    auto client = res.client;
    TEST_ASSERT(client != nullptr, "Connection failed");
    
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::seconds(2));
    co_await timer.async_wait(asio::use_awaitable);
    
    {
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool has_ping = false;
        for (const auto& r : server->received_requests) {
            if (r.has_ping_req()) {
                has_ping = true;
                break;
            }
        }
        TEST_ASSERT(has_ping, "Heartbeat Ping was not received by the mock server");
    }
    
    client->Close();
    server->Stop();
    std::cout << "TestHeartbeat PASSED!" << std::endl;
}

asio::awaitable<void> TestReconnectionAndQueueing() {
    std::cout << "Running TestReconnectionAndQueueing..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_reconnect");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;

    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    TEST_ASSERT(!res.error, "Connect error: " + res.error.message());
    auto client = res.client;
    TEST_ASSERT(client != nullptr, "Connection failed");
    
    livekit::proto::SignalRequest req;
    auto* mute = req.mutable_mute();
    mute->set_sid("track_123");
    mute->set_muted(true);
    
    auto rec_res = co_await client->Restart();
    TEST_ASSERT(!rec_res.error, "Restart error: " + rec_res.error.message());
    auto rec = rec_res.reconnect_response;
    TEST_ASSERT(rec != nullptr, "ReconnectResponse is null");
    
    client->Send(req);
    
    {
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool has_mute = false;
        for (const auto& r : server->received_requests) {
            if (r.has_mute()) has_mute = true;
        }
        TEST_ASSERT(!has_mute, "Mute should be queued during reconnect, not sent!");
    }
    
    client->SetReconnected();
    
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);
    
    {
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool has_mute = false;
        for (const auto& r : server->received_requests) {
            if (r.has_mute() && r.mute().sid() == "track_123" && r.mute().muted()) {
                has_mute = true;
            }
        }
        TEST_ASSERT(has_mute, "Queued request failed to send after SetReconnected");
    }
    
    client->Close();
    server->Stop();
    std::cout << "TestReconnectionAndQueueing PASSED!" << std::endl;
}

asio::awaitable<void> TestReconnectionInterrupted() {
    std::cout << "Running TestReconnectionInterrupted..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_interrupted");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;

    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    TEST_ASSERT(!res.error, "Connect error: " + res.error.message());
    auto client = res.client;
    TEST_ASSERT(client != nullptr, "Connection failed");
    
    auto rec_res = co_await client->Restart();
    TEST_ASSERT(!rec_res.error, "First Restart error: " + rec_res.error.message());
    
    // 强制关闭服务器，模拟重连完成前二次断网
    server->Stop();
    client->Close();
    
    TEST_ASSERT(!client->is_connected(), "Client should be disconnected after Close");

    // 此时往里 Send 消息，应该被挂起，不能发送
    livekit::proto::SignalRequest req;
    auto* mute = req.mutable_mute();
    mute->set_sid("track_456");
    mute->set_muted(true);
    client->Send(req);

    // 重新启动相同端口的服务器
    auto server2 = std::make_shared<MockServer>(server_io, server->port());
    g_keep_alive_servers.push_back(server2);
    server2->SetMockJoinSids("participant_interrupted");
    server2->StartAccept();

    // 触发第二次重连 (Restart)
    auto rec_res2 = co_await client->Restart();
    TEST_ASSERT(!rec_res2.error, "Second Restart error: " + rec_res2.error.message());
    TEST_ASSERT(client->is_connected(), "Client should be connected after second restart");

    // 调用 SetReconnected() 触发 Flush
    client->SetReconnected();

    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    {
        std::lock_guard<std::mutex> lock(server2->req_mutex);
        bool has_mute = false;
        for (const auto& r : server2->received_requests) {
            if (r.has_mute() && r.mute().sid() == "track_456" && r.mute().muted()) {
                has_mute = true;
            }
        }
        TEST_ASSERT(has_mute, "Queued request failed to send to server2 after second SetReconnected");
    }

    client->Close();
    server2->Stop();
    std::cout << "TestReconnectionInterrupted PASSED!" << std::endl;
}

asio::awaitable<void> TestReconnectionTimeout() {
    std::cout << "Running TestReconnectionTimeout..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_timeout");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.reconnect_timeout = std::chrono::milliseconds(100); // 设为很短的 100ms 超时

    auto res = co_await livekit::SignalClient::Connect(url, "test-token", opts, std::nullopt, [server](const livekit::SignalEvent&) {});
    TEST_ASSERT(!res.error, "Connect error: " + res.error.message());
    auto client = res.client;
    TEST_ASSERT(client != nullptr, "Connection failed");

    // 启用 MockServer 的“哑巴”模式，接收重连时装作听不见，不回任何握手应答
    server->SetDummyReconnect(true);

    // 发起 Restart，应当由于 100ms 无应答超时而退出
    auto rec_res = co_await client->Restart();
    
    bool caught_timeout = (rec_res.error == std::errc::timed_out);
    std::cout << "TestReconnectionTimeout: Restart returned, error=" << rec_res.error.message() << ", value=" << rec_res.error.value() << std::endl;

    TEST_ASSERT(caught_timeout, "Expected restart timeout, but did not trigger! Got: " + rec_res.error.message());
    
    // 此时应当正确复位，is_connected 应为 false，且可以接受进一步的排队消息
    TEST_ASSERT(!client->is_connected(), "Client should be disconnected after timeout");

    client->Close();
    server->Stop();
    std::cout << "TestReconnectionTimeout PASSED!" << std::endl;
}

class TestRoomListener : public livekit::RoomListener {
public:
    bool connected_called = false;
    bool disconnected_called = false;
    std::shared_ptr<livekit::RemoteParticipant> connected_participant = nullptr;
    std::shared_ptr<livekit::RemoteParticipant> disconnected_participant = nullptr;
    std::shared_ptr<livekit::Participant> muted_participant = nullptr;
    std::shared_ptr<livekit::TrackPublication> muted_pub = nullptr;
    bool muted_val = false;

    void OnConnected() override {
        connected_called = true;
    }
    void OnDisconnected(const std::string&) override {
        disconnected_called = true;
    }
    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        connected_participant = participant;
    }
    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        disconnected_participant = participant;
    }
    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant, std::shared_ptr<livekit::TrackPublication> publication, bool muted) override {
        muted_participant = participant;
        muted_pub = publication;
        muted_val = muted;
    }
};

asio::awaitable<void> TestAsyncPublishContract() {
    std::cout << "Running TestAsyncPublishContract..." << std::endl;

    auto participant = std::make_shared<livekit::LocalParticipant>(
        "local_async", "publisher", [](const livekit::proto::SignalRequest&) {});
    bool handler_called = false;
    bool request_muted = false;
    participant->SetAsyncPublishTrackHandler(
        [&handler_called, &request_muted](std::shared_ptr<livekit::Track> track,
                                          const livekit::proto::SignalRequest& request)
            -> asio::awaitable<std::shared_ptr<livekit::TrackPublication>> {
            handler_called = true;
            request_muted = request.has_add_track() && request.add_track().muted();
            co_return std::make_shared<livekit::TrackPublication>(
                std::move(track), "TR_async_ack", "camera");
        });

    livekit::ParticipantPermission denied;
    denied.can_publish = false;
    participant->set_permission(denied);
    auto denied_track = std::make_shared<livekit::Track>(
        "denied", "denied", livekit::TrackKind::Video);
    bool permission_failed = false;
    try {
        (void)co_await participant->PublishTrackAsync(denied_track);
    } catch (const livekit::OperationError& error) {
        permission_failed = error.code() == livekit::OperationErrorCode::PermissionDenied;
    }
    TEST_ASSERT(permission_failed, "PublishTrackAsync did not expose PermissionDenied");
    TEST_ASSERT(!handler_called, "Publish handler ran after permission rejection");

    livekit::ParticipantPermission allowed;
    participant->set_permission(allowed);
    auto muted_track = std::make_shared<livekit::Track>(
        "muted", "camera", livekit::TrackKind::Video);
    muted_track->set_muted(true);
    auto publication = co_await participant->PublishTrackAsync(muted_track);
    TEST_ASSERT(handler_called, "Async publish handler was not awaited");
    TEST_ASSERT(request_muted, "AddTrackRequest did not preserve the initial muted state");
    TEST_ASSERT(publication && publication->sid() == "TR_async_ack",
                "PublishTrackAsync did not return the committed publication");

    std::cout << "TestAsyncPublishContract PASSED!" << std::endl;
}

asio::awaitable<void> TestRoomStateMachine() {
    std::cout << "Running TestRoomStateMachine..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());
    
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_room_test");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.timeouts.publish = std::chrono::milliseconds(100);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<TestRoomListener>();
    room->AddListener(listener);

    // 1. 测试连接与加入，校验 ConnectionState 和 LocalParticipant 初始化
    bool ok = co_await room->Connect(url, "test-token", opts);
    std::cout << "[STEP] Room Connect returned. ok=" << ok << std::endl;
    
    TEST_ASSERT(ok, "Room connection failed");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Incorrect room connection state");
    TEST_ASSERT(room->local_participant() != nullptr, "LocalParticipant is null");
    TEST_ASSERT(room->local_participant()->sid() == "participant_room_test", "Incorrect LocalParticipant SID");
    TEST_ASSERT(listener->connected_called, "OnConnected callback was not triggered");
    std::cout << "[STEP] Room connection assertions passed." << std::endl;

    // 2. 测试远端用户加入事件，检验增量更新状态机
    {
        std::cout << "[STEP] Pushing remote participant joined event..." << std::endl;
        livekit::proto::SignalResponse resp;
        auto* update = resp.mutable_update();
        auto* part_info = update->add_participants();
        part_info->set_sid("remote_bob_456");
        part_info->set_identity("bob");
        part_info->set_state(livekit::proto::ParticipantInfo::JOINED);

        server->PushResponse(resp);
    }
    
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(listener->connected_participant != nullptr, "OnParticipantConnected was not triggered");
    TEST_ASSERT(listener->connected_participant->sid() == "remote_bob_456", "Incorrect remote participant SID");
    TEST_ASSERT(listener->connected_participant->identity() == "bob", "Incorrect remote participant identity");
    TEST_ASSERT(room->remote_participants().size() == 1, "Incorrect remote participant size");
    std::cout << "[STEP] Remote participant joined event processed and verified." << std::endl;

    // 3. 未收到 TrackPublished ACK 时必须超时失败，且不能提交伪 publication
    auto track = std::make_shared<livekit::Track>("local_track_sid_temp", "camera", livekit::TrackKind::Video);
    track->set_muted(true);
    bool publish_timed_out = false;
    try {
        (void)co_await room->local_participant()->PublishTrackAsync(track);
    } catch (const livekit::OperationError& error) {
        publish_timed_out = error.code() == livekit::OperationErrorCode::TrackPublishTimeout;
    }
    TEST_ASSERT(publish_timed_out, "PublishTrackAsync reported success without TrackPublished ACK");
    TEST_ASSERT(room->local_participant()->tracks().empty(),
                "Failed publish left a provisional publication behind");

    {
        std::lock_guard<std::mutex> lock(server->req_mutex);
        bool has_add_track = false;
        bool muted_preserved = false;
        for (const auto& r : server->received_requests) {
            if (r.has_add_track() && r.add_track().name() == "camera") {
                has_add_track = true;
                muted_preserved = r.add_track().muted();
            }
        }
        TEST_ASSERT(has_add_track, "AddTrackRequest was not sent by LocalParticipant");
        TEST_ASSERT(muted_preserved, "AddTrackRequest lost the track's muted state");
    }
    std::cout << "[STEP] Track publish timeout and rollback verified." << std::endl;

    // 4. 测试服务端推送静音消息驱动状态机更新与事件派发
    auto pub = std::make_shared<livekit::TrackPublication>(track, "track_real_999", "camera");
    room->local_participant()->add_publication(pub);

    {
        livekit::proto::SignalResponse resp;
        auto* mute = resp.mutable_mute();
        mute->set_sid("track_real_999");
        mute->set_muted(true);

        server->PushResponse(resp);
    }

    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(listener->muted_pub != nullptr, "OnTrackMuted was not triggered");
    TEST_ASSERT(listener->muted_pub->sid() == "track_real_999", "Incorrect track SID in mute callback");
    TEST_ASSERT(listener->muted_val == true, "Incorrect mute value in mute callback");
    TEST_ASSERT(track->muted() == true, "Track muted state was not updated in state machine");
    std::cout << "[STEP] Track mute event processed and verified." << std::endl;

    // 5. 测试远端用户离开事件，检验状态机清理
    {
        livekit::proto::SignalResponse resp;
        auto* update = resp.mutable_update();
        auto* part_info = update->add_participants();
        part_info->set_sid("remote_bob_456");
        part_info->set_state(livekit::proto::ParticipantInfo::DISCONNECTED);

        server->PushResponse(resp);
    }

    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(listener->disconnected_participant != nullptr, "OnParticipantDisconnected was not triggered");
    TEST_ASSERT(listener->disconnected_participant->sid() == "remote_bob_456", "Incorrect remote participant SID on disconnect");
    TEST_ASSERT(room->remote_participants().empty(), "Remote participants map should be empty after disconnect");
    std::cout << "[STEP] Remote participant disconnect event verified." << std::endl;

    room->Disconnect();
    server->Stop();
    std::cout << "TestRoomStateMachine PASSED!" << std::endl;
}

asio::awaitable<void> TestWebRTCIntegration() {
    std::cout << "Running TestWebRTCIntegration..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    // 1. 验证 WebRTCManager 环境生命周期与多线程初始化
    bool init_ok = livekit::WebRTCManager::Instance().Initialize();
    TEST_ASSERT(init_ok, "WebRTCManager Initialize failed");
    
    auto* sig_thread = livekit::WebRTCManager::Instance().signaling_thread();
    auto* work_thread = livekit::WebRTCManager::Instance().worker_thread();
    auto* net_thread = livekit::WebRTCManager::Instance().network_thread();

    TEST_ASSERT(sig_thread != nullptr, "Signaling thread is null");
    TEST_ASSERT(work_thread != nullptr, "Worker thread is null");
    TEST_ASSERT(net_thread != nullptr, "Network thread is null");

    // 2. 验证跨线程安全同步调用 (BlockingCall)
    bool task_run = false;
    sig_thread->BlockingCall([&task_run]() {
        task_run = true;
    });
    TEST_ASSERT(task_run, "BlockingCall to WebRTC signaling thread failed to execute");

    auto factory = livekit::WebRTCManager::Instance().factory();
    TEST_ASSERT(factory != nullptr, "PeerConnectionFactory is null");

    // Explicitly release the factory reference count before destroying the underlying WebRTC threads
    factory = nullptr;

    livekit::WebRTCManager::Instance().Deinitialize();

    // 3. 验证 Room 状态机在 Connect 时分配 PeerConnection 实例
    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("webrtc_room_test");
    server->SetJoinPublishCodecs({"video/H264"});
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;

    auto room = livekit::Room::Create(executor);
    
    bool ok = co_await room->Connect(url, "test-token", opts);
    TEST_ASSERT(ok, "Room connection failed");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Room not connected");
    TEST_ASSERT(room->join_response() &&
                room->join_response()->participant().sid() == "webrtc_room_test",
                "Room did not retain the consumed JoinResponse");
    TEST_ASSERT(room->enabled_publish_codecs() == std::vector<std::string>{"video/h264"},
                "Room did not normalize the server publish codec policy");

    room->Disconnect();
    server->Stop();

    // Gracefully wait 50ms for background coroutines to wrap up their socket event teardown
    asio::steady_timer clean_timer(executor);
    clean_timer.expires_after(std::chrono::milliseconds(50));
    co_await clean_timer.async_wait(asio::use_awaitable);

    std::cout << "TestWebRTCIntegration PASSED!" << std::endl;
}

asio::awaitable<void> TestConnectWaitsForMediaReadiness() {
    std::cout << "Running TestConnectWaitsForMediaReadiness..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_media_timeout");
    server->StartAccept();

    livekit::SignalOptions opts;
    opts.single_peer_connection = true;
    opts.create_webrtc_pc = true;
    opts.timeouts.negotiation = std::chrono::milliseconds(150);
    opts.timeouts.peer_connection = std::chrono::milliseconds(150);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<TestRoomListener>();
    room->AddListener(listener);

    bool failed_at_media_boundary = false;
    try {
        const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
        co_await room->ConnectAsync(url, "test-token", opts);
    } catch (const livekit::OperationError& error) {
        failed_at_media_boundary =
            error.code() == livekit::OperationErrorCode::NegotiationFailed ||
            error.code() == livekit::OperationErrorCode::PeerConnectionTimeout;
    }

    TEST_ASSERT(failed_at_media_boundary,
                "ConnectAsync reported success without SDP/media readiness");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Disconnected,
                "Failed ConnectAsync did not roll the Room back to Disconnected");
    TEST_ASSERT(!listener->connected_called,
                "Failed ConnectAsync emitted OnConnected before media readiness");
    TEST_ASSERT(room->local_participant() == nullptr,
                "Failed ConnectAsync retained a partial local participant");

    server->CloseActiveConnections();
    server->Stop();
    std::cout << "TestConnectWaitsForMediaReadiness PASSED!" << std::endl;
}

asio::awaitable<void> TestEventReadyHandshake() {
    std::cout << "Running TestEventReadyHandshake..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_event_ready");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<TestRoomListener>();
    room->AddListener(listener);

    bool ok = co_await room->Connect(url, "test-token", opts);
    TEST_ASSERT(ok, "Room connection failed");

    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(50));
    co_await timer.async_wait(asio::use_awaitable);

    auto participants = room->remote_participants();
    TEST_ASSERT(participants.count("ready_alice") == 1,
                "Buffered participant delta was not committed after Connect");
    TEST_ASSERT(listener->connected_participant != nullptr,
                "Buffered participant delta was not dispatched");
    TEST_ASSERT(listener->connected_participant->sid() == "ready_alice",
                "Incorrect buffered participant SID");
    TEST_ASSERT(listener->connected_called, "OnConnected was not emitted before buffered deltas");

    room->Disconnect();
    server->Stop();
    std::cout << "TestEventReadyHandshake PASSED!" << std::endl;
}

class TestReconnectListener : public livekit::RoomListener {
public:
    bool reconnecting_called = false;
    bool reconnected_called = false;
    std::string republished_prev_sid;
    std::shared_ptr<livekit::TrackPublication> republished_pub;

    void OnReconnecting() override {
        reconnecting_called = true;
    }
    void OnReconnected() override {
        reconnected_called = true;
    }
    void OnLocalTrackRepublished(const std::string& previous_sid, std::shared_ptr<livekit::TrackPublication> publication) override {
        republished_prev_sid = previous_sid;
        republished_pub = publication;
    }
};

asio::awaitable<void> TestRoomReconnectAndTrackRecovery() {
    std::cout << "Running TestRoomReconnectAndTrackRecovery..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_reconnect_test");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<TestReconnectListener>();
    room->AddListener(listener);

    bool ok = co_await room->Connect(url, "test-token", opts);
    TEST_ASSERT(ok, "Room connection failed");

    auto track = std::make_shared<livekit::Track>("local_track_sid_temp", "camera", livekit::TrackKind::Video);
    auto pub = std::make_shared<livekit::TrackPublication>(track, "track_real_123", "camera");
    room->local_participant()->add_publication(pub);

    TEST_ASSERT(room->local_participant()->tracks().size() == 1, "Track not in local participant");

    std::cout << "TestRoomReconnectAndTrackRecovery: Stopping server to trigger close..." << std::endl;
    server->CloseActiveConnections();
    server->Stop();

    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Reconnecting, "Room not in Reconnecting state");
    TEST_ASSERT(listener->reconnecting_called, "OnReconnecting not called");

    auto server2 = std::make_shared<MockServer>(server_io, server->port());
    g_keep_alive_servers.push_back(server2);
    server2->SetMockJoinSids("participant_reconnect_test");
    server2->StartAccept();

    std::cout << "TestRoomReconnectAndTrackRecovery: Waiting for reconnect to succeed..." << std::endl;
    timer.expires_after(std::chrono::milliseconds(800));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected, "Room did not reconnect, state=" + std::to_string((int)room->connection_state()));
    TEST_ASSERT(listener->reconnected_called, "OnReconnected not called");
    TEST_ASSERT(listener->republished_prev_sid.empty(),
                "Resume reconnect incorrectly republished an existing local track");
    TEST_ASSERT(listener->republished_pub == nullptr,
                "Resume reconnect emitted a full-restart republish callback");
    TEST_ASSERT(room->local_participant()->tracks().size() == 1,
                "Resume reconnect did not preserve the local publication state");
    {
        std::lock_guard<std::mutex> lock(server2->req_mutex);
        const livekit::proto::SyncState* sync = nullptr;
        for (const auto& request : server2->received_requests) {
            if (request.has_sync_state()) sync = &request.sync_state();
        }
        TEST_ASSERT(sync != nullptr, "Resume reconnect did not send SyncState");
        TEST_ASSERT(sync->has_subscription() && !sync->subscription().subscribe(),
                    "SyncState did not encode the auto-subscribe baseline");
        TEST_ASSERT(sync->publish_tracks_size() == 1 &&
                    sync->publish_tracks(0).track().sid() == "track_real_123",
                    "SyncState did not include the existing local publication");
    }

    room->Disconnect();
    server2->CloseActiveConnections();
    server2->Stop();
    std::cout << "TestRoomReconnectAndTrackRecovery PASSED!" << std::endl;
}

asio::awaitable<void> TestRoomReconnectExhaustion() {
    std::cout << "Running TestRoomReconnectExhaustion..." << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto& server_io = static_cast<asio::io_context&>(executor.context());

    auto server = std::make_shared<MockServer>(server_io);
    g_keep_alive_servers.push_back(server);
    server->SetMockJoinSids("participant_exhaust_test");
    server->StartAccept();

    std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::milliseconds(500);
    opts.reconnect_timeout = std::chrono::milliseconds(500);
    opts.timeouts.reconnect_total = std::chrono::seconds(5);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<TestRoomListener>();
    room->AddListener(listener);

    bool ok = co_await room->Connect(url, "test-token", opts);
    TEST_ASSERT(ok, "Room connection failed");

    server->CloseActiveConnections();
    server->Stop();

    std::cout << "TestRoomReconnectExhaustion: Waiting for reconnect attempts to exhaust..." << std::endl;
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(7000));
    co_await timer.async_wait(asio::use_awaitable);

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Disconnected, "Room should be Disconnected after reconnect exhaustion");
    TEST_ASSERT(listener->disconnected_called, "OnDisconnected was not called after reconnect exhaustion");

    room->Disconnect();
    std::cout << "TestRoomReconnectExhaustion PASSED!" << std::endl;
}

int main() {
    std::cout << "=== LiveKit C++ Signaling tests ===" << std::endl;
    
    asio::io_context io_ctx;
    auto work_guard = asio::make_work_guard(io_ctx);
    
    asio::co_spawn(io_ctx, []() -> asio::awaitable<void> {
        try {
            co_await TestConnectAndJoin();
            co_await TestV1FallbackOnlyOn404();
            co_await TestValidationFail();
            co_await TestHeartbeat();
            co_await TestReconnectionAndQueueing();
            co_await TestReconnectionInterrupted();
            co_await TestReconnectionTimeout();
            co_await TestAsyncPublishContract();
            co_await TestRoomStateMachine();
            co_await TestWebRTCIntegration();
            co_await TestConnectWaitsForMediaReadiness();
            co_await TestEventReadyHandshake();
            co_await TestRoomReconnectAndTrackRecovery();
            co_await TestRoomReconnectExhaustion();
            std::cout << "All tests PASSED!" << std::endl;
            std::exit(0);
        } catch (const std::exception& e) {
            std::cerr << "Test failed with exception: " << e.what() << std::endl;
            std::exit(1);
        } catch (...) {
            std::cerr << "Test failed with unknown exception" << std::endl;
            std::exit(1);
        }
    }, asio::detached);
    
    io_ctx.run();
    return 0;
}
