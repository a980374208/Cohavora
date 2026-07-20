#include "region_provider.h"
#include "websocket_client.h"
#include <istream>
#include <ostream>
#include <sstream>
#include <variant>
#include <nlohmann/json.hpp>

namespace livekit {

asio::awaitable<HttpResponse> HttpClient::Get(asio::ssl::context& ssl_ctx, 
                                             const std::string& url_str, 
                                             const std::string& token, 
                                             std::chrono::milliseconds timeout) {
    Url url = ParseUrl(url_str);
    if (url.scheme == "wss") url.scheme = "https";
    else if (url.scheme == "ws") url.scheme = "http";
    
    bool is_ssl = (url.scheme == "https");
    auto executor = co_await asio::this_coro::executor;
    
    // Resolve
    asio::ip::tcp::resolver resolver(executor);
    auto results = co_await resolver.async_resolve(url.host, url.port, asio::use_awaitable);
    
    // Connect
    auto socket = std::make_unique<asio::ip::tcp::socket>(executor);
    co_await asio::async_connect(*socket, results, asio::use_awaitable);
    
    // Setup stream
    using SocketPtr = std::unique_ptr<asio::ip::tcp::socket>;
    using SslStreamPtr = std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>>;
    std::variant<std::monostate, SocketPtr, SslStreamPtr> stream;
    
    if (is_ssl) {
        auto ssl_stream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), ssl_ctx);
        SSL_set_tlsext_host_name(ssl_stream->native_handle(), url.host.c_str());
        ssl_stream->set_verify_mode(asio::ssl::verify_peer);
        ssl_stream->set_verify_callback(asio::ssl::host_name_verification(url.host));
        co_await ssl_stream->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
        stream = std::move(ssl_stream);
    } else {
        stream = std::move(socket);
    }
    
    // Send Request
    std::string path_query = url.path;
    if (!url.query.empty()) {
        path_query += "?" + url.query;
    }
    std::string req = "GET " + path_query + " HTTP/1.1\r\n"
                      "Host: " + url.host + "\r\n"
                      "Accept: */*\r\n";
    if (!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    
    if (std::holds_alternative<SocketPtr>(stream)) {
        co_await asio::async_write(*std::get<SocketPtr>(stream), asio::buffer(req), asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream)) {
        co_await asio::async_write(*std::get<SslStreamPtr>(stream), asio::buffer(req), asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
    
    // Read Headers
    asio::streambuf response_buf;
    if (std::holds_alternative<SocketPtr>(stream)) {
        co_await asio::async_read_until(*std::get<SocketPtr>(stream), response_buf, "\r\n\r\n", asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream)) {
        co_await asio::async_read_until(*std::get<SslStreamPtr>(stream), response_buf, "\r\n\r\n", asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
    
    // Parse Headers
    std::istream response_stream(&response_buf);
    std::string http_version;
    response_stream >> http_version;
    int status_code = 0;
    response_stream >> status_code;
    
    std::string header;
    while (std::getline(response_stream, header) && header != "\r") {
        // Just consume headers
    }
    
    std::string response_data;
    if (response_buf.size() > 0) {
        response_data = std::string(asio::buffers_begin(response_buf.data()), asio::buffers_end(response_buf.data()));
        response_buf.consume(response_buf.size());
    }
    
    // Read Body until EOF
    char temp_buf[4096];
    try {
        while (true) {
            size_t bytes = 0;
            if (std::holds_alternative<SocketPtr>(stream)) {
                bytes = co_await std::get<SocketPtr>(stream)->async_read_some(asio::buffer(temp_buf, sizeof(temp_buf)), asio::use_awaitable);
            } else if (std::holds_alternative<SslStreamPtr>(stream)) {
                bytes = co_await std::get<SslStreamPtr>(stream)->async_read_some(asio::buffer(temp_buf, sizeof(temp_buf)), asio::use_awaitable);
            } else {
                throw std::system_error(asio::error::not_connected);
            }
            response_data.append(temp_buf, bytes);
        }
    } catch (const std::system_error& e) {
        if (e.code() != asio::error::eof) {
            throw;
        }
    }
    
    // Close stream
    std::visit([](auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SocketPtr>) {
            std::error_code ec;
            s->close(ec);
        } else if constexpr (std::is_same_v<T, SslStreamPtr>) {
            std::error_code ec;
            s->lowest_layer().close(ec);
        }
    }, stream);
    
    co_return HttpResponse{status_code, response_data};
}

asio::awaitable<std::vector<std::string>> RegionUrlProvider::FetchRegionUrls(asio::ssl::context& ssl_ctx,
                                                                            const std::string& url_str,
                                                                            const std::string& token) {
    Url url = ParseUrl(url_str);
    bool is_cloud = url.host.ends_with(".livekit.cloud") || url.host.ends_with(".livekit.run");
    if (!is_cloud) {
        co_return std::vector<std::string>{};
    }
    
    std::string scheme = (url.scheme == "wss" || url.scheme == "https") ? "https" : "http";
    std::string regions_url = scheme + "://" + url.host + "/settings/regions";
    
    HttpResponse res;
    try {
        res = co_await HttpClient::Get(ssl_ctx, regions_url, token, std::chrono::seconds(3));
    } catch (...) {
        co_return std::vector<std::string>{};
    }
    
    if (res.status_code != 200) {
        co_return std::vector<std::string>{};
    }
    
    std::vector<std::string> urls;
    try {
        auto j = nlohmann::json::parse(res.body);
        if (j.contains("regions") && j["regions"].is_array()) {
            for (const auto& r : j["regions"]) {
                if (r.contains("url") && r["url"].is_string()) {
                    urls.push_back(r["url"].get<std::string>());
                }
            }
        }
    } catch (...) {
        // parse error
    }
    co_return urls;
}

} // namespace livekit
