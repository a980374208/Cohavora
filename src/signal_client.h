#pragma once

#include "coro_allocator.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <system_error>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <thread>
#include <optional>
#include "websocket_client.h"
#include "signal_stream.h"

// Forward declare generated protobuf messages
namespace livekit {
namespace proto {
class JoinResponse;
class ReconnectResponse;
class SignalRequest;
class SignalResponse;
}
}

namespace livekit {

struct SignalEvent {
    enum Type {
        Message,
        Close
    } type;
    std::shared_ptr<proto::SignalResponse> message;
    std::string close_reason;
};

using SignalEventHandler = std::function<void(const SignalEvent&)>;

struct SignalSdkOptions {
    std::string sdk = "cpp";
    std::optional<std::string> sdk_version = "0.1.0";
};

struct SignalOptions {
    bool auto_subscribe = true;
    bool adaptive_stream = false;
    SignalSdkOptions sdk_options;
    bool single_peer_connection = true;
    bool create_webrtc_pc = true; // Toggle actual creation of WebRTC PC instances
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(5);
    std::chrono::milliseconds reconnect_timeout = std::chrono::seconds(5);
};

class SignalClient;

struct ConnectResult {
    std::shared_ptr<SignalClient> client;
    std::shared_ptr<proto::JoinResponse> join_response;
    std::error_code error;
};

struct RestartResult {
    std::shared_ptr<proto::ReconnectResponse> reconnect_response;
    std::error_code error;
};

class SignalClient : public std::enable_shared_from_this<SignalClient> {
public:
    static asio::awaitable<ConnectResult> Connect(
                        const std::string& url_str,
                        const std::string& token,
                        const SignalOptions& options,
                        const std::optional<std::vector<uint8_t>>& publisher_offer_sdp, // serialized SessionDescription
                        SignalEventHandler event_handler);

    SignalClient(const std::string& url_str,
                 const std::string& token,
                 const SignalOptions& options,
                 bool single_pc_mode_active,
                 std::shared_ptr<proto::JoinResponse> join_response,
                 SignalEventHandler event_handler,
                 asio::any_io_executor executor);
    ~SignalClient();

    // Restart connection, returns awaitable RestartResult
    asio::awaitable<RestartResult> Restart();

    // Call by engine once fully connected
    void SetReconnected();

    // Set signaling events ready and dispatch buffered events
    void SetEventReady();

    // Send a message
    void Send(const proto::SignalRequest& req);

    // Close signaling connection
    void Close();

    // Accessors
    std::shared_ptr<proto::JoinResponse> join_response() const { return join_response_; }
    SignalOptions options() const { return options_; }
    std::string url() const { return url_; }
    std::string token() const;
    uint32_t next_request_id() { return request_id_.fetch_add(1); }
    bool is_single_pc_mode_active() const { return single_pc_mode_active_; }
    bool is_connected() const;

private:
    void StartHeartbeat();
    void StopHeartbeat();
    void FlushQueue();
    void HandleIncomingMessage(std::shared_ptr<proto::SignalResponse> msg);
    void HandleClose(const std::string& reason);
    
    // Coroutine internals
    asio::awaitable<void> HeartbeatLoop(uint32_t interval_sec, uint32_t timeout_sec);
    asio::awaitable<std::shared_ptr<proto::JoinResponse>> ConnectInternal(
        const std::optional<std::vector<uint8_t>>& publisher_offer_sdp);
    asio::awaitable<std::shared_ptr<proto::ReconnectResponse>> ReconnectInternal();
    asio::awaitable<std::shared_ptr<proto::JoinResponse>> TryConnectInternal(const std::string& connect_url);
    asio::awaitable<std::shared_ptr<proto::JoinResponse>> FallbackRegionsInternal(
        const std::error_code& last_error,
        const std::optional<std::vector<uint8_t>>& publisher_offer_sdp);
    asio::awaitable<void> ValidateInternal(const std::string& url_str, const std::string& token);

private:
    // Network resources
    asio::any_io_executor executor_;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;

    // Connection states
    std::shared_ptr<SignalStream> stream_;
    mutable std::shared_mutex stream_mutex_; // Protects stream_ swaps

    std::string url_;
    SignalOptions options_;
    bool single_pc_mode_active_ = false;
    std::shared_ptr<proto::JoinResponse> join_response_;
    SignalEventHandler event_handler_;

    std::atomic<bool> reconnecting_{false};
    std::atomic<uint32_t> request_id_{1};

    mutable std::mutex token_mutex_;
    std::string token_;

    // Queued mutations during reconnect
    std::mutex queue_mutex_;
    std::vector<proto::SignalRequest> queued_requests_;

    // Heartbeats
    std::atomic<bool> heartbeat_active_{false};
    std::shared_ptr<asio::steady_timer> heartbeat_timer_;
    std::atomic<std::chrono::steady_clock::time_point> last_received_time_;
    std::atomic<int64_t> last_rtt_{0};

    // Event Buffering
    std::mutex event_buffer_mutex_;
    std::vector<std::shared_ptr<proto::SignalResponse>> buffered_events_;
    std::atomic<bool> event_ready_{false};
};

} // namespace livekit
