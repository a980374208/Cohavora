#pragma once

#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <functional>
#include <string>

namespace OpenMeeting {

enum class ErrorCode {
    Success = 0,
    NetworkError = -1,
    ParseError = -2,
    InvalidServiceUrl = -3,
    InsecureTransport = -4,
    RedirectRejected = -5,
    InvalidCredentials = -6,
    // OpenMeeting uses openimsdk/tools/errs for token validation errors.
    // Its application codes 100002/100010 mean password error/session replaced.
    TokenExpired = 1501,
    TokenInvalid = 1502,
    TokenMalformed = 1503,
    TokenNotValidYet = 1504,
    TokenUnknown = 1505,
    TokenKicked = 1506,
    TokenNotExist = 1507,
    TokenNotFound = 100004,
    SessionReplaced = 100010,
    UnknownError = -999
};

enum class HttpFailureLayer { None, Policy, Http, Network, Business, Parse };

struct HttpRequestContext {
    QString anonymousSessionId;
    QString parentOperationId;
};

struct HttpError {
    int code = 0;
    QString message;
    QString operationId;
    QString requestId;
    QString parentOperationId;
    QString anonymousSessionId;
    int httpStatus = 0;
    int networkError = 0;
    int businessCode = 0;
    HttpFailureLayer failureLayer = HttpFailureLayer::None;

    bool isTokenExpired() const {
        switch (static_cast<ErrorCode>(code)) {
        case ErrorCode::TokenExpired:
        case ErrorCode::TokenInvalid:
        case ErrorCode::TokenMalformed:
        case ErrorCode::TokenNotValidYet:
        case ErrorCode::TokenUnknown:
        case ErrorCode::TokenKicked:
        case ErrorCode::TokenNotExist:
        case ErrorCode::TokenNotFound:
        case ErrorCode::SessionReplaced:
            return true;
        default:
            return false;
        }
    }
};

struct UserInfo {
    QString token;
    QString userId;
    QString nickname;
    QString faceURL;

    static UserInfo fromJson(const QJsonObject &obj) {
        UserInfo info;
        info.token = obj.value("token").toString();
        info.userId = obj.value("userID").toString();
        info.nickname = obj.value("nickname").toString();
        info.faceURL = obj.value("faceURL").toString();
        return info;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["token"] = token;
        obj["userID"] = userId;
        obj["nickname"] = nickname;
        obj["faceURL"] = faceURL;
        return obj;
    }
};

struct LiveKitAuthInfo {
    QString url;
    QString token;
    QString meetingId;
};

// 通用异步回调函数模板
template <typename T>
using ResultCallback = std::function<void(bool success, const T &result, const HttpError &error)>;

} // namespace OpenMeeting
