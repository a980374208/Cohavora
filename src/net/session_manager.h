#pragma once

#include <QtCore/QObject>
#include <QtCore/QMetaType>
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

// 业务账号会话的失效原因。它与 LiveKit 房间连接的断开原因严格分离：
// 前者负责清理全局认证状态，后者只影响当前会议。
enum class SessionInvalidationReason {
    Unknown,
    DuplicatedLogin,
    TokenExpired,
    TokenInvalid,
    ServerLogout,
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

    // 统一清理内存与持久化登录态。Token 已失效时不再请求业务端 logout。
    void logout(bool notifyServer = true);

    // 由可信业务服务端事件或 HTTP 鉴权失败触发的全局失效入口。该调用幂等，
    // 并统一复用 logout(false) 清理 HTTP token 和持久化用户信息。
    void invalidateSession(SessionInvalidationReason reason);
    bool isSessionInvalidating() const { return _sessionInvalidating; }

    // 加载与持久化
    void loadFromSettings();
    void saveToSettings();

signals:
    void loggedIn(const UserInfo &user);
    void loggedOut();
    void sessionInvalidated(SessionInvalidationReason reason);
    // 兼容已有 TokenExpired/TokenInvalid UI 订阅；新逻辑优先监听
    // sessionInvalidated，以便同时处理重复登录等业务失效原因。
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
    bool _sessionInvalidating = false;
    QString _serverBaseUrl = "http://116.205.175.233:11102";

    std::unique_ptr<QSettings> _settings;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::SessionInvalidationReason)
