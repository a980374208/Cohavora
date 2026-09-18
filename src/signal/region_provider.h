#pragma once

#include "credential_url.h"

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
    // Native validate/region metadata budgets (wire headers and response body).
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;
    static constexpr size_t kMaxBodyBytes = 1024 * 1024;

    static asio::awaitable<HttpResponse> Get(asio::ssl::context& ssl_ctx, 
                                             const std::string& url_str, 
                                             const std::string& token, 
                                             std::chrono::milliseconds timeout,
                                             CredentialUrlPolicy policy = {});
};

class RegionUrlProvider {
public:
    static asio::awaitable<std::vector<std::string>> FetchRegionUrls(asio::ssl::context& ssl_ctx,
                                                                     const std::string& url_str,
                                                                     const std::string& token,
                                                                     CredentialUrlPolicy policy);
};

} // namespace livekit
