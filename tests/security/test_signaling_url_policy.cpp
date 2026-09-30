#include "credential_url.h"
#include "region_provider.h"
#include "signal_client.h"
#include "websocket_client.h"
#include "tests/support/test_check.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <array>

namespace {

using livekit::CredentialUrlKind;
using livekit::CredentialUrlPolicy;

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name) {
        if (const char* current = std::getenv(name)) original_ = current;
        Set(value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        Set(original_ ? original_->c_str() : nullptr);
    }

private:
    void Set(const char* value) noexcept {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) {
            setenv(name_.c_str(), value, 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> original_;
};

std::optional<livekit::Url> Admit(
    const std::string& url,
    CredentialUrlKind kind,
    CredentialUrlPolicy policy,
    std::error_code& error) {
    return livekit::AdmitCredentialUrl(url, kind, policy, error);
}

void CheckParserAndPolicy() {
    const CredentialUrlPolicy strict{};
    const CredentialUrlPolicy debug{true, false};
    const CredentialUrlPolicy secure_derived{true, true};
    std::error_code error;

    auto mixed_case = Admit(
        "WSS://Example.COM:443/base?region=one",
        CredentialUrlKind::LiveKitBase, strict, error);
    TEST_CHECK(mixed_case);
    TEST_CHECK(!error);
    TEST_CHECK(mixed_case->scheme == "wss");
    TEST_CHECK(mixed_case->host == "example.com");
    TEST_CHECK(livekit::FormatCredentialUrl(*mixed_case) ==
               "wss://example.com/base?region=one");

    auto https = Admit(
        "HTTPS://secure.example.test:8443/prefix",
        CredentialUrlKind::LiveKitBase, strict, error);
    TEST_CHECK(https && https->secure);
    const auto websocket = livekit::ConvertCredentialUrl(
        *https, CredentialUrlKind::WebSocket);
    TEST_CHECK(livekit::FormatCredentialUrl(websocket) ==
               "wss://secure.example.test:8443/prefix");
    const auto http = livekit::ConvertCredentialUrl(
        websocket, CredentialUrlKind::Http);
    TEST_CHECK(livekit::FormatCredentialUrl(http) ==
               "https://secure.example.test:8443/prefix");

    auto ws_default = Admit(
        "ws://example.test:80/rtc", CredentialUrlKind::WebSocket, debug, error);
    TEST_CHECK(ws_default && !error);
    TEST_CHECK(livekit::FormatUrlAuthority(*ws_default) == "example.test");
    auto ws_explicit_443 = Admit(
        "ws://example.test:443/rtc", CredentialUrlKind::WebSocket, debug, error);
    TEST_CHECK(ws_explicit_443 && !error);
    TEST_CHECK(livekit::FormatUrlAuthority(*ws_explicit_443) ==
               "example.test:443");
    auto wss_default = Admit(
        "wss://example.test:443/rtc", CredentialUrlKind::WebSocket, strict, error);
    TEST_CHECK(wss_default && !error);
    TEST_CHECK(livekit::FormatUrlAuthority(*wss_default) == "example.test");
    auto wss_explicit_80 = Admit(
        "wss://example.test:80/rtc", CredentialUrlKind::WebSocket, strict, error);
    TEST_CHECK(wss_explicit_80 && !error);
    TEST_CHECK(livekit::FormatUrlAuthority(*wss_explicit_80) ==
               "example.test:80");

    auto cross_origin_wss = Admit(
        "wss://region-two.example.test/edge",
        CredentialUrlKind::LiveKitBase, secure_derived, error);
    TEST_CHECK(cross_origin_wss && !error);

    auto secure_region_hop = Admit(
        "WSS://region.example.test/edge",
        CredentialUrlKind::LiveKitBase, debug, error);
    TEST_CHECK(secure_region_hop && !error);
    const auto region_derived_policy =
        livekit::PolicyForDerivedCredentialUrl(debug, *secure_region_hop);
    TEST_CHECK(region_derived_policy.require_secure);
    TEST_CHECK(!Admit(
        "http://redirect.example.test/edge",
        CredentialUrlKind::LiveKitBase, region_derived_policy, error));
    TEST_CHECK(error == std::errc::permission_denied);

    TEST_CHECK(!Admit(
        "ws://127.0.0.1:7880", CredentialUrlKind::LiveKitBase, strict, error));
    TEST_CHECK(error == std::errc::permission_denied);
    TEST_CHECK(Admit(
        "WS://127.0.0.1:7880", CredentialUrlKind::LiveKitBase, debug, error));
    TEST_CHECK(!error);

    // A secure initial hop must never be downgraded by alternative_url, region,
    // validate, or reconnect, even when application debug mode is enabled.
    for (const auto* derived : {
             "ws://redirect.example.test/edge",
             "http://region.example.test/edge",
             "ws://validate.example.test/edge",
             "http://reconnect.example.test/edge"}) {
        TEST_CHECK(!Admit(
            derived, CredentialUrlKind::LiveKitBase, secure_derived, error));
        TEST_CHECK(error == std::errc::permission_denied);
    }

    TEST_CHECK(!Admit(
        "ftp://example.test/room", CredentialUrlKind::LiveKitBase, debug, error));
    TEST_CHECK(error == std::errc::protocol_not_supported);
    TEST_CHECK(!Admit(
        "wss://user:secret@example.test/room",
        CredentialUrlKind::LiveKitBase, strict, error));
    TEST_CHECK(error == std::errc::invalid_argument);
    TEST_CHECK(!Admit(
        "wss://example.test/room\r\nInjected: value",
        CredentialUrlKind::LiveKitBase, strict, error));
    TEST_CHECK(error == std::errc::invalid_argument);
    TEST_CHECK(!Admit(
        "wss://example.test:0/room", CredentialUrlKind::LiveKitBase, strict, error));
    TEST_CHECK(error == std::errc::invalid_argument);

    auto ipv6 = Admit(
        "WSS://[::1]:8443/room", CredentialUrlKind::LiveKitBase, strict, error);
    TEST_CHECK(ipv6);
    TEST_CHECK(livekit::FormatCredentialUrl(*ipv6) == "wss://[::1]:8443/room");
}

void CheckRejectedCredentialsDoNotReachSocket() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(
        io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto endpoint = "127.0.0.1:" +
        std::to_string(acceptor.local_endpoint().port());
    asio::ssl::context tls(asio::ssl::context::tls_client);

    auto websocket = std::make_shared<livekit::WebSocketClient>(io, tls);
    auto ws_result = asio::co_spawn(
        io,
        websocket->Connect(
            "ws://" + endpoint + "/rtc", "sentinel-websocket-token",
            std::chrono::seconds(1), CredentialUrlPolicy{}),
        asio::use_future);
    io.run();
    TEST_CHECK(ws_result.get() == std::errc::permission_denied);

    io.restart();
    livekit::SignalOptions strict_options;
    const std::string rejected_signal_url = "ws://" + endpoint;
    const std::string rejected_signal_token = "sentinel-signal-token";
    const std::optional<std::vector<uint8_t>> no_offer;
    auto signal_result = asio::co_spawn(
        io,
        livekit::SignalClient::Connect(
            rejected_signal_url, rejected_signal_token, strict_options,
            no_offer, [](const livekit::SignalEvent&) {}),
        asio::use_future);
    io.run();
    const auto rejected_signal = signal_result.get();
    TEST_CHECK(rejected_signal.error == std::errc::permission_denied);

    io.restart();
    const std::string rejected_http_url = "http://" + endpoint + "/validate";
    const std::string rejected_http_token = "sentinel-http-token";
    auto http_result = asio::co_spawn(
        io,
        livekit::HttpClient::Get(
            tls, rejected_http_url, rejected_http_token,
            std::chrono::seconds(1), CredentialUrlPolicy{}),
        asio::use_future);
    io.run();
    bool http_rejected = false;
    try {
        static_cast<void>(http_result.get());
    } catch (const std::system_error& exception) {
        http_rejected = exception.code() == std::errc::permission_denied;
    }
    TEST_CHECK(http_rejected);

    acceptor.non_blocking(true);
    asio::ip::tcp::socket accepted(io);
    std::error_code accept_error;
    acceptor.accept(accepted, accept_error);
    TEST_CHECK(accept_error == asio::error::would_block);
}

void CheckExplicitNonDefaultPortInWebSocketHostHeader() {
    asio::io_context proxy_io;
    asio::ip::tcp::acceptor acceptor(
        proxy_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto proxy_url = "http://127.0.0.1:" +
        std::to_string(acceptor.local_endpoint().port());

    std::promise<std::string> captured_request;
    auto captured_future = captured_request.get_future();
    std::thread proxy_thread([&] {
        try {
            asio::ip::tcp::socket socket(proxy_io);
            acceptor.accept(socket);

            asio::streambuf connect_buffer;
            asio::read_until(socket, connect_buffer, "\r\n\r\n");
            const std::string established =
                "HTTP/1.1 200 Connection Established\r\n\r\n";
            asio::write(socket, asio::buffer(established));

            asio::streambuf upgrade_buffer;
            asio::read_until(socket, upgrade_buffer, "\r\n\r\n");
            captured_request.set_value(std::string(
                asio::buffers_begin(upgrade_buffer.data()),
                asio::buffers_end(upgrade_buffer.data())));
        } catch (...) {
            captured_request.set_exception(std::current_exception());
        }
    });

    {
        ScopedEnvironmentVariable proxy("HTTP_PROXY", proxy_url);
        asio::io_context io;
        asio::ssl::context tls(asio::ssl::context::tls_client);
        auto websocket = std::make_shared<livekit::WebSocketClient>(io, tls);
        auto result = asio::co_spawn(
            io,
            websocket->Connect(
                "ws://example.test:443/rtc", "sentinel-port-token",
                std::chrono::seconds(2), CredentialUrlPolicy{true, false}),
            asio::use_future);
        io.run();
        static_cast<void>(result.get());
    }
    proxy_thread.join();
    const std::string request = captured_future.get();

    TEST_CHECK(request.find("\r\nHost: example.test:443\r\n") != std::string::npos);
    TEST_CHECK(request.find(
        "\r\nAuthorization: Bearer sentinel-port-token\r\n") != std::string::npos);
}

struct HttpWrite {
    std::chrono::milliseconds delay;
    std::string bytes;
};

void CheckHttpExchange(const char* label, std::vector<HttpWrite> writes,
                       bool eof, std::error_code expected_error,
                       const std::string& expected_body = {},
                       std::chrono::milliseconds timeout = std::chrono::seconds(2),
                       bool tls_stall = false, bool cancel = false, int expected_status = 200) {
    asio::io_context io;
    // Exercise an unstranded caller on two workers. Fixture mutations have their
    // own strand; the production HTTP operation must serialize its own deadline.
    auto fixture = asio::make_strand(io);
    asio::ip::tcp::acceptor acceptor(fixture, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(fixture);
    asio::steady_timer step(fixture);
    asio::steady_timer watchdog(fixture, std::chrono::seconds(5));
    asio::steady_timer cancellation_timer(fixture);
    asio::cancellation_signal cancellation;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    const std::string url = std::string(tls_stall ? "https://" : "http://") + "127.0.0.1:" +
        std::to_string(acceptor.local_endpoint().port()) + "/validate";
    const std::string token = "synthetic-http-budget-token";
    bool watchdog_fired = false;
    bool client_done = false;
    bool server_done = false;
    bool peer_saw_eof = false;
    std::string request;
    std::error_code actual_error;
    livekit::HttpResponse response;
    std::exception_ptr unexpected;
    std::chrono::steady_clock::duration elapsed{};
    auto finish = [&] {
        if (client_done && server_done) watchdog.cancel();
    };
    watchdog.async_wait([&](std::error_code error) {
        if (error) return;
        watchdog_fired = true;
        std::error_code ignored;
        acceptor.close(ignored);
        peer.close(ignored);
        step.cancel();
        cancellation.emit(asio::cancellation_type::terminal);
    });
    asio::co_spawn(fixture, [&]() -> asio::awaitable<void> {
        co_await acceptor.async_accept(peer, asio::use_awaitable);
        if (!tls_stall) {
            asio::streambuf buffer(4096);
            co_await asio::async_read_until(peer, buffer, "\r\n\r\n", asio::use_awaitable);
            request.assign(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
            for (const auto& write : writes) {
                if (write.delay.count()) {
                    step.expires_after(write.delay);
                    co_await step.async_wait(asio::use_awaitable);
                }
                std::error_code error;
                co_await asio::async_write(peer, asio::buffer(write.bytes),
                                           asio::redirect_error(asio::use_awaitable, error));
                if (error) co_return;
            }
        }
        if (eof) {
            std::error_code ignored;
            peer.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
        // Hold the peer open. Success with Content-Length, cancellation, size
        // rejection and TLS/header/body deadlines must all close the transport.
        std::array<char, 4096> probe;
        std::error_code error;
        do {
            co_await peer.async_read_some(asio::buffer(probe),
                                          asio::redirect_error(asio::use_awaitable, error));
        } while (!error);
        peer_saw_eof = error == asio::error::eof || error == asio::error::connection_reset;
    }, [&](std::exception_ptr error) {
        if (error && !cancel && !watchdog_fired) unexpected = error;
        server_done = true;
        finish();
    });
    if (cancel) {
        cancellation_timer.expires_after(std::chrono::milliseconds(100));
        cancellation_timer.async_wait([&](std::error_code error) {
            if (!error) cancellation.emit(asio::cancellation_type::terminal);
        });
    }
    const auto started = std::chrono::steady_clock::now();
    asio::co_spawn(io, livekit::HttpClient::Get(tls, url, token, timeout, CredentialUrlPolicy{true, false}),
        asio::bind_cancellation_slot(cancellation.slot(), asio::bind_executor(fixture,
            [&](std::exception_ptr error, livekit::HttpResponse value) {
                elapsed = std::chrono::steady_clock::now() - started;
                response = std::move(value);
                if (error) {
                    try { std::rethrow_exception(error); }
                    catch (const std::system_error& e) { actual_error = e.code(); }
                    catch (...) { unexpected = error; }
                }
                client_done = true;
                cancellation_timer.cancel();
                finish();
            })));
    std::thread worker([&] { io.run(); });
    io.run();
    worker.join();
    if (unexpected) std::rethrow_exception(unexpected);
    if (actual_error != expected_error) {
        std::cerr << label << ": expected " << expected_error << ", got " << actual_error << '\n';
    }
    TEST_CHECK(!watchdog_fired && client_done && server_done);
    TEST_CHECK(actual_error == expected_error);
    TEST_CHECK(elapsed < timeout + std::chrono::seconds(1));
    if (expected_error == std::errc::timed_out) {
        TEST_CHECK(elapsed >= timeout - std::chrono::milliseconds(30));
    }
    if (!expected_error) {
        TEST_CHECK(response.status_code == expected_status);
        TEST_CHECK(response.body == expected_body);
        TEST_CHECK(peer_saw_eof);
        TEST_CHECK(request.find("Authorization: Bearer " + token) != std::string::npos);
        TEST_CHECK(std::chrono::steady_clock::now() - started < timeout);
    }
    std::cout << "PASS native HTTP " << label << '\n';
}

void CheckHttpBudgets() {
    using namespace std::chrono_literals;
    const auto too_large = std::make_error_code(std::errc::message_size);
    const auto timed_out = std::make_error_code(std::errc::timed_out);
    const auto protocol_error = std::make_error_code(std::errc::protocol_error);
    const std::string header = "HTTP/1.1 200 OK\r\n\r\n";
    const std::string length_prefix = "HTTP/1.1 200 OK\r\nContent-Length: ";
    const std::string body(livekit::HttpClient::kMaxBodyBytes, 'x');

    CheckHttpExchange("coalesced body / keep-alive", {{0ms, length_prefix + "2\r\n\r\nOK"}}, false, {}, "OK");
    CheckHttpExchange("empty / keep-alive", {{0ms, length_prefix + "0\r\n\r\n"}}, false, {});
    CheckHttpExchange("status and business detail", {{0ms, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 6\r\n\r\ndenied"}},
                      false, {}, "denied", 2s, false, false, 401);
    CheckHttpExchange("EOF JSON", {{0ms, header + "{\"regions\":[]}"}}, true, {}, "{\"regions\":[]}");
    CheckHttpExchange("body exactly at cap / length", {{0ms, length_prefix + std::to_string(body.size()) + "\r\n\r\n" + body}}, false, {}, body);
    CheckHttpExchange("body exactly at cap / EOF", {{0ms, header + body}}, true, {}, body);
    CheckHttpExchange("body cap plus one / EOF", {{0ms, header + body + 'x'}}, false, too_large);
    for (const auto* length : {"1048577", "1073741824", "18446744073709551615", "18446744073709551616"}) {
        CheckHttpExchange("declared oversized / header only", {{0ms, length_prefix + length + "\r\n\r\n"}}, false, too_large);
    }
    const std::string header_prefix = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Padding: ";
    const std::string exact_header = header_prefix +
        std::string(livekit::HttpClient::kMaxHeaderBytes - header_prefix.size() - 4, 'x') + "\r\n\r\n";
    CheckHttpExchange("header exactly at cap", {{0ms, exact_header}}, false, {});
    CheckHttpExchange("header cap plus one", {{0ms, exact_header.substr(0, exact_header.size() - 4) + "x\r\n\r\n"}}, false, too_large);
    CheckHttpExchange("unterminated oversized header", {{0ms, std::string(livekit::HttpClient::kMaxHeaderBytes + 1, 'x')}}, false, too_large);
    CheckHttpExchange("stalled header", {}, false, timed_out, {}, 150ms);
    CheckHttpExchange("stalled EOF body", {{0ms, header}}, false, timed_out, {}, 150ms);
    CheckHttpExchange("stalled declared body", {{0ms, length_prefix + "2\r\n\r\nO"}}, false, timed_out, {}, 150ms);
    CheckHttpExchange("one deadline across header and body", {{100ms, length_prefix + "2\r\n\r\nO"}, {250ms, "K"}}, false, timed_out, {}, 250ms);
    CheckHttpExchange("slow header cannot renew deadline", {{100ms, "HTTP/1.1 "}, {100ms, "200 OK\r\n"}, {150ms, "\r\n"}}, false, timed_out, {}, 250ms);
    CheckHttpExchange("TLS handshake stall", {}, false, timed_out, {}, 150ms, true);
    CheckHttpExchange("external cancellation", {}, false, asio::error::operation_aborted, {}, 2s, false, true);
    CheckHttpExchange("truncated declared body", {{0ms, length_prefix + "2\r\n\r\nO"}}, true, asio::error::eof);
    CheckHttpExchange("malformed content length", {{0ms, length_prefix + "oops\r\n\r\n"}}, false, protocol_error);
    CheckHttpExchange("conflicting content lengths", {{0ms, length_prefix + "1\r\nContent-Length: 2\r\n\r\n"}}, false, protocol_error);
    // Keep existing transfer-encoded wire-body semantics, but enforce the budget.
    const std::string chunked = "2\r\nOK\r\n0\r\n\r\n";
    CheckHttpExchange("transfer encoding retains EOF behavior", {{0ms, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked}}, true, {}, chunked);

    // Already-expired budgets fail before starting a socket operation.
    asio::io_context io;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    const std::string url = "http://127.0.0.1:1/validate";
    const std::string token;
    for (const auto timeout : {0ms, -1ms}) {
        io.restart();
        auto result = asio::co_spawn(io, livekit::HttpClient::Get(tls, url, token, timeout, CredentialUrlPolicy{true, false}), asio::use_future);
        io.run();
        bool rejected = false;
        try { static_cast<void>(result.get()); }
        catch (const std::system_error& error) { rejected = error.code() == timed_out; }
        TEST_CHECK(rejected);
    }
    std::cout << "PR_SEC_008_HTTP_BUDGET_EXECUTED=1 PASSED=1 FAILED=0\n";
}

} // namespace

int main() {
    CheckParserAndPolicy();
    CheckRejectedCredentialsDoNotReachSocket();
    CheckExplicitNonDefaultPortInWebSocketHostHeader();
    CheckHttpBudgets();
    std::cout << "PR_SEC_006_SIGNALING_URL_POLICY_EXECUTED=1 PASSED=1 FAILED=0\n";
    return 0;
}
