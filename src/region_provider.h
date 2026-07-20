#pragma once

#include <string>
#include <vector>
#include <system_error>
#include <chrono>
#include <asio.hpp>
#include <asio/ssl.hpp>

namespace livekit {

struct HttpResponse {
    int status_code = 0;
    std::string body;
};

class HttpClient {
public:
    static asio::awaitable<HttpResponse> Get(asio::ssl::context& ssl_ctx, 
                                             const std::string& url_str, 
                                             const std::string& token, 
                                             std::chrono::milliseconds timeout);
};

class RegionUrlProvider {
public:
    static asio::awaitable<std::vector<std::string>> FetchRegionUrls(asio::ssl::context& ssl_ctx,
                                                                     const std::string& url_str,
                                                                     const std::string& token);
};

} // namespace livekit
