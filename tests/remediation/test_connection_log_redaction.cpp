#include "websocket_client.h"
#include "tests/support/test_check.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

class StreamCapture final {
public:
    StreamCapture()
        : cout_buffer_(std::cout.rdbuf(output_.rdbuf())),
          cerr_buffer_(std::cerr.rdbuf(output_.rdbuf())) {}

    ~StreamCapture() {
        std::cout.rdbuf(cout_buffer_);
        std::cerr.rdbuf(cerr_buffer_);
    }

    std::string str() const { return output_.str(); }

private:
    std::ostringstream output_;
    std::streambuf* cout_buffer_;
    std::streambuf* cerr_buffer_;
};

struct FailureCaseResult {
    std::error_code error;
    std::string request;
    std::string output;
};

FailureCaseResult RunFailureCase(bool token_in_query) {
    constexpr const char* kQuerySecret = "synthetic-query-secret";
    constexpr const char* kHeaderSecret = "synthetic-header-secret";
    constexpr const char* kResponseSecret = "synthetic-response-secret";

    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(
        server_io,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    std::string captured_request;
    std::exception_ptr server_error;

    std::thread server([&]() {
        try {
            asio::ip::tcp::socket socket(server_io);
            acceptor.accept(socket);
            asio::streambuf request_buffer;
            asio::read_until(socket, request_buffer, "\r\n\r\n");
            captured_request.assign(
                asio::buffers_begin(request_buffer.data()),
                asio::buffers_end(request_buffer.data()));

            const std::string body = std::string("server body ") + kResponseSecret;
            const std::string response =
                "HTTP/1.1 401 " + std::string(kResponseSecret) + "\r\n" +
                "Location: https://redirect.invalid/rtc?access_token=" + kResponseSecret + "\r\n" +
                "Set-Cookie: session=" + kResponseSecret + "\r\n" +
                "X-Opaque: " + kResponseSecret + "\r\n" +
                "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                "Connection: close\r\n\r\n" + body;
            asio::write(socket, asio::buffer(response));
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    asio::io_context client_io;
    asio::ssl::context ssl(asio::ssl::context::tls_client);
    auto client = std::make_shared<livekit::WebSocketClient>(client_io, ssl);
    const std::string token = token_in_query ? kQuerySecret : kHeaderSecret;
    const std::string query = token_in_query
        ? std::string("?access_token=") + token + "&join_request=synthetic-join-payload"
        : "?protocol=14";
    const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/rtc/v1" + query;

    std::future<std::error_code> completion;
    std::string output;
    {
        StreamCapture capture;
        completion = asio::co_spawn(
            client_io,
            [client, url, token]() -> asio::awaitable<std::error_code> {
                co_return co_await client->Connect(url, token, std::chrono::seconds(3));
            },
            asio::use_future);
        client_io.run();
        output = capture.str();
    }
    const auto error = completion.get();
    server.join();
    if (server_error) std::rethrow_exception(server_error);

    TEST_CHECK(captured_request.find(token) != std::string::npos);
    if (token_in_query) {
        TEST_CHECK(captured_request.find("access_token=") != std::string::npos);
    } else {
        TEST_CHECK(captured_request.find("Authorization: Bearer ") != std::string::npos);
    }
    TEST_CHECK(livekit::WebSocketHttpStatus(error).has_value());
    TEST_CHECK(*livekit::WebSocketHttpStatus(error) == 401);
    TEST_CHECK(output.find(kQuerySecret) == std::string::npos);
    TEST_CHECK(output.find(kHeaderSecret) == std::string::npos);
    TEST_CHECK(output.find(kResponseSecret) == std::string::npos);
    TEST_CHECK(output.find("GET /rtc") == std::string::npos);
    TEST_CHECK(output.find("Authorization: Bearer") == std::string::npos);
    TEST_CHECK(output.find("Set-Cookie:") == std::string::npos);
    TEST_CHECK(output.find("stage=websocket_upgrade") != std::string::npos);
    TEST_CHECK(output.find("category=websocket_http") != std::string::npos);
    TEST_CHECK(output.find("code=401") != std::string::npos);
    TEST_CHECK(output.find("endpoint{scheme=ws,route=rtc_v1") != std::string::npos);

    return {error, captured_request, output};
}

} // namespace

int main() {
    const auto query_case = RunFailureCase(true);
    const auto header_case = RunFailureCase(false);
    TEST_CHECK(query_case.error == header_case.error);
    return 0;
}
