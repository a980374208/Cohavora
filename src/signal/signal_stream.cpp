#include "signal_stream.h"
#include "livekit_rtc.pb.h"
#include <iostream>

namespace livekit {

SignalStream::SignalStream(std::shared_ptr<WebSocketClient> ws_client)
    : ws_client_(ws_client) {
}

SignalStream::~SignalStream() {
    {
        std::lock_guard lock(callback_mutex_);
        message_cb_ = nullptr;
        close_cb_ = nullptr;
    }
    if (ws_client_) {
        ws_client_->Abort();
    }
}

asio::awaitable<SignalStream::ConnectResult> SignalStream::Connect(
                    asio::ssl::context& ssl_ctx,
                    std::string url_str,
                    std::string token,
                    std::chrono::milliseconds timeout,
                    CredentialUrlPolicy policy) {
    auto executor = co_await asio::this_coro::executor;
    auto& io_ctx = static_cast<asio::io_context&>(executor.context());
    
    auto ws_client = std::make_shared<WebSocketClient>(io_ctx, ssl_ctx);
    auto ec = co_await ws_client->Connect(url_str, token, timeout, policy);
    
    if (ec) {
        co_return ConnectResult{nullptr, ec};
    }
    
    auto stream = std::make_shared<SignalStream>(ws_client);
    stream->SetupCallbacks();
    co_return ConnectResult{stream, {}};
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

void SignalStream::Abort() {
    {
        std::lock_guard lock(callback_mutex_);
        message_cb_ = nullptr;
        close_cb_ = nullptr;
    }
    if (!ws_client_) return;
    ws_client_->Abort();
}

void SignalStream::SetOnMessage(MessageCallback cb) {
    std::lock_guard lock(callback_mutex_);
    message_cb_ = std::move(cb);
}

void SignalStream::SetOnClose(CloseCallback cb) {
    std::lock_guard lock(callback_mutex_);
    close_cb_ = std::move(cb);
}

void SignalStream::StartRead() {
    if (ws_client_) {
        ws_client_->StartRead();
    }
}

void SignalStream::SetupCallbacks() {
    if (!ws_client_) return;

    auto weak_self = weak_from_this();

    ws_client_->SetOnMessage([weak_self](const std::vector<uint8_t>& payload) {
        auto self = weak_self.lock();
        if (!self) return;

        auto resp = std::make_shared<livekit::proto::SignalResponse>();
        if (resp->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            MessageCallback callback;
            {
                std::lock_guard lock(self->callback_mutex_);
                callback = self->message_cb_;
            }
            if (callback) callback(resp);
        } else {
            std::cout << "SignalStream::SetupCallbacks: Failed to parse SignalResponse!" << std::endl;
        }
    });

    ws_client_->SetOnClose([weak_self](uint16_t, const std::string& reason) {
        auto self = weak_self.lock();
        if (!self) return;

        CloseCallback callback;
        {
            std::lock_guard lock(self->callback_mutex_);
            callback = self->close_cb_;
        }
        if (callback) callback(reason);
    });

    ws_client_->SetOnError([weak_self](const std::error_code& ec) {
        auto self = weak_self.lock();
        if (!self) return;

        CloseCallback callback;
        {
            std::lock_guard lock(self->callback_mutex_);
            callback = self->close_cb_;
        }
        if (callback) callback("WebSocket Error: " + ec.message());
    });
}

} // namespace livekit
