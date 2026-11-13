#include "src/net/session_manager.h"
#include "src/net/account_credentials_policy.h"
#include "src/net/service_endpoint_policy.h"
#include "src/e2ee/meeting_encryption_key.h"
#include "src/media/screen_share_quality.h"
#include "src/rtc/webrtc_manager.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QUuid>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#ifdef Q_OS_LINUX
#include <QtCore/QTimer>
#endif
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace OpenMeeting {
namespace {
constexpr auto kSettingsMigrationVersion = "migration/cohavoraSettingsVersion";
constexpr auto kRegistrationBaseUrl = "network/registrationServerBaseUrl";
constexpr auto kRegistrationServiceBinding = "network/registrationServiceBinding";
constexpr auto kLegacyQuickMeetingsAndScreenShareE2ee = "security/quickMeetingsAndScreenShareE2ee";
constexpr auto kQuickMeetingsE2ee = "security/quickMeetingsE2ee";
constexpr auto kScreenShareE2ee = "security/screenShareE2ee";
constexpr auto kMeetingDetailsE2ee = "security/meetingDetailsE2ee";
constexpr auto kAllMeetingsE2ee = "security/allMeetingsE2ee";
constexpr int kCurrentSettingsMigrationVersion = 1;
constexpr auto kMigratedSettingsKeys = std::to_array<const char *>({
    "network/serverBaseUrl",
    "media/enableMicrophone",
    "media/enableSpeaker",
    "media/enableVideo",
    "media/videoMirroring",
    "media/videoMirrorMode",
    "media/pushToTalkWhenMuted",
    "media/echoCancellation",
    "media/noiseSuppression",
    "media/autoGainControl",
    "media/cameraDeviceId",
    "media/microphoneDeviceId",
    "media/speakerDeviceId",
    "media/cameraVideoCodec",
    "media/screenShareVideoCodec",
    "media/videoEncoderPreference",
    "media/screenShareResolution",
    "media/screenShareFps",
    "media/receiveResolutionMatch",
    "media/videoCaptureWidth",
    "media/videoCaptureHeight",
    "media/videoCaptureFps",
    "general/quitOnMainWindowClose",
    "general/showActiveSpeaker",
    "general/participantNameDisplay",
    "general/stayInMeetingWhenLocked",
    "auth/account",
#if !defined(__linux__)
    "auth/protectedSessionV2",
    "auth/restoreDisabled",
#endif
});
constexpr std::array<const char *, 5> kLegacyCredentialKeys = {
    "auth/protectedSessionV2",
    "auth/restoreDisabled",
    "auth/password",
    "auth/rememberPassword",
    "auth/autoLogin",
};

QString participantNameDisplayId(ParticipantNameDisplayMode mode) {
    switch (mode) {
    case ParticipantNameDisplayMode::Always: return QStringLiteral("always");
    case ParticipantNameDisplayMode::OnHover: return QStringLiteral("hover");
    default: return QStringLiteral("auto");
    }
}

ParticipantNameDisplayMode participantNameDisplayFromId(const QString &id) {
    if (id == QStringLiteral("always")) return ParticipantNameDisplayMode::Always;
    if (id == QStringLiteral("hover")) return ParticipantNameDisplayMode::OnHover;
    return ParticipantNameDisplayMode::Auto;
}

bool clearLegacyCredentials(QSettings &legacy) {
#if defined(__linux__)
    // Secret Service imports only cleanup references into its own sidecar.
    // Do not dirty the shared legacy file: implicit/destructor sync could
    // block Qt on a foreign QSettings lock, and migration cannot claim erase.
    Q_UNUSED(legacy);
    return true;
#else
    for (const auto *key : kLegacyCredentialKeys) legacy.remove(key);
    legacy.remove(QStringLiteral("user"));
    legacy.sync();
    return legacy.status() == QSettings::NoError;
#endif
}

std::unique_ptr<QSettings> makeApplicationSettings() {
    auto selected = migrateCohavoraSettings(
        std::make_unique<QSettings>(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("Cohavora"), QStringLiteral("Cohavora")),
        std::make_unique<QSettings>(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("OpenMeeting"), QStringLiteral("LiveKitClient")));
    return std::move(selected.settings);
}
} // namespace

QString normalizeVideoCodecPreference(const QString &codec) {
    const auto normalized = codec.trimmed().toLower();
    static const std::array<QString, 6> supported{
        QStringLiteral("auto"), QStringLiteral("vp8"),
        QStringLiteral("h264"), QStringLiteral("vp9"),
        QStringLiteral("av1"), QStringLiteral("h265")};
    return std::find(supported.begin(), supported.end(), normalized) != supported.end()
        ? normalized : QStringLiteral("auto");
}

QString normalizeVideoEncoderPreference(const QString &encoder) {
    const auto normalized = encoder.trimmed().toLower();
    static const std::array<QString, 5> supported{
        QStringLiteral("auto"), QStringLiteral("software"),
        QStringLiteral("amf"), QStringLiteral("nvenc"), QStringLiteral("qsv")};
    return std::find(supported.begin(), supported.end(), normalized) != supported.end()
        ? normalized : QStringLiteral("auto");
}

SettingsMigrationSelection migrateCohavoraSettings(
        std::unique_ptr<QSettings> current, std::unique_ptr<QSettings> legacy) {
    if (!current || !legacy) {
        throw std::invalid_argument("Settings migration requires current and legacy storage");
    }
    current->setFallbacksEnabled(false);
    legacy->setFallbacksEnabled(false);
#if defined(__linux__)
    QVariantMap metadata;
    constexpr auto cleanupKeys = std::to_array<const char *>({
        "auth/keyringV2/creating", "auth/keyringV2/pending", "auth/keyringV2/journal",
        "auth/keyringV2/revision", "auth/keyringV2/transaction", "auth/keyringV2/active",
        "auth/keyringV2/service", "auth/keyringV2/account"});
    for (const auto *key : cleanupKeys) if (legacy->contains(key)) metadata.insert(QString::fromLatin1(key), legacy->value(key));
    metadata.insert(QStringLiteral("source_path"), legacy->fileName());
    metadata.insert(QStringLiteral("has_legacy_secret_fields"),
        legacy->contains("auth/password") || legacy->contains("auth/protectedSessionV2") ||
        legacy->contains("auth/rememberPassword") || legacy->contains("auth/autoLogin") || legacy->contains("user/token"));
    // QObject property only: never setValue(), never copy secrets, never write
    // credential/tombstone metadata into either shared QSettings namespace.
    current->setProperty("cohavoraLegacyKeyringMetadata", metadata);
#endif
    current->sync();
    legacy->sync();

    if (current->value(kSettingsMigrationVersion, 0).toInt()
            >= kCurrentSettingsMigrationVersion) {
        clearLegacyCredentials(*legacy);
        return {std::move(current), SettingsMigrationStatus::Current};
    }
    if (current->status() != QSettings::NoError) {
        return {std::move(legacy), SettingsMigrationStatus::LegacyFallback};
    }

    for (const auto *key : kMigratedSettingsKeys) {
        if (!current->contains(key) && legacy->contains(key)) {
            current->setValue(key, legacy->value(key));
        }
    }
    current->sync();
    if (current->status() != QSettings::NoError) {
        return {std::move(legacy), SettingsMigrationStatus::LegacyFallback};
    }

    if (!clearLegacyCredentials(*legacy)) {
        return {std::move(legacy), SettingsMigrationStatus::LegacyFallback};
    }

    current->setValue(kSettingsMigrationVersion, kCurrentSettingsMigrationVersion);
    current->sync();
    if (current->status() != QSettings::NoError) {
        return {std::move(legacy), SettingsMigrationStatus::LegacyFallback};
    }
    return {std::move(current), SettingsMigrationStatus::Migrated};
}

SessionManager::SessionManager(QObject *parent)
    : SessionManager(makeApplicationSettings(), parent) {
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

SessionManager::~SessionManager() {
#ifdef Q_OS_LINUX
    // main drains shutdownCredentialsAsync while its Qt dispatcher is alive.
    // No late credential result may project into an owner being destroyed.
    invalidateCredentialOperation();
#endif
}

OpenMeetingHttpClient &SessionManager::httpClient() {
    return *_client;
}

bool SessionManager::isLoggedIn() const {
    return !_currentUser.token.isEmpty() && !_currentUser.userId.isEmpty();
}

bool SessionManager::canPersistSession() const {
    return credentialPersistenceSupported() && serviceAllowsCredentialPersistence(_serverBaseUrl);
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
    return _currentUser.userId.isEmpty() ? QCoreApplication::translate("MeetingUI", "Not Signed In") : _currentUser.userId;
}

void SessionManager::setMediaPreferences(const MediaPreferences &prefs) {
    _mediaPrefs = prefs;
    _mediaPrefs.participantNameDisplay = participantNameDisplayFromId(
        participantNameDisplayId(_mediaPrefs.participantNameDisplay));
    _mediaPrefs.receiveResolutionMatch = livekit::ParseVideoResolutionMatch(
        livekit::VideoResolutionMatchName(_mediaPrefs.receiveResolutionMatch));
    _mediaPrefs.cameraVideoCodec = normalizeVideoCodecPreference(
        _mediaPrefs.cameraVideoCodec);
    _mediaPrefs.screenShareVideoCodec = normalizeVideoCodecPreference(
        _mediaPrefs.screenShareVideoCodec);
    _mediaPrefs.videoEncoderPreference = normalizeVideoEncoderPreference(
        _mediaPrefs.videoEncoderPreference);
    livekit::SetVideoEncoderPreference(_mediaPrefs.videoEncoderPreference.toStdString());
    _mediaPrefs.screenShareResolution = std::clamp(_mediaPrefs.screenShareResolution,
        static_cast<int>(livekit::ScreenShareResolution::Auto),
        static_cast<int>(livekit::ScreenShareResolution::P2160));
    if (_mediaPrefs.screenShareFps != 15 && _mediaPrefs.screenShareFps != 20 && _mediaPrefs.screenShareFps != 30)
        _mediaPrefs.screenShareFps = 20;
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

void SessionManager::setVideoMirrorMode(VideoMirrorMode mode) {
    if (_mediaPrefs.mirrorMode != mode) {
        _mediaPrefs.mirrorMode = mode;
        saveToSettings();
        emit preferencesChanged(_mediaPrefs);
    }
}

const MeetingSecurityPreferences &SessionManager::meetingSecurityPreferences() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return _meetingSecurityPrefs;
}

void SessionManager::setMeetingSecurityPreferences(const MeetingSecurityPreferences &prefs) {
    Q_ASSERT(QThread::currentThread() == thread());
    // The global flag overrides entry behavior without overwriting each
    // entry's independent choice, so turning it off restores those choices.
    if (_meetingSecurityPrefs.quickMeetingsE2ee == prefs.quickMeetingsE2ee &&
        _meetingSecurityPrefs.screenShareE2ee == prefs.screenShareE2ee &&
        _meetingSecurityPrefs.meetingDetailsE2ee == prefs.meetingDetailsE2ee &&
        _meetingSecurityPrefs.allMeetingsE2ee == prefs.allMeetingsE2ee) {
        return;
    }
    _meetingSecurityPrefs = prefs;
    saveToSettings();
    emit meetingSecurityPreferencesChanged();
}

bool SessionManager::hasMeetingEncryptionKey() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return static_cast<bool>(_meetingEncryptionKey);
}

void SessionManager::setMeetingEncryptionKey(std::vector<uint8_t> material) {
    Q_ASSERT(QThread::currentThread() == thread());
    // Validate before replacing the previous key. Invalid input is cleansed by
    // the temporary holder and cannot silently erase a usable configuration.
    _meetingEncryptionKey = livekit::MeetingEncryptionKey::Create(std::move(material));
    emit meetingSecurityPreferencesChanged();
}

void SessionManager::clearMeetingEncryptionKey() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!_meetingEncryptionKey) return;
    _meetingEncryptionKey.reset();
    emit meetingSecurityPreferencesChanged();
}

livekit::MeetingEncryptionRequest SessionManager::meetingEncryptionRequest(MeetingEncryptionEntry entry) const {
    Q_ASSERT(QThread::currentThread() == thread());
    bool entryEnabled = false;
    switch (entry) {
    case MeetingEncryptionEntry::QuickMeeting:
        entryEnabled = _meetingSecurityPrefs.quickMeetingsE2ee;
        break;
    case MeetingEncryptionEntry::ScreenShare:
        entryEnabled = _meetingSecurityPrefs.screenShareE2ee;
        break;
    case MeetingEncryptionEntry::MeetingDetails:
        entryEnabled = _meetingSecurityPrefs.meetingDetailsE2ee;
        break;
    case MeetingEncryptionEntry::JoinMeeting:
        break;
    default:
        throw livekit::EncryptionRequestException(livekit::EncryptionRequestError::InvalidPolicy);
    }
    const bool required = _meetingSecurityPrefs.allMeetingsE2ee || entryEnabled;
    // Preferences become effective only after a usable default key is saved.
    if (!required || !_meetingEncryptionKey) return {};
    return {livekit::MeetingEncryptionMode::Required,
            _meetingEncryptionKey->CreateMeetingSecret()};
}

QString SessionManager::registrationServerBaseUrl(const QString &serviceUrl) const {
    const auto policy = evaluateServiceEndpoint(serviceUrl.isNull() ? _serverBaseUrl : serviceUrl);
    if (policy.canonicalUrl.isEmpty()) return {};
    if (_settings->value(kRegistrationServiceBinding).toString() == policy.canonicalUrl) {
        const auto configured = evaluateServiceEndpoint(_settings->value(kRegistrationBaseUrl).toString());
        if (!configured.canonicalUrl.isEmpty()) return configured.canonicalUrl;
    }
    QUrl endpoint(policy.canonicalUrl);
    // Only infer the stock direct deployment. Reverse-proxy prefixes and custom
    // ports retain their configured address until explicitly overridden in the UI.
    if (endpoint.port() == 11102 && (endpoint.path().isEmpty() || endpoint.path() == "/")) {
        endpoint.setPort(11022);
    }
    return endpoint.toString(QUrl::FullyEncoded);
}

bool SessionManager::setRegistrationServerBaseUrl(const QString &url) {
    if (url.trimmed().isEmpty()) {
        _settings->remove(kRegistrationBaseUrl);
        _settings->remove(kRegistrationServiceBinding);
    } else {
        const auto policy = evaluateServiceEndpoint(url);
        if (!policy.requestAllowed() || !evaluateServiceEndpoint(_serverBaseUrl).requestAllowed()) return false;
        _settings->setValue(kRegistrationBaseUrl, policy.canonicalUrl);
        _settings->setValue(kRegistrationServiceBinding, _serverBaseUrl);
    }
    _settings->sync();
    return true;
}

bool SessionManager::setServerBaseUrl(const QString &url) {
#ifdef Q_OS_LINUX
    if (_credentialShutdown) return false;
#endif
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
#ifdef Q_OS_LINUX
    invalidateCredentialOperation();
#endif
    _loginPending = false;
    _currentUser = {};
    const bool clearedMeetingKey = static_cast<bool>(_meetingEncryptionKey);
    _meetingEncryptionKey.reset();
    httpClient().setCurrentUser({});
    const QPointer<SessionManager> self(this);
    const auto generation = _authGeneration;
    emit authenticationReset(generation);
    if (clearedMeetingKey && self && self->_authGeneration == generation) {
        emit meetingSecurityPreferencesChanged();
    }
    if (self && self->_authGeneration == generation) emit persistenceStateChanged();
}

void SessionManager::cancelPendingLogin() {
    if (_loginPending) resetAuthentication();
}

bool SessionManager::forgetSavedSession() {
#ifdef Q_OS_LINUX
    forgetSavedSessionAsync();
    return false; // The deletion has not completed synchronously.
#else
    if (_loginPending) {
        const QPointer<SessionManager> self(this);
        const auto generation = _authGeneration + 1;
        resetAuthentication();
        if (!self || self->_authGeneration != generation) return false;
    }
    _savedSession.reset();
    _rememberSession = false;
    _autoLogin = false;
    _credentialStatus = _credentials->clear();
    return _credentialStatus == CredentialStatus::Empty;
#endif
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
#ifdef Q_OS_LINUX
    if (_credentialShutdown) return false;
    Q_UNUSED(automatic);
    Q_UNUSED(autoLoginChoice);
    return false; // Callers must wait for resumeSavedSessionAsync's actual result.
#else
    if (!canPersistSession() || !_savedSession || (automatic && !_autoLogin) || _loginPending) return false;
    // Recheck the tombstone and binding at activation, not just at startup.
    auto loaded = _credentials->load(_serverBaseUrl, _savedAccount);
    _credentialStatus = loaded.status;
    if (loaded.status != CredentialStatus::Ready || (automatic && !loaded.session.autoLogin)) {
        _savedSession.reset();
        _rememberSession = _autoLogin = false;
        return false;
    }
    const QPointer<SessionManager> self(this);
    const auto generation = _authGeneration + 1;
    resetAuthentication();
    if (!self || self->_authGeneration != generation) return false;
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
    return announceLogin(generation);
#endif
}

#ifdef Q_OS_LINUX
void SessionManager::invalidateCredentialOperation() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++_credentialGeneration;
    const auto request = std::exchange(_credentialRequest, 0);
    _credentialPending = false;
    _savedSessionRetryAvailable = false;
    if (request && _credentials) _credentials->cancel(request);
}

void SessionManager::restoreSavedSessionAsync(std::function<void()> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (_credentialShutdown || _loginPending || isLoggedIn()) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] { if (completion) completion(); });
        return;
    }
    invalidateCredentialOperation();
    const auto credential = _credentialGeneration;
    const auto authentication = _authGeneration;
    const auto service = _serverBaseUrl;
    const auto account = _savedAccount;
    _credentialRestoreComplete = false;
    _credentialPending = true;
    _savedSession.reset();
    _rememberSession = _autoLogin = false;
    const QPointer<SessionManager> self(this);
    auto finished = [self, credential, authentication, service, account,
                     completion = std::move(completion)](CredentialLoadResult loaded) mutable {
        if (!self || self->_credentialShutdown || self->_credentialGeneration != credential ||
            self->_authGeneration != authentication || self->_serverBaseUrl != service ||
            self->_savedAccount != account) return;
        self->_credentialRequest = 0;
        self->_credentialPending = false;
        self->_credentialRestoreComplete = true;
        self->_credentialStatus = loaded.status;
        if (loaded.status == CredentialStatus::Ready && loaded.session.service == service &&
            loaded.session.account == account && !loaded.session.user.token.isEmpty() &&
            !loaded.session.user.userId.isEmpty()) {
            self->_savedSession = std::move(loaded.session);
            self->_rememberSession = true;
            self->_autoLogin = self->_savedSession->autoLogin;
        } else {
            // These load terminals follow a matching active local reference.
            // They permit another explicit attempt, never token activation or
            // an inference about the unread auto-login choice.
            self->_savedSessionRetryAvailable = loaded.status == CredentialStatus::Locked ||
                loaded.status == CredentialStatus::Denied;
            if (loaded.status == CredentialStatus::Ready)
                self->_credentialStatus = CredentialStatus::InvalidRecord;
        }
        emit self->persistenceStateChanged();
        if (self && self->_credentialGeneration == credential && self->_authGeneration == authentication && completion)
            completion();
    };
    if (!canPersistSession()) {
        QTimer::singleShot(0, this, [finished = std::move(finished)]() mutable { finished({}); });
    } else {
        _credentialRequest = _credentials->loadAsync(service, account,
            CredentialStore::Interaction::Background, this, std::move(finished));
    }
    emit persistenceStateChanged();
}

void SessionManager::resumeSavedSessionAsync(bool automatic, std::optional<bool> autoLoginChoice,
                                            std::function<void(bool)> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (_credentialShutdown || !canPersistSession() || !canResumeSavedSession() ||
        (automatic && (!_savedSession || !_autoLogin)) || _loginPending || _credentialPending) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] { if (completion) completion(false); });
        return;
    }
    resetAuthentication();
    const auto credential = _credentialGeneration;
    const auto authentication = _authGeneration;
    const auto service = _serverBaseUrl;
    const auto account = _savedAccount;
    _credentialPending = true;
    _loginPending = true;
    const QPointer<SessionManager> self(this);
    _credentialRequest = _credentials->loadAsync(service, account,
        automatic ? CredentialStore::Interaction::Background : CredentialStore::Interaction::UserInitiated,
        this, [self, credential, authentication, service, account, automatic, autoLoginChoice,
               completion = std::move(completion)](CredentialLoadResult loaded) mutable {
            if (!self || self->_credentialShutdown || self->_credentialGeneration != credential ||
                self->_authGeneration != authentication || self->_serverBaseUrl != service ||
                self->_savedAccount != account || !self->_loginPending) return;
            self->_credentialRequest = 0;
            self->_credentialPending = false;
            self->_credentialStatus = loaded.status;
            const bool valid = loaded.status == CredentialStatus::Ready && loaded.session.service == service &&
                loaded.session.account == account && !loaded.session.user.token.isEmpty() &&
                !loaded.session.user.userId.isEmpty() && (!automatic || loaded.session.autoLogin);
            if (!valid) {
                self->_loginPending = false;
                if (loaded.status == CredentialStatus::Ready && (!automatic || loaded.session.autoLogin))
                    self->_credentialStatus = CredentialStatus::InvalidRecord;
                self->_savedSession.reset();
                self->_savedSessionRetryAvailable = loaded.status == CredentialStatus::Locked ||
                    loaded.status == CredentialStatus::Denied;
                self->_rememberSession = self->_autoLogin = false;
                emit self->persistenceStateChanged();
                if (self && self->_credentialGeneration == credential && self->_authGeneration == authentication && completion)
                    completion(false);
                return;
            }
            // The backend just rechecked tombstone/binding/revision. Activate
            // on this same Qt completion, with no intervening queued hop.
            self->_sessionInvalidating = false;
            self->_savedSession = std::move(loaded.session);
            self->_currentUser = self->_savedSession->user;
            self->_rememberSession = true;
            self->_autoLogin = self->_savedSession->autoLogin;
            self->httpClient().setCurrentUser(self->_currentUser);
            const bool changedChoice = autoLoginChoice && *autoLoginChoice != self->_autoLogin;
            auto replacement = *self->_savedSession;
            if (changedChoice) replacement.autoLogin = *autoLoginChoice;
            emit self->persistenceStateChanged();
            if (!self || self->_credentialGeneration != credential || self->_authGeneration != authentication) return;
            const bool committed = self->announceLogin(authentication);
            if (!self || self->_authGeneration != authentication) return;
            if (committed && changedChoice) {
                self->persistSessionAsync(std::move(replacement), authentication,
                    [self, authentication, completion = std::move(completion)](bool) mutable {
                        if (!self || self->_authGeneration != authentication || !self->_loginPending) return;
                        self->_loginPending = false;
                        // Save failure keeps the authenticated identity. The
                        // dialog reads the terminal persistence warning.
                        if (completion) completion(true);
                    });
            } else {
                self->_loginPending = false;
                if (completion) completion(committed);
            }
        });
    emit persistenceStateChanged();
}

void SessionManager::persistSessionAsync(StoredSession record, quint64 authentication,
                                        std::function<void(bool)> completion) {
    if (_credentialShutdown || _authGeneration != authentication || !canPersistSession()) {
        if (completion) completion(false);
        return;
    }
    invalidateCredentialOperation();
    const auto credential = _credentialGeneration;
    _credentialPending = true;
    const auto service = _serverBaseUrl;
    const auto account = _savedAccount;
    const QPointer<SessionManager> self(this);
    // Keep a separate value for the callback: argument evaluation order must
    // not move this token-bearing record before it is passed to the backend.
    const auto resultRecord = record;
    _credentialRequest = _credentials->saveAsync(std::move(record), CredentialStore::Interaction::UserInitiated,
        this, [self, credential, authentication, service, account, resultRecord,
               completion = std::move(completion)](CredentialStatus status) mutable {
            if (!self || self->_credentialShutdown || self->_credentialGeneration != credential ||
                self->_authGeneration != authentication || self->_serverBaseUrl != service ||
                self->_savedAccount != account) return;
            self->_credentialRequest = 0;
            self->_credentialPending = false;
            self->_credentialStatus = status;
            if (status == CredentialStatus::Ready) {
                self->_savedSession = resultRecord;
                self->_rememberSession = true;
                self->_autoLogin = resultRecord.autoLogin;
            } else {
                self->_savedSession.reset();
                self->_rememberSession = self->_autoLogin = false;
            }
            emit self->persistenceStateChanged();
            if (self && self->_credentialGeneration == credential && self->_authGeneration == authentication && completion)
                completion(status == CredentialStatus::Ready);
        });
    emit persistenceStateChanged();
}

void SessionManager::forgetSavedSessionAsync(std::function<void(bool)> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (_loginPending) resetAuthentication();
    else invalidateCredentialOperation();
    const auto credential = _credentialGeneration;
    const auto authentication = _authGeneration;
    _savedSession.reset();
    _rememberSession = _autoLogin = false;
    _credentialRestoreComplete = true;
    _credentialPending = !_credentialShutdown;
    const QPointer<SessionManager> self(this);
    if (_credentialShutdown) {
        _credentialStatus = CredentialStatus::Cancelled;
        QTimer::singleShot(0, this, [completion = std::move(completion)] { if (completion) completion(false); });
    } else {
        // clearAsync commits the tombstone before returning. A subsequent
        // logout/endpoint switch cannot resurrect an in-flight saved token.
        _credentialRequest = _credentials->clearAsync(this,
            [self, credential, authentication, completion = std::move(completion)](CredentialStatus status) mutable {
                if (!self || self->_credentialShutdown || self->_credentialGeneration != credential ||
                    self->_authGeneration != authentication) return;
                self->_credentialRequest = 0;
                self->_credentialPending = false;
                self->_credentialStatus = status;
                emit self->persistenceStateChanged();
                if (self && self->_credentialGeneration == credential && self->_authGeneration == authentication && completion)
                    completion(status == CredentialStatus::Empty);
            });
    }
    emit persistenceStateChanged();
}

void SessionManager::shutdownCredentialsAsync(std::function<void()> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (_credentialShutdownComplete) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] { if (completion) completion(); });
        return;
    }
    if (completion) _credentialShutdownWaiters.push_back(std::move(completion));
    if (_credentialShutdown) return;
    _credentialShutdown = true;
    resetAuthentication();
    const QPointer<SessionManager> self(this);
    _credentials->shutdownAsync(this, [self] {
        if (!self) return;
        self->_credentialShutdownComplete = true;
        auto waiters = std::move(self->_credentialShutdownWaiters);
        emit self->persistenceStateChanged();
        for (auto &waiter : waiters) if (waiter) waiter();
    });
    emit persistenceStateChanged();
}
#endif

void SessionManager::loginWithPassword(const QString &account,
                                       const QString &password,
                                       bool remember,
                                       bool autoLogin,
                                       std::function<void(bool success, const QString &errMsg)> callback) {
#ifdef Q_OS_LINUX
    if (_credentialShutdown) {
        if (callback) callback(false, QCoreApplication::translate("MeetingUI", "The application is closing."));
        return;
    }
#endif
    const auto validation = validateAccountCredentials(account, password);
    if (validation != CredentialValidationError::None) {
        if (callback) callback(false, credentialValidationMessage(validation));
        return;
    }
    resetAuthentication();
    forgetSavedSession();
    const auto generation = _authGeneration;
    const auto service = _serverBaseUrl;
    const auto endpoint = evaluateServiceEndpoint(service);
    if (!endpoint.requestAllowed()) {
        if (callback) callback(false, serviceEndpointErrorMessage(endpoint.status));
        return;
    }
    const bool persist = remember && credentialPersistenceSupported() &&
        serviceAllowsCredentialPersistence(service);
    const bool persistAutomatically = persist && autoLogin;
    _loginPending = true;
    const QPointer<SessionManager> self(this);
    const auto httpRevision = httpClient().authRevision();
    httpClient().requestLogin(account, password,
        [self, generation, httpRevision, service, account, persist, persistAutomatically, callback]
        (bool ok, const UserInfo &info, const HttpError &err) {
            const auto superseded = QCoreApplication::translate("MeetingUI", "This sign-in attempt was canceled. Please sign in again.");
            if (!self || self->_authGeneration != generation || !self->_loginPending ||
                self->_serverBaseUrl != service || self->httpClient().authRevision() != httpRevision ||
                self->httpClient().baseUrl() != service) {
                if (callback) callback(false, superseded);
                return;
            }
            if (!ok || info.token.isEmpty() || info.userId.isEmpty()) {
                self->_loginPending = false;
                if (callback) callback(false, ok ? QCoreApplication::translate("MeetingUI", "The sign-in response is missing required credentials.") : err.message);
                return;
            }
            self->_sessionInvalidating = false;
            self->_currentUser = info;
            self->_savedAccount = account;
            self->saveToSettings();
#ifdef Q_OS_LINUX
            if (persist) {
                // The identity is usable while the keyring request is pending,
                // but keep this login transaction open until its save result.
                // The dialog must still exist to show a persistence failure.
                self->httpClient().setCurrentUser(info);
                if (!self->announceLogin(generation)) {
                    if (callback) callback(false, superseded);
                    return;
                }
                self->persistSessionAsync(
                    StoredSession{service, account, info, persistAutomatically}, generation,
                    [self, generation, service, callback](bool saved) {
                        if (!self || self->_authGeneration != generation ||
                            self->_serverBaseUrl != service || !self->_loginPending) return;
                        self->_loginPending = false;
                        if (callback) callback(true, saved ? QString() : self->persistenceMessage());
                    });
                return;
            }
#else
            if (persist) {
                StoredSession record{service, account, info, persistAutomatically};
                self->_credentialStatus = self->_credentials->save(record);
                if (self->_credentialStatus == CredentialStatus::Ready) {
                    self->_savedSession = std::move(record);
                    self->_rememberSession = true;
                    self->_autoLogin = persistAutomatically;
                }
            }
#endif
            if (!self || self->_authGeneration != generation || !self->_loginPending) {
                if (callback) callback(false, superseded);
                return;
            }
            self->_loginPending = false;
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
    }, registrationServerBaseUrl());
}

void SessionManager::loginAsGuest(const QString &nickname, const QString &customUserId) {
#ifdef Q_OS_LINUX
    if (_credentialShutdown) return;
#endif
    resetAuthentication();
    forgetSavedSession();
    _sessionInvalidating = false;
    _currentUser.userId = customUserId.isEmpty()
        ? QString("guest_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(8))
        : customUserId;
    _currentUser.nickname = nickname.isEmpty() ? QCoreApplication::translate("MeetingUI", "Guest User") : nickname;
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
    const auto legacyMirroring = _settings->value("media/videoMirroring", false).toBool();
    const auto mirrorValue = _settings->value(
        "media/videoMirrorMode",
        legacyMirroring ? static_cast<int>(VideoMirrorMode::LocalOnly)
                        : static_cast<int>(VideoMirrorMode::Off)).toInt();
    _mediaPrefs.mirrorMode = mirrorValue >= static_cast<int>(VideoMirrorMode::Off) &&
            mirrorValue <= static_cast<int>(VideoMirrorMode::LocalAndRemote)
        ? static_cast<VideoMirrorMode>(mirrorValue)
        : VideoMirrorMode::Off;
    _mediaPrefs.quitOnMainWindowClose =
        _settings->value("general/quitOnMainWindowClose", true).toBool();
    _mediaPrefs.showActiveSpeaker =
        _settings->value("general/showActiveSpeaker", true).toBool();
    _mediaPrefs.participantNameDisplay = participantNameDisplayFromId(
        _settings->value("general/participantNameDisplay", QStringLiteral("auto")).toString());
    _mediaPrefs.stayInMeetingWhenLocked =
        _settings->value("general/stayInMeetingWhenLocked", true).toBool();
    _mediaPrefs.pushToTalkWhenMuted =
        _settings->value("media/pushToTalkWhenMuted", false).toBool();
    _mediaPrefs.echoCancellation =
        _settings->value("media/echoCancellation", true).toBool();
    _mediaPrefs.noiseSuppression =
        _settings->value("media/noiseSuppression", true).toBool();
    _mediaPrefs.autoGainControl =
        _settings->value("media/autoGainControl", true).toBool();
    _mediaPrefs.cameraDeviceId = _settings->value("media/cameraDeviceId").toString();
    _mediaPrefs.microphoneDeviceId = _settings->value("media/microphoneDeviceId").toString();
    _mediaPrefs.speakerDeviceId = _settings->value("media/speakerDeviceId").toString();
    _mediaPrefs.cameraVideoCodec = normalizeVideoCodecPreference(
        _settings->value("media/cameraVideoCodec", QStringLiteral("auto")).toString());
    _mediaPrefs.screenShareVideoCodec = normalizeVideoCodecPreference(
        _settings->value("media/screenShareVideoCodec", QStringLiteral("auto")).toString());
    _mediaPrefs.videoEncoderPreference = normalizeVideoEncoderPreference(
        _settings->value("media/videoEncoderPreference", QStringLiteral("auto")).toString());
    livekit::SetVideoEncoderPreference(_mediaPrefs.videoEncoderPreference.toStdString());
    _mediaPrefs.videoCaptureWidth =
        std::max(0, _settings->value("media/videoCaptureWidth", 0).toInt());
    _mediaPrefs.videoCaptureHeight =
        std::max(0, _settings->value("media/videoCaptureHeight", 0).toInt());
    _mediaPrefs.screenShareResolution = std::clamp(
        _settings->value("media/screenShareResolution", 0).toInt(),
        static_cast<int>(livekit::ScreenShareResolution::Auto),
        static_cast<int>(livekit::ScreenShareResolution::P2160));
    _mediaPrefs.screenShareFps = _settings->value("media/screenShareFps", 20).toInt();
    _mediaPrefs.receiveResolutionMatch = livekit::ParseVideoResolutionMatch(
        _settings->value("media/receiveResolutionMatch", QStringLiteral("auto")).toString().toStdString());
    if (_mediaPrefs.screenShareFps != 15 && _mediaPrefs.screenShareFps != 20 && _mediaPrefs.screenShareFps != 30)
        _mediaPrefs.screenShareFps = 20;
    _mediaPrefs.videoCaptureFps =
        std::max(1, _settings->value("media/videoCaptureFps", 30).toInt());

    _meetingSecurityPrefs.allMeetingsE2ee = _settings->value(kAllMeetingsE2ee, false).toBool();
    // Existing independent keys (including explicit false) take precedence
    // over the old combined value. Meeting details has no legacy equivalent.
    if (_settings->contains(kLegacyQuickMeetingsAndScreenShareE2ee)) {
        const bool legacy = _settings->value(kLegacyQuickMeetingsAndScreenShareE2ee).toBool();
        if (!_settings->contains(kQuickMeetingsE2ee)) _settings->setValue(kQuickMeetingsE2ee, legacy);
        if (!_settings->contains(kScreenShareE2ee)) _settings->setValue(kScreenShareE2ee, legacy);
        _settings->remove(kLegacyQuickMeetingsAndScreenShareE2ee);
        _settings->sync();
    }
    _meetingSecurityPrefs.quickMeetingsE2ee = _settings->value(kQuickMeetingsE2ee, false).toBool();
    _meetingSecurityPrefs.screenShareE2ee = _settings->value(kScreenShareE2ee, false).toBool();
    _meetingSecurityPrefs.meetingDetailsE2ee = _settings->value(kMeetingDetailsE2ee, false).toBool();

    httpClient().setBaseUrl(_serverBaseUrl);
    resetAuthentication();
#ifdef Q_OS_LINUX
    _credentialRestoreComplete = false;
    _savedSession.reset();
    _rememberSession = _autoLogin = false;
    _credentialStatus = CredentialStatus::Empty;
#else
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
#endif
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
    _settings->setValue("media/videoMirrorMode", static_cast<int>(_mediaPrefs.mirrorMode));
    _settings->remove("media/videoMirroring");
    _settings->setValue("general/quitOnMainWindowClose", _mediaPrefs.quitOnMainWindowClose);
    _settings->setValue("general/showActiveSpeaker", _mediaPrefs.showActiveSpeaker);
    _settings->setValue("general/participantNameDisplay",
        participantNameDisplayId(_mediaPrefs.participantNameDisplay));
    _settings->setValue("general/stayInMeetingWhenLocked", _mediaPrefs.stayInMeetingWhenLocked);
    _settings->setValue("media/pushToTalkWhenMuted", _mediaPrefs.pushToTalkWhenMuted);
    _settings->setValue("media/echoCancellation", _mediaPrefs.echoCancellation);
    _settings->setValue("media/noiseSuppression", _mediaPrefs.noiseSuppression);
    _settings->setValue("media/autoGainControl", _mediaPrefs.autoGainControl);
    _settings->setValue("media/cameraDeviceId", _mediaPrefs.cameraDeviceId);
    _settings->setValue("media/microphoneDeviceId", _mediaPrefs.microphoneDeviceId);
    _settings->setValue("media/speakerDeviceId", _mediaPrefs.speakerDeviceId);
    _settings->setValue("media/cameraVideoCodec", _mediaPrefs.cameraVideoCodec);
    _settings->setValue("media/screenShareVideoCodec", _mediaPrefs.screenShareVideoCodec);
    _settings->setValue("media/videoEncoderPreference", _mediaPrefs.videoEncoderPreference);
    _settings->setValue("media/screenShareResolution", _mediaPrefs.screenShareResolution);
    _settings->setValue("media/screenShareFps", _mediaPrefs.screenShareFps);
    _settings->setValue("media/receiveResolutionMatch",
        QString::fromLatin1(livekit::VideoResolutionMatchName(_mediaPrefs.receiveResolutionMatch)));
    _settings->setValue("media/videoCaptureWidth", _mediaPrefs.videoCaptureWidth);
    _settings->setValue("media/videoCaptureHeight", _mediaPrefs.videoCaptureHeight);
    _settings->setValue("media/videoCaptureFps", _mediaPrefs.videoCaptureFps);

    _settings->setValue(kQuickMeetingsE2ee, _meetingSecurityPrefs.quickMeetingsE2ee);
    _settings->setValue(kScreenShareE2ee, _meetingSecurityPrefs.screenShareE2ee);
    _settings->setValue(kMeetingDetailsE2ee, _meetingSecurityPrefs.meetingDetailsE2ee);
    _settings->setValue(kAllMeetingsE2ee, _meetingSecurityPrefs.allMeetingsE2ee);
    _settings->remove(kLegacyQuickMeetingsAndScreenShareE2ee);

    _settings->sync();
}

} // namespace OpenMeeting
