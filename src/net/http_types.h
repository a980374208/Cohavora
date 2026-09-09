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
    TokenExpired = 100010,
    TokenInvalid = 100002,
    UnknownError = -999
};

struct HttpError {
    int code = 0;
    QString message;
    QString operationId;

    bool isTokenExpired() const {
        return code == static_cast<int>(ErrorCode::TokenExpired) ||
               code == static_cast<int>(ErrorCode::TokenInvalid);
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
