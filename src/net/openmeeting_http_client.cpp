#include "openmeeting_http_client.h"
#include <QtCore/QUrl>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkProxy>

namespace OpenMeeting {

OpenMeetingHttpClient::OpenMeetingHttpClient(QObject *parent)
    : QObject(parent)
    , _nam(std::make_unique<QNetworkAccessManager>(this)) {
    // 针对音视频会议服务直连，避免本地 HTTP 代理软件（如 Clash 127.0.0.1:7890）误拦截非标端口造成 502 错误
    _nam->setProxy(QNetworkProxy::NoProxy);
}

OpenMeetingHttpClient &OpenMeetingHttpClient::instance() {
    static OpenMeetingHttpClient s_instance;
    return s_instance;
}

void OpenMeetingHttpClient::setBaseUrl(const QString &url) {
    auto effectiveUrl = url;
    if (effectiveUrl.endsWith('/')) effectiveUrl.chop(1);
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
    bool authenticated) {

    QString fullUrl;
    if (path.startsWith("http://", Qt::CaseInsensitive) || path.startsWith("https://", Qt::CaseInsensitive)) {
        fullUrl = path;
    } else {
        fullUrl = _baseUrl + (path.startsWith('/') ? path : ("/" + path));
    }
    QNetworkRequest request{QUrl(fullUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // 注入全链路追踪 operationID (毫秒时间戳)
    QString opId = QString::number(QDateTime::currentMSecsSinceEpoch());
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
    connect(reply, &QNetworkReply::finished, this, [reply, opId, cb, expireCurrent]() {
        reply->deleteLater();

        HttpError err;
        err.operationId = opId;

        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray responseData = reply->readAll();

        // 优先检查响应体中是否含有服务端返回的 JSON 业务错误信息 (即使 HTTP 状态码非 200)
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseErr);
        if (parseErr.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject root = doc.object();
            int errCode = root.value("errCode").toInt(-1);
            QString errMsg = root.value("errMsg").toString();
            QJsonValue dataVal = root.value("data");

            if (errCode == 0 && reply->error() == QNetworkReply::NoError) {
                err.code = 0;
                if (cb) cb(true, dataVal, err);
                return;
            } else if (!errMsg.isEmpty() || errCode != -1) {
                err.code = (errCode != -1) ? errCode : static_cast<int>(ErrorCode::NetworkError);
                err.message = errMsg;
                if (err.isTokenExpired()) {
                    expireCurrent();
                }
                if (cb) cb(false, dataVal, err);
                return;
            }
        }

        // 处理网络/网关/协议层错误
        if (reply->error() != QNetworkReply::NoError) {
            err.code = static_cast<int>(ErrorCode::NetworkError);
            QString rawErr = reply->errorString();
            if (httpStatus == 502 || rawErr.contains("Bad Gateway", Qt::CaseInsensitive)) {
                err.message = QString::fromUtf8("服务器网关错误 (502 Bad Gateway)：OpenMeeting 服务端进程未启动，请在服务器执行 mage start");
            } else if (httpStatus == 504 || rawErr.contains("Gateway Timeout", Qt::CaseInsensitive)) {
                err.message = QString::fromUtf8("网关响应超时 (504 Gateway Timeout)：请检查服务器网络负载");
            } else if (httpStatus == 404 || rawErr.contains("Not Found", Qt::CaseInsensitive)) {
                err.message = QString::fromUtf8("请求接口不存在 (404 Not Found)：请检查服务器路由配置");
            } else if (reply->error() == QNetworkReply::ConnectionRefusedError) {
                err.message = QString::fromUtf8("无法连接到服务器：连接被拒绝，请检查服务器 IP 和端口是否正确开放");
            } else if (reply->error() == QNetworkReply::TimeoutError) {
                err.message = QString::fromUtf8("连接服务器超时，请检查网络或安全组防火墙设置");
            } else if (reply->error() == QNetworkReply::RemoteHostClosedError || rawErr.contains("Connection closed", Qt::CaseInsensitive)) {
                err.message = QString::fromUtf8("服务器连接中断 (Connection closed)：请确认阿里云安全组已放行 11102 和 11022 端口");
            } else {
                err.message = rawErr;
            }
            if (cb) cb(false, QJsonValue(), err);
            return;
        }

        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            err.code = static_cast<int>(ErrorCode::ParseError);
            err.message = "Failed to parse response JSON: " + parseErr.errorString();
            if (cb) cb(false, QJsonValue(), err);
            return;
        }

        QJsonObject root = doc.object();
        int errCode = root.value("errCode").toInt(-1);
        QString errMsg = root.value("errMsg").toString();
        QJsonValue dataVal = root.value("data");

        if (errCode == 0) {
            err.code = 0;
            if (cb) cb(true, dataVal, err);
        } else {
            err.code = errCode;
            err.message = errMsg;
            if (err.isTokenExpired()) {
                expireCurrent();
            }
            if (cb) cb(false, dataVal, err);
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
            error.message = QString::fromUtf8("本次登录已取消，请重新登录。");
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
            auto error = err;
            error.code = static_cast<int>(ErrorCode::ParseError);
            error.message = QString::fromUtf8("登录响应缺少必要凭据。");
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
            auto error = err;
            error.code = static_cast<int>(ErrorCode::ParseError);
            error.message = QString::fromUtf8("登录响应数据格式无效。");
            if (callback) callback(false, UserInfo{}, error);
            return;
        }
        UserInfo user = UserInfo::fromJson(data.toObject());
        if (callback) callback(true, user, err);
    }, false);
}

void OpenMeetingHttpClient::registerUser(const QString &account, const QString &password, const QString &nickname, ResultCallback<UserInfo> callback) {
    QJsonObject body;
    body["account"] = account;
    body["password"] = password;
    body["nickname"] = nickname.isEmpty() ? account : nickname;

    // OpenMeeting 服务端注册接口位于 openmeeting-admin-api (端口 11022，路由 /admin/user/register)
    QString regUrl = _baseUrl;
    if (regUrl.contains(":11102")) {
        regUrl.replace(":11102", ":11022");
    }
    QString targetPath = regUrl.contains(":11022") ? (regUrl + "/admin/user/register") : "/user/register";

    sendPost(targetPath, body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
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
    ResultCallback<LiveKitAuthInfo> callback) {

    QJsonObject definedInfo;
    definedInfo["title"] = title;
    definedInfo["scheduledTime"] = QDateTime::currentSecsSinceEpoch();
    definedInfo["meetingDuration"] = durationSeconds;
    definedInfo["password"] = "";

    QJsonObject setting;
    setting["canParticipantsEnableCamera"] = true;
    setting["canParticipantsShareScreen"] = true;
    setting["canParticipantsUnmuteMicrophone"] = true;
    setting["disableCameraOnJoin"] = true;
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
    });
}

void OpenMeetingHttpClient::joinMeeting(const QString &meetingId, const QString &password, ResultCallback<bool> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;
    body["meetingID"] = meetingId;
    if (!password.isEmpty()) {
        body["password"] = password;
    }

    sendPost("/meeting/join_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    });
}

void OpenMeetingHttpClient::getMeetingToken(const QString &meetingId, ResultCallback<LiveKitAuthInfo> callback) {
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
    });
}

void OpenMeetingHttpClient::getMeetings(const std::vector<int> &statusList, ResultCallback<QJsonArray> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;

    QJsonArray arr;
    for (int st : statusList) {
        if (st == 1) arr.append("Scheduled");
        else if (st == 2) arr.append("In-Progress");
        else if (st == 3) arr.append("Completed");
    }
    body["status"] = arr;

    sendPost("/meeting/get_meetings", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok || !data.isObject()) {
            if (callback) callback(false, QJsonArray{}, err);
            return;
        }
        QJsonArray details = data.toObject().value("meetingDetails").toArray();
        if (callback) callback(true, details, err);
    });
}

void OpenMeetingHttpClient::getMeetingInfo(const QString &meetingId, ResultCallback<QJsonObject> callback) {
    QJsonObject body;
    body["userID"] = _currentUser.userId;
    body["meetingID"] = meetingId;

    sendPost("/meeting/get_meeting", body, [callback](bool ok, const QJsonValue &data, const HttpError &err) {
        if (!ok || !data.isObject()) {
            if (callback) callback(false, QJsonObject{}, err);
            return;
        }
        QJsonObject detail = data.toObject().value("meetingDetail").toObject();
        if (callback) callback(true, detail, err);
    });
}

void OpenMeetingHttpClient::leaveMeeting(const QString &meetingId, ResultCallback<bool> callback) {
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;

    sendPost("/meeting/leave_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    });
}

void OpenMeetingHttpClient::endMeeting(const QString &meetingId, ResultCallback<bool> callback) {
    QJsonObject body;
    body["meetingID"] = meetingId;
    body["userID"] = _currentUser.userId;
    body["endType"] = 1; // EndType

    sendPost("/meeting/end_meeting", body, [callback](bool ok, const QJsonValue &, const HttpError &err) {
        if (callback) callback(ok, ok, err);
    });
}

} // namespace OpenMeeting
