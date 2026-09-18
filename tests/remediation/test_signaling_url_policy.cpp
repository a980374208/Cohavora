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

} // namespace

int main() {
    CheckParserAndPolicy();
    CheckRejectedCredentialsDoNotReachSocket();
    CheckExplicitNonDefaultPortInWebSocketHostHeader();
    std::cout << "PR_SEC_006_SIGNALING_URL_POLICY_EXECUTED=1 PASSED=1 FAILED=0\n";
    return 0;
}
