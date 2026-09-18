#include "src/net/credential_store.h"
#include "src/net/openmeeting_http_client.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QHash>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <functional>
#include <cstdio>
#include <memory>
#include <utility>

using namespace OpenMeeting;

namespace OpenMeeting {
class SessionManagerTestAccess {
public:
    using Owner = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;

    static Owner create(
            std::unique_ptr<QSettings> settings, OpenMeetingHttpClient &client) {
        return Owner(new SessionManager(std::move(settings), nullptr, &client), &destroy);
    }

private:
    static void destroy(SessionManager *session) { delete session; }
};

class OpenMeetingHttpClientTestAccess {
public:
    static std::unique_ptr<OpenMeetingHttpClient> create(
            std::unique_ptr<QNetworkAccessManager> networkManager) {
        return std::unique_ptr<OpenMeetingHttpClient>(
            new OpenMeetingHttpClient(std::move(networkManager), nullptr));
    }
};
} // namespace OpenMeeting

namespace {

void waitFor(const std::function<bool()> &done) {
    for (int i = 0; i != 500 && !done(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    TEST_CHECK(done());
}

std::unique_ptr<QSettings> settingsAt(const QString &path) {
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    settings->setFallbacksEnabled(false);
    return settings;
}

class SingleResponseServer final : public QObject {
public:
    SingleResponseServer(int status, QByteArray body, QByteArray location = {})
        : status_(status), body_(std::move(body)), location_(std::move(location)) {
        TEST_CHECK(server_.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server_.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { receive(socket); });
                receive(socket);
            }
        });
    }

    QString url(const QString &prefix = {}) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(prefix);
    }
    int requests() const { return requests_; }
    QString path() const { return path_; }

private:
    void receive(QTcpSocket *socket) {
        auto &buffer = buffers_[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        int contentLength = 0;
        const auto headers = buffer.left(headerEnd).split('\n');
        for (const auto &header : headers) {
            const int colon = header.indexOf(':');
            if (colon >= 0 && header.left(colon).trimmed().toLower() == "content-length") {
                contentLength = header.mid(colon + 1).trimmed().toInt();
            }
        }
        if (buffer.size() < headerEnd + 4 + contentLength) return;
        const auto requestLine = headers.front().trimmed().split(' ');
        TEST_CHECK(requestLine.size() >= 2);
        path_ = QString::fromUtf8(requestLine[1]);
        ++requests_;
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status_) + " Response\r\n";
        if (!location_.isEmpty()) response += "Location: " + location_ + "\r\n";
        response += "Content-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
        TEST_CHECK(socket->write(response) == response.size());
        socket->disconnectFromHost();
        buffers_.remove(socket);
    }

    QTcpServer server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    int status_ = 200;
    QByteArray body_;
    QByteArray location_;
    int requests_ = 0;
    QString path_;
};

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

Key makeKey() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    TEST_CHECK(context);
    TEST_CHECK(EVP_PKEY_keygen_init(context.get()) == 1);
    TEST_CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
    EVP_PKEY *key = nullptr;
    TEST_CHECK(EVP_PKEY_keygen(context.get(), &key) == 1);
    return Key(key, EVP_PKEY_free);
}

void addExtension(X509 *certificate, X509 *issuer, int nid, const char *value) {
    X509V3_CTX context{};
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_nconf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
    TEST_CHECK(extension);
    TEST_CHECK(X509_add_ext(certificate, extension.get(), -1) == 1);
}

Certificate makeCertificate(EVP_PKEY *key, X509 *issuer, EVP_PKEY *issuerKey,
                            const char *san) {
    static long serial = 1000;
    Certificate certificate(X509_new(), X509_free);
    TEST_CHECK(certificate);
    TEST_CHECK(X509_set_version(certificate.get(), 2) == 1);
    TEST_CHECK(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial++) == 1);
    TEST_CHECK(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600));
    TEST_CHECK(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400));
    TEST_CHECK(X509_set_pubkey(certificate.get(), key) == 1);
    auto *subject = X509_get_subject_name(certificate.get());
    const auto *name = reinterpret_cast<const unsigned char *>("PR-SEC-004 ephemeral test");
    TEST_CHECK(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                          name, -1, -1, 0) == 1);
    TEST_CHECK(X509_set_issuer_name(certificate.get(),
        issuer ? X509_get_subject_name(issuer) : subject) == 1);
    addExtension(certificate.get(), issuer ? issuer : certificate.get(),
                 NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
    addExtension(certificate.get(), issuer ? issuer : certificate.get(),
                 NID_key_usage, issuer ? "critical,digitalSignature,keyEncipherment"
                                       : "critical,keyCertSign,cRLSign");
    addExtension(certificate.get(), issuer ? issuer : certificate.get(),
                 NID_subject_key_identifier, "hash");
    if (issuer) {
        addExtension(certificate.get(), issuer, NID_authority_key_identifier, "keyid:always");
        addExtension(certificate.get(), issuer, NID_ext_key_usage, "serverAuth");
        addExtension(certificate.get(), issuer, NID_subject_alt_name, san);
    }
    TEST_CHECK(X509_sign(certificate.get(), issuerKey ? issuerKey : key, EVP_sha256()) > 0);
    return certificate;
}

QByteArray certificatePem(X509 *certificate) {
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    TEST_CHECK(output && PEM_write_bio_X509(output.get(), certificate) == 1);
    char *data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    TEST_CHECK(size > 0 && data);
    return QByteArray(data, int(size));
}

QByteArray privateKeyPem(EVP_PKEY *key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    TEST_CHECK(output && PEM_write_bio_PrivateKey(
        output.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char *data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    TEST_CHECK(size > 0 && data);
    return QByteArray(data, int(size));
}

class ConfiguredNetworkAccessManager final : public QNetworkAccessManager {
public:
    explicit ConfiguredNetworkAccessManager(QList<QSslCertificate> extraAuthorities)
        : extraAuthorities_(std::move(extraAuthorities)) {
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &original,
                                 QIODevice *outgoingData) override {
        auto request = original;
        auto ssl = request.sslConfiguration();
        auto authorities = ssl.caCertificates();
        authorities.append(extraAuthorities_);
        ssl.setCaCertificates(authorities);
        request.setSslConfiguration(ssl);
        return QNetworkAccessManager::createRequest(operation, request, outgoingData);
    }

private:
    QList<QSslCertificate> extraAuthorities_;
};

class TlsResponseServer final : public QTcpServer {
public:
    TlsResponseServer(const QSslCertificate &certificate, const QSslKey &key)
        : certificate_(certificate), key_(key) {
        TEST_CHECK(!certificate_.isNull() && !key_.isNull());
        TEST_CHECK(listen(QHostAddress::LocalHost, 0));
    }

    QString url() const {
        return QStringLiteral("https://127.0.0.1:%1/api").arg(serverPort());
    }
    const QByteArray &applicationBytes() const { return applicationBytes_; }
    int requests() const { return requests_; }

protected:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        TEST_CHECK(socket->setSocketDescriptor(descriptor));
        socket->setLocalCertificate(certificate_);
        socket->setPrivateKey(key_);
        QObject::connect(socket, &QSslSocket::readyRead, socket, [this, socket] {
            auto &buffer = buffers_[socket];
            const auto bytes = socket->readAll();
            applicationBytes_ += bytes;
            buffer += bytes;
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            int contentLength = 0;
            for (const auto &header : buffer.left(headerEnd).split('\n')) {
                const int colon = header.indexOf(':');
                if (colon >= 0 && header.left(colon).trimmed().toLower() == "content-length") {
                    contentLength = header.mid(colon + 1).trimmed().toInt();
                }
            }
            if (buffer.size() < headerEnd + 4 + contentLength) return;
            ++requests_;
            const QByteArray body = R"({"errCode":0,"errMsg":"","data":{"token":"tls-token","userID":"tls-user"}})";
            const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            TEST_CHECK(socket->write(response) == response.size());
            buffers_.remove(socket);
            socket->disconnectFromHost();
        });
        QObject::connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }

private:
    QSslCertificate certificate_;
    QSslKey key_;
    QHash<QSslSocket *, QByteArray> buffers_;
    QByteArray applicationBytes_;
    int requests_ = 0;
};

void runTlsLoginCase(const QSslCertificate &serverCertificate, const QSslKey &serverKey,
                     const QList<QSslCertificate> &trustedAuthorities, bool expectSuccess) {
    TlsResponseServer server(serverCertificate, serverKey);
    auto manager = std::make_unique<ConfiguredNetworkAccessManager>(trustedAuthorities);
    auto client = OpenMeetingHttpClientTestAccess::create(std::move(manager));
    client->setBaseUrl(server.url());
    int callbacks = 0;
    int loginSignals = 0;
    QObject::connect(client.get(), &OpenMeetingHttpClient::userLoggedIn,
                     client.get(), [&](const UserInfo &) { ++loginSignals; });
    client->login("tls-account", "synthetic-tls-password",
        [&](bool ok, const UserInfo &user, const HttpError &error) {
            TEST_CHECK(ok == expectSuccess);
            if (expectSuccess) {
                TEST_CHECK(error.code == 0 && user.token == "tls-token" && user.userId == "tls-user");
            } else {
                TEST_CHECK(error.code == static_cast<int>(ErrorCode::NetworkError));
                TEST_CHECK(user.token.isEmpty() && user.userId.isEmpty());
            }
            ++callbacks;
        });
    waitFor([&] { return callbacks == 1; });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (expectSuccess) {
        TEST_CHECK(server.requests() == 1 && loginSignals == 1);
        TEST_CHECK(server.applicationBytes().contains("POST /api/user/login HTTP/1.1"));
    } else {
        TEST_CHECK(server.requests() == 0 && server.applicationBytes().isEmpty());
        TEST_CHECK(loginSignals == 0 && client->token().isEmpty());
    }
}

void verifyTlsPeerValidation() {
    const auto rootKey = makeKey();
    const auto root = makeCertificate(rootKey.get(), nullptr, nullptr, nullptr);
    const auto serverKey = makeKey();
    const auto validIp = makeCertificate(serverKey.get(), root.get(), rootKey.get(), "IP:127.0.0.1");
    const auto wrongIp = makeCertificate(serverKey.get(), root.get(), rootKey.get(), "IP:127.0.0.2");
    const QSslCertificate rootCertificate(certificatePem(root.get()), QSsl::Pem);
    const QSslCertificate validCertificate(certificatePem(validIp.get()), QSsl::Pem);
    const QSslCertificate wrongCertificate(certificatePem(wrongIp.get()), QSsl::Pem);
    const QSslKey qtServerKey(privateKeyPem(serverKey.get()), QSsl::Rsa, QSsl::Pem,
                              QSsl::PrivateKey);
    TEST_CHECK(!rootCertificate.isNull() && !validCertificate.isNull() &&
               !wrongCertificate.isNull() && !qtServerKey.isNull());

    runTlsLoginCase(validCertificate, qtServerKey, {rootCertificate}, true);
    runTlsLoginCase(wrongCertificate, qtServerKey, {rootCertificate}, false);
    runTlsLoginCase(validCertificate, qtServerKey, {}, false);
}

void verifyCommonPolicy() {
    TEST_CHECK(QSslSocket::supportsSsl());
    TEST_CHECK(evaluateServiceEndpoint({}).status == ServiceEndpointStatus::Unconfigured);
    TEST_CHECK(evaluateServiceEndpoint("example.invalid").status == ServiceEndpointStatus::InvalidUrl);
    TEST_CHECK(evaluateServiceEndpoint("ftp://example.invalid").status == ServiceEndpointStatus::InvalidUrl);
    TEST_CHECK(evaluateServiceEndpoint("https://user:secret@example.invalid").status ==
               ServiceEndpointStatus::InvalidUrl);
    const auto https = evaluateServiceEndpoint("https://EXAMPLE.invalid:443/api/");
    TEST_CHECK(https.status == ServiceEndpointStatus::HttpsAllowed);
    TEST_CHECK(https.canonicalUrl == "https://example.invalid/api");
    TEST_CHECK(https.requestAllowed() && https.persistenceAllowed());
    for (const auto &url : {
             QStringLiteral("https://example.invalid/api/../admin"),
             QStringLiteral("https://example.invalid/api/./v1"),
             QStringLiteral("https://example.invalid/api/%2e%2e/admin"),
             QStringLiteral("https://example.invalid/api%2fadmin"),
             QStringLiteral("https://example.invalid/api%5cadmin"),
             QStringLiteral("https://example.invalid/api/%252e%252e/admin"),
             QStringLiteral("https://example.invalid/api%252fadmin"),
             QStringLiteral("https://example.invalid/api%255cadmin")}) {
        TEST_CHECK(evaluateServiceEndpoint(url).status == ServiceEndpointStatus::InvalidUrl);
    }
}

#if defined(LIVEKIT_EXPECT_DEBUG_HTTP)
void verifyDevelopmentPolicy() {
    for (const auto &url : {
             QStringLiteral("http://example.invalid/api"),
             QStringLiteral("http://123.56.225.164:11102/api"),
             QStringLiteral("http://192.168.1.20:8080/openmeeting"),
             QStringLiteral("http://localhost:8080/api"),
             QStringLiteral("http://127.0.0.1:8080/api"),
             QStringLiteral("http://127.1:8080/api"),
             QStringLiteral("http://2130706433:8080/api"),
             QStringLiteral("http://[::1]:8080/api"),
             QStringLiteral("http://[::ffff:127.0.0.1]:8080/api")}) {
        const auto policy = evaluateServiceEndpoint(url);
        TEST_CHECK(policy.status == ServiceEndpointStatus::DebugHttpAllowed);
        TEST_CHECK(policy.requestAllowed() && policy.persistenceAllowed());
    }
    TEST_CHECK(evaluateServiceEndpoint("http://127.0.0.1:0").status ==
               ServiceEndpointStatus::InvalidUrl);

    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    auto settings = settingsAt(directory.filePath("credentials.ini"));
    auto store = makeCredentialStore(*settings);
    StoredSession record{"http://127.0.0.1:8080", "account",
                         {"token", "user", {}, {}}, true};
    TEST_CHECK(store->save(record) == CredentialStatus::Ready);
    TEST_CHECK(settings->contains("auth/protectedSessionV2"));
    TEST_CHECK(store->load(record.service, record.account).status == CredentialStatus::Ready);

    const QByteArray success = R"({"errCode":0,"errMsg":"","data":{}})";
    SingleResponseServer registration(200, success);
    OpenMeetingHttpClient client;
    client.setBaseUrl(registration.url("/api"));
    int registered = 0;
    client.registerUser("account", "password", "name",
        [&](bool ok, const UserInfo &, const HttpError &error) {
            TEST_CHECK(ok && error.code == 0);
            ++registered;
        });
    waitFor([&] { return registered == 1; });
    TEST_CHECK(registration.requests() == 1);
    TEST_CHECK(registration.path() == "/api/user/register");

    SingleResponseServer redirectTarget(200, success);
    SingleResponseServer redirect(302, success, redirectTarget.url("/leak").toUtf8());
    client.setBaseUrl(redirect.url());
    int redirected = 0;
    client.login("account", "password", [&](bool ok, const UserInfo &, const HttpError &error) {
        TEST_CHECK(!ok && error.code == static_cast<int>(ErrorCode::RedirectRejected));
        ++redirected;
    });
    waitFor([&] { return redirected == 1; });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    TEST_CHECK(redirect.requests() == 1);
    TEST_CHECK(redirectTarget.requests() == 0);
}
#else
void verifyStrictDispatchAndStorage() {
    QTcpServer server;
    TEST_CHECK(server.listen(QHostAddress::LocalHost, 0));
    const auto httpUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    for (const auto &url : {
             QStringLiteral("http://example.invalid/api"),
             QStringLiteral("http://123.56.225.164:11102/api"),
             QStringLiteral("http://192.168.1.20:8080/openmeeting"),
             QStringLiteral("http://localhost:8080/api"),
             httpUrl}) {
        TEST_CHECK(evaluateServiceEndpoint(url).status ==
                   ServiceEndpointStatus::InsecureTransportBlocked);
    }

    OpenMeetingHttpClient client;
    TEST_CHECK(client.baseUrl().isEmpty());
    client.setBaseUrl(httpUrl);
    client.setCurrentUser({"old-token", "old-user", {}, {}});
    int callbacks = 0;
    int loggedIn = 0;
    QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn,
                     &client, [&](const UserInfo &) { ++loggedIn; });
    client.login("account", "password", [&](bool ok, const UserInfo &, const HttpError &error) {
        TEST_CHECK(!ok && error.code == static_cast<int>(ErrorCode::InsecureTransport));
        TEST_CHECK(!error.operationId.isEmpty());
        ++callbacks;
    });
    client.registerUser("account", "password", "name",
        [&](bool ok, const UserInfo &, const HttpError &error) {
            TEST_CHECK(!ok && error.code == static_cast<int>(ErrorCode::InsecureTransport));
            ++callbacks;
        });
    waitFor([&] { return callbacks == 2; });
    TEST_CHECK(!server.hasPendingConnections());
    TEST_CHECK(loggedIn == 0 && client.token() == "old-token");

    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    auto settings = settingsAt(directory.filePath("credentials.ini"));
    auto store = makeCredentialStore(*settings);
    StoredSession record{httpUrl, "account", {"token", "user", {}, {}}, true};
    TEST_CHECK(store->save(record) == CredentialStatus::SaveFailed);
    TEST_CHECK(!settings->contains("auth/protectedSessionV2"));

    settings->setValue("network/serverBaseUrl", httpUrl);
    settings->sync();
    OpenMeetingHttpClient sessionClient;
    auto session = SessionManagerTestAccess::create(std::move(settings), sessionClient);
    TEST_CHECK(session->serverBaseUrl() == httpUrl);
    TEST_CHECK(sessionClient.baseUrl() == httpUrl);
    TEST_CHECK(!session->hasSavedSession() && !session->resumeSavedSession(true));
    TEST_CHECK(!session->setServerBaseUrl(httpUrl));
    TEST_CHECK(session->setServerBaseUrl("https://example.invalid/api"));
    TEST_CHECK(!session->setServerBaseUrl("http://10.0.0.1"));
    TEST_CHECK(session->serverBaseUrl() == "https://example.invalid/api");
}
#endif

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    TEST_CHECK(!isDebugHttpTransportEnabled());
    const bool debugHttp = app.arguments().contains(QStringLiteral("--debug"));
    initializeServiceEndpointPolicy(debugHttp);
    verifyCommonPolicy();
#if defined(LIVEKIT_EXPECT_DEBUG_HTTP)
    TEST_CHECK(debugHttp && isDebugHttpTransportEnabled());
    verifyDevelopmentPolicy();
    std::puts("PR-SEC-004 RUNTIME DEBUG HTTP POLICY PASS");
#else
    TEST_CHECK(!debugHttp && !isDebugHttpTransportEnabled());
    TEST_CHECK(app.arguments().contains(QStringLiteral("--debugger")));
    verifyStrictDispatchAndStorage();
    verifyTlsPeerValidation();
    std::puts("PR-SEC-004 STRICT HTTPS POLICY PASS");
#endif
    return 0;
}
