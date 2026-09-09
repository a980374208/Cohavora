#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QSettings>
#include <functional>
#include <memory>
#include "src/net/http_types.h"
#include "src/net/openmeeting_http_client.h"

namespace OpenMeeting {

struct MediaPreferences {
    bool enableMicrophone = true;
    bool enableSpeaker = true;
    bool enableVideo = false;
    bool videoIsMirroring = false;
};

class SessionManager : public QObject {
    Q_OBJECT
public:
    static SessionManager &instance();

    // 会话与用户信息
    bool isLoggedIn() const;
    const UserInfo &currentUser() const;
    QString token() const;
    QString userId() const;
    QString nickname() const;

    // 持久化的登录与网络配置
    QString savedAccount() const { return _savedAccount; }
    QString savedPassword() const { return _savedPassword; }
    bool isRememberPassword() const { return _rememberPassword; }
    bool isAutoLogin() const { return _autoLogin; }
    QString serverBaseUrl() const { return _serverBaseUrl; }

    // 媒体首选项
    const MediaPreferences &mediaPreferences() const { return _mediaPrefs; }
    void setMediaPreferences(const MediaPreferences &prefs);
    void setEnableMicrophone(bool enable);
    void setEnableSpeaker(bool enable);
    void setEnableVideo(bool enable);
    void setVideoMirroring(bool enable);

    // HTTP 客户端获取
    OpenMeetingHttpClient &httpClient();

    // 登录业务操作
    void setServerBaseUrl(const QString &url);
    void loginWithPassword(const QString &account,
                           const QString &password,
                           bool remember,
                           bool autoLogin,
                           std::function<void(bool success, const QString &errMsg)> callback);

    void registerUser(const QString &account,
                      const QString &password,
                      const QString &nickname,
                      std::function<void(bool success, const QString &errMsg, const UserInfo &user)> callback);

    // 访客/离线调试入会模式
    void loginAsGuest(const QString &nickname, const QString &customUserId = QString());

    // 登出
    void logout();

    // 加载与持久化
    void loadFromSettings();
    void saveToSettings();

signals:
    void loggedIn(const UserInfo &user);
    void loggedOut();
    void sessionExpired();
    void preferencesChanged(const MediaPreferences &prefs);

private:
    explicit SessionManager(QObject *parent = nullptr);
    ~SessionManager() override = default;

    UserInfo _currentUser;
    MediaPreferences _mediaPrefs;
    QString _savedAccount;
    QString _savedPassword;
    bool _rememberPassword = false;
    bool _autoLogin = false;
    QString _serverBaseUrl = "http://116.205.175.233:11102";

    std::unique_ptr<QSettings> _settings;
};

} // namespace OpenMeeting
