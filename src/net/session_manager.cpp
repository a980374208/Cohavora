#include "src/net/session_manager.h"
#include "src/net/service_endpoint_policy.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QUuid>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <stdexcept>
#include <utility>

namespace OpenMeeting {

SessionManager::SessionManager(QObject *parent)
    : SessionManager(std::make_unique<QSettings>("OpenMeeting", "LiveKitClient"), parent) {
}

SessionManager::SessionManager(std::unique_ptr<QSettings> settings, QObject *parent)
    : SessionManager(std::move(settings), nullptr, nullptr, parent) {
}

SessionManager::SessionManager(std::unique_ptr<QSettings> settings,
                               std::unique_ptr<CredentialStore> credentials,
                               OpenMeetingHttpClient *client, QObject *parent)
    : QObject(parent)
    , _settings(std::move(settings))
    , _credentials(std::move(credentials))
    , _client(client ? client : &OpenMeetingHttpClient::instance()) {
    if (!_settings) {
        throw std::invalid_argument("SessionManager requires explicit settings storage");
    }
    if (!_credentials) _credentials = makeCredentialStore(*_settings);
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
    return *_client;
}

bool SessionManager::isLoggedIn() const {
    return !_currentUser.token.isEmpty() && !_currentUser.userId.isEmpty();
}

bool SessionManager::canPersistSession() const {
    return serviceAllowsCredentialPersistence(_serverBaseUrl);
}

bool SessionManager::isDebugHttp() const {
    return evaluateServiceEndpoint(_serverBaseUrl).isDebugHttp();
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

bool SessionManager::setServerBaseUrl(const QString &url) {
    const auto policy = evaluateServiceEndpoint(url);
    if (!policy.requestAllowed()) return false;
    const auto &service = policy.canonicalUrl;
    if (_serverBaseUrl != service) {
        resetAuthentication();
        forgetSavedSession();
        _serverBaseUrl = service;
        httpClient().setBaseUrl(service);
        saveToSettings();
    }
    return true;
}

void SessionManager::resetAuthentication() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++_authGeneration;
    _loginPending = false;
    _currentUser = {};
    httpClient().setCurrentUser({});
}

void SessionManager::cancelPendingLogin() {
    if (_loginPending) resetAuthentication();
}

bool SessionManager::forgetSavedSession() {
    if (_loginPending) resetAuthentication();
    _savedSession.reset();
    _rememberSession = false;
    _autoLogin = false;
    _credentialStatus = _credentials->clear();
    return _credentialStatus == CredentialStatus::Empty;
}

bool SessionManager::announceLogin(quint64 generation) {
    const QPointer<SessionManager> self(this);
    const auto user = _currentUser;
    emit httpClient().userLoggedIn(user);
    if (!self || _authGeneration != generation) return false;
    emit loggedIn(user);
    return self && self->_authGeneration == generation && self->isLoggedIn();
}

bool SessionManager::resumeSavedSession(bool automatic, std::optional<bool> autoLoginChoice) {
    if (!canPersistSession() || !_savedSession || (automatic && !_autoLogin) || _loginPending) return false;
    // Recheck the tombstone and binding at activation, not just at startup.
    auto loaded = _credentials->load(_serverBaseUrl, _savedAccount);
    _credentialStatus = loaded.status;
    if (loaded.status != CredentialStatus::Ready || (automatic && !loaded.session.autoLogin)) {
        _savedSession.reset();
        _rememberSession = _autoLogin = false;
        return false;
    }
    resetAuthentication();
    _sessionInvalidating = false;
    _savedSession = std::move(loaded.session);
    _currentUser = _savedSession->user;
    _rememberSession = true;
    _autoLogin = _savedSession->autoLogin;
    if (autoLoginChoice && *autoLoginChoice != _autoLogin) {
        _savedSession->autoLogin = *autoLoginChoice;
        _credentialStatus = _credentials->save(*_savedSession);
        if (_credentialStatus == CredentialStatus::Ready) {
            _autoLogin = *autoLoginChoice;
        } else {
            _savedSession.reset();
            _rememberSession = _autoLogin = false;
        }
    }
    httpClient().setCurrentUser(_currentUser);
    return announceLogin(_authGeneration);
}

void SessionManager::loginWithPassword(const QString &account,
                                       const QString &password,
                                       bool remember,
                                       bool autoLogin,
                                       std::function<void(bool success, const QString &errMsg)> callback) {
    resetAuthentication();
    forgetSavedSession();
    const auto generation = _authGeneration;
    const auto service = _serverBaseUrl;
    const auto endpoint = evaluateServiceEndpoint(service);
    if (!endpoint.requestAllowed()) {
        if (callback) callback(false, serviceEndpointErrorMessage(endpoint.status));
        return;
    }
    const bool persist = remember && serviceAllowsCredentialPersistence(service);
    const bool persistAutomatically = persist && autoLogin;
    _loginPending = true;
    const QPointer<SessionManager> self(this);
    const auto httpRevision = httpClient().authRevision();
    httpClient().requestLogin(account, password,
        [self, generation, httpRevision, service, account, persist, persistAutomatically, callback]
        (bool ok, const UserInfo &info, const HttpError &err) {
            const auto superseded = QString::fromUtf8("本次登录已取消，请重新登录。");
            if (!self || self->_authGeneration != generation || !self->_loginPending ||
                self->_serverBaseUrl != service || self->httpClient().authRevision() != httpRevision ||
                self->httpClient().baseUrl() != service) {
                if (callback) callback(false, superseded);
                return;
            }
            self->_loginPending = false;
            if (!ok || info.token.isEmpty() || info.userId.isEmpty()) {
                if (callback) callback(false, ok ? QString::fromUtf8("登录响应缺少必要凭据。") : err.message);
                return;
            }
            self->_sessionInvalidating = false;
            self->_currentUser = info;
            self->_savedAccount = account;
            self->saveToSettings();
            if (persist) {
                StoredSession record{service, account, info, persistAutomatically};
                self->_credentialStatus = self->_credentials->save(record);
                if (self->_credentialStatus == CredentialStatus::Ready) {
                    self->_savedSession = std::move(record);
                    self->_rememberSession = true;
                    self->_autoLogin = persistAutomatically;
                }
            }
            self->httpClient().setCurrentUser(info);
            const bool committed = self->announceLogin(generation);
            if (callback) callback(committed, committed ? QString() : superseded);
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
    resetAuthentication();
    forgetSavedSession();
    _sessionInvalidating = false;
    _currentUser.userId = customUserId.isEmpty()
        ? QString("guest_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(8))
        : customUserId;
    _currentUser.nickname = nickname.isEmpty() ? QString::fromUtf8("访客用户") : nickname;
    _currentUser.token = QString("guest_token_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    _currentUser.faceURL = "";

    httpClient().setCurrentUser(_currentUser);
    announceLogin(_authGeneration);
}

void SessionManager::logout(bool notifyServer) {
    if (notifyServer && isLoggedIn()) {
        httpClient().requestLogout();
    }

    resetAuthentication();
    forgetSavedSession();
    const auto generation = _authGeneration;
    const QPointer<SessionManager> self(this);
    emit httpClient().userLoggedOut();
    if (!self || self->_authGeneration != generation) return;
    emit loggedOut();
}

void SessionManager::invalidateSession(SessionInvalidationReason reason) {
    if (reason == SessionInvalidationReason::Unknown) {
        qWarning() << "[SessionManager] Ignore session invalidation with unknown reason.";
        return;
    }
    if (_sessionInvalidating || (!_loginPending && !isLoggedIn() && httpClient().token().isEmpty())) {
        qInfo() << "[SessionManager] Ignore duplicate/stale session invalidation:" << static_cast<int>(reason);
        return;
    }

    _sessionInvalidating = true;
    qWarning() << "[SessionManager] Invalidate complete local session, reason="
               << static_cast<int>(reason);

    // 已被服务端撤销的 token 不得再请求 /user/logout；统一复用本地清理路径，
    // 确保 HTTP client、内存用户和 QSettings 三处状态同步归零。
    const QPointer<SessionManager> self(this);
    const auto invalidatedGeneration = _authGeneration + 1;
    logout(false);
    if (!self || self->_authGeneration != invalidatedGeneration) return;
    emit sessionInvalidated(reason);
    if (!self || self->_authGeneration != invalidatedGeneration) return;

    if (reason == SessionInvalidationReason::TokenExpired ||
        reason == SessionInvalidationReason::TokenInvalid) {
        emit sessionExpired();
    }
}

void SessionManager::loadFromSettings() {
    if (_settingsLoaded) return;
    _settingsLoaded = true;

    _savedAccount = _settings->value("auth/account", "").toString();
    const auto configuredEndpoint = evaluateServiceEndpoint(
        _settings->value("network/serverBaseUrl", QString()).toString());
    const bool recognizedEndpoint = configuredEndpoint.status != ServiceEndpointStatus::Unconfigured &&
        configuredEndpoint.status != ServiceEndpointStatus::InvalidUrl;
    _serverBaseUrl = recognizedEndpoint ? configuredEndpoint.canonicalUrl : QString();

    _mediaPrefs.enableMicrophone = _settings->value("media/enableMicrophone", true).toBool();
    _mediaPrefs.enableSpeaker = _settings->value("media/enableSpeaker", true).toBool();
    _mediaPrefs.enableVideo = _settings->value("media/enableVideo", false).toBool();
    _mediaPrefs.videoIsMirroring = _settings->value("media/videoMirroring", false).toBool();

    httpClient().setBaseUrl(_serverBaseUrl);
    resetAuthentication();
    if (canPersistSession()) {
        auto loaded = _credentials->load(_serverBaseUrl, _savedAccount);
        _credentialStatus = loaded.status;
        if (loaded.status == CredentialStatus::Ready) {
            _savedSession = std::move(loaded.session);
            _rememberSession = true;
            _autoLogin = _savedSession->autoLogin;
        }
    } else {
        _credentialStatus = CredentialStatus::Empty;
    }
    if (!recognizedEndpoint) {
        _settings->setValue("network/serverBaseUrl", QString());
        _settings->sync();
    }
}

void SessionManager::saveToSettings() {
    if (!_settings) return;

    _settings->setValue("auth/account", _savedAccount);
    _settings->setValue("network/serverBaseUrl", _serverBaseUrl);

    _settings->setValue("media/enableMicrophone", _mediaPrefs.enableMicrophone);
    _settings->setValue("media/enableSpeaker", _mediaPrefs.enableSpeaker);
    _settings->setValue("media/enableVideo", _mediaPrefs.enableVideo);
    _settings->setValue("media/videoMirroring", _mediaPrefs.videoIsMirroring);

    _settings->sync();
}

} // namespace OpenMeeting
