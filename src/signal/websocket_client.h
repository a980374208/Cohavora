#pragma once

#include "coro_allocator.h"
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <variant>
#include <mutex>
#include <queue>
#include <optional>
#include <system_error>

namespace livekit {

struct Url {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
};

Url ParseUrl(const std::string& url_str);

class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>& binary_payload)>;
    using TextMessageCallback = std::function<void(const std::string& text_payload)>;
    using CloseCallback = std::function<void(uint16_t code, const std::string& reason)>;
    using ErrorCallback = std::function<void(const std::error_code& ec)>;

    WebSocketClient(asio::io_context& io_ctx, asio::ssl::context& ssl_ctx);
    ~WebSocketClient();

    // Connect to WebSocket server, returns awaitable error_code
    asio::awaitable<std::error_code> Connect(std::string url_str, 
                                             std::string token, 
                                             std::chrono::milliseconds timeout);

    // Start background read loop
    void StartRead();

    // Send messages
    asio::awaitable<void> SendBinary(std::vector<uint8_t> data);
    asio::awaitable<void> SendText(std::string text);

    // Close WebSocket
    asio::awaitable<void> Close(uint16_t code, const std::string& reason);

    // Setup Callbacks
    void SetOnMessage(MessageCallback cb) { message_cb_ = std::move(cb); }
    void SetOnTextMessage(TextMessageCallback cb) { text_message_cb_ = std::move(cb); }
    void SetOnClose(CloseCallback cb) { close_cb_ = std::move(cb); }
    void SetOnError(ErrorCallback cb) { error_cb_ = std::move(cb); }

    bool IsConnected() const { return connected_; }

private:
    struct QueuedMessage {
        std::vector<uint8_t> data;
    };

    asio::awaitable<void> ReadLoop();
    asio::awaitable<void> ReadFrame();
    asio::awaitable<void> HandleFrame(uint8_t opcode, bool fin, std::vector<uint8_t> payload);

    asio::awaitable<void> SendRawFrame(uint8_t opcode, const std::vector<uint8_t>& payload);
    asio::awaitable<void> WriteLoop();
    std::shared_ptr<QueuedMessage> PopWriteQueue();
    void ResetWritingState();

    // Stream operations
    asio::awaitable<void> AsyncConnectSocket(std::string host, std::string port);
    asio::awaitable<void> AsyncHttpProxyConnect(std::string proxy_host, std::string proxy_port, std::string target_host, std::string target_port, std::optional<std::string> auth_header);
    asio::awaitable<void> AsyncSslHandshake(std::string host);
    asio::awaitable<void> AsyncWsHandshake(std::string host, std::string path, std::string query, std::string token);

    // Send and read Helpers
    asio::awaitable<size_t> async_read_stream(asio::mutable_buffer buf);
    asio::awaitable<size_t> async_write_stream(asio::const_buffer buf);

    void shutdown_stream();
    void handle_error(const std::error_code& ec);

private:
    asio::io_context& io_ctx_;
    asio::ssl::context& ssl_ctx_;
    asio::strand<asio::io_context::executor_type> strand_;
    
    // Stream variant
    using SocketPtr = std::unique_ptr<asio::ip::tcp::socket>;
    using SslStreamPtr = std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>>;
    std::variant<std::monostate, SocketPtr, SslStreamPtr> stream_;

    bool is_ssl_ = false;
    bool connected_ = false;
    bool closed_by_us_ = false;

    // Callbacks
    MessageCallback message_cb_;
    TextMessageCallback text_message_cb_;
    CloseCallback close_cb_;
    ErrorCallback error_cb_;

    // Buffer to cache extra bytes read during handshake
    asio::streambuf response_buf_;

    // Writing states
    std::queue<std::shared_ptr<QueuedMessage>> write_queue_;
    bool writing_ = false;
    std::mutex write_mutex_;
};

} // namespace livekit
