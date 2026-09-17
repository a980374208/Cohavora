#include "src/net/session_manager.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QUuid>
#include <QtCore/QDebug>
#include <stdexcept>
#include <utility>

namespace OpenMeeting {

SessionManager::SessionManager(QObject *parent)
    : SessionManager(std::make_unique<QSettings>("OpenMeeting", "LiveKitClient"), parent) {
}

SessionManager::SessionManager(std::unique_ptr<QSettings> settings, QObject *parent)
    : QObject(parent)
    , _settings(std::move(settings)) {
    if (!_settings) {
        throw std::invalid_argument("SessionManager requires explicit settings storage");
    }
    qRegisterMetaType<SessionInvalidationReason>("OpenMeeting::SessionInvalidationReason");

    // 监听网络客户端的 Token 失效信号
    connect(&httpClient(), &OpenMeetingHttpClient::tokenExpired, this, [this]() {
        if (!isLoggedIn() && httpClient().token().isEmpty()) {
            return;
        }
        invalidateSession(SessionInvalidationReason::TokenExpired);
    });

    loadFromSettings();
}

SessionManager &SessionManager::instance() {
    static SessionManager inst;
    return inst;
}

OpenMeetingHttpClient &SessionManager::httpClient() {
    return OpenMeetingHttpClient::instance();
}

bool SessionManager::isLoggedIn() const {
    return !_currentUser.token.isEmpty() && !_currentUser.userId.isEmpty();
}

const UserInfo &SessionManager::currentUser() const {
    return _currentUser;
}

QString SessionManager::token() const {
    return _currentUser.token;
}

QString SessionManager::userId() const {
    return _currentUser.userId;
}

QString SessionManager::nickname() const {
    if (!_currentUser.nickname.isEmpty()) {
        return _currentUser.nickname;
    }
    return _currentUser.userId.isEmpty() ? QString::fromUtf8("未登录") : _currentUser.userId;
}

void SessionManager::setMediaPreferences(const MediaPreferences &prefs) {
    _mediaPrefs = prefs;
    saveToSettings();
    emit preferencesChanged(_mediaPrefs);
}

void SessionManager::setEnableMicrophone(bool enable) {
    if (_mediaPrefs.enableMicrophone != enable) {
        _mediaPrefs.enableMicrophone = enable;
        saveToSettings();
        emit preferencesChanged(_mediaPrefs);
    }
}

void SessionManager::setEnableSpeaker(bool enable) {
    if (_mediaPrefs.enableSpeaker != enable) {
        _mediaPrefs.enableSpeaker = enable;
        saveToSettings();
        emit preferencesChanged(_mediaPrefs);
    }
}

void SessionManager::setEnableVideo(bool enable) {
    if (_mediaPrefs.enableVideo != enable) {
        _mediaPrefs.enableVideo = enable;
        saveToSettings();
        emit preferencesChanged(_mediaPrefs);
    }
}

void SessionManager::setVideoMirroring(bool enable) {
    if (_mediaPrefs.videoIsMirroring != enable) {
        _mediaPrefs.videoIsMirroring = enable;
        saveToSettings();
        emit preferencesChanged(_mediaPrefs);
    }
}

void SessionManager::setServerBaseUrl(const QString &url) {
    if (_serverBaseUrl != url && !url.isEmpty()) {
        _serverBaseUrl = url;
        httpClient().setBaseUrl(url);
        saveToSettings();
    }
}

void SessionManager::loginWithPassword(const QString &account,
                                       const QString &password,
                                       bool remember,
                                       bool autoLogin,
                                       std::function<void(bool success, const QString &errMsg)> callback) {
    httpClient().login(account, password, [this, account, password, remember, autoLogin, callback](bool ok, const UserInfo &info, const HttpError &err) {
        if (ok) {
            _sessionInvalidating = false;
            _currentUser = info;
            _savedAccount = account;
            _savedPassword = remember ? password : "";
            _rememberPassword = remember;
            _autoLogin = autoLogin;
            httpClient().setToken(info.token);
            httpClient().setCurrentUser(info);
            saveToSettings();
            emit loggedIn(_currentUser);
            if (callback) callback(true, QString());
        } else {
            if (callback) callback(false, err.message);
        }
    });
}

void SessionManager::registerUser(const QString &account,
                                  const QString &password,
                                  const QString &nickname,
                                  std::function<void(bool success, const QString &errMsg, const UserInfo &user)> callback) {
    httpClient().registerUser(account, password, nickname, [callback](bool ok, const UserInfo &user, const HttpError &err) {
        if (ok) {
            if (callback) callback(true, QString(), user);
        } else {
            if (callback) callback(false, err.message, UserInfo{});
        }
    });
}

void SessionManager::loginAsGuest(const QString &nickname, const QString &customUserId) {
    _sessionInvalidating = false;
    _currentUser.userId = customUserId.isEmpty()
        ? QString("guest_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(8))
        : customUserId;
    _currentUser.nickname = nickname.isEmpty() ? QString::fromUtf8("访客用户") : nickname;
    _currentUser.token = QString("guest_token_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    _currentUser.faceURL = "";

    httpClient().setToken(_currentUser.token);
    httpClient().setCurrentUser(_currentUser);
    emit loggedIn(_currentUser);
}

void SessionManager::logout(bool notifyServer) {
    if (notifyServer && isLoggedIn()) {
        httpClient().logout();
    }

    _currentUser = UserInfo();
    _autoLogin = false;
    // OpenMeetingHttpClient::setCurrentUser(empty) 不会覆盖已有 token，
    // 因而必须显式清空 token，避免后续 REST 请求继续携带失效凭据。
    httpClient().setToken(QString());
    httpClient().setCurrentUser(UserInfo{});

    if (_settings) {
        _settings->setValue("auth/autoLogin", false);
        _settings->remove("user");
        _settings->sync();
    }
    emit loggedOut();
}

void SessionManager::invalidateSession(SessionInvalidationReason reason) {
    if (reason == SessionInvalidationReason::Unknown) {
        qWarning() << "[SessionManager] Ignore session invalidation with unknown reason.";
        return;
    }
    if (_sessionInvalidating || (!isLoggedIn() && httpClient().token().isEmpty())) {
        qInfo() << "[SessionManager] Ignore duplicate/stale session invalidation:" << static_cast<int>(reason);
        return;
    }

    _sessionInvalidating = true;
    qWarning() << "[SessionManager] Invalidate complete local session, reason="
               << static_cast<int>(reason);

    // 已被服务端撤销的 token 不得再请求 /user/logout；统一复用本地清理路径，
    // 确保 HTTP client、内存用户和 QSettings 三处状态同步归零。
    logout(false);
    emit sessionInvalidated(reason);

    if (reason == SessionInvalidationReason::TokenExpired ||
        reason == SessionInvalidationReason::TokenInvalid) {
        emit sessionExpired();
    }
}

void SessionManager::loadFromSettings() {
    if (!_settings) return;

    _savedAccount = _settings->value("auth/account", "").toString();
    _savedPassword = _settings->value("auth/password", "").toString();
    _rememberPassword = _settings->value("auth/rememberPassword", false).toBool();
    _autoLogin = _settings->value("auth/autoLogin", false).toBool();
    _serverBaseUrl = _settings->value("network/serverBaseUrl", "http://123.56.225.164:11102").toString();

    _mediaPrefs.enableMicrophone = _settings->value("media/enableMicrophone", true).toBool();
    _mediaPrefs.enableSpeaker = _settings->value("media/enableSpeaker", true).toBool();
    _mediaPrefs.enableVideo = _settings->value("media/enableVideo", false).toBool();
    _mediaPrefs.videoIsMirroring = _settings->value("media/videoMirroring", false).toBool();

    httpClient().setBaseUrl(_serverBaseUrl);

    if (_rememberPassword) {
        _currentUser.token = _settings->value("user/token", "").toString();
        _currentUser.userId = _settings->value("user/userId", "").toString();
        _currentUser.nickname = _settings->value("user/nickname", "").toString();
        _currentUser.faceURL = _settings->value("user/faceURL", "").toString();

        if (!_currentUser.token.isEmpty()) {
            httpClient().setToken(_currentUser.token);
            httpClient().setCurrentUser(_currentUser);
        }
    }
}

void SessionManager::saveToSettings() {
    if (!_settings) return;

    _settings->setValue("auth/account", _savedAccount);
    _settings->setValue("auth/password", _rememberPassword ? _savedPassword : "");
    _settings->setValue("auth/rememberPassword", _rememberPassword);
    _settings->setValue("auth/autoLogin", _autoLogin);
    _settings->setValue("network/serverBaseUrl", _serverBaseUrl);

    _settings->setValue("media/enableMicrophone", _mediaPrefs.enableMicrophone);
    _settings->setValue("media/enableSpeaker", _mediaPrefs.enableSpeaker);
    _settings->setValue("media/enableVideo", _mediaPrefs.enableVideo);
    _settings->setValue("media/videoMirroring", _mediaPrefs.videoIsMirroring);

    if (isLoggedIn() && _rememberPassword) {
        _settings->setValue("user/token", _currentUser.token);
        _settings->setValue("user/userId", _currentUser.userId);
        _settings->setValue("user/nickname", _currentUser.nickname);
        _settings->setValue("user/faceURL", _currentUser.faceURL);
    }
    _settings->sync();
}

} // namespace OpenMeeting
