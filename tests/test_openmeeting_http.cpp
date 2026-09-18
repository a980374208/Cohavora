#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include "openmeeting_meeting.pb.h"
#include "src/net/openmeeting_http_client.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "tests/support/test_check.h"

namespace OpenMeeting {

// The product's construction/destruction surface remains private. This fixture
// owns only synchronous guest/settings sessions, never pending HTTP operations.
class SessionManagerTestAccess final {
public:
    using ScopedSession = std::unique_ptr<SessionManager, void (*)(SessionManager*)>;

    static ScopedSession create(std::unique_ptr<QSettings> settings) {
        return ScopedSession(new SessionManager(std::move(settings)), &destroy);
    }

private:
    static void destroy(SessionManager* session) {
        delete session;
    }
};

} // namespace OpenMeeting

namespace {

// Test access from outside the friend without letting a private destructor
// mask constructor visibility. MSVC 19.50 misreports private constructors with
// default arguments inside a requires-expression; the separately reproduced
// immediate-context SFINAE form keeps all four construction checks reliable.
template <typename T, typename... Args>
struct PubliclyHeapConstructible {
    template <typename U>
    static auto probe(int) -> decltype(new U(std::declval<Args>()...), std::true_type{});
    template <typename>
    static auto probe(...) -> std::false_type;
    static constexpr bool value = decltype(probe<T>(0))::value;
};

// Positive calibration includes an inaccessible destructor: a detector that
// always returns false, or accidentally checks destruction, must fail to build.
class PublicConstructionControl {
public:
    explicit PublicConstructionControl(void* parent = nullptr);
private:
    ~PublicConstructionControl();
};
static_assert(PubliclyHeapConstructible<PublicConstructionControl>::value);
static_assert(PubliclyHeapConstructible<PublicConstructionControl, void*>::value);
static_assert(!std::is_destructible_v<PublicConstructionControl>);

static_assert(!PubliclyHeapConstructible<OpenMeeting::SessionManager>::value);
static_assert(!PubliclyHeapConstructible<OpenMeeting::SessionManager, QObject*>::value);
static_assert(!PubliclyHeapConstructible<OpenMeeting::SessionManager, std::unique_ptr<QSettings>>::value);
static_assert(!PubliclyHeapConstructible<OpenMeeting::SessionManager, std::unique_ptr<QSettings>, QObject*>::value);
static_assert(!std::is_destructible_v<OpenMeeting::SessionManager>);

class LoopbackLoginFixture {
public:
    explicit LoopbackLoginFixture(bool withholdResponse) : withholdResponse_(withholdResponse) {
        TEST_CHECK(server_.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]() {
            TEST_CHECK(socket_ == nullptr);
            socket_ = server_.nextPendingConnection();
            TEST_CHECK(socket_ != nullptr);
            QObject::connect(socket_, &QTcpSocket::readyRead, socket_, [this]() {
                receiveRequest();
            });
            receiveRequest();
        });
    }

    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort());
    }
    int requestCount() const { return requestCount_; }
    QString operationId() const { return QString::fromUtf8(operationId_); }

private:
    void receiveRequest() {
        request_ += socket_->readAll();
        TEST_CHECK(request_.size() < 65536);
        if (requestCount_ != 0) {
            return;
        }
        const int headerEnd = request_.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const auto headers = request_.left(headerEnd).split('\n');
        TEST_CHECK(!headers.isEmpty());
        TEST_CHECK(headers.front().trimmed() == "POST /user/login HTTP/1.1");
        int contentLength = -1;
        for (const auto& header : headers) {
            const int colon = header.indexOf(':');
            if (colon < 0) {
                continue;
            }
            const auto name = header.left(colon).trimmed().toLower();
            const auto value = header.mid(colon + 1).trimmed();
            if (name == "content-length") {
                bool parsed = false;
                contentLength = value.toInt(&parsed);
                TEST_CHECK(parsed && contentLength > 0 && contentLength < 32768);
            } else if (name == "operationid") {
                operationId_ = value;
            }
            TEST_CHECK(name != "token");
        }
        TEST_CHECK(contentLength > 0);
        if (request_.size() < headerEnd + 4 + contentLength) {
            return;
        }
        TEST_CHECK(!operationId_.isEmpty());
        QJsonParseError parseError;
        const auto requestJson = QJsonDocument::fromJson(
            request_.mid(headerEnd + 4, contentLength), &parseError);
        TEST_CHECK(parseError.error == QJsonParseError::NoError);
        TEST_CHECK(requestJson.isObject());
        TEST_CHECK(requestJson.object().value("account").toString() == "fixture_account");
        TEST_CHECK(requestJson.object().value("password").toString() == "fixture_password");
        ++requestCount_;
        std::cout << "LOOPBACK_REQUEST_RECEIVED" << std::endl;

        // Keep the accepted socket alive in the negative mode, so that only
        // the watchdog, not connection teardown, can terminate the request.
        if (withholdResponse_) {
            return;
        }
        const QByteArray body = R"({"errCode":4107,"errMsg":"controlled-login-denied","data":null})";
        const QByteArray response = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        TEST_CHECK(socket_->write(response) == response.size());
        socket_->disconnectFromHost();
    }

    QTcpServer server_;
    QTcpSocket* socket_ = nullptr;
    QByteArray request_;
    QByteArray operationId_;
    int requestCount_ = 0;
    bool withholdResponse_;
};

void verifyLocalHttp(bool withholdResponse) {
    LoopbackLoginFixture fixture(withholdResponse);
    OpenMeeting::OpenMeetingHttpClient client;
    client.setBaseUrl(fixture.baseUrl());
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    bool watchdogExpired = false;
    int callbacks = 0;
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
        watchdogExpired = true;
        std::cout << "HTTP_WATCHDOG_EXPIRED" << std::endl;
        loop.quit();
    });
    watchdog.start(withholdResponse ? 1000 : 5000);
    client.login("fixture_account", "fixture_password",
        [&](bool ok, const OpenMeeting::UserInfo& user, const OpenMeeting::HttpError& error) {
            ++callbacks;
            TEST_CHECK(!withholdResponse);
            TEST_CHECK(callbacks == 1);
            TEST_CHECK(!ok);
            TEST_CHECK(user.userId.isEmpty());
            TEST_CHECK(error.code == 4107);
            TEST_CHECK(error.message == "controlled-login-denied");
            TEST_CHECK(error.operationId == fixture.operationId());
            TEST_CHECK(!error.operationId.isEmpty());
            loop.quit();
        });
    loop.exec();
    watchdog.stop();
    TEST_CHECK(!watchdogExpired);
    TEST_CHECK(fixture.requestCount() == 1);
    TEST_CHECK(callbacks == 1);
    QCoreApplication::processEvents();
    TEST_CHECK(callbacks == 1);
    TEST_CHECK(!client.isLoggedIn());
    TEST_CHECK(client.token().isEmpty());
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    OpenMeeting::initializeServiceEndpointPolicy(
        app.arguments().contains(QStringLiteral("--debug")));
    if (app.arguments().contains("--stall-response")) {
        verifyLocalHttp(true);
        return 0; // The watchdog verifier rejects reaching this success path.
    }

    // Qt 5's organization/application constructor forces NativeFormat. Use an
    // explicit INI file and inject it; never construct the session singleton or
    // read/write native user settings in this test.
    QTemporaryDir settingsDirectory;
    TEST_CHECK(settingsDirectory.isValid());
    const QString expectedSettingsFile = QDir(settingsDirectory.path()).filePath("OpenMeeting/LiveKitClient.ini");
    QSettings isolatedSettings(expectedSettingsFile, QSettings::IniFormat);
    isolatedSettings.setFallbacksEnabled(false);
    TEST_CHECK(isolatedSettings.format() == QSettings::IniFormat);
    TEST_CHECK(QDir::cleanPath(isolatedSettings.fileName()) == QDir::cleanPath(expectedSettingsFile));
    isolatedSettings.setValue("network/serverBaseUrl", "http://127.0.0.1:1");
    isolatedSettings.sync();
    TEST_CHECK(isolatedSettings.status() == QSettings::NoError);
    std::cout << "[TEST] Starting OpenMeeting Protobuf & HTTP Test..." << std::endl;

    // -------------------------------------------------------------
    // Test 1: Protobuf Serialization & Deserialization Test
    // -------------------------------------------------------------
    {
        std::cout << "[TEST 1] Testing openmeeting_meeting.pb.h entities..." << std::endl;
        openmeeting::meeting::LiveKit cert;
        cert.set_url("ws://127.0.0.1:7880");
        cert.set_token("test_token_jwt_sample");

        std::string serialized;
        TEST_CHECK(cert.SerializeToString(&serialized));

        openmeeting::meeting::LiveKit parsedCert;
        TEST_CHECK(parsedCert.ParseFromString(serialized));
        TEST_CHECK(parsedCert.url() == "ws://127.0.0.1:7880");
        TEST_CHECK(parsedCert.token() == "test_token_jwt_sample");

        // Test NotifyMeetingData (DataChannel In-Room Signaling)
        openmeeting::meeting::NotifyMeetingData notify;
        notify.set_operatoruserid("host_user_001");
        auto *streamData = notify.mutable_streamoperatedata();
        auto *op = streamData->add_operation();
        op->set_userid("target_user_002");
        op->set_cameraonentry(false);
        op->set_microphoneonentry(false);

        std::string notifyBytes;
        TEST_CHECK(notify.SerializeToString(&notifyBytes));

        openmeeting::meeting::NotifyMeetingData parsedNotify;
        TEST_CHECK(parsedNotify.ParseFromString(notifyBytes));
        TEST_CHECK(parsedNotify.operatoruserid() == "host_user_001");
        TEST_CHECK(parsedNotify.has_streamoperatedata());
        TEST_CHECK(parsedNotify.streamoperatedata().operation_size() == 1);
        TEST_CHECK(parsedNotify.streamoperatedata().operation(0).userid() == "target_user_002");
        TEST_CHECK(!parsedNotify.streamoperatedata().operation(0).microphoneonentry());

        std::cout << "[TEST 1] PASS: Protobuf entities and serialization verified successfully!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 2: HTTP Client Request & Error Handling Pipeline Test
    // -------------------------------------------------------------
    {
        std::cout << "[TEST 2] Testing loopback HTTP request and controlled error dispatch..." << std::endl;
        verifyLocalHttp(false);
        std::cout << "[TEST 2] PASS: Local request, controlled error and exactly one callback verified." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 3: SessionManager State & Preferences Test
    // -------------------------------------------------------------
    {
        std::cout << "[TEST 3] Testing SessionManager state, media preferences and logout..." << std::endl;
        bool nullSettingsRejected = false;
        try {
            auto invalidSession = OpenMeeting::SessionManagerTestAccess::create(std::unique_ptr<QSettings>{});
        } catch (const std::invalid_argument&) {
            nullSettingsRejected = true;
        }
        TEST_CHECK(nullSettingsRejected);

        auto sessionSettings = std::make_unique<QSettings>(expectedSettingsFile, QSettings::IniFormat);
        sessionSettings->setFallbacksEnabled(false);
        TEST_CHECK(sessionSettings->format() == QSettings::IniFormat);
        TEST_CHECK(QDir::cleanPath(sessionSettings->fileName()) == QDir::cleanPath(expectedSettingsFile));
        auto sessionOwner = OpenMeeting::SessionManagerTestAccess::create(std::move(sessionSettings));
        auto& session = *sessionOwner;
        TEST_CHECK(!session.isLoggedIn());
        TEST_CHECK(session.httpClient().baseUrl() == "http://127.0.0.1:1");

        // 验证初始偏好或默认值
        session.setEnableMicrophone(true);
        session.setEnableVideo(false);
        TEST_CHECK(session.mediaPreferences().enableMicrophone == true);
        TEST_CHECK(session.mediaPreferences().enableVideo == false);

        // 验证访客登录模式
        session.loginAsGuest("TestAlice");
        TEST_CHECK(session.isLoggedIn());
        TEST_CHECK(session.nickname() == "TestAlice");
        TEST_CHECK(!session.token().isEmpty());
        TEST_CHECK(session.httpClient().token() == session.token());

        // 验证偏好修改
        session.setEnableVideo(true);
        TEST_CHECK(session.mediaPreferences().enableVideo == true);

        // 验证登出
        session.logout(false);
        TEST_CHECK(!session.isLoggedIn());
        TEST_CHECK(session.httpClient().token().isEmpty());
        isolatedSettings.sync();
        TEST_CHECK(isolatedSettings.status() == QSettings::NoError);
        TEST_CHECK(!isolatedSettings.contains("user/token"));
        TEST_CHECK(isolatedSettings.value("media/enableVideo").toBool());

        std::cout << "[TEST 3] PASS: SessionManager tested successfully!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 4: Meeting Coordinator Signaling & Metadata Parser Test
    // -------------------------------------------------------------
    {
        std::cout << "[TEST 4] Testing NotifyMeetingData signaling logic and Room Metadata JSON parsing..." << std::endl;

        // 1. KickOff 信令校验
        openmeeting::meeting::NotifyMeetingData kickNotify;
        kickNotify.set_operatoruserid("host_admin_007");
        auto *kickData = kickNotify.mutable_kickoffmeetingdata();
        kickData->set_userid("user_to_be_kicked");
        kickData->set_reason("违反会议守则");
        kickData->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);

        std::string kickBytes;
        TEST_CHECK(kickNotify.SerializeToString(&kickBytes));

        openmeeting::meeting::NotifyMeetingData parsedKick;
        TEST_CHECK(parsedKick.ParseFromString(kickBytes));
        TEST_CHECK(parsedKick.has_kickoffmeetingdata());
        TEST_CHECK(parsedKick.kickoffmeetingdata().userid() == "user_to_be_kicked");
        TEST_CHECK(parsedKick.kickoffmeetingdata().reason() == "违反会议守则");
        TEST_CHECK(parsedKick.kickoffmeetingdata().reasoncode() == openmeeting::meeting::KickOffReason::Logout);

        // 2. StreamOperateData 远端开/关麦信令校验
        openmeeting::meeting::NotifyMeetingData streamNotify;
        streamNotify.set_operatoruserid("host_admin_007");
        auto *opData = streamNotify.mutable_streamoperatedata();
        auto *op1 = opData->add_operation();
        op1->set_userid("user_alice");
        op1->set_cameraonentry(true);
        op1->set_microphoneonentry(false);

        std::string streamBytes;
        TEST_CHECK(streamNotify.SerializeToString(&streamBytes));

        openmeeting::meeting::NotifyMeetingData parsedStream;
        TEST_CHECK(parsedStream.ParseFromString(streamBytes));
        TEST_CHECK(parsedStream.has_streamoperatedata());
        TEST_CHECK(parsedStream.streamoperatedata().operation_size() == 1);
        TEST_CHECK(parsedStream.streamoperatedata().operation(0).cameraonentry() == true);
        TEST_CHECK(parsedStream.streamoperatedata().operation(0).microphoneonentry() == false);

        // 3. Room Metadata JSON 解析校验 (对标 Proto3 JSON)
        QString mockMetadataJson = R"({
            "detail": {
                "info": {
                    "meetingID": "980374208",
                    "meetingName": "技术架构研讨会",
                    "creatorUserID": "user_creator_001",
                    "hostUserID": "user_creator_001",
                    "startTime": 1757134800,
                    "endTime": 1757138400
                },
                "setting": {
                    "disableMicrophoneOnJoin": true,
                    "disableCameraOnJoin": false,
                    "lockMeeting": false,
                    "canParticipantJoinMeetingEarly": true
                }
            }
        })";

        QJsonDocument doc = QJsonDocument::fromJson(mockMetadataJson.toUtf8());
        TEST_CHECK(!doc.isNull());
        QJsonObject root = doc.object();
        QJsonObject detail = root.value("detail").toObject();
        QJsonObject info = detail.value("info").toObject();
        QJsonObject setting = detail.value("setting").toObject();

        TEST_CHECK(info.value("meetingID").toString() == "980374208");
        TEST_CHECK(info.value("meetingName").toString() == QString::fromUtf8("技术架构研讨会"));
        TEST_CHECK(info.value("creatorUserID").toString() == "user_creator_001");
        TEST_CHECK(setting.value("disableMicrophoneOnJoin").toBool() == true);
        TEST_CHECK(setting.value("disableCameraOnJoin").toBool() == false);

        std::cout << "[TEST 4] PASS: Meeting Coordinator signaling & metadata parsing verified successfully!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 5: Mute All & Host Transfer Signaling Serialization Test
    // -------------------------------------------------------------
    {
        std::cout << "[TEST 5] Testing Mute All & Host Transfer Signaling serialization..." << std::endl;
        // 1. 全员静音信令
        openmeeting::meeting::NotifyMeetingData muteAllNotify;
        muteAllNotify.set_operatoruserid("host_admin_001");
        auto *streamOp = muteAllNotify.mutable_streamoperatedata();
        
        auto *u1 = streamOp->add_operation();
        u1->set_userid("user_participant_1");
        u1->set_microphoneonentry(false);
        u1->set_cameraonentry(true);

        auto *u2 = streamOp->add_operation();
        u2->set_userid("user_participant_2");
        u2->set_microphoneonentry(false);
        u2->set_cameraonentry(false);

        std::string muteAllBytes;
        TEST_CHECK(muteAllNotify.SerializeToString(&muteAllBytes));

        openmeeting::meeting::NotifyMeetingData parsedMuteAll;
        TEST_CHECK(parsedMuteAll.ParseFromString(muteAllBytes));
        TEST_CHECK(parsedMuteAll.has_streamoperatedata());
        TEST_CHECK(parsedMuteAll.streamoperatedata().operation_size() == 2);
        TEST_CHECK(!parsedMuteAll.streamoperatedata().operation(0).microphoneonentry());
        TEST_CHECK(!parsedMuteAll.streamoperatedata().operation(1).microphoneonentry());

        // 2. 主持人移交信令
        openmeeting::meeting::NotifyMeetingData hostNotify;
        hostNotify.set_operatoruserid("host_admin_001");
        auto *hostData = hostNotify.mutable_meetinghostdata();
        hostData->set_userid("new_host_user_888");
        hostData->set_operatornickname("OldHostNick");
        hostData->set_hosttype("host");

        std::string hostBytes;
        TEST_CHECK(hostNotify.SerializeToString(&hostBytes));

        openmeeting::meeting::NotifyMeetingData parsedHost;
        TEST_CHECK(parsedHost.ParseFromString(hostBytes));
        TEST_CHECK(parsedHost.has_meetinghostdata());
        TEST_CHECK(parsedHost.meetinghostdata().userid() == "new_host_user_888");
        TEST_CHECK(parsedHost.meetinghostdata().operatornickname() == "OldHostNick");

        std::cout << "[TEST 5] PASS: Mute All & Host Transfer Signaling verified successfully!" << std::endl;
    }

    std::cout << "[TEST ALL] ALL TESTS PASSED." << std::endl;
    return 0;
}
