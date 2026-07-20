#include "websocket_client.h"
#include <random>
#include <iostream>
#include <sstream>
#include <istream>
#include <ostream>
#include <openssl/sha.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

namespace livekit {

Url ParseUrl(const std::string& url_str) {
    Url url;
    size_t scheme_end = url_str.find("://");
    if (scheme_end == std::string::npos) return url;
    url.scheme = url_str.substr(0, scheme_end);
    
    std::string host_port_path = url_str.substr(scheme_end + 3);
    size_t path_start = host_port_path.find('/');
    std::string host_port = (path_start == std::string::npos) ? host_port_path : host_port_path.substr(0, path_start);
    
    if (path_start != std::string::npos) {
        std::string path_query = host_port_path.substr(path_start);
        size_t query_start = path_query.find('?');
        if (query_start != std::string::npos) {
            url.path = path_query.substr(0, query_start);
            url.query = path_query.substr(query_start + 1);
        } else {
            url.path = path_query;
        }
    } else {
        url.path = "/";
    }
    
    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        url.host = host_port.substr(0, colon);
        url.port = host_port.substr(colon + 1);
    } else {
        url.host = host_port;
        url.port = (url.scheme == "wss" || url.scheme == "https") ? "443" : "80";
    }
    return url;
}

static std::string Base64Encode(const unsigned char* buffer, size_t length) {
    static const char char_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i < length) {
        uint32_t octet_a = i < length ? buffer[i++] : 0;
        uint32_t octet_b = i < length ? buffer[i++] : 0;
        uint32_t octet_c = i < length ? buffer[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        result.push_back(char_set[(triple >> 3 * 6) & 0x3F]);
        result.push_back(char_set[(triple >> 2 * 6) & 0x3F]);
        result.push_back(i > length + 1 ? '=' : char_set[(triple >> 1 * 6) & 0x3F]);
        result.push_back(i > length ? '=' : char_set[(triple >> 0 * 6) & 0x3F]);
    }
    return result;
}

static std::string GenerateWebSocketKey() {
    unsigned char key[16];
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> distribution(0, 255);
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<unsigned char>(distribution(generator));
    }
    return Base64Encode(key, 16);
}

static std::optional<std::string> GetProxyFromEnv(bool is_secure) {
    const char* proxy = nullptr;
    if (is_secure) {
        proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
    } else {
        proxy = std::getenv("HTTP_PROXY");
        if (!proxy) proxy = std::getenv("http_proxy");
    }
    if (proxy && strlen(proxy) > 0) {
        return std::string(proxy);
    }
    return std::nullopt;
}

static void LoadSystemCertificates(asio::ssl::context& ssl_ctx) {
#ifdef _WIN32
    HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
    if (hStore) {
        X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx.native_handle());
        PCCERT_CONTEXT pContext = NULL;
        while ((pContext = CertFindCertificateInStore(hStore, X509_ASN_ENCODING, 0, CERT_FIND_ANY, NULL, pContext)) != NULL) {
            const unsigned char* d2i_ptr = pContext->pbCertEncoded;
            X509* x509 = d2i_X509(NULL, &d2i_ptr, pContext->cbCertEncoded);
            if (x509) {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
            }
        }
        CertCloseStore(hStore, 0);
    }
#else
    ssl_ctx.set_default_verify_paths();
#endif
}

WebSocketClient::WebSocketClient(asio::io_context& io_ctx, asio::ssl::context& ssl_ctx)
    : io_ctx_(io_ctx), ssl_ctx_(ssl_ctx), strand_(asio::make_strand(io_ctx)) {
    LoadSystemCertificates(ssl_ctx_);
}

WebSocketClient::~WebSocketClient() {
    shutdown_stream();
}

asio::awaitable<std::error_code> WebSocketClient::Connect(std::string url_str, 
                                                        std::string token, 
                                                        std::chrono::milliseconds timeout) {
    std::cout << "WebSocketClient::Connect: 1 (shared_from_this)" << std::endl;
    auto self = shared_from_this();
    std::cout << "WebSocketClient::Connect: 2" << std::endl;
    Url url = ParseUrl(url_str);
    is_ssl_ = (url.scheme == "wss");
     closed_by_us_ = false;

    auto executor = co_await asio::this_coro::executor;
    std::cout << "WebSocketClient::Connect: 3" << std::endl;
    auto timer = std::make_shared<asio::steady_timer>(executor);
    timer->expires_after(timeout);
    
    auto connect_done = std::make_shared<std::atomic<bool>>(false);
    
    timer->async_wait([this, self, connect_done](const std::error_code& ec) {
        if (!ec && !connect_done->load()) {
            shutdown_stream();
        }
    });

    try {
        std::cout << "WebSocketClient::Connect: 4 (try connect)" << std::endl;
        auto proxy_env = GetProxyFromEnv(is_ssl_);
        std::cout << "WebSocketClient::Connect: 4.1 (proxy env checked)" << std::endl;
        if (proxy_env) {
            std::cout << "WebSocketClient::Connect: 4.1.1 (using proxy)" << std::endl;
            std::string proxy_str = *proxy_env;
            Url p_url = ParseUrl(proxy_str.find("://") == std::string::npos ? "http://" + proxy_str : proxy_str);
            
            std::string auth_hdr;
            size_t at_sign = p_url.host.find('@');
            std::string proxy_host = p_url.host;
            if (at_sign != std::string::npos) {
                std::string credentials = p_url.host.substr(0, at_sign);
                proxy_host = p_url.host.substr(at_sign + 1);
                auth_hdr = "Proxy-Authorization: Basic " + Base64Encode(reinterpret_cast<const unsigned char*>(credentials.data()), credentials.size()) + "\r\n";
            }
            
            co_await AsyncConnectSocket(proxy_host, p_url.port);
            std::optional<std::string> auth_opt;
            if (!auth_hdr.empty()) auth_opt = auth_hdr;
            co_await AsyncHttpProxyConnect(url.host, url.port, url.host, url.port, auth_opt);
        } else {
            std::cout << "WebSocketClient::Connect: 4.1.2 (direct connection)" << std::endl;
            co_await AsyncConnectSocket(url.host, url.port);
        }
        std::cout << "WebSocketClient::Connect: 4.2 (socket connected)" << std::endl;

        if (is_ssl_) {
            std::cout << "WebSocketClient::Connect: 4.3 (SSL handshake starting)" << std::endl;
            co_await AsyncSslHandshake(url.host);
            std::cout << "WebSocketClient::Connect: 4.4 (SSL handshake completed)" << std::endl;
        }

        std::cout << "WebSocketClient::Connect: 4.5 (WS handshake starting)" << std::endl;
        co_await AsyncWsHandshake(url.host, url.path, url.query, token);
        std::cout << "WebSocketClient::Connect: 4.6 (WS handshake completed)" << std::endl;
        
        connect_done->store(true);
        timer->cancel();
        connected_ = true;
        
        co_return std::error_code{};
    } catch (const std::system_error& e) {
        connect_done->store(true);
        timer->cancel();
        shutdown_stream();
        co_return e.code();
    } catch (...) {
        connect_done->store(true);
        timer->cancel();
        shutdown_stream();
        co_return std::make_error_code(std::errc::connection_aborted);
    }
}

void WebSocketClient::StartRead() {
    std::cout << "WebSocketClient::StartRead: posting ReadLoop spawn" << std::endl;
    asio::post(io_ctx_, [self = this->shared_from_this()]() {
        std::cout << "WebSocketClient::StartRead: spawning ReadLoop via post" << std::endl;
        asio::co_spawn(self->io_ctx_, [self]() -> asio::awaitable<void> {
            std::cout << "WebSocketClient::ReadLoop: coroutine started" << std::endl;
            try {
                co_await self->ReadLoop();
                std::cout << "WebSocketClient::ReadLoop: coroutine exited cleanly" << std::endl;
            } catch (const std::exception& e) {
                std::cout << "WebSocketClient::ReadLoop: coroutine exited with exception: " << e.what() << std::endl;
            } catch (...) {
                std::cout << "WebSocketClient::ReadLoop: coroutine exited with unknown exception" << std::endl;
            }
        }, asio::detached);
    });
}

asio::awaitable<void> WebSocketClient::SendBinary(std::vector<uint8_t> data) {
    co_await SendRawFrame(0x2, data);
}

asio::awaitable<void> WebSocketClient::SendText(std::string text) {
    std::vector<uint8_t> data(text.begin(), text.end());
    co_await SendRawFrame(0x1, data);
}

asio::awaitable<void> WebSocketClient::Close(uint16_t code, const std::string& reason) {
    if (!connected_) co_return;
    closed_by_us_ = true;
    
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());
    
    try {
        co_await SendRawFrame(0x8, payload);
    } catch (...) {
        // Ignore errors during close frame send
    }
    shutdown_stream();
}

asio::awaitable<void> WebSocketClient::ReadLoop() {
    try {
        while (connected_) {
            co_await ReadFrame();
        }
    } catch (const std::system_error& e) {
        handle_error(e.code());
    } catch (...) {
        std::error_code ec = asio::error::operation_aborted;
        handle_error(ec);
    }
}

asio::awaitable<void> WebSocketClient::ReadFrame() {
    uint8_t header[2];
    co_await async_read_stream(asio::buffer(header, 2));

    bool fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint8_t base_len = header[1] & 0x7F;

    uint64_t payload_len = 0;
    if (base_len <= 125) {
        payload_len = base_len;
    } else if (base_len == 126) {
        uint8_t ext_len[2];
        co_await async_read_stream(asio::buffer(ext_len, 2));
        payload_len = (static_cast<uint64_t>(ext_len[0]) << 8) | ext_len[1];
    } else if (base_len == 127) {
        uint8_t ext_len[8];
        co_await async_read_stream(asio::buffer(ext_len, 8));
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | ext_len[i];
        }
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        co_await async_read_stream(asio::buffer(mask_key, 4));
    }

    std::vector<uint8_t> payload(payload_len);
    if (payload_len > 0) {
        if (payload_len > 64 * 1024 * 1024) {
            throw std::system_error(std::make_error_code(std::errc::message_size));
        }
        co_await async_read_stream(asio::buffer(payload));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= mask_key[i % 4];
            }
        }
    }

    co_await HandleFrame(opcode, fin, std::move(payload));
}

asio::awaitable<void> WebSocketClient::HandleFrame(uint8_t opcode, bool fin, std::vector<uint8_t> payload) {
    if (opcode == 0x8) { 
        uint16_t code = 1000;
        std::string reason;
        if (payload.size() >= 2) {
            code = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
            reason = std::string(payload.begin() + 2, payload.end());
        }
        if (close_cb_) {
            close_cb_(code, reason);
        }
        shutdown_stream();
    } else if (opcode == 0x9) { 
        co_await SendRawFrame(0xA, payload);
    } else if (opcode == 0xA) { 
        // Pong
    } else if (opcode == 0x2) { 
        if (message_cb_) {
            message_cb_(payload);
        }
    } else if (opcode == 0x1) { 
        if (text_message_cb_) {
            text_message_cb_(std::string(payload.begin(), payload.end()));
        }
    }
}

asio::awaitable<void> WebSocketClient::SendRawFrame(uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::cout << "WebSocketClient::SendRawFrame: opcode=" << (int)opcode << " len=" << payload.size() << std::endl;
    std::vector<uint8_t> frame;
    frame.reserve(10 + payload.size());
    
    frame.push_back(0x80 | (opcode & 0x0F));
    uint8_t mask_bit = 0x80;
    
    size_t payload_len = payload.size();
    if (payload_len <= 125) {
        frame.push_back(mask_bit | static_cast<uint8_t>(payload_len));
    } else if (payload_len <= 65535) {
        frame.push_back(mask_bit | 126);
        frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        frame.push_back(mask_bit | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
        }
    }
    
    uint8_t mask_key[4];
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> distribution(0, 255);
    for (int i = 0; i < 4; ++i) {
        mask_key[i] = static_cast<uint8_t>(distribution(generator));
        frame.push_back(mask_key[i]);
    }
    
    size_t header_size = frame.size();
    frame.resize(header_size + payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        frame[header_size + i] = payload[i] ^ mask_key[i % 4];
    }
 
    std::shared_ptr<QueuedMessage> msg;
    bool should_spawn = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        msg = std::make_shared<QueuedMessage>(QueuedMessage{std::move(frame)});
        write_queue_.push(msg);
        
        if (!writing_) {
            writing_ = true;
            should_spawn = true;
        }
    }
    
    if (should_spawn) {
        std::cout << "WebSocketClient::SendRawFrame: spawning WriteLoop" << std::endl;
        asio::co_spawn(io_ctx_, [self = shared_from_this()]() -> asio::awaitable<void> {
            co_await self->WriteLoop();
        }, asio::detached);
    }
    co_return;
}

asio::awaitable<void> WebSocketClient::WriteLoop() {
    auto self = shared_from_this();
    try {
        while (true) {
            auto msg = PopWriteQueue();
            if (!msg) {
                std::cout << "WebSocketClient::WriteLoop: queue empty or disconnected, exiting loop" << std::endl;
                break;
            }
            
            std::cout << "WebSocketClient::WriteLoop: writing " << msg->data.size() << " bytes to stream" << std::endl;
            co_await async_write_stream(asio::buffer(msg->data));
            std::cout << "WebSocketClient::WriteLoop: successfully wrote " << msg->data.size() << " bytes" << std::endl;
        }
    } catch (const std::system_error& e) {
        std::cout << "WebSocketClient::WriteLoop: system_error: " << e.what() << std::endl;
        ResetWritingState();
        handle_error(e.code());
    } catch (...) {
        std::cout << "WebSocketClient::WriteLoop: unknown exception" << std::endl;
        ResetWritingState();
        std::error_code ec = asio::error::operation_aborted;
        handle_error(ec);
    }
}

std::shared_ptr<WebSocketClient::QueuedMessage> WebSocketClient::PopWriteQueue() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_queue_.empty() || !connected_) {
        writing_ = false;
        return nullptr;
    }
    auto msg = write_queue_.front();
    write_queue_.pop();
    return msg;
}

void WebSocketClient::ResetWritingState() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    writing_ = false;
}

asio::awaitable<void> WebSocketClient::AsyncConnectSocket(std::string host, std::string port) {
    std::cout << "WebSocketClient::AsyncConnectSocket: 1 (host: " << host << ", port: " << port << ")" << std::endl;
    auto executor = co_await asio::this_coro::executor;
    std::cout << "WebSocketClient::AsyncConnectSocket: 2" << std::endl;
    
    auto socket = std::make_unique<asio::ip::tcp::socket>(executor);
    auto* socket_raw = socket.get();
    stream_ = std::move(socket);
    
    std::error_code ec;
    auto addr = asio::ip::make_address(host, ec);
    if (!ec) {
        std::cout << "WebSocketClient::AsyncConnectSocket: IP address detected, connecting directly" << std::endl;
        unsigned short port_num = static_cast<unsigned short>(std::stoi(port));
        asio::ip::tcp::endpoint ep(addr, port_num);
        co_await socket_raw->async_connect(ep, asio::use_awaitable);
    } else {
        asio::ip::tcp::resolver resolver(executor);
        std::cout << "WebSocketClient::AsyncConnectSocket: 3 (resolving)" << std::endl;
        auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
        std::cout << "WebSocketClient::AsyncConnectSocket: 4 (resolved)" << std::endl;
        std::cout << "WebSocketClient::AsyncConnectSocket: 5 (connecting)" << std::endl;
        co_await asio::async_connect(*socket_raw, results, asio::use_awaitable);
    }
    std::cout << "WebSocketClient::AsyncConnectSocket: 6 (connected)" << std::endl;
    
    if (is_ssl_) {
        auto socket_ptr = std::move(std::get<SocketPtr>(stream_));
        stream_ = std::monostate{};
        auto ssl_stream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket_ptr), ssl_ctx_);
        stream_ = std::move(ssl_stream);
    }
}

asio::awaitable<void> WebSocketClient::AsyncHttpProxyConnect(std::string proxy_host, std::string proxy_port, std::string target_host, std::string target_port, std::optional<std::string> auth_header) {
    std::string req = "CONNECT " + target_host + ":" + target_port + " HTTP/1.1\r\n"
                      "Host: " + target_host + ":" + target_port + "\r\n";
    if (auth_header) {
        req += *auth_header;
    }
    req += "\r\n";

    auto& socket = std::get<SocketPtr>(stream_);
    co_await asio::async_write(*socket, asio::buffer(req), asio::use_awaitable);

    asio::streambuf response_buf;
    co_await asio::async_read_until(*socket, response_buf, "\r\n\r\n", asio::use_awaitable);

    std::istream response_stream(&response_buf);
    std::string http_version;
    response_stream >> http_version;
    unsigned int status_code;
    response_stream >> status_code;
    if (status_code != 200) {
        throw std::system_error(std::make_error_code(std::errc::connection_refused));
    }
}

asio::awaitable<void> WebSocketClient::AsyncSslHandshake(std::string host) {
    auto& ssl_stream = std::get<SslStreamPtr>(stream_);
    SSL_set_tlsext_host_name(ssl_stream->native_handle(), host.c_str());
    
    ssl_stream->set_verify_mode(asio::ssl::verify_peer);
    ssl_stream->set_verify_callback(asio::ssl::host_name_verification(host));

    co_await ssl_stream->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::AsyncWsHandshake(std::string host, std::string path, std::string query, std::string token) {
    std::cout << "WebSocketClient::AsyncWsHandshake: 1" << std::endl;
    std::string path_query = path;
    if (!query.empty()) {
        path_query += "?" + query;
    }
    std::string ws_key = GenerateWebSocketKey();
    
    std::string req = "GET " + path_query + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + ws_key + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n";
    if (!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    req += "\r\n";

    std::cout << "WebSocketClient::AsyncWsHandshake: 2 (writing request...)" << std::endl;
    co_await async_write_stream(asio::buffer(req));
    std::cout << "WebSocketClient::AsyncWsHandshake: 3 (request written, reading response...)" << std::endl;

    std::cout << "WebSocketClient::AsyncWsHandshake: 4" << std::endl;
    try {
        std::cout << "WebSocketClient::AsyncWsHandshake: 4.1 (entering read_until)" << std::endl;
        if (std::holds_alternative<SocketPtr>(stream_)) {
            co_await asio::async_read_until(*std::get<SocketPtr>(stream_), response_buf_, "\r\n\r\n", asio::use_awaitable);
        } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
            co_await asio::async_read_until(*std::get<SslStreamPtr>(stream_), response_buf_, "\r\n\r\n", asio::use_awaitable);
        } else {
            throw std::system_error(asio::error::not_connected);
        }
        std::cout << "WebSocketClient::AsyncWsHandshake: 4.2 (read_until finished)" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "WebSocketClient::AsyncWsHandshake: 4.3 exception: " << e.what() << std::endl;
        throw;
    } catch (...) {
        std::cout << "WebSocketClient::AsyncWsHandshake: 4.4 unknown exception" << std::endl;
        throw;
    }
    std::cout << "WebSocketClient::AsyncWsHandshake: 5 (response read)" << std::endl;

    std::istream response_stream(&response_buf_);
    std::string http_version;
    response_stream >> http_version;
    unsigned int status_code;
    response_stream >> status_code;
    if (status_code != 101) {
        throw std::system_error(std::make_error_code(std::errc::connection_refused));
    }
    
    std::string header;
    bool accept_verified = false;
    
    std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string key_magic = ws_key + magic;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(key_magic.data()), key_magic.size(), hash);
    std::string expected_accept = Base64Encode(hash, SHA_DIGEST_LENGTH);
    
    while (std::getline(response_stream, header) && header != "\r") {
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        if (header.rfind("Sec-WebSocket-Accept:", 0) == 0 || header.rfind("sec-websocket-accept:", 0) == 0) {
            size_t colon = header.find(':');
            if (colon != std::string::npos) {
                std::string val = header.substr(colon + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t") + 1);
                if (val == expected_accept) {
                    accept_verified = true;
                }
            }
        }
    }
    
    if (!accept_verified) {
        throw std::system_error(std::make_error_code(std::errc::connection_refused));
    }
}

asio::awaitable<size_t> WebSocketClient::async_read_stream(asio::mutable_buffer buf) {
    std::cout << "WebSocketClient::async_read_stream: requested bytes=" << buf.size() << " cached=" << response_buf_.size() << std::endl;
    if (response_buf_.size() > 0) {
        size_t bytes_to_copy = std::min(response_buf_.size(), buf.size());
        std::memcpy(buf.data(), asio::buffer_cast<const void*>(response_buf_.data()), bytes_to_copy);
        response_buf_.consume(bytes_to_copy);
        std::cout << "WebSocketClient::async_read_stream: consumed " << bytes_to_copy << " bytes from cache" << std::endl;
        
        if (bytes_to_copy < buf.size()) {
            asio::mutable_buffer remaining_buf = buf + bytes_to_copy;
            size_t extra_bytes = co_await async_read_stream(remaining_buf);
            co_return bytes_to_copy + extra_bytes;
        }
        co_return bytes_to_copy;
    }

    std::cout << "WebSocketClient::async_read_stream: reading from socket" << std::endl;
    size_t read_bytes = 0;
    if (std::holds_alternative<SocketPtr>(stream_)) {
        read_bytes = co_await asio::async_read(*std::get<SocketPtr>(stream_), buf, asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
        read_bytes = co_await asio::async_read(*std::get<SslStreamPtr>(stream_), buf, asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
    std::cout << "WebSocketClient::async_read_stream: read " << read_bytes << " bytes from socket" << std::endl;
    co_return read_bytes;
}

asio::awaitable<size_t> WebSocketClient::async_write_stream(asio::const_buffer buf) {
    if (std::holds_alternative<SocketPtr>(stream_)) {
        co_return co_await asio::async_write(*std::get<SocketPtr>(stream_), buf, asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
        co_return co_await asio::async_write(*std::get<SslStreamPtr>(stream_), buf, asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
}

void WebSocketClient::shutdown_stream() {
    connected_ = false;
    std::visit([](auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SocketPtr>) {
            std::error_code ec;
            s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            s->close(ec);
        } else if constexpr (std::is_same_v<T, SslStreamPtr>) {
            std::error_code ec;
            s->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            s->lowest_layer().close(ec);
        }
    }, stream_);
    stream_ = std::monostate{};
}

void WebSocketClient::handle_error(const std::error_code& ec) {
    if (ec == asio::error::operation_aborted) return;
    if (closed_by_us_) return;
    shutdown_stream();
    if (error_cb_) {
        error_cb_(ec);
    }
}

} // namespace livekit
