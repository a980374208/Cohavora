#include <QtCore/QCoreApplication>
#include "openmeeting_http_client.h"
#include "src/net/service_endpoint_policy.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include <QtCore/QUrl>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

namespace OpenMeeting {
namespace {

HttpError parseError(const HttpError &source, const QString &message) {
    auto error = source;
    error.code = static_cast<int>(ErrorCode::ParseError);
    error.message = message;
    error.failureLayer = HttpFailureLayer::Parse;
    if (!source.requestId.isEmpty()) {
        livekit::diagnostic::Event event;
        event.kind = livekit::diagnostic::EventKind::HttpResponseDecodeFailed;
        event.thread_role = livekit::diagnostic::ThreadRole::Ui;
        event.error_layer = livekit::diagnostic::ErrorLayer::Parse;
        event.outcome = livekit::diagnostic::Outcome::Failure;
        event.http_status = source.httpStatus;
        event.context.request_id.Assign(source.requestId.toStdString());
        event.context.legacy_operation_id.Assign(source.operationId.toStdString());
        event.context.parent_operation_id.Assign(source.parentOperationId.toStdString());
        event.context.anonymous_session_id.Assign(source.anonymousSessionId.toStdString());
        livekit::diagnostic::EmitBusinessEvent(event);
    }
    return error;
}

livekit::diagnostic::Route DiagnosticRoute(const QString &path) noexcept {
    using Route = livekit::diagnostic::Route;
    if (path == QStringLiteral("/user/login")) return Route::Login;
    if (path == QStringLiteral("/user/logout")) return Route::Logout;
    if (path == QStringLiteral("/admin/user/register")) return Route::Register;
    if (path == QStringLiteral("/meeting/create_immediate_meeting")) return Route::CreateMeeting;
    if (path == QStringLiteral("/meeting/join_meeting")) return Route::JoinMeeting;
    if (path == QStringLiteral("/meeting/get_meeting_token")) return Route::GetMeetingToken;
    if (path == QStringLiteral("/meeting/get_meetings")) return Route::GetMeetings;
    if (path == QStringLiteral("/meeting/get_meeting")) return Route::GetMeeting;
    if (path == QStringLiteral("/meeting/book_meeting")) return Route::BookMeeting;
    if (path == QStringLiteral("/meeting/update_meeting")) return Route::UpdateMeeting;
    if (path == QStringLiteral("/meeting/end_meeting")) return Route::EndMeeting;
    if (path == QStringLiteral("/meeting/leave_meeting")) return Route::LeaveMeeting;
    return Route::Unknown;
}

livekit::diagnostic::ErrorLayer DiagnosticLayer(HttpFailureLayer layer) noexcept {
    using Layer = livekit::diagnostic::ErrorLayer;
    switch (layer) {
    case HttpFailureLayer::Policy: return Layer::Policy;
    case HttpFailureLayer::Http: return Layer::Http;
    case HttpFailureLayer::Network: return Layer::Network;
    case HttpFailureLayer::Business: return Layer::Business;
    case HttpFailureLayer::Parse: return Layer::Parse;
    default: return Layer::None;
    }
}

} // namespace

OpenMeetingHttpClient::OpenMeetingHttpClient(QObject *parent)
    : OpenMeetingHttpClient(std::make_unique<QNetworkAccessManager>(), parent) {
}

OpenMeetingHttpClient::OpenMeetingHttpClient(
        std::unique_ptr<QNetworkAccessManager> networkManager, QObject *parent)
    : QObject(parent)
    , _nam(std::move(networkManager)) {
    Q_ASSERT(_nam);
    _nam->setParent(this);
    // 针对音视频会议服务直连，避免本地 HTTP 代理软件（如 Clash 127.0.0.1:7890）误拦截非标端口造成 502 错误
    _nam->setProxy(QNetworkProxy::NoProxy);
}

OpenMeetingHttpClient &OpenMeetingHttpClient::instance() {
    static OpenMeetingHttpClient s_instance;
    return s_instance;
}

void OpenMeetingHttpClient::setBaseUrl(const QString &url) {
    const auto policy = evaluateServiceEndpoint(url);
    const auto effectiveUrl = policy.status == ServiceEndpointStatus::InvalidUrl ||
            policy.status == ServiceEndpointStatus::Unconfigured
        ? QString() : policy.canonicalUrl;
    if (_baseUrl == effectiveUrl) return;
    _baseUrl = effectiveUrl;
    setCurrentUser(UserInfo{});
}

void OpenMeetingHttpClient::setToken(const QString &token) {
    ++_authRevision;
    ++_authStateRevision;
    _token = token;
    _currentUser.token = token;
}

void OpenMeetingHttpClient::setCurrentUser(const UserInfo &user) {
    ++_authRevision;
    commitCurrentUser(user);
}

void OpenMeetingHttpClient::commitCurrentUser(const UserInfo &user) {
    ++_authStateRevision;
    _currentUser = user;
    _token = user.token;
}

void OpenMeetingHttpClient::sendPost(
    const QString &path,
    const QJsonObject &body,
    std::function<void(bool ok, const QJsonValue &data, const HttpError &err)> cb,
    bool authenticated, HttpRequestContext context) {
    sendPostToEndpoint(_baseUrl, path, body, std::move(cb), authenticated,
                       std::move(context));
}

void OpenMeetingHttpClient::sendPostToEndpoint(
    const QString &endpointBaseUrl,
    const QString &path,
    const QJsonObject &body,
    std::function<void(bool ok, const QJsonValue &data, const HttpError &err)> cb,
    bool authenticated, HttpRequestContext context) {

    const auto opId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const auto requestId = QUuid::createUuid().toString(QUuid::Id128);
    const auto startedAt = std::chrono::steady_clock::now();
    const auto route = DiagnosticRoute(path);
    livekit::diagnostic::Context diagnosticContext;
    diagnosticContext.request_id.Assign(requestId.toStdString());
    diagnosticContext.legacy_operation_id.Assign(opId.toStdString());
    diagnosticContext.parent_operation_id.Assign(context.parentOperationId.toStdString());
    diagnosticContext.anonymous_session_id.Assign(context.anonymousSessionId.toStdString());
    auto started = livekit::diagnostic::Event{};
    started.kind = livekit::diagnostic::EventKind::HttpRequestStarted;
    started.thread_role = livekit::diagnostic::ThreadRole::Ui;
    started.context = diagnosticContext;
    started.route = route;
    livekit::diagnostic::EmitBusinessEvent(started);
    auto finish = [cb = std::move(cb), requestId, opId,
                   context = std::move(context), diagnosticContext,
                   startedAt, route](bool ok, const QJsonValue &data,
                                     HttpError error) {
        error.operationId = opId;
        error.requestId = requestId;
        error.parentOperationId = context.parentOperationId;
        error.anonymousSessionId = context.anonymousSessionId;
        auto completed = livekit::diagnostic::Event{};
        completed.kind = livekit::diagnostic::EventKind::HttpRequestCompleted;
        completed.thread_role = livekit::diagnostic::ThreadRole::Ui;
        completed.context = diagnosticContext;
        completed.route = route;
        completed.outcome = ok ? livekit::diagnostic::Outcome::Success
                               : livekit::diagnostic::Outcome::Failure;
        completed.error_layer = DiagnosticLayer(error.failureLayer);
        completed.http_status = error.httpStatus;
        completed.network_error = error.networkError;
        completed.business_code = error.businessCode;
        completed.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        livekit::diagnostic::EmitBusinessEvent(completed);
        if (cb) cb(ok, data, error);
    };
    const auto basePolicy = evaluateServiceEndpoint(endpointBaseUrl);
    const bool internalPath = path.startsWith('/') && !path.startsWith("//") &&
        !path.contains('?') && !path.contains('#');
    const QString fullUrl = internalPath ? basePolicy.canonicalUrl + path : QString();
    const auto requestPolicy = evaluateServiceEndpoint(fullUrl);
    const QUrl baseUrl(basePolicy.canonicalUrl, QUrl::StrictMode);
    const QUrl targetUrl(requestPolicy.canonicalUrl, QUrl::StrictMode);
    const bool tlsAvailable = targetUrl.scheme() != QStringLiteral("https") ||
        QSslSocket::supportsSsl();
    const bool sameOrigin = internalPath && basePolicy.requestAllowed() &&
        requestPolicy.requestAllowed() && baseUrl.scheme() == targetUrl.scheme() &&
        baseUrl.host() == targetUrl.host() && baseUrl.port(-1) == targetUrl.port(-1);
    if (!sameOrigin || !tlsAvailable) {
        HttpError error;
        const auto status = !basePolicy.requestAllowed() ? basePolicy.status : requestPolicy.status;
        error.code = static_cast<int>(!tlsAvailable ? ErrorCode::NetworkError
            : status == ServiceEndpointStatus::InsecureTransportBlocked
                ? ErrorCode::InsecureTransport : ErrorCode::InvalidServiceUrl);
        error.message = !tlsAvailable
            ? QCoreApplication::translate("MeetingUI", "HTTPS is not supported in this environment. The request was canceled.")
            : serviceEndpointErrorMessage(status);
        if (error.message.isEmpty()) {
            error.message = QCoreApplication::translate("MeetingUI", "The request URL does not belong to the configured server.");
        }
        error.operationId = opId;
        error.failureLayer = HttpFailureLayer::Policy;
        QTimer::singleShot(0, this, [finish = std::move(finish), error]() {
            finish(false, QJsonValue(), error);
        });
        return;
    }

    QNetworkRequest request{targetUrl};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (targetUrl.scheme() == QStringLiteral("https")) {
        auto ssl = request.sslConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        request.setSslConfiguration(ssl);
    }

    // 注入全链路追踪 operationID (毫秒时间戳)
    request.setRawHeader("operationID", opId.toUtf8());

    // 注入身份 Token
    if (authenticated && !_token.isEmpty()) {
        request.setRawHeader("token", _token.toUtf8());
    }

    QByteArray postData = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = _nam->post(request, postData);
    const auto revision = _authRevision;
    const auto service = _baseUrl;
    const QPointer<OpenMeetingHttpClient> self(this);
    auto expireCurrent = [self, authenticated, revision, service]() {
        if (self && authenticated && self->_authRevision == revision &&
            self->_baseUrl == service && !self->_token.isEmpty()) {
            emit self->tokenExpired();
        }
    };
    connect(reply, &QNetworkReply::finished, this, [reply, finish, expireCurrent]() {
        reply->deleteLater();

        HttpError err;
        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        err.httpStatus = httpStatus;
        err.networkError = static_cast<int>(reply->error());
        QByteArray responseData = reply->readAll();

        if (httpStatus >= 300 && httpStatus < 400) {
            err.code = static_cast<int>(ErrorCode::RedirectRejected);
            err.failureLayer = HttpFailureLayer::Http;
            err.message = QCoreApplication::translate("MeetingUI", "The server returned a disallowed redirect.");
            finish(false, QJsonValue(), err);
            return;
        }

        // 优先检查响应体中是否含有服务端返回的 JSON 业务错误信息 (即使 HTTP 状态码非 200)
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseErr);
        if (parseErr.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject root = doc.object();
            int errCode = root.value("errCode").toInt(-1);
            err.businessCode = errCode;
            QString errMsg = root.value("errMsg").toString();
            QJsonValue dataVal = root.value("data");

            if (errCode == 0 && reply->error() == QNetworkReply::NoError) {
                err.code = 0;
                finish(true, dataVal, err);
                return;
            } else if (!errMsg.isEmpty() || errCode != -1) {
                err.code = (errCode != -1) ? errCode : static_cast<int>(ErrorCode::NetworkError);
                err.message = errMsg;
                err.failureLayer = errCode != -1
                    ? HttpFailureLayer::Business : HttpFailureLayer::Network;
                if (err.isTokenExpired()) {
                    expireCurrent();
                }
                finish(false, dataVal, err);
                return;
            }
        }

        // 处理网络/网关/协议层错误
        if (reply->error() != QNetworkReply::NoError) {
            err.code = static_cast<int>(ErrorCode::NetworkError);
            err.failureLayer = HttpFailureLayer::Network;
            QString rawErr = reply->errorString();
            if (httpStatus == 502 || rawErr.contains("Bad Gateway", Qt::CaseInsensitive)) {
                err.message = QCoreApplication::translate("MeetingUI", "502 Bad Gateway: The OpenMeeting server is not running. Run mage start on the server.");
            } else if (httpStatus == 504 || rawErr.contains("Gateway Timeout", Qt::CaseInsensitive)) {
                err.message = QCoreApplication::translate("MeetingUI", "504 Gateway Timeout: Check the server's network and load.");
            } else if (httpStatus == 404 || rawErr.contains("Not Found", Qt::CaseInsensitive)) {
                err.message = QCoreApplication::translate("MeetingUI", "404 Not Found: Check the server's routing configuration.");
            } else if (reply->error() == QNetworkReply::ConnectionRefusedError) {
                err.message = QCoreApplication::translate("MeetingUI", "Connection refused. Check the server IP address and port accessibility.");
            } else if (reply->error() == QNetworkReply::TimeoutError) {
                err.message = QCoreApplication::translate("MeetingUI", "Connection timed out. Check your network and firewall or security group settings.");
            } else if (reply->error() == QNetworkReply::RemoteHostClosedError || rawErr.contains("Connection closed", Qt::CaseInsensitive)) {
                err.message = QCoreApplication::translate("MeetingUI", "The server connection was interrupted. Check the HTTPS service.");
            } else {
                err.message = rawErr;
            }
            finish(false, QJsonValue(), err);
            return;
        }

        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            err.code = static_cast<int>(ErrorCode::ParseError);
            err.failureLayer = HttpFailureLayer::Parse;
            err.message = "Failed to parse response JSON: " + parseErr.errorString();
            finish(false, QJsonValue(), err);
            return;
        }

        QJsonObject root = doc.object();
        int errCode = root.value("errCode").toInt(-1);
        err.businessCode = errCode;
        QString errMsg = root.value("errMsg").toString();
        QJsonValue dataVal = root.value("data");

        if (errCode == 0) {
            err.code = 0;
            finish(true, dataVal, err);
        } else {
            err.code = errCode;
            err.message = errMsg;
            err.failureLayer = HttpFailureLayer::Business;
            if (err.isTokenExpired()) {
                expireCurrent();
            }
            finish(false, dataVal, err);
        }
    });
}

// -------------------------------------------------------------
// 业务 API 具体实现
// -------------------------------------------------------------

void OpenMeetingHttpClient::login(const QString &account, const QString &password, ResultCallback<UserInfo> callback) {
    const auto revision = ++_authRevision;
    const QPointer<OpenMeetingHttpClient> self(this);
    requestLogin(account, password, [self, revision, callback](bool ok, const UserInfo &user, const HttpError &err) {
        auto superseded = [&] {
            auto error = err;
            error.code = static_cast<int>(ErrorCode::UnknownError);
            error.message = QCoreApplication::translate("MeetingUI", "This sign-in attempt was canceled. Please sign in again.");
            if (callback) callback(false, UserInfo{}, error);
        };
        if (!self || self->_authRevision != revision) {
            superseded();
            return;
        }
        if (!ok) {
            if (callback) callback(false, UserInfo{}, err);
            return;
        }
        if (user.token.isEmpty() || user.userId.isEmpty()) {
            auto error = parseError(err, QCoreApplication::translate(
                "MeetingUI", "The sign-in response is missing required credentials."));
            if (callback) callback(false, UserInfo{}, error);
            return;
        }
        self->setCurrentUser(user);
        const auto committedRevision = self->_authRevision;
        emit self->userLoggedIn(user);
        if (!self || self->_authRevision != committedRevision) {
            superseded();
            return;
        }
        if (callback) callback(true, user, err);
    });
}

void OpenMeetingHttpClient::requestLogin(const QString &account, const QString &password, ResultCallback<UserInfo> callback) {
    QJsonObject body;
    body["account"] = account;
    body["password"] = password;

    sendPost("/user/login", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok) {
            if (callback) callback(false, UserInfo{}, err);
            return;
        }
        if (!data.isObject()) {
            auto error = parseError(err, QCoreApplication::translate(
                "MeetingUI", "The sign-in response contains invalid data."));
            if (callback) callback(false, UserInfo{}, error);
            return;
        }
        UserInfo user = UserInfo::fromJson(data.toObject());
        if (callback) callback(true, user, err);
    }, false);
}

void OpenMeetingHttpClient::registerUser(const QString &account, const QString &password,
        const QString &nickname, ResultCallback<UserInfo> callback, const QString &registrationBaseUrl) {
    QJsonObject body;
    body["account"] = account;
    body["password"] = password;
    body["nickname"] = nickname.isEmpty() ? account : nickname;

    // /user/register only imports meeting identities; it does not store account passwords.
    // Keep registration anonymous and leave the meeting/login endpoint and its auth state intact.
    sendPostToEndpoint(registrationBaseUrl.isEmpty() ? _baseUrl : registrationBaseUrl,
        "/admin/user/register", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok) {
            if (callback) callback(false, UserInfo{}, err);
            return;
        }
        UserInfo user;
        if (data.isObject()) {
            user = UserInfo::fromJson(data.toObject());
        }
        if (callback) callback(true, user, err);
    }, false);
}

void OpenMeetingHttpClient::logout(ResultCallback<bool> callback) {
    const auto revision = ++_authRevision;
    const auto stateRevision = _authStateRevision;
    const QPointer<OpenMeetingHttpClient> self(this);
    requestLogout([self, revision, stateRevision, callback](bool ok, bool result, const HttpError &err) {
        if (self && self->_authStateRevision == stateRevision) {
            // A newer login request alone has not replaced the old account.
            // Clear that account without cancelling the newer pending login.
            if (self->_authRevision == revision) ++self->_authRevision;
            self->commitCurrentUser({});
            emit self->userLoggedOut();
        }
        if (callback) callback(ok, result, err);
    });
}

void OpenMeetingHttpClient::requestLogout(ResultCallback<bool> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;

    // The session owner clears local authentication synchronously. This
    // response may belong to an account that has already been replaced.
    sendPost("/user/logout", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    });
}

void OpenMeetingHttpClient::createImmediateMeeting(
    const QString &title,
    int durationSeconds,
    ResultCallback<LiveKitAuthInfo> callback,
    HttpRequestContext context) {

    QJsonObject definedInfo;
    definedInfo["title"] = title;
    definedInfo["scheduledTime"] = QDateTime::currentSecsSinceEpoch();
    definedInfo["meetingDuration"] = durationSeconds;
    definedInfo["password"] = "";

    QJsonObject setting;
    setting["canParticipantsEnableCamera"] = true;
    setting["canParticipantsShareScreen"] = true;
    setting["canParticipantsUnmuteMicrophone"] = true;
    // Quick meetings allow each client to apply its persisted camera preference.
    setting["disableCameraOnJoin"] = false;
    setting["disableMicrophoneOnJoin"] = false;

    QJsonObject body;
    body["creatorUserID"] = _currentUser.userId;
    body["creatorDefinedMeetingInfo"] = definedInfo;
    body["setting"] = setting;

    sendPost("/meeting/create_immediate_meeting", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok || !data.isObject()) {
            if (callback) callback(false, LiveKitAuthInfo{}, err);
            return;
        }

        QJsonObject root = data.toObject();
        QJsonObject liveKit = root.value("liveKit").toObject();
        LiveKitAuthInfo auth;
        auth.url = liveKit.value("url").toString();
        auth.token = liveKit.value("token").toString();

        QJsonObject detailObj = root.value("detail").toObject();
        QJsonObject infoObj = detailObj.value("info").toObject();
        QJsonObject sysGenObj = infoObj.value("systemGenerated").toObject();
        auth.meetingId = sysGenObj.value("meetingID").toString();
        if (auth.meetingId.isEmpty()) {
            auth.meetingId = root.value("meetingID").toString();
        }

        if (callback) callback(true, auth, err);
    }, true, std::move(context));
}

void OpenMeetingHttpClient::joinMeeting(const QString &meetingId, const QString &password,
                                        ResultCallback<bool> callback, HttpRequestContext context) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;
    body["meetingID"] = meetingId;
    if (!password.isEmpty()) {
        body["password"] = password;
    }

    sendPost("/meeting/join_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    }, true, std::move(context));
}

void OpenMeetingHttpClient::getMeetingToken(const QString &meetingId,
                                            ResultCallback<LiveKitAuthInfo> callback,
                                            HttpRequestContext context) {
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;

    sendPost("/meeting/get_meeting_token", body, [callback, meetingId](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok || !data.isObject()) {
            if (callback) callback(false, LiveKitAuthInfo{}, err);
            return;
        }
        QJsonObject root = data.toObject();
        QJsonObject liveKit = root.value("liveKit").toObject();
        LiveKitAuthInfo auth;
        auth.url = liveKit.value("url").toString();
        auth.token = liveKit.value("token").toString();
        auth.meetingId = root.value("meetingID").toString();
        if (auth.meetingId.isEmpty()) {
            auth.meetingId = meetingId;
        }

        if (callback) callback(true, auth, err);
    }, true, std::move(context));
}

void OpenMeetingHttpClient::getMeetings(
        const std::vector<MeetingStatus> &statusList, ResultCallback<MeetingList> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;

    QJsonArray arr;
    for (const auto status : statusList) {
        const auto wire = meetingStatusToWire(status);
        if (!wire.isEmpty()) arr.append(wire);
    }
    body["status"] = arr;

    sendPost("/meeting/get_meetings", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok) {
            if (callback) callback(false, MeetingList{}, err);
            return;
        }
        if (!data.isObject()) {
            if (callback) callback(false, MeetingList{},
                parseError(err, QStringLiteral("Meeting list response data is not an object.")));
            return;
        }
        MeetingList meetings;
        QString message;
        if (!parseMeetingList(data.toObject().value(QStringLiteral("meetingDetails")),
                              &meetings, &message)) {
            if (callback) callback(false, MeetingList{}, parseError(err, message));
            return;
        }
        if (callback) callback(true, meetings, err);
    });
}

void OpenMeetingHttpClient::getMeetingInfo(
        const QString &meetingId, ResultCallback<MeetingCatalogDetail> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;
    body["meetingID"] = meetingId;

    sendPost("/meeting/get_meeting", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok) {
            if (callback) callback(false, MeetingCatalogDetail{}, err);
            return;
        }
        if (!data.isObject() || !data.toObject().value(QStringLiteral("meetingDetail")).isObject()) {
            if (callback) callback(false, MeetingCatalogDetail{},
                parseError(err, QStringLiteral("Meeting detail response structure is invalid.")));
            return;
        }
        MeetingCatalogDetail detail;
        QString message;
        if (!parseMeetingCatalogDetail(
                data.toObject().value(QStringLiteral("meetingDetail")).toObject(), &detail, &message)) {
            if (callback) callback(false, MeetingCatalogDetail{}, parseError(err, message));
            return;
        }
        if (callback) callback(true, detail, err);
    });
}

void OpenMeetingHttpClient::bookMeeting(
        const MeetingBookingRequest &request, ResultCallback<MeetingCatalogDetail> callback) {
    QString message;
    if (!validateMeetingBookingRequest(request, &message) || _currentUser.userId.isEmpty()) {
        if (_currentUser.userId.isEmpty()) message = QStringLiteral("Current user is missing.");
        const auto error = parseError({}, message);
        QTimer::singleShot(0, this, [callback, error] {
            if (callback) callback(false, MeetingCatalogDetail{}, error);
        });
        return;
    }
    const auto body = meetingBookingToJson(request, _currentUser.userId);
    sendPost("/meeting/book_meeting", body,
        [callback](bool ok, const QJsonValue &data, const HttpError &err) {
            if (!ok) {
                if (callback) callback(false, MeetingCatalogDetail{}, err);
                return;
            }
            if (!data.isObject() || !data.toObject().value(QStringLiteral("detail")).isObject()) {
                if (callback) callback(false, MeetingCatalogDetail{},
                    parseError(err, QStringLiteral("Booked meeting response structure is invalid.")));
                return;
            }
            MeetingCatalogDetail detail;
            QString message;
            if (!parseMeetingCatalogDetail(
                    data.toObject().value(QStringLiteral("detail")).toObject(), &detail, &message)) {
                if (callback) callback(false, MeetingCatalogDetail{}, parseError(err, message));
                return;
            }
            if (callback) callback(true, detail, err);
        });
}

void OpenMeetingHttpClient::updateMeeting(
        const MeetingUpdateRequest &request, ResultCallback<bool> callback) {
    QString message;
    if (!validateMeetingUpdateRequest(request, &message) || _currentUser.userId.isEmpty()) {
        if (_currentUser.userId.isEmpty()) message = QStringLiteral("Current user is missing.");
        const auto error = parseError({}, message);
        QTimer::singleShot(0, this, [callback, error] {
            if (callback) callback(false, false, error);
        });
        return;
    }
    const auto body = meetingUpdateToJson(request, _currentUser.userId);
    sendPost("/meeting/update_meeting", body,
        [callback](bool ok, const QJsonValue &, const HttpError &err) {
            if (callback) callback(ok, ok, err);
        });
}

void OpenMeetingHttpClient::cancelMeeting(
        const QString &meetingId, ResultCallback<bool> callback) {
    if (meetingId.isEmpty() || _currentUser.userId.isEmpty()) {
        const auto error = parseError({}, meetingId.isEmpty()
            ? QStringLiteral("Meeting ID is empty.") : QStringLiteral("Current user is missing."));
        QTimer::singleShot(0, this, [callback, error] {
            if (callback) callback(false, false, error);
        });
        return;
    }
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;
    body["endType"] = 0;
    sendPost("/meeting/end_meeting", body,
        [callback](bool ok, const QJsonValue &, const HttpError &err) {
            if (callback) callback(ok, ok, err);
        });
}

void OpenMeetingHttpClient::leaveMeeting(const QString &meetingId,
                                         ResultCallback<bool> callback,
                                         HttpRequestContext context) {
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;

    sendPost("/meeting/leave_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    }, true, std::move(context));
}

void OpenMeetingHttpClient::endMeeting(const QString &meetingId,
                                       ResultCallback<bool> callback,
                                       HttpRequestContext context) {
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;
    body["endType"] = 1; // EndType

    sendPost("/meeting/end_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    }, true, std::move(context));
}

} // namespace OpenMeeting
