#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <memory>
#include "http_types.h"

namespace OpenMeeting {

class OpenMeetingHttpClient : public QObject {
    Q_OBJECT
public:
    explicit OpenMeetingHttpClient(QObject *parent = nullptr);
    ~OpenMeetingHttpClient() override = default;

    // 单例访问便捷方法
    static OpenMeetingHttpClient &instance();

    // 基础配置
    void setBaseUrl(const QString &url);
    QString baseUrl() const { return _baseUrl; }

    void setToken(const QString &token);
    QString token() const { return _token; }

    void setCurrentUser(const UserInfo &user);
    UserInfo currentUser() const { return _currentUser; }
    bool isLoggedIn() const { return !_token.isEmpty() && !_currentUser.userId.isEmpty(); }

    // -------------------------------------------------------------
    // 业务 API 列表 (异步调用，基于 Qt 事件循环回调)
    // -------------------------------------------------------------

    // 1. 登录
    void login(const QString &account, const QString &password, ResultCallback<UserInfo> callback);

    // 1.1 注册
    void registerUser(const QString &account, const QString &password, const QString &nickname, ResultCallback<UserInfo> callback);

    // 2. 登出
    void logout(ResultCallback<bool> callback = nullptr);

    // 3. 创建即时会议 (返回 LiveKit url 与 token)
    void createImmediateMeeting(const QString &title, int durationSeconds, ResultCallback<LiveKitAuthInfo> callback);

    // 4. 加入会议 (第一阶段：业务校验)
    void joinMeeting(const QString &meetingId, const QString &password, ResultCallback<bool> callback);

    // 5. 换取会议 LiveKit Token (第二阶段：凭据获取)
    void getMeetingToken(const QString &meetingId, ResultCallback<LiveKitAuthInfo> callback);

    // 6. 查询会议列表 (statusList: 1-待开始, 2-进行中, 3-已结束)
    void getMeetings(const std::vector<int> &statusList, ResultCallback<QJsonArray> callback);

    // 7. 查询单个会议详情
    void getMeetingInfo(const QString &meetingId, ResultCallback<QJsonObject> callback);

    // 8. 离开会议
    void leaveMeeting(const QString &meetingId, ResultCallback<bool> callback);

    // 9. 结束全员会议 (主持人)
    void endMeeting(const QString &meetingId, ResultCallback<bool> callback);

signals:
    void tokenExpired();
    void userLoggedIn(const UserInfo &user);
    void userLoggedOut();

private:
    void sendPost(const QString &path,
                  const QJsonObject &body,
                  std::function<void(bool ok, const QJsonValue &data, const HttpError &err)> cb);

    QString _baseUrl = "http://116.205.175.233:11102";
    QString _token;
    UserInfo _currentUser;
    std::unique_ptr<QNetworkAccessManager> _nam;
};

} // namespace OpenMeeting
