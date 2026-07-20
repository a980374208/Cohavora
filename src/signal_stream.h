#pragma once

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <system_error>
#include "websocket_client.h"

namespace livekit {
namespace proto {
class SignalRequest;
class SignalResponse;
}
}

namespace livekit {

class SignalStream : public std::enable_shared_from_this<SignalStream> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<livekit::proto::SignalResponse>)>;
    using CloseCallback = std::function<void(const std::string& reason)>;

    struct ConnectResult {
        std::shared_ptr<SignalStream> stream;
        std::error_code error;
    };

    static asio::awaitable<ConnectResult> Connect(
                        asio::ssl::context& ssl_ctx,
                        std::string url_str,
                        std::string token,
                        std::chrono::milliseconds timeout);

    SignalStream(std::shared_ptr<WebSocketClient> ws_client);
    ~SignalStream();

    asio::awaitable<void> Send(const livekit::proto::SignalRequest& req);
    asio::awaitable<void> Close(bool notify_close);

    void StartRead();

    void SetOnMessage(MessageCallback cb) { message_cb_ = std::move(cb); }
    void SetOnClose(CloseCallback cb) { close_cb_ = std::move(cb); }

    bool IsConnected() const { return ws_client_->IsConnected(); }

private:
    void SetupCallbacks();

private:
    std::shared_ptr<WebSocketClient> ws_client_;
    MessageCallback message_cb_;
    CloseCallback close_cb_;
};

} // namespace livekit
