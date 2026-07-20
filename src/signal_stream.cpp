#include "signal_stream.h"
#include "livekit_rtc.pb.h"

namespace livekit {

SignalStream::SignalStream(std::shared_ptr<WebSocketClient> ws_client)
    : ws_client_(ws_client) {
    SetupCallbacks();
}

SignalStream::~SignalStream() {
}

asio::awaitable<SignalStream::ConnectResult> SignalStream::Connect(
                    asio::ssl::context& ssl_ctx,
                    std::string url_str,
                    std::string token,
                    std::chrono::milliseconds timeout) {
    std::cout << "SignalStream::Connect: 1" << std::endl;
    auto executor = co_await asio::this_coro::executor;
    std::cout << "SignalStream::Connect: 2" << std::endl;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    std::cout << "SignalStream::Connect: 3" << std::endl;
    
    auto ws_client = std::make_shared<WebSocketClient>(io_ctx, ssl_ctx);
    std::cout << "SignalStream::Connect: 4" << std::endl;
    auto ec = co_await ws_client->Connect(url_str, token, timeout);
    std::cout << "SignalStream::Connect: 5, ec=" << ec.message() << std::endl;
    
    if (ec) {
        co_return ConnectResult{nullptr, ec};
    }
    
    co_return ConnectResult{std::make_shared<SignalStream>(ws_client), {}};
}

asio::awaitable<void> SignalStream::Send(const livekit::proto::SignalRequest& req) {
    std::vector<uint8_t> payload(req.ByteSizeLong());
    req.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
    co_await ws_client_->SendBinary(std::move(payload));
}

asio::awaitable<void> SignalStream::Close(bool notify_close) {
    if (ws_client_ && ws_client_->IsConnected()) {
        if (notify_close) {
            co_await ws_client_->Close(1000, "Normal Closure");
        } else {
            co_await ws_client_->Close(1005, "");
        }
    }
}

void SignalStream::StartRead() {
    if (ws_client_) {
        ws_client_->StartRead();
    }
}

void SignalStream::SetupCallbacks() {
    ws_client_->SetOnMessage([this](const std::vector<uint8_t>& payload) {
        std::cout << "SignalStream::SetupCallbacks: Got payload of size=" << payload.size() << std::endl;
        auto resp = std::make_shared<livekit::proto::SignalResponse>();
        if (resp->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            std::cout << "SignalStream::SetupCallbacks: Parsed SignalResponse successfully, has_join=" << resp->has_join() << std::endl;
            if (message_cb_) {
                std::cout << "SignalStream::SetupCallbacks: Dispatching message_cb_" << std::endl;
                message_cb_(resp);
            } else {
                std::cout << "SignalStream::SetupCallbacks: message_cb_ is null!" << std::endl;
            }
        } else {
            std::cout << "SignalStream::SetupCallbacks: Failed to parse SignalResponse!" << std::endl;
        }
    });

    ws_client_->SetOnClose([this](uint16_t, const std::string& reason) {
        if (close_cb_) {
            close_cb_(reason);
        }
    });

    ws_client_->SetOnError([this](const std::error_code& ec) {
        if (close_cb_) {
            close_cb_("WebSocket Error: " + ec.message());
        }
    });
}

} // namespace livekit
