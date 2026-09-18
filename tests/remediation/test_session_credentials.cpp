#include "src/net/session_manager.h"
#include "src/net/service_endpoint_policy.h"
#include "src/ui/login_dialog.h"
#include "tests/support/test_check.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QApplication>
#include <QtPlugin>
#include <cstdio>
#include <vector>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace OpenMeeting {
class SessionManagerTestAccess final {
public:
    using Owner = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;
    static Owner create(std::unique_ptr<QSettings> settings, OpenMeetingHttpClient &client,
                        std::unique_ptr<CredentialStore> store = {}) {
        return Owner(new SessionManager(std::move(settings), std::move(store), &client),
                     [](SessionManager *session) { delete session; });
    }
};
} // namespace OpenMeeting

namespace {
using namespace OpenMeeting;
constexpr auto kPassword = "synthetic-password-sec002";
constexpr auto kToken = "synthetic-token-sec002";

template <typename Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    TEST_CHECK(predicate());
}

std::unique_ptr<QSettings> settingsAt(const QString &path) {
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    settings->setFallbacksEnabled(false);
    return settings;
}

StoredSession record(const QString &service = "https://example.invalid/api") {
    return {service, "account", {kToken, "user", "Synthetic user", {}}, true};
}

void verifyStore() {
    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    const auto path = directory.filePath("session.ini");
    auto settings = settingsAt(path);
    auto store = makeCredentialStore(*settings);
    auto saved = record();
    TEST_CHECK(store->save(saved) == CredentialStatus::Ready);
    const auto cipher = settings->value("auth/protectedSessionV2").toByteArray();
    TEST_CHECK(!cipher.isEmpty() && !cipher.contains(kToken));
    auto loaded = store->load(saved.service, saved.account);
    TEST_CHECK(loaded.status == CredentialStatus::Ready);
    TEST_CHECK(loaded.session.user.token == kToken && loaded.session.autoLogin);
    QFile file(path);
    TEST_CHECK(file.open(QIODevice::ReadOnly));
    const auto disk = file.readAll();
    TEST_CHECK(!disk.contains(kToken) && !disk.contains(kPassword));
    file.close();

    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--restore", path});
    TEST_CHECK(child.waitForFinished(10000));
    TEST_CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);

    TEST_CHECK(store->load("https://example.invalid/other", saved.account).status == CredentialStatus::InvalidRecord);
    TEST_CHECK(!settings->contains("auth/protectedSessionV2"));
    TEST_CHECK(store->save(saved) == CredentialStatus::Ready);
    TEST_CHECK(store->load(saved.service, "different-account").status == CredentialStatus::InvalidRecord);
    TEST_CHECK(store->save(saved) == CredentialStatus::Ready);
    settings->setValue("auth/protectedSessionV2", QByteArray("corrupt"));
    settings->sync();
    TEST_CHECK(store->load(saved.service, saved.account).status == CredentialStatus::InvalidRecord);

    TEST_CHECK(store->save(saved) == CredentialStatus::Ready);
    settings->setValue("auth/restoreDisabled", true); // interrupted clear/save
    settings->sync();
    TEST_CHECK(store->load(saved.service, saved.account).status == CredentialStatus::Empty);
    TEST_CHECK(!settings->contains("auth/protectedSessionV2"));
    settings->setValue("auth/password", kPassword);
    settings->setValue("user/token", kToken);
    settings->setValue("auth/rememberPassword", true);
    settings->setValue("auth/autoLogin", true);
    settings->setValue("auth/account", saved.account);
    settings->setValue("media/enableVideo", true);
    settings->sync();
    TEST_CHECK(store->load(saved.service, saved.account).status == CredentialStatus::Migrated);
    TEST_CHECK(!settings->contains("auth/password") && !settings->contains("user/token"));
    TEST_CHECK(settings->value("auth/account").toString() == saved.account);
    TEST_CHECK(settings->value("media/enableVideo").toBool());
    TEST_CHECK(store->load(saved.service, saved.account).status == CredentialStatus::Empty);
    saved.user.token = QString(70000, 's');
    TEST_CHECK(store->save(saved) == CredentialStatus::SaveFailed);
    TEST_CHECK(!settings->contains("auth/protectedSessionV2"));

    QFile blocker(directory.filePath("not-a-directory"));
    TEST_CHECK(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    auto unwritable = settingsAt(blocker.fileName() + "/settings.ini");
    auto failedStore = makeCredentialStore(*unwritable);
    TEST_CHECK(failedStore->save(record()) == CredentialStatus::CleanupFailed);
    TEST_CHECK(failedStore->clear() == CredentialStatus::CleanupFailed);
    TEST_CHECK(canonicalServiceUrl("https://EXAMPLE.invalid:443/api/") == "https://example.invalid/api");
    TEST_CHECK(canonicalServiceUrl("https://example.invalid/API") != canonicalServiceUrl("https://example.invalid/api"));
    TEST_CHECK(canonicalServiceUrl("https://user:password@example.invalid").isEmpty());
    TEST_CHECK(canonicalServiceUrl("https://example.invalid?token=value").isEmpty());
    std::puts("STORE PASS: DPAPI, child-process restore, binding, migration, crash marker, failure");
}

void verifyDebugHttpPersistenceAcrossStartupModes() {
    TEST_CHECK(isDebugHttpTransportEnabled());
    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    const auto path = directory.filePath("debug-http-session.ini");
    const auto service = QStringLiteral("http://debug.example.invalid:8080/api");
    auto settings = settingsAt(path);
    settings->setValue("network/serverBaseUrl", service);
    settings->setValue("auth/account", QStringLiteral("account"));
    auto store = makeCredentialStore(*settings);
    TEST_CHECK(store->save(record(service)) == CredentialStatus::Ready);
    const auto cipher = settings->value("auth/protectedSessionV2").toByteArray();
    TEST_CHECK(!cipher.isEmpty() && !cipher.contains(kToken));
    settings.reset();

    QProcess strictChild;
    strictChild.start(QCoreApplication::applicationFilePath(),
                      {"--strict-http-restore", path});
    TEST_CHECK(strictChild.waitForFinished(10000));
    TEST_CHECK(strictChild.exitStatus() == QProcess::NormalExit && strictChild.exitCode() == 0);
    TEST_CHECK(settingsAt(path)->contains("auth/protectedSessionV2"));

    QProcess debugChild;
    debugChild.start(QCoreApplication::applicationFilePath(),
                     {"--debug-http-restore", path, "--debug"});
    TEST_CHECK(debugChild.waitForFinished(10000));
    TEST_CHECK(debugChild.exitStatus() == QProcess::NormalExit && debugChild.exitCode() == 0);
    TEST_CHECK(settingsAt(path)->contains("auth/protectedSessionV2"));
    std::puts("DEBUG HTTP PERSISTENCE PASS: encrypted save, strict dormancy, debug auto restore");
}

class Server {
public:
    struct Request { QPointer<QTcpSocket> socket; QByteArray bytes, path, token, operationId; QJsonObject body; bool parsed = false; };
    Server() {
        TEST_CHECK(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto request = std::make_shared<Request>();
                request->socket = server.nextPendingConnection();
                QObject::connect(request->socket, &QTcpSocket::readyRead, &server, [this, request] {
                    request->bytes += request->socket->readAll();
                    if (request->parsed) return;
                    const int end = request->bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto headers = request->bytes.left(end).split('\n');
                    int length = 0;
                    for (const auto &header : headers) {
                        const int colon = header.indexOf(':');
                        const auto name = header.left(colon).trimmed().toLower();
                        if (name == "content-length") length = header.mid(colon + 1).trimmed().toInt();
                        if (name == "token") request->token = header.mid(colon + 1).trimmed();
                        if (name == "operationid") request->operationId = header.mid(colon + 1).trimmed();
                    }
                    if (request->bytes.size() < end + 4 + length) return;
                    request->path = headers.front().split(' ').at(1);
                    request->body = QJsonDocument::fromJson(request->bytes.mid(end + 4, length)).object();
                    request->parsed = true;
                    requests.push_back(request);
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1").arg(server.serverPort()); }
    void received(size_t count) { waitFor([&] { return requests.size() >= count; }); }
    void reply(size_t index, const QString &token = kToken, int error = 0) {
        QJsonObject data{{"token", token}, {"userID", "user"}, {"nickname", "Synthetic"}};
        replyData(index, data, error);
    }
    void replyData(size_t index, const QJsonValue &data, int error = 0) {
        const auto body = QJsonDocument(QJsonObject{{"errCode", error}, {"errMsg", error ? "controlled-denial" : ""},
                                                   {"data", data}}).toJson(QJsonDocument::Compact);
        const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        TEST_CHECK(requests.at(index)->socket);
        TEST_CHECK(requests.at(index)->socket->write(response) == response.size());
        requests.at(index)->socket->disconnectFromHost();
    }
    QTcpServer server;
    std::vector<std::shared_ptr<Request>> requests;
};

void verifyPublicAuthContract() {
    // No SessionManager: exercise the supported public HTTP API directly.
    for (const int logoutError : {0, 4107}) {
        Server server;
        OpenMeetingHttpClient client;
        client.setBaseUrl(server.url());
        client.setCurrentUser({"previous-token", "previous-user", {}, {}});
        int loggedIn = 0, loggedOut = 0, callbacks = 0;
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn, &client, [&](const UserInfo &user) {
            TEST_CHECK(client.isLoggedIn() && client.token() == kToken);
            TEST_CHECK(client.currentUser().toJson() == user.toJson());
            TEST_CHECK(callbacks == 0);
            ++loggedIn;
        });
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [&] {
            TEST_CHECK(!client.isLoggedIn() && client.token().isEmpty());
            TEST_CHECK(client.currentUser().toJson() == UserInfo{}.toJson());
            TEST_CHECK(callbacks == 1);
            ++loggedOut;
        });
        client.login("account", kPassword, [&](bool ok, const UserInfo &user, const HttpError &err) {
            TEST_CHECK(ok && err.code == 0 && !err.operationId.isEmpty());
            TEST_CHECK(loggedIn == 1 && client.isLoggedIn());
            TEST_CHECK(client.currentUser().toJson() == user.toJson());
            ++callbacks;
        });
        server.received(1);
        TEST_CHECK(server.requests[0]->path == "/user/login" && server.requests[0]->token.isEmpty());
        server.reply(0);
        waitFor([&] { return callbacks == 1; });
        client.logout([&](bool ok, bool result, const HttpError &err) {
            TEST_CHECK(ok == (logoutError == 0) && result == ok && err.code == logoutError);
            TEST_CHECK(loggedOut == 1 && !client.isLoggedIn() && client.token().isEmpty());
            ++callbacks;
        });
        server.received(2);
        TEST_CHECK(server.requests[1]->path == "/user/logout" && server.requests[1]->token == kToken);
        TEST_CHECK(server.requests[1]->body.value("userID").toString() == "user");
        server.reply(1, {}, logoutError);
        waitFor([&] { return callbacks == 2; });
        QCoreApplication::processEvents();
        TEST_CHECK(loggedIn == 1 && loggedOut == 1 && callbacks == 2);
    }
    // Failed or incomplete login must not publish a user or replace existing auth.
    for (int scenario = 0; scenario < 3; ++scenario) {
        Server server;
        OpenMeetingHttpClient client;
        client.setBaseUrl(server.url());
        const UserInfo previous{"previous-token", "previous-user", {}, {}};
        client.setCurrentUser(previous);
        int callbacks = 0;
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn, &client, [] { TEST_CHECK(false); });
        client.login("account", kPassword, [&](bool ok, const UserInfo &user, const HttpError &err) {
            TEST_CHECK(!ok && user.token.isEmpty());
            TEST_CHECK(err.code == (scenario == 0 ? 4107 : static_cast<int>(ErrorCode::ParseError)));
            TEST_CHECK(client.currentUser().toJson() == previous.toJson());
            ++callbacks;
        });
        server.received(1);
        TEST_CHECK(server.requests[0]->token.isEmpty());
        QJsonObject data{{"token", kToken}, {"userID", "user"}};
        if (scenario == 1) data.remove("token");
        if (scenario == 2) data.remove("userID");
        server.replyData(0, data, scenario == 0 ? 4107 : 0);
        waitFor([&] { return callbacks == 1; });
    }
    std::puts("PUBLIC AUTH PASS: login/logout state, signal-before-callback, exactly once, failures, request credentials");
}

void verifyPublicInvalidLoginData() {
    const std::vector<QJsonValue> invalidData{
        QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Undefined),
        QJsonArray{"unexpected"}, QJsonValue("unexpected"), QJsonValue(42), QJsonValue(true)
    };
    for (const auto &data : invalidData) {
        for (const int error : {0, 4107}) {
            Server server;
            OpenMeetingHttpClient client;
            client.setBaseUrl(server.url());
            const UserInfo previous{"previous-token", "previous-user", {}, {}};
            client.setCurrentUser(previous);
            int callbacks = 0;
            QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn, &client, [] { TEST_CHECK(false); });
            QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [] { TEST_CHECK(false); });
            client.login("account", kPassword, [&](bool ok, const UserInfo &user, const HttpError &err) {
                TEST_CHECK(!ok && user.toJson() == UserInfo{}.toJson());
                TEST_CHECK(err.code == (error ? error : static_cast<int>(ErrorCode::ParseError)));
                TEST_CHECK(!err.message.isEmpty());
                if (error) TEST_CHECK(err.message == "controlled-denial");
                TEST_CHECK(!err.operationId.isEmpty());
                TEST_CHECK(err.operationId.toUtf8() == server.requests.at(0)->operationId);
                TEST_CHECK(client.isLoggedIn() && client.token() == previous.token);
                TEST_CHECK(client.currentUser().toJson() == previous.toJson());
                ++callbacks;
            });
            server.received(1);
            server.replyData(0, data, error);
            waitFor([&] { return callbacks == 1; });
            QCoreApplication::processEvents();
            TEST_CHECK(callbacks == 1);
        }
    }
    std::puts("PUBLIC INVALID DATA PASS: null, absent, array, scalar, parse error, server error, operation ID, auth preservation");
}

void verifyPublicAuthOrdering() {
    // The newest request wins even when the older login responds first.
    for (const bool newerFirst : {false, true}) {
        Server server;
        OpenMeetingHttpClient client;
        client.setBaseUrl(server.url());
        int oldDone = 0, newDone = 0, loggedIn = 0;
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn, &client,
                         [&](const UserInfo &user) { TEST_CHECK(user.token == "new-token"); ++loggedIn; });
        client.login("old", kPassword, [&](bool ok, const UserInfo &user, const HttpError &err) {
            TEST_CHECK(!ok && user.token.isEmpty() && err.code != 0); ++oldDone;
        });
        server.received(1);
        client.login("new", kPassword, [&](bool ok, const UserInfo &, const HttpError &) {
            TEST_CHECK(ok && client.token() == "new-token"); ++newDone;
        });
        server.received(2);
        if (newerFirst) {
            server.reply(1, "new-token");
            waitFor([&] { return newDone == 1; });
        }
        server.reply(0, "old-token");
        waitFor([&] { return oldDone == 1; });
        if (!newerFirst) {
            TEST_CHECK(!client.isLoggedIn() && loggedIn == 0);
            server.reply(1, "new-token");
            waitFor([&] { return newDone == 1; });
        }
        TEST_CHECK(loggedIn == 1 && client.token() == "new-token");

        int logoutDone = 0;
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [] { TEST_CHECK(false); });
        client.logout([&](bool ok, bool result, const HttpError &) {
            TEST_CHECK(ok && result); ++logoutDone;
        });
        server.received(3);
        client.login("new", kPassword, [&](bool ok, const UserInfo &, const HttpError &) {
            TEST_CHECK(ok); ++newDone;
        });
        server.received(4);
        server.reply(3, "new-token");
        waitFor([&] { return newDone == 2; });
        server.reply(2);
        waitFor([&] { return logoutDone == 1; });
        TEST_CHECK(client.isLoggedIn() && client.token() == "new-token" && loggedIn == 2);
    }
    // A newer login may still be pending or fail: the old logout must still clear
    // the old account, without cancelling the newer login when it succeeds.
    for (const bool loginFirst : {false, true}) {
        for (const bool loginSucceeds : {false, true}) {
            Server server;
            OpenMeetingHttpClient client;
            client.setBaseUrl(server.url());
            client.setCurrentUser({"old-token", "old-user", {}, {}});
            int loginDone = 0, logoutDone = 0, loggedOut = 0;
            QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [&] {
                TEST_CHECK(!client.isLoggedIn()); ++loggedOut;
            });
            client.logout([&](bool ok, bool result, const HttpError &) {
                TEST_CHECK(ok && result); ++logoutDone;
            });
            server.received(1);
            client.login("new", kPassword, [&](bool ok, const UserInfo &, const HttpError &) {
                TEST_CHECK(ok == loginSucceeds); ++loginDone;
            });
            server.received(2);
            if (loginFirst) {
                server.reply(1, "new-token", loginSucceeds ? 0 : 4107);
                waitFor([&] { return loginDone == 1; });
            }
            server.reply(0);
            waitFor([&] { return logoutDone == 1; });
            if (!loginFirst) {
                TEST_CHECK(client.token().isEmpty());
                server.reply(1, "new-token", loginSucceeds ? 0 : 4107);
                waitFor([&] { return loginDone == 1; });
            }
            TEST_CHECK(client.isLoggedIn() == loginSucceeds);
            TEST_CHECK(client.token() == (loginSucceeds ? "new-token" : ""));
            TEST_CHECK(loggedOut == (loginFirst && loginSucceeds ? 0 : 1));
        }
    }
    // Explicit logout, server replacement and external state replacement cancel pending login.
    for (int scenario = 0; scenario < 3; ++scenario) {
        Server server;
        OpenMeetingHttpClient client;
        client.setBaseUrl(server.url());
        int cancelled = 0, loggedOut = 0;
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedIn, &client, [] { TEST_CHECK(false); });
        QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [&] { ++loggedOut; });
        client.login("account", kPassword, [&](bool ok, const UserInfo &, const HttpError &err) {
            TEST_CHECK(!ok && err.code != 0); ++cancelled;
        });
        server.received(1);
        if (scenario == 0) client.logout();
        if (scenario == 1) client.setBaseUrl("https://other.invalid/api");
        if (scenario == 2) client.setCurrentUser({"replacement-token", "replacement-user", {}, {}});
        server.reply(0);
        waitFor([&] { return cancelled == 1; });
        if (scenario == 0) {
            server.received(2);
            server.reply(1);
            waitFor([&] { return loggedOut == 1; });
        }
        TEST_CHECK(client.token() == (scenario == 2 ? "replacement-token" : ""));
    }
    // Direct signal handlers may replace auth or destroy the client synchronously.
    for (const bool destroy : {false, true}) {
        Server server;
        auto client = std::make_unique<OpenMeetingHttpClient>();
        client->setBaseUrl(server.url());
        int callbacks = 0, loggedIn = 0;
        QObject::connect(client.get(), &OpenMeetingHttpClient::userLoggedIn, &server.server, [&] {
            ++loggedIn;
            if (destroy) client.reset();
            else client->setCurrentUser({});
        });
        client->login("account", kPassword, [&](bool ok, const UserInfo &user, const HttpError &err) {
            TEST_CHECK(!ok && user.token.isEmpty() && err.code != 0); ++callbacks;
        });
        server.received(1);
        server.reply(0);
        waitFor([&] { return callbacks == 1; });
        TEST_CHECK(loggedIn == 1 && (!client || !client->isLoggedIn()));
    }
    std::puts("PUBLIC ORDERING PASS: replacement, late logout, cancellation, server change, signal reentry/destruction");
}

void verifyPublicLogoutBoundaries() {
    const UserInfo previous{"previous-token", "previous-user", {}, {}};
    const UserInfo replacement{"replacement-token", "replacement-user", {}, {}};
    for (const int logoutError : {0, 4107}) {
        // A late response must not clear or notify for a different server/session.
        for (int scenario = 0; scenario < 3; ++scenario) {
            Server server;
            OpenMeetingHttpClient client;
            client.setBaseUrl(server.url());
            client.setCurrentUser(previous);
            int callbacks = 0;
            QObject::connect(&client, &OpenMeetingHttpClient::userLoggedOut, &client, [] { TEST_CHECK(false); });
            client.logout([&](bool ok, bool result, const HttpError &err) {
                TEST_CHECK(ok == (logoutError == 0) && result == ok && err.code == logoutError);
                ++callbacks;
            });
            server.received(1);
            TEST_CHECK(server.requests[0]->token == previous.token.toUtf8());
            TEST_CHECK(server.requests[0]->body.value("userID").toString() == previous.userId);
            if (scenario == 0) client.setBaseUrl("https://other.invalid/api");
            if (scenario == 1) client.setCurrentUser(replacement);
            if (scenario == 2) client.setToken(replacement.token);
            const auto expected = client.currentUser();
            const auto expectedUrl = client.baseUrl();
            server.reply(0, {}, logoutError);
            waitFor([&] { return callbacks == 1; });
            QCoreApplication::processEvents();
            TEST_CHECK(callbacks == 1 && client.baseUrl() == expectedUrl);
            TEST_CHECK(client.currentUser().toJson() == expected.toJson() && client.token() == expected.token);
            TEST_CHECK(client.isLoggedIn() == (scenario != 0));
        }
        // Direct logout slots can replace auth, start a login, or delete the client.
        for (int scenario = 0; scenario < 3; ++scenario) {
            Server server;
            auto client = std::make_unique<OpenMeetingHttpClient>();
            client->setBaseUrl(server.url());
            client->setCurrentUser(previous);
            int loggedOut = 0, logoutDone = 0, loggedIn = 0, loginDone = 0;
            QObject::connect(client.get(), &OpenMeetingHttpClient::userLoggedIn, &server.server,
                [&](const UserInfo &user) {
                    TEST_CHECK(scenario == 1 && logoutDone == 1 && loginDone == 0);
                    TEST_CHECK(user.token == kToken && client->token() == kToken);
                    ++loggedIn;
                }, Qt::DirectConnection);
            QObject::connect(client.get(), &OpenMeetingHttpClient::userLoggedOut, &server.server, [&] {
                TEST_CHECK(logoutDone == 0 && loggedOut == 0);
                TEST_CHECK(!client->isLoggedIn() && client->currentUser().toJson() == UserInfo{}.toJson());
                ++loggedOut;
                if (scenario == 0) client->setCurrentUser(replacement);
                if (scenario == 1) client->login("new", kPassword,
                    [&](bool ok, const UserInfo &user, const HttpError &err) {
                        TEST_CHECK(ok && err.code == 0 && loggedIn == 1 && logoutDone == 1);
                        TEST_CHECK(client->currentUser().toJson() == user.toJson());
                        ++loginDone;
                    });
                if (scenario == 2) client.reset();
            }, Qt::DirectConnection);
            client->logout([&](bool ok, bool result, const HttpError &err) {
                TEST_CHECK(ok == (logoutError == 0) && result == ok && err.code == logoutError);
                TEST_CHECK(loggedOut == 1);
                if (scenario == 0) TEST_CHECK(client->currentUser().toJson() == replacement.toJson());
                if (scenario == 1) TEST_CHECK(!client->isLoggedIn());
                if (scenario == 2) TEST_CHECK(!client);
                ++logoutDone;
            });
            server.received(1);
            server.reply(0, {}, logoutError);
            waitFor([&] { return logoutDone == 1; });
            if (scenario == 1) {
                server.received(2);
                TEST_CHECK(server.requests[1]->path == "/user/login" && server.requests[1]->token.isEmpty());
                server.reply(1);
                waitFor([&] { return loginDone == 1; });
                TEST_CHECK(client->isLoggedIn() && client->token() == kToken);
            }
            QCoreApplication::processEvents();
            TEST_CHECK(loggedOut == 1 && logoutDone == 1);
            TEST_CHECK(loggedIn == (scenario == 1 ? 1 : 0) && loginDone == loggedIn);
        }
    }
    std::puts("PUBLIC LOGOUT BOUNDARIES PASS: server/auth replacement, signal reentry/destruction, server result preservation");
}

struct Failures { bool save = false, clear = false; };
class FaultStore final : public CredentialStore {
public:
    FaultStore(QSettings &settings, Failures &failures) : real(makeCredentialStore(settings)), failures(failures) {}
    CredentialLoadResult load(const QString &service, const QString &account) override { return real->load(service, account); }
    CredentialStatus save(const StoredSession &value) override {
        return failures.save ? CredentialStatus::SaveFailed : real->save(value);
    }
    CredentialStatus clear() override {
        return failures.clear ? CredentialStatus::CleanupFailed : real->clear();
    }
    std::unique_ptr<CredentialStore> real;
    Failures &failures;
};

struct Fixture {
    Fixture() : session(nullptr, [](SessionManager *) {}) {
        TEST_CHECK(directory.isValid());
        auto settings = settingsAt(path());
        settings->setValue("network/serverBaseUrl", server.url());
        settings->sync();
        auto store = std::make_unique<FaultStore>(*settings, failures);
        session = SessionManagerTestAccess::create(std::move(settings), client, std::move(store));
    }
    QString path() const { return directory.filePath("auth.ini"); }
    bool hasCipher() {
        auto probe = settingsAt(path());
        probe->sync();
        return probe->contains("auth/protectedSessionV2");
    }
    void restart() {
        session.reset();
        session = SessionManagerTestAccess::create(settingsAt(path()), client);
    }
    void login(bool remember = true, bool automatic = true, const QString &token = kToken) {
        const auto request = server.requests.size();
        int done = 0;
        session->loginWithPassword("account", kPassword, remember, automatic,
            [&](bool ok, const QString &) { TEST_CHECK(ok); ++done; });
        server.received(request + 1);
        TEST_CHECK(server.requests[request]->token.isEmpty());
        TEST_CHECK(server.requests[request]->body.value("password").toString() == kPassword);
        server.reply(request, token);
        waitFor([&] { return done == 1; });
        TEST_CHECK(session->token() == token && client.token() == token);
    }
    QTemporaryDir directory;
    Server server;
    OpenMeetingHttpClient client;
    Failures failures;
    SessionManagerTestAccess::Owner session;
};

void verifySessionInvalidLoginData() {
    for (const auto &data : std::vector<QJsonValue>{QJsonValue(QJsonValue::Null), QJsonArray{}}) {
        Fixture f;
        f.login();
        int callbacks = 0;
        QObject::connect(&f.client, &OpenMeetingHttpClient::userLoggedIn, f.session.get(), [] { TEST_CHECK(false); });
        QObject::connect(f.session.get(), &SessionManager::loggedIn, f.session.get(), [] { TEST_CHECK(false); });
        f.session->loginWithPassword("new", kPassword, true, true, [&](bool ok, const QString &message) {
            TEST_CHECK(!ok && !message.isEmpty());
            TEST_CHECK(!f.session->isLoggedIn() && !f.client.isLoggedIn() && f.client.token().isEmpty());
            TEST_CHECK(!f.hasCipher() && !f.session->isRememberSession());
            ++callbacks;
        });
        f.server.received(2);
        f.server.replyData(1, data);
        waitFor([&] { return callbacks == 1; });
        QCoreApplication::processEvents();
        TEST_CHECK(callbacks == 1);
    }
    std::puts("SESSION INVALID DATA PASS: nonempty error, no auth commit, no persisted credentials");
}

void verifySessionAndUi() {
    Fixture f;
    int httpLoginSignals = 0, httpLogoutSignals = 0;
    QObject::connect(&f.client, &OpenMeetingHttpClient::userLoggedIn, f.session.get(), [&](const UserInfo &user) {
        TEST_CHECK(f.session->isLoggedIn() && f.session->token() == user.token && f.hasCipher());
        ++httpLoginSignals;
    });
    QObject::connect(&f.client, &OpenMeetingHttpClient::userLoggedOut, f.session.get(), [&] {
        TEST_CHECK(!f.session->isLoggedIn() && f.client.token().isEmpty() && !f.hasCipher());
        ++httpLogoutSignals;
    });
    f.login(true, false);
    TEST_CHECK(httpLoginSignals == 1 && httpLogoutSignals == 0);
    f.session->logout(false);
    TEST_CHECK(httpLoginSignals == 1 && httpLogoutSignals == 1);
    f.login(true, false);
    TEST_CHECK(f.hasCipher() && f.session->isRememberSession() && !f.session->isAutoLogin());
    auto before = settingsAt(f.path())->value("auth/protectedSessionV2").toByteArray();
    f.session->setEnableVideo(true);
    TEST_CHECK(settingsAt(f.path())->value("auth/protectedSessionV2").toByteArray() == before);
    f.restart();
    TEST_CHECK(!f.session->isLoggedIn() && f.client.token().isEmpty());
    TEST_CHECK(!f.session->resumeSavedSession(true));
    {
        MeetingUI::LoginDialog dialog(*f.session);
        auto password = dialog.findChild<QLineEdit *>("loginPassword");
        auto resume = dialog.findChild<QPushButton *>("resumeSavedSession");
        TEST_CHECK(password && password->text().isEmpty());
        TEST_CHECK(resume && !resume->isHidden());
        dialog.findChild<QCheckBox *>("autoLogin")->setChecked(true);
        resume->click();
        TEST_CHECK(dialog.result() == QDialog::Accepted && f.session->isLoggedIn());
        TEST_CHECK(f.session->isAutoLogin());
    }
    f.session->logout(false);
    TEST_CHECK(!f.hasCipher() && f.client.token().isEmpty());
    f.session->loadFromSettings();
    TEST_CHECK(!f.session->isLoggedIn());
    f.login();
    f.restart();
    TEST_CHECK(f.session->resumeSavedSession(true) && f.session->isLoggedIn());
    {
        MeetingUI::LoginDialog dialog(*f.session);
        dialog.findChild<QCheckBox *>("rememberSession")->setChecked(false);
        TEST_CHECK(!f.hasCipher() && !f.session->isAutoLogin());
        TEST_CHECK(!dialog.findChild<QCheckBox *>("autoLogin")->isEnabled());
    }
    f.login(false);
    TEST_CHECK(!f.hasCipher() && !f.session->isRememberSession());
    f.login();
    f.session->invalidateSession(SessionInvalidationReason::TokenExpired);
    TEST_CHECK(!f.hasCipher() && !f.session->isLoggedIn());

    Fixture failure;
    failure.failures.save = true;
    failure.login();
    TEST_CHECK(failure.session->isLoggedIn() && !failure.session->isRememberSession());
    TEST_CHECK(!failure.hasCipher() && !failure.session->persistenceMessage().isEmpty());
    failure.failures.clear = true;
    failure.session->logout(false);
    TEST_CHECK(failure.client.token().isEmpty());
    TEST_CHECK(failure.session->credentialStatus() == CredentialStatus::CleanupFailed);
    TEST_CHECK(!failure.session->persistenceMessage().isEmpty());
    std::puts("SESSION/UI PASS: explicit resume, auto login, no password refill, preferences, failures");
}

void verifyOrdering() {
    Fixture f;
    int oldDone = 0, newDone = 0;
    f.session->loginWithPassword("old", kPassword, true, true,
        [&](bool ok, const QString &) { TEST_CHECK(!ok); ++oldDone; });
    f.server.received(1);
    f.session->loginWithPassword("new", kPassword, true, true,
        [&](bool ok, const QString &) { TEST_CHECK(ok); ++newDone; });
    f.server.received(2);
    f.server.reply(1, "synthetic-new-token");
    waitFor([&] { return newDone == 1; });
    f.server.reply(0, "synthetic-old-token");
    waitFor([&] { return oldDone == 1; });
    TEST_CHECK(f.session->token() == "synthetic-new-token");
    TEST_CHECK(f.client.token() == "synthetic-new-token");

    f.session->logout(); // keep old logout response pending while login succeeds
    f.server.received(3);
    f.login();
    f.server.reply(2);
    waitFor([&] { return f.server.requests[2]->socket->state() == QAbstractSocket::UnconnectedState; });
    QCoreApplication::processEvents();
    TEST_CHECK(f.client.token() == kToken && f.session->isLoggedIn());

    int staleError = 0;
    f.client.getMeetings({}, [&](bool ok, const QJsonArray &, const HttpError &) { TEST_CHECK(!ok); ++staleError; });
    const auto staleIndex = f.server.requests.size();
    f.server.received(staleIndex + 1);
    f.login();
    f.server.reply(staleIndex, {}, static_cast<int>(ErrorCode::TokenExpired));
    waitFor([&] { return staleError == 1; });
    TEST_CHECK(f.session->isLoggedIn() && f.hasCipher());

    int cancelled = 0;
    const auto pending = f.server.requests.size();
    f.session->loginWithPassword("account", kPassword, true, true,
        [&](bool ok, const QString &) { TEST_CHECK(!ok); ++cancelled; });
    f.server.received(pending + 1);
    TEST_CHECK(f.session->setServerBaseUrl("https://other.invalid/api"));
    f.server.reply(pending);
    waitFor([&] { return cancelled == 1; });
    TEST_CHECK(!f.session->isLoggedIn() && f.client.token().isEmpty() && !f.hasCipher());

    Fixture ui;
    {
        MeetingUI::LoginDialog dialog(*ui.session);
        dialog.findChild<QLineEdit *>("loginAccount")->setText("account");
        dialog.findChild<QLineEdit *>("loginPassword")->setText(kPassword);
        dialog.findChild<QCheckBox *>("rememberSession")->setChecked(true);
        TEST_CHECK(QMetaObject::invokeMethod(&dialog, "onLoginClicked", Qt::DirectConnection));
        ui.server.received(1);
        dialog.reject();
        ui.server.reply(0);
        waitFor([&] { return dialog.findChild<QLineEdit *>("loginPassword")->isEnabled(); });
        TEST_CHECK(!ui.session->isLoggedIn() && !ui.hasCipher());
    }
    Fixture reentrant;
    auto connection = QObject::connect(reentrant.session.get(), &SessionManager::loggedIn,
        reentrant.session.get(), [&] { reentrant.session->logout(false); }, Qt::DirectConnection);
    int reenteredDone = 0;
    reentrant.session->loginWithPassword("account", kPassword, true, true,
        [&](bool ok, const QString &) { TEST_CHECK(!ok); ++reenteredDone; });
    reentrant.server.received(1);
    reentrant.server.reply(0);
    waitFor([&] { return reenteredDone == 1; });
    TEST_CHECK(!reentrant.hasCipher() && !reentrant.session->isLoggedIn());
    QObject::disconnect(connection);

    int destroyedDone = 0;
    reentrant.session->loginWithPassword("account", kPassword, true, true,
        [&](bool ok, const QString &) { TEST_CHECK(!ok); ++destroyedDone; });
    reentrant.server.received(2);
    reentrant.session.reset();
    reentrant.server.reply(1);
    waitFor([&] { return destroyedDone == 1; });
    TEST_CHECK(reentrant.client.token().isEmpty() && !reentrant.hasCipher());
    std::puts("ORDERING PASS: login replacement, late logout/401, server change, UI cancel, reentry, owner destruction");
}
} // namespace

int main(int argc, char **argv) {
    if (argc >= 3 && QByteArray(argv[1]) == "--restore") {
        QCoreApplication app(argc, argv);
        OpenMeeting::initializeServiceEndpointPolicy(
            app.arguments().contains(QStringLiteral("--debug")));
        auto settings = settingsAt(QString::fromLocal8Bit(argv[2]));
        auto store = makeCredentialStore(*settings);
        const auto loaded = store->load(record().service, record().account);
        TEST_CHECK(loaded.status == CredentialStatus::Ready && loaded.session.user.token == kToken);
        return 0;
    }
    if (argc >= 3 && (QByteArray(argv[1]) == "--strict-http-restore" ||
                      QByteArray(argv[1]) == "--debug-http-restore")) {
        QCoreApplication app(argc, argv);
        const bool debugHttp = app.arguments().contains(QStringLiteral("--debug"));
        OpenMeeting::initializeServiceEndpointPolicy(debugHttp);
        const auto path = QString::fromLocal8Bit(argv[2]);
        const auto expectedService = QStringLiteral("http://debug.example.invalid:8080/api");
        OpenMeeting::OpenMeetingHttpClient client;
        auto session = OpenMeeting::SessionManagerTestAccess::create(settingsAt(path), client);
        TEST_CHECK(session->serverBaseUrl() == expectedService);
        TEST_CHECK(client.baseUrl() == expectedService);
        if (QByteArray(argv[1]) == "--strict-http-restore") {
            TEST_CHECK(!debugHttp && !session->hasSavedSession());
            TEST_CHECK(!session->resumeSavedSession(true));
            TEST_CHECK(!session->isLoggedIn() && client.token().isEmpty());
        } else {
            TEST_CHECK(debugHttp && session->hasSavedSession());
            TEST_CHECK(session->resumeSavedSession(true));
            TEST_CHECK(session->isLoggedIn() && client.token() == kToken);
        }
        TEST_CHECK(settingsAt(path)->contains("auth/protectedSessionV2"));
        return 0;
    }
    QApplication app(argc, argv);
    OpenMeeting::initializeServiceEndpointPolicy(
        app.arguments().contains(QStringLiteral("--debug")));
    TEST_CHECK(OpenMeeting::isDebugHttpTransportEnabled());
    app.setQuitOnLastWindowClosed(false);
    verifyPublicAuthContract();
    verifyPublicInvalidLoginData();
    verifyPublicAuthOrdering();
    verifyPublicLogoutBoundaries();
    verifyStore();
    verifyDebugHttpPersistenceAcrossStartupModes();
    verifySessionInvalidLoginData();
    verifySessionAndUi();
    verifyOrdering();
    std::puts("PR-SEC-002 focused cases PASS");
    return 0;
}
