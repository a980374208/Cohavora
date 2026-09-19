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
#include <condition_variable>
#include <asio.hpp>
#include <openssl/sha.h>
#include "signal_client.h"
#include "room.h"
#include "participant.h"
#include "track.h"
#include "webrtc_manager.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/media_stream_track.h"
#include "api/make_ref_counted.h"

namespace livekit {

class RoomConnectAttemptTestAccess final {
public:
    using BeforeJoinCommit = std::function<asio::awaitable<void>(uint64_t)>;
    using BeforeSignalEventCommit = std::function<void(uint64_t)>;
    using BeforeLifecycleListenerDelivery =
        std::function<void(uint64_t, ConnectionState)>;

    static void SetBeforeJoinCommit(Room& room, BeforeJoinCommit hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) {
            room.connect_attempt_test_hooks_ =
                std::make_shared<Room::ConnectAttemptTestHooks>();
        }
        room.connect_attempt_test_hooks_->before_join_commit = std::move(hook);
    }

    static void SetBeforeSignalEventCommit(Room& room, BeforeSignalEventCommit hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) {
            room.connect_attempt_test_hooks_ =
                std::make_shared<Room::ConnectAttemptTestHooks>();
        }
        room.connect_attempt_test_hooks_->before_signal_event_commit = std::move(hook);
    }

    static void SetBeforeLifecycleListenerDelivery(
        Room& room,
        BeforeLifecycleListenerDelivery hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) {
            room.connect_attempt_test_hooks_ =
                std::make_shared<Room::ConnectAttemptTestHooks>();
        }
        room.connect_attempt_test_hooks_->before_lifecycle_listener_delivery =
            std::move(hook);
    }

    static void SetReconnectDisabled(Room& room, bool disabled) {
        std::lock_guard lock(room.room_mutex_);
        room.reconnect_disabled_ = disabled;
    }

    static void Clear(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        room.connect_attempt_test_hooks_.reset();
    }

    static uint64_t InstalledGeneration(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.installed_session_generation_;
    }

    static void DispatchSignalEvent(
        Room& room,
        const SignalEvent& event,
        uint64_t generation) {
        room.HandleSignalEvent(event, generation);
    }

    static void SetBeforeNativeCommit(Room& room, std::function<void(uint64_t)> hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) room.connect_attempt_test_hooks_ = std::make_shared<Room::ConnectAttemptTestHooks>();
        room.connect_attempt_test_hooks_->before_native_event_commit = std::move(hook);
    }
    static std::shared_ptr<webrtc::PeerConnectionObserver> NativeObserver(Room& room, uint64_t generation) {
        return room.CreatePeerConnectionObserver(1, generation);
    }
    static std::shared_ptr<webrtc::DataChannelObserver> DataObserver(Room& room, uint64_t generation) {
        return room.CreateDataChannelObserver(true, generation);
    }
    static void ScanTrack(Room& room, webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, uint64_t generation) {
        room.PostRemoteTrack(receiver, receiver->track(), generation);
    }
    static size_t PendingTracks(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        size_t count = 0;
        for (const auto& [sid, tracks] : room.pending_track_queue_) count += tracks.size();
        return count;
    }
    static size_t RemoteChannels(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.remote_data_channels_.size();
    }
    static size_t StreamReaders(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.active_text_readers_.size() + room.active_byte_readers_.size();
    }
    static size_t VideoBindings(Room& room) {
        std::lock_guard lock(room.remote_media_mutex_);
        return room.remote_video_tracks_.size();
    }
    static std::shared_ptr<AwaitableState<void>> InstallPCWait(Room& room, uint64_t generation) {
        auto state = std::make_shared<AwaitableState<void>>(room.executor_);
        std::lock_guard lock(room.room_mutex_);
        room.pending_pc_waits_.push_back({generation, 1, state});
        return state;
    }
    static void PrepareRepublish(Room& room, std::shared_ptr<Track> track,
                                 std::function<void(uint64_t)> before_delivery) {
        std::lock_guard lock(room.room_mutex_);
        room.reconnect_active_ = true;
        room.reconnect_disabled_ = false;
        room.published_track_records_ = {{track, "TR_OLD"}};
        if (!room.connect_attempt_test_hooks_) room.connect_attempt_test_hooks_ = std::make_shared<Room::ConnectAttemptTestHooks>();
        room.connect_attempt_test_hooks_->before_republish_listener_delivery = std::move(before_delivery);
    }
    static asio::awaitable<void> Republish(Room& room, uint64_t generation) {
        co_await room.RepublishLocalTracks(generation);
    }
    static asio::awaitable<void> ObserveTransportShutdown(Room& room, std::function<void(bool)> observer) {
        std::shared_ptr<WebSocketClient> transport;
        {
            std::lock_guard lock(room.room_mutex_);
            auto signal = room.signal_client_;
            std::shared_lock stream_lock(signal->stream_mutex_);
            transport = signal->stream_->ws_client_;
        }
        co_await asio::co_spawn(transport->strand_,
            [transport, observer = std::move(observer)]() mutable -> asio::awaitable<void> {
                transport->before_shutdown_for_testing_ = std::move(observer);
                co_return;
            }, asio::use_awaitable);
    }
};

} // namespace livekit

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

// Only WebRTC leaf interfaces are faked. The real Room observer, posted scan,
// pending queue, media attachment, commit, and detach paths run unchanged.
class NativeTestVideoTrack : public webrtc::MediaStreamTrack<webrtc::VideoTrackInterface> {
public:
    explicit NativeTestVideoTrack(const std::string& id) : MediaStreamTrack(id) {}
    std::string kind() const override { return kVideoKind; }
    webrtc::VideoTrackSourceInterface* GetSource() const override { return nullptr; }
    bool set_enabled(bool enabled) override {
        if (before_enable) before_enable();
        return MediaStreamTrack::set_enabled(enabled);
    }
    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>*, const webrtc::VideoSinkWants&) override {
        ++adds;
        if (before_add) before_add();
    }
    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>*) override {
        ++removes;
        if (before_remove) before_remove();
    }
    std::atomic<int> adds{0}, removes{0};
    std::function<void()> before_add;
    std::function<void()> before_enable;
    std::function<void()> before_remove;
};

class NativeTestReceiver : public webrtc::RtpReceiverInterface {
public:
    NativeTestReceiver(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, std::string stream)
        : track_(std::move(track)), stream_(std::move(stream)) {}
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track() const override { return track_; }
    std::vector<std::string> stream_ids() const override { return {stream_}; }
    webrtc::MediaType media_type() const override { return webrtc::MediaType::VIDEO; }
    std::string id() const override { return track_->id(); }
    webrtc::RtpParameters GetParameters() const override { return {}; }
    void SetObserver(webrtc::RtpReceiverObserverInterface*) override {}
    void SetJitterBufferMinimumDelay(std::optional<double>) override {}
private:
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track_;
    std::string stream_;
};

class RoomCallbackWorker {
public:
    RoomCallbackWorker() : guard_(asio::make_work_guard(io)), worker_([this]() { io.run(); }) {}
    ~RoomCallbackWorker() {
        guard_.reset(); io.stop(); worker_.join();
    }
    void Drain() {
        std::promise<void> promise;
        auto future = promise.get_future();
        asio::post(io, [done = std::move(promise)]() mutable { done.set_value(); });
        TEST_ASSERT(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                    "Native callback executor did not drain (possible lock-held callback)");
    }
    asio::io_context io;
private:
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread worker_;
};

// Cross-executor Signal sends retain coroutine frames until the main executor
// drains. Keep each callback context alive until then, just like the mock server.
std::vector<std::shared_ptr<RoomCallbackWorker>> g_callback_workers;

void RequireCrossThreadRoomRead(const std::shared_ptr<livekit::Room>& room, bool disconnect = false) {
    std::promise<void> promise;
    auto future = promise.get_future();
    std::thread caller([room, disconnect, done = std::move(promise)]() mutable {
        (void)room->connection_state();
        if (disconnect) room->Disconnect();
        done.set_value();
    });
    TEST_ASSERT(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "External callback holds room_mutex_ while waiting for Room API on another thread");
    caller.join();
}

class ConnectAttemptGate final {
public:
    explicit ConnectAttemptGate(asio::any_io_executor executor)
        : arrived_(std::make_shared<livekit::AwaitableState<void>>(executor)),
          release_(std::make_shared<livekit::AwaitableState<void>>(executor)) {}

    asio::awaitable<void> PauseFirst(uint64_t) {
        if (claimed_.exchange(true, std::memory_order_acq_rel)) co_return;
        livekit::CompleteAwaitable(arrived_);
        co_await livekit::WaitAwaitable<void>(
            release_, std::chrono::seconds(5), livekit::OperationKind::Connect,
            livekit::OperationErrorCode::Cancelled, "sig001_release_attempt_a");
    }

    asio::awaitable<void> WaitUntilPaused() {
        co_await livekit::WaitAwaitable<void>(
            arrived_, std::chrono::seconds(5), livekit::OperationKind::Connect,
            livekit::OperationErrorCode::JoinTimeout, "sig001_wait_attempt_a");
    }

    void Release() { livekit::CompleteAwaitable(release_); }

private:
    std::atomic<bool> claimed_{false};
    std::shared_ptr<livekit::AwaitableState<void>> arrived_;
    std::shared_ptr<livekit::AwaitableState<void>> release_;
};

class SignalEventCommitGate final {
public:
    SignalEventCommitGate(asio::any_io_executor executor, int expected)
        : executor_(std::move(executor)),
          expected_(expected),
          arrived_state_(
              std::make_shared<livekit::AwaitableState<void>>(executor_)) {}

    void ArriveAndWait(uint64_t) {
        const int arrived = arrived_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (arrived == expected_) {
            asio::post(executor_, [state = arrived_state_]() {
                livekit::CompleteAwaitable(state);
            });
        }

        std::unique_lock lock(mutex_);
        released_cv_.wait(lock, [this]() { return released_; });
    }

    asio::awaitable<void> WaitUntilAllArrived() {
        co_await livekit::WaitAwaitable<void>(
            arrived_state_, std::chrono::seconds(5),
            livekit::OperationKind::Connect,
            livekit::OperationErrorCode::JoinTimeout,
            "sig001_wait_stale_events");
    }

    void Release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        released_cv_.notify_all();
    }

private:
    asio::any_io_executor executor_;
    int expected_ = 0;
    std::atomic<int> arrived_{0};
    std::shared_ptr<livekit::AwaitableState<void>> arrived_state_;
    std::mutex mutex_;
    std::condition_variable released_cv_;
    bool released_ = false;
};

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

    int PeerClosedCount() const {
        return peer_closed_count_.load(std::memory_order_acquire);
    }

    asio::awaitable<void> WaitForPeerCloseAfter(int baseline) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        asio::steady_timer timer(io_ctx_);
        while (peer_closed_count_.load(std::memory_order_acquire) <= baseline) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw livekit::OperationError(
                    livekit::OperationKind::Connect,
                    livekit::OperationErrorCode::SessionClosed,
                    "sig001_wait_peer_close",
                    "old transport did not close on its owner executor");
            }
            timer.expires_after(std::chrono::milliseconds(2));
            std::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
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
                } else {
                    self->NotePeerClosed();
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
                } else {
                    self->NotePeerClosed();
                }
            });
    }

    void NotePeerClosed() {
        peer_closed_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    asio::io_context& io_ctx_;
    asio::ip::tcp::acceptor acceptor_;
    uint16_t port_ = 0;
    std::mutex socket_mutex_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> active_sockets_;
    std::atomic<int> peer_closed_count_{0};
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

class ReentrantLifecycleListener final : public livekit::RoomListener {
public:
    enum class Mode {
        ObserveOnly,
        ReenterOnReconnecting,
        ReenterOnNetworkDisconnected
    };

    explicit ReentrantLifecycleListener(std::weak_ptr<livekit::Room> room)
        : room_(std::move(room)) {}

    void SetMode(Mode mode) {
        mode_.store(static_cast<int>(mode), std::memory_order_release);
    }

    void OnReconnecting() override {
        reconnecting_count.fetch_add(1, std::memory_order_acq_rel);
        if (CurrentMode() != Mode::ReenterOnReconnecting) return;
        if (auto room = room_.lock()) {
            reconnecting_observed_state.store(
                static_cast<int>(room->connection_state()),
                std::memory_order_release);
            RequireCrossThreadRoomRead(room, true);
        }
    }

    void OnDisconnected(
        livekit::RoomDisconnectReason reason,
        const std::string&) override {
        if (reason != livekit::RoomDisconnectReason::NetworkError) return;
        network_disconnected_count.fetch_add(1, std::memory_order_acq_rel);
        if (CurrentMode() != Mode::ReenterOnNetworkDisconnected) return;
        if (auto room = room_.lock()) {
            disconnected_observed_state.store(
                static_cast<int>(room->connection_state()),
                std::memory_order_release);
            RequireCrossThreadRoomRead(room, true);
        }
    }

    std::atomic<int> reconnecting_count{0};
    std::atomic<int> network_disconnected_count{0};
    std::atomic<int> reconnecting_observed_state{-1};
    std::atomic<int> disconnected_observed_state{-1};

private:
    Mode CurrentMode() const {
        return static_cast<Mode>(mode_.load(std::memory_order_acquire));
    }

    std::weak_ptr<livekit::Room> room_;
    std::atomic<int> mode_{static_cast<int>(Mode::ObserveOnly)};
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
    opts.allow_insecure_transport = true;
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
    opts.allow_insecure_transport = true;
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
    opts.allow_insecure_transport = true;
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
    opts.allow_insecure_transport = true;
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
// Case 5: SIG-001 stale attempt rollback must not clean the replacement session
// ============================================================================
asio::awaitable<void> TestCase5_StaleConnectRollbackOwnership(asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 5] Starting stale Connect rollback ownership test..." << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);

    auto room = livekit::Room::Create(executor);
    auto gate = std::make_shared<ConnectAttemptGate>(executor);
    livekit::RoomConnectAttemptTestAccess::SetBeforeJoinCommit(
        *room, [gate](uint64_t generation) -> asio::awaitable<void> {
            co_await gate->PauseFirst(generation);
        });

    auto attempt_a_done = std::make_shared<livekit::AwaitableState<bool>>(executor);
    livekit::safe_co_spawn(executor,
        [room, url, opts, attempt_a_done]() -> asio::awaitable<void> {
            const bool connected = co_await room->Connect(url, "token-sig001-a", opts);
            livekit::CompleteAwaitable(attempt_a_done, connected);
        });

    co_await gate->WaitUntilPaused();
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connecting,
                "Attempt A did not reach the pre-commit production hook");

    room->Disconnect();
    const bool replacement_connected =
        co_await room->Connect(url, "token-sig001-b", opts);
    TEST_ASSERT(replacement_connected, "Replacement attempt B failed to connect");

    auto replacement_local = room->local_participant();
    const uint64_t replacement_generation =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);
    TEST_ASSERT(replacement_local != nullptr, "Replacement local participant is missing");
    TEST_ASSERT(replacement_generation != 0, "Replacement resource owner was not installed");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "Replacement room was not connected before releasing A");

    const int peer_closes_before_stale_cleanup = server->PeerClosedCount();
    gate->Release();
    const bool attempt_a_connected = co_await livekit::WaitAwaitable<bool>(
        attempt_a_done, std::chrono::seconds(5), livekit::OperationKind::Connect,
        livekit::OperationErrorCode::JoinTimeout, "sig001_wait_attempt_a_completion");
    TEST_ASSERT(!attempt_a_connected, "Stale attempt A unexpectedly committed");
    co_await server->WaitForPeerCloseAfter(peer_closes_before_stale_cleanup);

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "Stale A rollback changed B's connected state");
    TEST_ASSERT(room->local_participant() == replacement_local,
                "Stale A rollback replaced or cleared B's local participant");
    TEST_ASSERT(livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room) ==
                    replacement_generation,
                "Stale A rollback changed B's resource owner generation");

    livekit::RoomConnectAttemptTestAccess::Clear(*room);
    room->Disconnect();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 5: Stale Connect rollback preserved replacement ownership." << std::endl;
}

// ============================================================================
// Case 6: Events admitted by A must revalidate at B's mutation boundary
// ============================================================================
asio::awaitable<void> TestCase6_StaleSignalEventsCannotMutateReplacement(
    asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 6] Starting stale signal event commit-boundary test..."
              << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<StressRoomListener>();
    room->AddListener(listener);
    TEST_ASSERT(co_await room->Connect(url, "token-sig001-event-a", opts),
                "Attempt A failed to connect for stale-event test");
    const uint64_t generation_a =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);

    auto gate = std::make_shared<SignalEventCommitGate>(executor, 2);
    livekit::RoomConnectAttemptTestAccess::SetBeforeSignalEventCommit(
        *room, [gate](uint64_t generation) {
            gate->ArriveAndWait(generation);
        });

    livekit::SignalEvent stale_close;
    stale_close.type = livekit::SignalEvent::Close;
    stale_close.close_reason = "stale transport close";

    livekit::SignalEvent stale_message;
    stale_message.type = livekit::SignalEvent::Message;
    stale_message.message = std::make_shared<livekit::proto::SignalResponse>();
    auto* stale_participant =
        stale_message.message->mutable_update()->add_participants();
    stale_participant->set_sid("PA_STALE");
    stale_participant->set_identity("stale-peer");
    stale_participant->set_state(livekit::proto::ParticipantInfo::ACTIVE);

    std::thread close_thread([room, stale_close, generation_a]() {
        livekit::RoomConnectAttemptTestAccess::DispatchSignalEvent(
            *room, stale_close, generation_a);
    });
    std::thread message_thread([room, stale_message, generation_a]() {
        livekit::RoomConnectAttemptTestAccess::DispatchSignalEvent(
            *room, stale_message, generation_a);
    });

    co_await gate->WaitUntilAllArrived();
    room->Disconnect();
    TEST_ASSERT(co_await room->Connect(url, "token-sig001-event-b", opts),
                "Replacement B failed to connect for stale-event test");

    const auto replacement_local = room->local_participant();
    const uint64_t replacement_generation =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);
    const int disconnected_before_release = listener->disconnected_count.load();
    const int reconnecting_before_release = listener->reconnecting_count.load();

    gate->Release();
    close_thread.join();
    message_thread.join();

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "A stale close changed B's Room state");
    TEST_ASSERT(room->local_participant() == replacement_local,
                "A stale event changed B's local participant");
    TEST_ASSERT(livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room) ==
                    replacement_generation,
                "A stale event changed B's installed generation");
    TEST_ASSERT(room->remote_participants().count("PA_STALE") == 0,
                "A stale participant update committed into B");
    TEST_ASSERT(listener->disconnected_count.load() == disconnected_before_release,
                "A stale close emitted a replacement disconnect");
    TEST_ASSERT(listener->reconnecting_count.load() == reconnecting_before_release,
                "A stale close started reconnect for B");

    livekit::RoomConnectAttemptTestAccess::Clear(*room);
    room->Disconnect();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 6: Stale signal events were rejected at commit."
              << std::endl;
}

// ============================================================================
// Case 7: Cross-thread cleanup is posted to the old transport owner
// ============================================================================
asio::awaitable<void> TestCase7_CrossThreadCleanupUsesTransportOwner(
    asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 7] Starting cross-thread transport cleanup test..."
              << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);

    auto room = livekit::Room::Create(executor);
    TEST_ASSERT(co_await room->Connect(url, "token-sig001-owner-a", opts),
                "Attempt A failed to connect for cross-thread cleanup test");
    const int peer_closes_before_disconnect = server->PeerClosedCount();

    struct ShutdownObservation {
        std::atomic<int> calls{0};
        std::atomic<int> foreign_calls{0};
    };
    auto shutdown = std::make_shared<ShutdownObservation>();
    co_await livekit::RoomConnectAttemptTestAccess::ObserveTransportShutdown(*room,
        [shutdown](bool on_owner) {
            shutdown->calls.fetch_add(1);
            if (!on_owner) shutdown->foreign_calls.fetch_add(1);
        });

    std::promise<void> disconnect_done;
    auto disconnect_future = disconnect_done.get_future();
    std::thread cleanup_thread([room, done = std::move(disconnect_done)]() mutable {
        room->Disconnect();
        room->Disconnect();
        done.set_value();
    });

    const auto cleanup_status = disconnect_future.wait_for(std::chrono::seconds(2));
    TEST_ASSERT(cleanup_status == std::future_status::ready,
                "Cross-thread Disconnect did not finish without owner executor progress");
    cleanup_thread.join();
    // Observe shutdown itself, not EOF on the blocked peer io_context. A direct
    // caller-thread Abort is detected synchronously before owner work resumes.
    TEST_ASSERT(shutdown->foreign_calls == 0 && shutdown->calls == 0,
                "Transport shutdown executed before owner release (caller-thread shutdown)");

    TEST_ASSERT(co_await room->Connect(url, "token-sig001-owner-b", opts),
                "Replacement B failed during old transport cleanup");
    co_await server->WaitForPeerCloseAfter(peer_closes_before_disconnect);

    TEST_ASSERT(shutdown->calls > 0 && shutdown->foreign_calls == 0,
                "Transport shutdown did not execute on its owning strand");
    TEST_ASSERT(server->PeerClosedCount() == peer_closes_before_disconnect + 1,
                "Repeated Disconnect did not close exactly one old transport");

    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "Old cross-thread cleanup changed B's Room state");
    TEST_ASSERT(room->local_participant() != nullptr,
                "Old cross-thread cleanup cleared B's local participant");
    TEST_ASSERT(livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room) != 0,
                "Replacement B has no installed owner generation");

    const int peer_closes_before_final_cleanup = server->PeerClosedCount();
    room->Disconnect();
    room->Disconnect();
    co_await server->WaitForPeerCloseAfter(peer_closes_before_final_cleanup);
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 7: Cross-thread cleanup stayed on the transport owner."
              << std::endl;
}

// ============================================================================
// Case 8: Lifecycle delivery is stale-safe and never calls listeners under lock
// ============================================================================
asio::awaitable<void> TestCase8_LifecycleListenerDeliveryIsReentrant(
    asio::any_io_executor executor) {
    std::cout << "[STRESS TEST 8] Starting lifecycle listener delivery test..."
              << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io_ctx);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();

    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);

    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<ReentrantLifecycleListener>(room);
    room->AddListener(listener);
    TEST_ASSERT(co_await room->Connect(url, "token-sig001-delivery-a", opts),
                "Attempt A failed to connect for lifecycle delivery test");
    const uint64_t generation_a =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);

    auto delivery_gate = std::make_shared<SignalEventCommitGate>(executor, 1);
    livekit::RoomConnectAttemptTestAccess::SetBeforeLifecycleListenerDelivery(
        *room,
        [delivery_gate, generation_a](
            uint64_t generation,
            livekit::ConnectionState required_state) {
            if (generation == generation_a &&
                required_state == livekit::ConnectionState::Reconnecting) {
                delivery_gate->ArriveAndWait(generation);
            }
        });

    livekit::SignalEvent stale_close;
    stale_close.type = livekit::SignalEvent::Close;
    stale_close.close_reason = "stale lifecycle close";
    std::promise<void> stale_delivery_done;
    auto stale_delivery_future = stale_delivery_done.get_future();
    std::thread stale_delivery_thread(
        [room, stale_close, generation_a,
         done = std::move(stale_delivery_done)]() mutable {
            livekit::RoomConnectAttemptTestAccess::DispatchSignalEvent(
                *room, stale_close, generation_a);
            done.set_value();
        });

    co_await delivery_gate->WaitUntilAllArrived();
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Reconnecting,
                "A Close did not commit before lifecycle delivery pause");
    room->Disconnect();
    TEST_ASSERT(co_await room->Connect(url, "token-sig001-delivery-b", opts),
                "Replacement B failed during paused lifecycle delivery");
    delivery_gate->Release();
    TEST_ASSERT(stale_delivery_future.wait_for(std::chrono::seconds(2)) ==
                    std::future_status::ready,
                "Stale lifecycle delivery did not finish after replacement");
    stale_delivery_thread.join();
    TEST_ASSERT(listener->reconnecting_count.load(std::memory_order_acquire) == 0,
                "Invalidated A reconnecting notification reached replacement B");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "Invalidated lifecycle delivery changed B state");
    livekit::RoomConnectAttemptTestAccess::Clear(*room);

    listener->SetMode(ReentrantLifecycleListener::Mode::ReenterOnReconnecting);
    const uint64_t generation_b =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);
    livekit::SignalEvent reconnecting_close;
    reconnecting_close.type = livekit::SignalEvent::Close;
    reconnecting_close.close_reason = "reentrant reconnecting close";
    std::promise<void> reconnecting_done;
    auto reconnecting_future = reconnecting_done.get_future();
    std::thread reconnecting_thread(
        [room, reconnecting_close, generation_b,
         done = std::move(reconnecting_done)]() mutable {
            livekit::RoomConnectAttemptTestAccess::DispatchSignalEvent(
                *room, reconnecting_close, generation_b);
            done.set_value();
        });
    TEST_ASSERT(reconnecting_future.wait_for(std::chrono::seconds(2)) ==
                    std::future_status::ready,
                "OnReconnecting deadlocked while reentering Room APIs");
    reconnecting_thread.join();
    TEST_ASSERT(listener->reconnecting_count.load(std::memory_order_acquire) == 1,
                "Reentrant OnReconnecting was not delivered exactly once");
    TEST_ASSERT(listener->reconnecting_observed_state.load(std::memory_order_acquire) ==
                    static_cast<int>(livekit::ConnectionState::Reconnecting),
                "OnReconnecting could not synchronously read Room state");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Disconnected,
                "OnReconnecting reentrant Disconnect did not complete");

    TEST_ASSERT(co_await room->Connect(url, "token-sig001-delivery-c", opts),
                "Attempt C failed before OnDisconnected reentrancy test");
    listener->SetMode(
        ReentrantLifecycleListener::Mode::ReenterOnNetworkDisconnected);
    livekit::RoomConnectAttemptTestAccess::SetReconnectDisabled(*room, true);
    const uint64_t generation_c =
        livekit::RoomConnectAttemptTestAccess::InstalledGeneration(*room);
    livekit::SignalEvent disconnected_close;
    disconnected_close.type = livekit::SignalEvent::Close;
    disconnected_close.close_reason = "reentrant disconnected close";
    std::promise<void> disconnected_done;
    auto disconnected_future = disconnected_done.get_future();
    std::thread disconnected_thread(
        [room, disconnected_close, generation_c,
         done = std::move(disconnected_done)]() mutable {
            livekit::RoomConnectAttemptTestAccess::DispatchSignalEvent(
                *room, disconnected_close, generation_c);
            done.set_value();
        });
    TEST_ASSERT(disconnected_future.wait_for(std::chrono::seconds(2)) ==
                    std::future_status::ready,
                "OnDisconnected deadlocked while reentering Room APIs");
    disconnected_thread.join();
    TEST_ASSERT(listener->network_disconnected_count.load(std::memory_order_acquire) == 1,
                "Reentrant network OnDisconnected was not delivered exactly once");
    TEST_ASSERT(listener->disconnected_observed_state.load(std::memory_order_acquire) ==
                    static_cast<int>(livekit::ConnectionState::Disconnected),
                "OnDisconnected could not synchronously read Room state");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Disconnected,
                "OnDisconnected reentrant cleanup did not leave Room disconnected");

    room->RemoveListener(listener);
    room->Disconnect();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 8: Lifecycle delivery was stale-safe and reentrant."
              << std::endl;
}

// ============================================================================
// Cases 9/10 exercise the native and full-restart republish production paths.
// ============================================================================
asio::awaitable<void> TestCase9_NativeGenerationCommit(asio::any_io_executor executor) {
    using Access = livekit::RoomConnectAttemptTestAccess;
    std::cout << "[STRESS TEST 9] Native generation and attachment ownership..." << std::endl;
    auto& io = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();
    auto callback_owner = std::make_shared<RoomCallbackWorker>();
    g_callback_workers.push_back(callback_owner);
    auto& callbacks = *callback_owner;
    auto room = livekit::Room::Create(callbacks.io.get_executor());
    const auto url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);
    TEST_ASSERT(co_await room->Connect(url, "native-a", opts), "Native A Connect failed");
    const auto generation_a = Access::InstalledGeneration(*room);
    auto observer_a = Access::NativeObserver(*room, generation_a);
    auto data_observer_a = Access::DataObserver(*room, generation_a);

    auto gate = std::make_shared<SignalEventCommitGate>(executor, 1);
    Access::SetBeforeNativeCommit(*room, [gate, generation_a](uint64_t generation) {
        if (generation == generation_a) gate->ArriveAndWait(generation);
    });
    auto stale_track = webrtc::make_ref_counted<NativeTestVideoTrack>("rtc-stale");
    auto stale_receiver = webrtc::make_ref_counted<NativeTestReceiver>(stale_track, "PA_NATIVE|TR_NATIVE");
    observer_a->OnAddTrack(stale_receiver, {});
    co_await gate->WaitUntilAllArrived();

    livekit::proto::DataPacket stream_header;
    stream_header.mutable_stream_header()->set_stream_id("stale-stream");
    stream_header.mutable_stream_header()->mutable_text_header();
    auto encoded = stream_header.SerializeAsString();
    data_observer_a->OnMessage(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(encoded.data(), encoded.size()), true));
    data_observer_a->OnBufferedAmountChange(3);
    observer_a->OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState::kConnected);

    room->Disconnect();
    TEST_ASSERT(co_await room->Connect(url, "native-b", opts), "Native B Connect failed");
    const auto generation_b = Access::InstalledGeneration(*room);
    auto local_b = room->local_participant();
    auto waiter_b = Access::InstallPCWait(*room, generation_b);
    gate->Release();
    callbacks.Drain();
    TEST_ASSERT(Access::PendingTracks(*room) == 0, "A native track entered B pending queue");
    TEST_ASSERT(room->remote_participants().empty() && stale_track->adds == 0, "A native track mutated B media");
    TEST_ASSERT(Access::RemoteChannels(*room) == 0 && Access::StreamReaders(*room) == 0,
                "A DC event mutated B channel/stream state");
    {
        std::lock_guard lock(waiter_b->mutex);
        TEST_ASSERT(!waiter_b->completed, "A PC state completed B startup waiter");
    }
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected &&
                room->local_participant() == local_b && Access::InstalledGeneration(*room) == generation_b,
                "A native callback changed B Room/session state");
    Access::Clear(*room);

    // The exact posted helper used by both SDP scans must run without an outer
    // Room lock. Positive control reaches the pending queue then real attachment.
    auto current_track = webrtc::make_ref_counted<NativeTestVideoTrack>("rtc-current");
    auto receiver_b = webrtc::make_ref_counted<NativeTestReceiver>(current_track, "PA_NATIVE|TR_NATIVE");
    auto observer_b = Access::NativeObserver(*room, generation_b);
    observer_b->OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState::kConnected);
    auto data_observer_b = Access::DataObserver(*room, generation_b);
    data_observer_b->OnMessage(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(encoded.data(), encoded.size()), true));
    callbacks.Drain();
    {
        std::lock_guard lock(waiter_b->mutex);
        TEST_ASSERT(waiter_b->completed && !waiter_b->error, "Current PC state failed to complete B waiter");
    }
    TEST_ASSERT(Access::StreamReaders(*room) == 1, "Current DC header failed to install B reader");
    Access::SetBeforeNativeCommit(*room, [room](uint64_t) { RequireCrossThreadRoomRead(room); });
    Access::ScanTrack(*room, receiver_b, generation_b);
    callbacks.Drain();
    Access::Clear(*room);
    TEST_ASSERT(Access::PendingTracks(*room) == 1, "Current SDP scan did not queue track");
    current_track->before_add = [room]() { RequireCrossThreadRoomRead(room); };
    current_track->before_enable = [room]() { RequireCrossThreadRoomRead(room); };
    livekit::proto::ParticipantUpdate update;
    auto* participant = update.add_participants();
    participant->set_sid("PA_NATIVE");
    participant->set_identity("native");
    participant->set_state(livekit::proto::ParticipantInfo::ACTIVE);
    room->UpdateParticipantsForTesting(update);
    TEST_ASSERT(current_track->adds == 1 && Access::PendingTracks(*room) == 0,
                "Pending track did not attach through production flush");
    auto publication_b = room->remote_participants().at("PA_NATIVE")->get_publication("TR_NATIVE");
    TEST_ASSERT(publication_b && publication_b->track()->rtc_track() == current_track,
                "Current track binding was not committed");

    // Replacement after native AddSink but before retain_binding must detach
    // the attempt-local sink even though its generation has become stale.
    auto attach_gate = std::make_shared<SignalEventCommitGate>(executor, 1);
    auto uncommitted_track = webrtc::make_ref_counted<NativeTestVideoTrack>("rtc-uncommitted");
    uncommitted_track->before_add = [attach_gate]() { attach_gate->ArriveAndWait(0); };
    auto receiver_pending = webrtc::make_ref_counted<NativeTestReceiver>(uncommitted_track, "PA_NATIVE|TR_NATIVE");
    Access::ScanTrack(*room, receiver_pending, generation_b);
    co_await attach_gate->WaitUntilAllArrived();
    room->Disconnect();
    TEST_ASSERT(co_await room->Connect(url, "native-c", opts), "Native C Connect failed");
    room->UpdateParticipantsForTesting(update);
    attach_gate->Release();
    callbacks.Drain();
    TEST_ASSERT(uncommitted_track->adds == 1 && uncommitted_track->removes == 1,
                "Invalidated local media sink leaked or detached twice");
    TEST_ASSERT(current_track->removes == 1, "Replaced committed media sink did not detach once");
    TEST_ASSERT(room->remote_participants().at("PA_NATIVE")->tracks().empty(),
                "Late retain_binding modified C canonical participant");
    TEST_ASSERT(Access::StreamReaders(*room) == 0, "Disconnect did not retire old stream readers");

    // A slow old native detach must not clear the replacement media index.
    auto track_c = webrtc::make_ref_counted<NativeTestVideoTrack>("rtc-c");
    auto receiver_c = webrtc::make_ref_counted<NativeTestReceiver>(track_c, "PA_NATIVE|TR_NATIVE");
    Access::ScanTrack(*room, receiver_c, Access::InstalledGeneration(*room));
    callbacks.Drain();
    auto detach_gate = std::make_shared<SignalEventCommitGate>(executor, 1);
    track_c->before_remove = [detach_gate]() { detach_gate->ArriveAndWait(0); };
    std::promise<void> cleanup_done;
    auto cleanup_future = cleanup_done.get_future();
    std::thread cleanup_thread([room, done = std::move(cleanup_done)]() mutable {
        room->Disconnect();
        done.set_value();
    });
    co_await detach_gate->WaitUntilAllArrived();
    TEST_ASSERT(co_await room->Connect(url, "native-d", opts), "Native D Connect failed");
    room->UpdateParticipantsForTesting(update);
    auto track_d = webrtc::make_ref_counted<NativeTestVideoTrack>("rtc-d");
    auto receiver_d = webrtc::make_ref_counted<NativeTestReceiver>(track_d, "PA_NATIVE|TR_NATIVE");
    Access::ScanTrack(*room, receiver_d, Access::InstalledGeneration(*room));
    callbacks.Drain();
    TEST_ASSERT(Access::VideoBindings(*room) == 1, "Replacement D media binding missing");
    detach_gate->Release();
    TEST_ASSERT(cleanup_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "Old native cleanup did not complete");
    cleanup_thread.join();
    TEST_ASSERT(Access::VideoBindings(*room) == 1 && track_d->removes == 0,
                "Old Disconnect cleared D media index or native sink");
    TEST_ASSERT(track_c->removes == 1, "Old native cleanup did not detach exactly once");
    room->Disconnect();
    callbacks.Drain();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 9: Native callbacks and local resources preserved ownership." << std::endl;
}

class RepublishReentryListener final : public livekit::RoomListener {
public:
    explicit RepublishReentryListener(std::shared_ptr<livekit::Room> room) : room_(room) {}
    void OnLocalTrackRepublished(const std::string&, std::shared_ptr<livekit::TrackPublication>) override {
        ++calls;
        if (reenter) RequireCrossThreadRoomRead(room_.lock(), true);
    }
    std::atomic<int> calls{0};
    bool reenter = false;
private:
    std::weak_ptr<livekit::Room> room_;
};

asio::awaitable<void> TestCase10_RepublishDelivery(asio::any_io_executor executor) {
    using Access = livekit::RoomConnectAttemptTestAccess;
    std::cout << "[STRESS TEST 10] Republish replacement and cross-thread reentry..." << std::endl;
    auto& io = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();
    auto callback_owner = std::make_shared<RoomCallbackWorker>();
    g_callback_workers.push_back(callback_owner);
    auto& callbacks = *callback_owner;
    auto room = livekit::Room::Create(callbacks.io.get_executor());
    const auto url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);
    auto listener = std::make_shared<RepublishReentryListener>(room);
    auto trailing_listener = std::make_shared<RepublishReentryListener>(room);
    room->AddListener(listener);
    room->AddListener(trailing_listener);
    auto track = std::make_shared<livekit::Track>("TR_OLD", "republish", livekit::TrackKind::Video);
    const auto prepare_publisher = [&]() {
        auto local = room->local_participant();
        auto permission = local->permission();
        permission.can_publish = true;
        local->set_permission(permission);
        // Replace only the transport-facing publish operation. The real restart
        // republish coroutine, post-await validation and listener delivery run.
        local->SetAsyncPublishTrackHandler([](std::shared_ptr<livekit::Track> value,
            const livekit::proto::SignalRequest&) -> asio::awaitable<std::shared_ptr<livekit::TrackPublication>> {
            co_return std::make_shared<livekit::TrackPublication>(value, "TR_NEW", "republish");
        });
    };
    TEST_ASSERT(co_await room->Connect(url, "republish-a", opts), "Republish A Connect failed");
    prepare_publisher();
    auto generation_a = Access::InstalledGeneration(*room);
    auto gate = std::make_shared<SignalEventCommitGate>(executor, 1);
    Access::PrepareRepublish(*room, track, [gate](uint64_t generation) { gate->ArriveAndWait(generation); });
    auto stale_done = asio::co_spawn(callbacks.io, Access::Republish(*room, generation_a), asio::use_future);
    co_await gate->WaitUntilAllArrived();
    room->Disconnect();
    TEST_ASSERT(co_await room->Connect(url, "republish-b", opts), "Republish B Connect failed");
    gate->Release();
    TEST_ASSERT(stale_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "Stale republish failed to complete");
    stale_done.get();
    TEST_ASSERT(listener->calls == 0 && trailing_listener->calls == 0,
                "Replacement failed to invalidate A republish delivery");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected,
                "A republish changed B state");
    Access::Clear(*room);
    prepare_publisher();
    Access::PrepareRepublish(*room, track, {});
    listener->reenter = true;
    const auto generation_b = Access::InstalledGeneration(*room);
    auto current_done = asio::co_spawn(callbacks.io, Access::Republish(*room, generation_b), asio::use_future);
    TEST_ASSERT(current_done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                "Republish listener deadlocked with cross-thread Room API");
    current_done.get();
    TEST_ASSERT(listener->calls == 1 && trailing_listener->calls == 0,
                "Reentrant Disconnect failed to invalidate remaining republish listeners");
    TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Disconnected,
                "Republish listener Disconnect did not finish");
    Access::Clear(*room);
    room->Disconnect();
    callbacks.Drain();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 10: Republish delivery was invalidatable and lock-free." << std::endl;
}

enum class DeliveryKind { Participant, ParticipantValue, Metadata, Mute, Speakers, Quality, Stream, Permission };

class SignalDeliveryListener final : public livekit::RoomListener {
public:
    explicit SignalDeliveryListener(DeliveryKind kind) : kind_(kind) {}
    std::function<void()> on_call;
    mutable std::function<void()> on_query;
    std::atomic<int> calls{0};
    std::atomic<int> room_updates{0};
    bool ConsumesParticipantEvents() const override {
        if (on_query) on_query();
        return kind_ == DeliveryKind::ParticipantValue;
    }
    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        if (p->sid() == "PA_DELIVERY") Observe(DeliveryKind::Participant);
    }
    void OnParticipantEvent(const livekit::ParticipantEvent& event) override {
        if (event.kind == livekit::ParticipantEventKind::Upsert && event.participant.key.sid == "PA_DELIVERY") {
            Observe(DeliveryKind::ParticipantValue);
        }
    }
    void OnRoomMetadataChanged(const livekit::RoomInfo&, const std::string&, const std::string&) override {
        Observe(DeliveryKind::Metadata);
    }
    void OnRoomUpdated(const livekit::RoomInfo&) override { ++room_updates; }
    void OnTrackMuted(std::shared_ptr<livekit::Participant>, std::shared_ptr<livekit::TrackPublication>, bool) override {
        Observe(DeliveryKind::Mute);
    }
    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>>&) override {
        Observe(DeliveryKind::Speakers);
    }
    void OnConnectionQualityChanged(std::shared_ptr<livekit::Participant>, livekit::ConnectionQuality, float) override {
        Observe(DeliveryKind::Quality);
    }
    void OnTrackStreamStateChanged(std::shared_ptr<livekit::Participant>, std::shared_ptr<livekit::TrackPublication>,
                                  livekit::TrackPublication::StreamState) override { Observe(DeliveryKind::Stream); }
    void OnTrackSubscriptionPermissionChanged(const livekit::TrackSubscriptionPermission&,
        std::shared_ptr<livekit::Participant>, std::shared_ptr<livekit::TrackPublication>) override { Observe(DeliveryKind::Permission); }
private:
    void Observe(DeliveryKind kind) {
        if (kind != kind_) return;
        ++calls;
        if (on_call) on_call();
    }
    DeliveryKind kind_;
};

livekit::SignalEvent DeliverySignal(DeliveryKind kind) {
    livekit::SignalEvent event;
    event.type = livekit::SignalEvent::Message;
    event.message = std::make_shared<livekit::proto::SignalResponse>();
    auto& response = *event.message;
    switch (kind) {
    case DeliveryKind::Participant:
    case DeliveryKind::ParticipantValue: {
        auto* participant = response.mutable_update()->add_participants();
        participant->set_sid("PA_DELIVERY");
        participant->set_identity("delivery");
        participant->set_state(livekit::proto::ParticipantInfo::ACTIVE);
        break;
    }
    case DeliveryKind::Metadata:
        response.mutable_room_update()->mutable_room()->set_sid("RM_STRESS_1");
        response.mutable_room_update()->mutable_room()->set_metadata("A metadata");
        break;
    case DeliveryKind::Mute:
        response.mutable_mute()->set_sid("TR_DELIVERY");
        response.mutable_mute()->set_muted(true);
        break;
    case DeliveryKind::Speakers: {
        auto* speaker = response.mutable_speakers_changed()->add_speakers();
        speaker->set_sid("PA_FIXTURE"); speaker->set_active(true); speaker->set_level(0.5f);
        break;
    }
    case DeliveryKind::Quality: {
        auto* quality = response.mutable_connection_quality()->add_updates();
        quality->set_participant_sid("PA_FIXTURE"); quality->set_quality(livekit::proto::POOR); quality->set_score(0.2f);
        break;
    }
    case DeliveryKind::Stream: {
        auto* stream = response.mutable_stream_state_update()->add_stream_states();
        stream->set_participant_sid("PA_FIXTURE"); stream->set_track_sid("TR_DELIVERY"); stream->set_state(livekit::proto::PAUSED);
        break;
    }
    case DeliveryKind::Permission: {
        auto* permission = response.mutable_subscription_permission_update();
        permission->set_participant_sid("PA_FIXTURE"); permission->set_track_sid("TR_DELIVERY"); permission->set_allowed(false);
        break;
    }
    }
    return event;
}

asio::awaitable<void> TestCase11_SignalListenerReplacement(asio::any_io_executor executor) {
    using Access = livekit::RoomConnectAttemptTestAccess;
    std::cout << "[STRESS TEST 11] Signal/participant listener replacement matrix..." << std::endl;
    auto& io = static_cast<asio::io_context&>(executor.context());
    auto server = std::make_shared<StressMockServer>(io);
    g_keep_alive_servers.push_back(server);
    server->StartAccept();
    auto worker = std::make_shared<RoomCallbackWorker>();
    g_callback_workers.push_back(worker);
    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    livekit::SignalOptions opts;
    opts.allow_insecure_transport = true;
    opts.single_peer_connection = false;
    opts.create_webrtc_pc = false;
    opts.connect_timeout = std::chrono::seconds(2);

    for (auto kind : {DeliveryKind::Participant, DeliveryKind::ParticipantValue, DeliveryKind::Metadata,
                      DeliveryKind::Mute, DeliveryKind::Speakers, DeliveryKind::Quality,
                      DeliveryKind::Stream, DeliveryKind::Permission}) {
        auto room = livekit::Room::Create(worker->io.get_executor());
        auto seed = [&]() {
            auto initial = DeliverySignal(DeliveryKind::Participant);
            auto* p = initial.message->mutable_update()->mutable_participants(0);
            p->set_sid("PA_FIXTURE");
            auto* track = p->add_tracks();
            track->set_sid("TR_DELIVERY"); track->set_type(livekit::proto::VIDEO);
            Access::DispatchSignalEvent(*room, initial, Access::InstalledGeneration(*room));
            worker->Drain();
        };
        TEST_ASSERT(co_await room->Connect(url, "delivery-a", opts), "Delivery A connect failed");
        seed();
        auto first = std::make_shared<SignalDeliveryListener>(kind);
        auto trailing = std::make_shared<SignalDeliveryListener>(kind);
        auto gate = std::make_shared<SignalEventCommitGate>(executor, 1);
        first->on_call = [gate, room]() {
            RequireCrossThreadRoomRead(room);
            gate->ArriveAndWait(0);
        };
        trailing->on_call = [room]() { room->Disconnect(); };
        room->AddListener(first);
        room->AddListener(trailing);
        const auto generation_a = Access::InstalledGeneration(*room);
        const auto event = DeliverySignal(kind);
        asio::post(worker->io, [room, event, generation_a]() {
            Access::DispatchSignalEvent(*room, event, generation_a);
        });
        co_await gate->WaitUntilAllArrived();
        room->Disconnect();
        TEST_ASSERT(co_await room->Connect(url, "delivery-b", opts), "Delivery B connect failed");
        auto local_b = room->local_participant();
        const auto generation_b = Access::InstalledGeneration(*room);
        gate->Release();
        worker->Drain();
        TEST_ASSERT(first->calls == 1 && trailing->calls == 0,
                    "A post-commit notification reached a trailing listener after B replacement");
        TEST_ASSERT(room->connection_state() == livekit::ConnectionState::Connected &&
                    room->local_participant() == local_b && Access::InstalledGeneration(*room) == generation_b,
                    "A post-commit listener changed B Room/session state");
        TEST_ASSERT(first->room_updates == 0 && trailing->room_updates == 0,
                    "A second notification to the same listener bypassed admission");

        // Positive control: current B events must still reach both listeners.
        first->on_call = {};
        trailing->on_call = {};
        seed();
        Access::DispatchSignalEvent(*room, event, generation_b);
        worker->Drain();
        TEST_ASSERT(first->calls == 2 && trailing->calls == 1, "Current B notification was lost");
        room->RemoveListener(first);
        room->RemoveListener(trailing);
        room->Disconnect();
        worker->Drain();
    }

    // A virtual legacy-consumption query can itself reenter Room. Admission
    // must be repeated after that query, not merely before invoking it.
    auto room = livekit::Room::Create(worker->io.get_executor());
    TEST_ASSERT(co_await room->Connect(url, "delivery-query", opts), "Query test connect failed");
    auto listener = std::make_shared<SignalDeliveryListener>(DeliveryKind::Participant);
    listener->on_query = [room]() { RequireCrossThreadRoomRead(room, true); };
    room->AddListener(listener);
    Access::DispatchSignalEvent(*room, DeliverySignal(DeliveryKind::Participant), Access::InstalledGeneration(*room));
    TEST_ASSERT(listener->calls == 0, "Listener query invalidated Room but event was still invoked");
    room->RemoveListener(listener);
    worker->Drain();

    // Registration is checked for every listener in an existing snapshot.
    TEST_ASSERT(co_await room->Connect(url, "delivery-registration", opts), "Registration test connect failed");
    auto removing = std::make_shared<SignalDeliveryListener>(DeliveryKind::Metadata);
    auto removed = std::make_shared<SignalDeliveryListener>(DeliveryKind::Metadata);
    removing->on_call = [room, removed]() { room->RemoveListener(removed); };
    room->AddListener(removing);
    room->AddListener(removed);
    Access::DispatchSignalEvent(*room, DeliverySignal(DeliveryKind::Metadata), Access::InstalledGeneration(*room));
    TEST_ASSERT(removing->calls == 1 && removing->room_updates == 1 &&
                removed->calls == 0 && removed->room_updates == 0,
                "Removed listener remained eligible in a previously captured snapshot");
    room->RemoveListener(removing);
    room->Disconnect();
    worker->Drain();
    server->CloseActiveConnections();
    server->Stop();
    std::cout << "[PASS] Test 11: All signal families and participant delivery reject replacement." << std::endl;
}

// 主入口
// ============================================================================
int main(int argc, char** argv) {
    const bool native_only = argc > 1 && std::string(argv[1]) == "--sig001-native";
    const bool delivery_only = argc > 1 && std::string(argv[1]) == "--sig001-delivery";
    const bool owner_only = argc > 1 && std::string(argv[1]) == "--sig001-owner";
    std::cout << "========================================================" << std::endl;
    std::cout << "  LiveKit Native C++ SDK Lifecycle & Reconnect Stress Harness" << std::endl;
    std::cout << "========================================================" << std::endl;

    asio::io_context io_ctx;
    auto work_guard = asio::make_work_guard(io_ctx);

    asio::co_spawn(io_ctx, [&]() -> asio::awaitable<void> {
        try {
            auto executor = co_await asio::this_coro::executor;
            if (!native_only && !delivery_only && !owner_only) {
            co_await TestCase1_RapidConnectDisconnect100Cycles(executor);
            co_await TestCase2_SuddenDropAndSoftReconnect(executor);
            co_await TestCase3_HardReconnectFallback(executor);
            co_await TestCase4_ConcurrentEventAndDestructionRace(executor);
            co_await TestCase5_StaleConnectRollbackOwnership(executor);
            co_await TestCase6_StaleSignalEventsCannotMutateReplacement(executor);
            co_await TestCase8_LifecycleListenerDeliveryIsReentrant(executor);
            }
            if (!native_only && !delivery_only) co_await TestCase7_CrossThreadCleanupUsesTransportOwner(executor);
            if (!delivery_only && !owner_only) {
            co_await TestCase9_NativeGenerationCommit(executor);
            co_await TestCase10_RepublishDelivery(executor);
            }
            if (!native_only && !owner_only) co_await TestCase11_SignalListenerReplacement(executor);

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
    g_callback_workers.clear();
    return 0;
}
