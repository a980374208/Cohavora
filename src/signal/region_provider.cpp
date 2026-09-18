#include "region_provider.h"
#include "websocket_client.h"
#include <istream>
#include <ostream>
#include <sstream>
#include <charconv>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace livekit {

namespace {

// Run and the timer handler share a request-local strand. The parent coroutine
// owns this object until Run completes; a queued timer holds only a weak owner.
class HttpGetOperation : public std::enable_shared_from_this<HttpGetOperation> {
public:
    HttpGetOperation(asio::any_io_executor executor, asio::ssl::context& ssl,
                     bool secure, std::chrono::steady_clock::time_point deadline)
        : resolver_(executor), timer_(executor, deadline),
          resolved_(executor, std::chrono::steady_clock::time_point::max()), socket_(executor) {
        if (secure) tls_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(executor, ssl);
    }

    asio::awaitable<HttpResponse> Run(Url url, std::string token) {
        timer_.async_wait([weak = weak_from_this()](const std::error_code& error) {
            if (error) return;
            if (auto self = weak.lock(); self && !self->finished_) {
                self->expired_ = true;
                self->CloseTransport();
            }
        });
        try {
            CheckDeadline();
            // Asio's background getaddrinfo may outlive resolver.cancel(). Wait
            // on a separately cancellable notification, never on that worker.
            resolver_.async_resolve(url.host, url.port,
                [weak = weak_from_this()](std::error_code error, asio::ip::tcp::resolver::results_type addresses) {
                    if (auto self = weak.lock(); self && !self->finished_ && !self->expired_) {
                        self->resolve_error_ = error;
                        self->addresses_ = std::move(addresses);
                        self->resolve_done_ = true;
                        self->resolved_.cancel();
                    }
                });
            std::error_code notification_error;
            co_await resolved_.async_wait(asio::redirect_error(asio::use_awaitable, notification_error));
            CheckDeadline();
            if (!resolve_done_) throw std::system_error(asio::error::operation_aborted);
            if (resolve_error_) throw std::system_error(resolve_error_);
            co_await asio::async_connect(Socket(), addresses_, asio::use_awaitable);
            CheckDeadline();
            HttpResponse response;
            if (tls_) {
                SSL_set_tlsext_host_name(tls_->native_handle(), url.host.c_str());
                tls_->set_verify_mode(asio::ssl::verify_peer);
                tls_->set_verify_callback(asio::ssl::host_name_verification(url.host));
                co_await tls_->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
                CheckDeadline();
                response = co_await Exchange(*tls_, url, token);
            } else {
                response = co_await Exchange(socket_, url, token);
            }
            CheckDeadline();
            Finish();
            co_return response;
        } catch (...) {
            const bool timeout = expired_ || std::chrono::steady_clock::now() >= timer_.expiry();
            Finish();
            if (timeout) throw std::system_error(std::make_error_code(std::errc::timed_out));
            throw;
        }
    }

private:
    asio::ip::tcp::socket& Socket() { return tls_ ? tls_->next_layer() : socket_; }

    void CloseTransport() {
        resolver_.cancel();
        resolved_.cancel();
        std::error_code ignored;
        Socket().close(ignored);
    }

    void Finish() {
        finished_ = true;
        timer_.cancel();
        CloseTransport();
    }

    void CheckDeadline() const {
        if (expired_ || std::chrono::steady_clock::now() >= timer_.expiry()) {
            throw std::system_error(std::make_error_code(std::errc::timed_out));
        }
    }

    template <typename Stream>
    asio::awaitable<HttpResponse> Exchange(Stream& stream, const Url& url, const std::string& token) {
        std::string path_query = url.path;
        if (!url.query.empty()) path_query += "?" + url.query;
        std::string request = "GET " + path_query + " HTTP/1.1\r\n"
                              "Host: " + FormatUrlAuthority(url) + "\r\nAccept: */*\r\n";
        if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
        request += "Connection: close\r\n\r\n";
        co_await asio::async_write(stream, asio::buffer(request), asio::use_awaitable);
        CheckDeadline();

        asio::streambuf buffer(HttpClient::kMaxHeaderBytes);
        std::error_code error;
        co_await asio::async_read_until(stream, buffer, "\r\n\r\n",
                                       asio::redirect_error(asio::use_awaitable, error));
        CheckDeadline();
        if (error == asio::error::not_found) {
            throw std::system_error(std::make_error_code(std::errc::message_size));
        }
        if (error) throw std::system_error(error);

        std::istream headers(&buffer);
        std::string version;
        HttpResponse response;
        headers >> version >> response.status_code;
        std::string header;
        std::getline(headers, header); // Remainder of status line.
        std::optional<size_t> content_length;
        bool transfer_encoded = false;
        while (std::getline(headers, header) && header != "\r") {
            const auto colon = header.find(':');
            if (colon == std::string::npos) continue;
            auto name = header.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
            });
            if (name == "transfer-encoding") transfer_encoded = true;
            if (name != "content-length") continue;
            const auto first = header.find_first_not_of(" \t", colon + 1);
            const auto last = header.find_last_not_of(" \t\r");
            if (first == std::string::npos || first > last) {
                throw std::system_error(std::make_error_code(std::errc::protocol_error));
            }
            uint64_t length = 0;
            const char* begin = header.data() + first;
            const char* end = header.data() + last + 1;
            const auto parsed = std::from_chars(begin, end, length);
            if (parsed.ec == std::errc::result_out_of_range || length > HttpClient::kMaxBodyBytes) {
                throw std::system_error(std::make_error_code(std::errc::message_size));
            }
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                (content_length && *content_length != length)) {
                throw std::system_error(std::make_error_code(std::errc::protocol_error));
            }
            content_length = static_cast<size_t>(length);
        }
        // Preserve the existing EOF-delimited handling of transfer-encoded wire
        // bodies; Content-Length must not truncate them. No chunk decoder added.
        if (transfer_encoded) content_length.reset();
        const auto buffered = content_length ? (std::min)(buffer.size(), *content_length) : buffer.size();
        if (buffered > HttpClient::kMaxBodyBytes) {
            throw std::system_error(std::make_error_code(std::errc::message_size));
        }
        response.body.assign(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + buffered);

        std::array<char, 4096> chunk;
        while (!content_length || response.body.size() < *content_length) {
            // For an EOF body allow one bounded probe byte at the exact ceiling,
            // so overflow is rejected before append while an exact fit can end.
            const auto remaining = content_length ? *content_length - response.body.size()
                : HttpClient::kMaxBodyBytes - response.body.size() + 1;
            const auto bytes = co_await stream.async_read_some(
                asio::buffer(chunk.data(), (std::min)(chunk.size(), remaining)),
                asio::redirect_error(asio::use_awaitable, error));
            CheckDeadline();
            if (bytes > HttpClient::kMaxBodyBytes - response.body.size()) {
                throw std::system_error(std::make_error_code(std::errc::message_size));
            }
            response.body.append(chunk.data(), bytes);
            if (error == asio::error::eof && (!content_length || response.body.size() == *content_length)) break;
            if (error) throw std::system_error(error);
        }
        co_return response;
    }

    asio::ip::tcp::resolver resolver_;
    asio::steady_timer timer_;
    asio::steady_timer resolved_;
    asio::ip::tcp::resolver::results_type addresses_;
    std::error_code resolve_error_;
    asio::ip::tcp::socket socket_;
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>> tls_;
    bool expired_ = false;
    bool finished_ = false;
    bool resolve_done_ = false;
};

} // namespace

asio::awaitable<HttpResponse> HttpClient::Get(asio::ssl::context& ssl_ctx,
                                             const std::string& url_str,
                                             const std::string& token,
                                             std::chrono::milliseconds timeout,
                                             CredentialUrlPolicy policy) {
    std::error_code admission_error;
    auto admitted = AdmitCredentialUrl(url_str, CredentialUrlKind::Http, policy, admission_error);
    if (!admitted) throw std::system_error(admission_error);
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::system_error(std::make_error_code(std::errc::timed_out));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto strand = asio::make_strand(co_await asio::this_coro::executor);
    auto operation = std::make_shared<HttpGetOperation>(strand, ssl_ctx, admitted->secure, deadline);
    co_return co_await asio::co_spawn(strand, operation->Run(std::move(*admitted), token), asio::use_awaitable);
}

asio::awaitable<std::vector<std::string>> RegionUrlProvider::FetchRegionUrls(asio::ssl::context& ssl_ctx,
                                                                             const std::string& url_str,
                                                                             const std::string& token,
                                                                             CredentialUrlPolicy policy) {
    std::error_code admission_error;
    auto admitted = AdmitCredentialUrl(
        url_str, CredentialUrlKind::LiveKitBase, policy, admission_error);
    if (!admitted) co_return std::vector<std::string>{};
    const Url url = std::move(*admitted);
    bool is_cloud = url.host.ends_with(".livekit.cloud") || url.host.ends_with(".livekit.run");
    if (!is_cloud) {
        co_return std::vector<std::string>{};
    }
    
    Url regions_endpoint = ConvertCredentialUrl(url, CredentialUrlKind::Http);
    regions_endpoint.path = "/settings/regions";
    regions_endpoint.query.clear();
    const std::string regions_url = FormatCredentialUrl(regions_endpoint);
    
    HttpResponse res;
    try {
        res = co_await HttpClient::Get(
            ssl_ctx, regions_url, token, std::chrono::seconds(3), policy);
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
                    std::error_code candidate_error;
                    auto candidate = AdmitCredentialUrl(
                        r["url"].get<std::string>(),
                        CredentialUrlKind::LiveKitBase,
                        policy,
                        candidate_error);
                    if (candidate) urls.push_back(FormatCredentialUrl(*candidate));
                }
            }
        }
    } catch (...) {
        // parse error
    }
    co_return urls;
}

} // namespace livekit
