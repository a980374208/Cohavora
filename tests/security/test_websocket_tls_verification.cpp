#include "websocket_client.h"
#include "region_provider.h"
#include "tests/support/test_check.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>

namespace {

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

Key MakeKey() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    TEST_CHECK(context);
    TEST_CHECK(EVP_PKEY_keygen_init(context.get()) == 1);
    TEST_CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
    EVP_PKEY* key = nullptr;
    TEST_CHECK(EVP_PKEY_keygen(context.get(), &key) == 1);
    return Key(key, EVP_PKEY_free);
}

void AddExtension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX context{};
    X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_nconf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
    TEST_CHECK(extension);
    TEST_CHECK(X509_add_ext(cert, extension.get(), -1) == 1);
}

Certificate MakeCertificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key,
                            const char* san, bool expired = false) {
    static long serial = 1;
    Certificate cert(X509_new(), X509_free);
    TEST_CHECK(cert);
    TEST_CHECK(X509_set_version(cert.get(), 2) == 1);
    TEST_CHECK(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial++) == 1);
    // Relative validity keeps the fixture independent of a checked-in expiry date.
    TEST_CHECK(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -172800));
    TEST_CHECK(X509_gmtime_adj(X509_getm_notAfter(cert.get()), expired ? -86400 : 86400));
    TEST_CHECK(X509_set_pubkey(cert.get(), key) == 1);
    auto* subject = X509_get_subject_name(cert.get());
    const auto* name = reinterpret_cast<const unsigned char*>("PR-SEC-003 ephemeral test");
    TEST_CHECK(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, name, -1, -1, 0) == 1);
    TEST_CHECK(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : subject) == 1);
    AddExtension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
                 issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
    AddExtension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
                 issuer ? "critical,digitalSignature,keyEncipherment" : "critical,keyCertSign,cRLSign");
    AddExtension(cert.get(), issuer ? issuer : cert.get(), NID_subject_key_identifier, "hash");
    if (issuer) {
        AddExtension(cert.get(), issuer, NID_authority_key_identifier, "keyid:always");
        AddExtension(cert.get(), issuer, NID_ext_key_usage, "serverAuth");
        AddExtension(cert.get(), issuer, NID_subject_alt_name, san);
    }
    TEST_CHECK(X509_sign(cert.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0);
    return cert;
}

std::string UpgradeResponse(const std::string& request) {
    const std::string key_header = "Sec-WebSocket-Key: ";
    const auto start = request.find(key_header);
    TEST_CHECK(start != std::string::npos);
    const auto value_start = start + key_header.size();
    const auto end = request.find("\r\n", value_start);
    TEST_CHECK(end != std::string::npos);
    const auto input = request.substr(value_start, end - value_start)
        + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    TEST_CHECK(SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest));
    unsigned char encoded[4 * ((SHA_DIGEST_LENGTH + 2) / 3) + 1];
    const auto size = EVP_EncodeBlock(encoded, digest, SHA_DIGEST_LENGTH);
    TEST_CHECK(size > 0);
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + std::string(reinterpret_cast<char*>(encoded), size) + "\r\n\r\n";
}

void RunCase(const char* label, asio::ssl::context& client_tls, X509* cert,
             EVP_PKEY* key, const std::string& host, bool query_token, bool accepted) {
    asio::io_context io;
    asio::ssl::context server_tls(asio::ssl::context::tls_server);
    TEST_CHECK(SSL_CTX_use_certificate(server_tls.native_handle(), cert) == 1);
    TEST_CHECK(SSL_CTX_use_PrivateKey(server_tls.native_handle(), key) == 1);
    TEST_CHECK(SSL_CTX_check_private_key(server_tls.native_handle()) == 1);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ssl::stream<asio::ip::tcp::socket> server(io, server_tls);
    asio::steady_timer deadline(io, std::chrono::seconds(5));
    bool timed_out = false;
    deadline.async_wait([&](const std::error_code& error) {
        if (error) return;
        timed_out = true;
        std::error_code ignored;
        acceptor.close(ignored);
        server.next_layer().close(ignored);
    });

    std::string request;
    std::string sni;
    std::exception_ptr server_exception;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await acceptor.async_accept(server.next_layer(), asio::use_awaitable);
        std::error_code error;
        co_await server.async_handshake(asio::ssl::stream_base::server,
                                       asio::redirect_error(asio::use_awaitable, error));
        if (!error) {
            if (const auto* name = SSL_get_servername(server.native_handle(), TLSEXT_NAMETYPE_host_name)) {
                sni = name;
            }
            asio::streambuf buffer;
            co_await asio::async_read_until(server, buffer, "\r\n\r\n",
                                           asio::redirect_error(asio::use_awaitable, error));
            // Also retain partial application data on failure: no token bytes may escape.
            request.assign(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
            if (!error) {
                const auto response = UpgradeResponse(request);
                co_await asio::async_write(server, asio::buffer(response), asio::use_awaitable);
            }
        }
        std::error_code ignored;
        server.next_layer().close(ignored);
    }, [&](std::exception_ptr error) {
        server_exception = error;
        deadline.cancel();
    });

    auto client = std::make_shared<livekit::WebSocketClient>(io, client_tls);
    const std::string token = "synthetic-tls-bearer-secret";
    const auto port = acceptor.local_endpoint().port();
    TEST_CHECK(port != 443);
    const auto url = "wss://" + host + ":" + std::to_string(port)
        + "/rtc/v1" + (query_token ? "?access_token=" + token : "?protocol=14");
    auto completion = asio::co_spawn(io, client->Connect(url, token, std::chrono::seconds(3)),
                                     asio::use_future);
    io.run();
    const auto error = completion.get();
    if (server_exception) std::rethrow_exception(server_exception);
    TEST_CHECK(!timed_out);
    TEST_CHECK(client->IsConnected() == accepted);
    if (accepted) {
        TEST_CHECK(!error);
        TEST_CHECK(sni == host);
        TEST_CHECK(request.find(
            "\r\nHost: " + host + ":" + std::to_string(port) + "\r\n") !=
            std::string::npos);
        TEST_CHECK(request.find(token) != std::string::npos);
        if (query_token) {
            TEST_CHECK(request.find("GET /rtc/v1?access_token=" + token) != std::string::npos);
            TEST_CHECK(request.find("Authorization: Bearer") == std::string::npos);
        } else {
            TEST_CHECK(request.find("Authorization: Bearer " + token) != std::string::npos);
        }
    } else {
        TEST_CHECK(error.category() == asio::error::get_ssl_category());
        TEST_CHECK(ERR_GET_REASON(error.value()) == SSL_R_CERTIFICATE_VERIFY_FAILED);
        TEST_CHECK(!livekit::WebSocketHttpStatus(error).has_value());
        TEST_CHECK(!livekit::IsWebSocketHttpStatus(error, 404));
        TEST_CHECK(request.empty());
    }
    std::cout << "PASS " << label << (query_token ? " query" : " header") << std::endl;
}

void RunHttpCase(const char* label, asio::ssl::context& client_tls, X509* cert,
                 EVP_PKEY* key, const std::string& wire, bool trusted,
                 std::error_code expected_error = {},
                 std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    asio::io_context io;
    asio::ssl::context server_tls(asio::ssl::context::tls_server);
    TEST_CHECK(SSL_CTX_use_certificate(server_tls.native_handle(), cert) == 1);
    TEST_CHECK(SSL_CTX_use_PrivateKey(server_tls.native_handle(), key) == 1);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ssl::stream<asio::ip::tcp::socket> peer(io, server_tls);
    asio::steady_timer watchdog(io, std::chrono::seconds(5));
    bool timed_out = false;
    bool client_done = false;
    bool server_done = false;
    std::string request;
    watchdog.async_wait([&](std::error_code error) {
        if (error) return;
        timed_out = true;
        std::error_code ignored;
        acceptor.close(ignored);
        peer.next_layer().close(ignored);
    });
    auto finish = [&] { if (client_done && server_done) watchdog.cancel(); };
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await acceptor.async_accept(peer.next_layer(), asio::use_awaitable);
        std::error_code error;
        co_await peer.async_handshake(asio::ssl::stream_base::server,
                                     asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return;
        asio::streambuf buffer(4096);
        co_await asio::async_read_until(peer, buffer, "\r\n\r\n", asio::use_awaitable);
        request.assign(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
        if (!wire.empty()) {
            co_await asio::async_write(peer, asio::buffer(wire), asio::redirect_error(asio::use_awaitable, error));
        }
        char probe;
        co_await peer.async_read_some(asio::buffer(&probe, 1), asio::redirect_error(asio::use_awaitable, error));
    }, [&](std::exception_ptr error) {
        if (error) std::rethrow_exception(error);
        server_done = true;
        finish();
    });
    const std::string url = "https://localhost:" + std::to_string(acceptor.local_endpoint().port()) + "/validate";
    const std::string token = "synthetic-http-tls-token";
    std::error_code actual_error;
    livekit::HttpResponse response;
    asio::co_spawn(io, livekit::HttpClient::Get(client_tls, url, token, timeout),
        [&](std::exception_ptr error, livekit::HttpResponse value) {
            response = std::move(value);
            if (error) {
                try { std::rethrow_exception(error); }
                catch (const std::system_error& e) { actual_error = e.code(); }
            }
            client_done = true;
            finish();
        });
    io.run();
    TEST_CHECK(!timed_out && client_done && server_done);
    if (trusted) {
        TEST_CHECK(actual_error == expected_error);
        TEST_CHECK(request.find("Authorization: Bearer " + token) != std::string::npos);
        if (!expected_error) TEST_CHECK(response.status_code == 200 && response.body == "OK");
    } else {
        TEST_CHECK(actual_error.category() == asio::error::get_ssl_category());
        TEST_CHECK(ERR_GET_REASON(actual_error.value()) == SSL_R_CERTIFICATE_VERIFY_FAILED);
        TEST_CHECK(request.empty());
    }
    std::cout << "PASS native HTTPS " << label << std::endl;
}

void ClearProxyEnvironment() {
    // This process only uses loopback peers; do not route fixtures through a user's proxy.
    for (const auto* name : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"}) {
#ifdef _WIN32
        TEST_CHECK(_putenv_s(name, "") == 0);
#else
        TEST_CHECK(unsetenv(name) == 0);
#endif
    }
}

} // namespace

int main() {
    ClearProxyEnvironment();
    const auto root_key = MakeKey();
    const auto root = MakeCertificate(root_key.get(), nullptr, nullptr, nullptr);
    const auto server_key = MakeKey();
    const auto dns = MakeCertificate(server_key.get(), root.get(), root_key.get(), "DNS:localhost");
    const auto ip = MakeCertificate(server_key.get(), root.get(), root_key.get(), "IP:127.0.0.1");
    const auto wrong_dns = MakeCertificate(server_key.get(), root.get(), root_key.get(), "DNS:wrong.invalid");
    const auto wrong_ip = MakeCertificate(server_key.get(), root.get(), root_key.get(), "IP:127.0.0.2");
    const auto expired = MakeCertificate(server_key.get(), root.get(), root_key.get(), "DNS:localhost", true);

    // The ephemeral CA is trusted only by this context, never installed in the OS.
    asio::ssl::context trusted(asio::ssl::context::tls_client);
    TEST_CHECK(X509_STORE_add_cert(SSL_CTX_get_cert_store(trusted.native_handle()), root.get()) == 1);
    asio::ssl::context untrusted(asio::ssl::context::tls_client);
    // Explicitly permissive caller contexts must not disable the public client's contract.
    trusted.set_verify_mode(asio::ssl::verify_none);
    untrusted.set_verify_mode(asio::ssl::verify_none);

    for (const bool query_token : {false, true}) {
        // Reuse a context across new streams, as reconnect does. A prior valid peer
        // must not make a later wrong-host peer valid; failures must permit retry.
        RunCase("trusted DNS", trusted, dns.get(), server_key.get(), "localhost", query_token, true);
        RunCase("wrong DNS", trusted, wrong_dns.get(), server_key.get(), "localhost", query_token, false);
        RunCase("untrusted CA", untrusted, dns.get(), server_key.get(), "localhost", query_token, false);
        RunCase("expired leaf", trusted, expired.get(), server_key.get(), "localhost", query_token, false);
        RunCase("wrong IP", trusted, wrong_ip.get(), server_key.get(), "127.0.0.1", query_token, false);
        RunCase("trusted IP", trusted, ip.get(), server_key.get(), "127.0.0.1", query_token, true);
    }
    const std::string http_ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    RunHttpCase("trusted", trusted, dns.get(), server_key.get(), http_ok, true);
    RunHttpCase("wrong host", trusted, wrong_dns.get(), server_key.get(), http_ok, false);
    RunHttpCase("untrusted CA", untrusted, dns.get(), server_key.get(), http_ok, false);
    RunHttpCase("oversized declaration", trusted, dns.get(), server_key.get(),
                "HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\n\r\n", true,
                std::make_error_code(std::errc::message_size));
    RunHttpCase("body deadline", trusted, dns.get(), server_key.get(),
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nO", true,
                std::make_error_code(std::errc::timed_out), std::chrono::milliseconds(300));
    std::cout << "PR_SEC_008_HTTPS_CASES_EXECUTED=5 PASSED=5 FAILED=0\n";
    return 0;
}
