#include <QtCore/QCoreApplication>
#include "src/net/credential_store.h"
#include "src/net/service_endpoint_policy.h"

#include <QtCore/QJsonDocument>
#ifdef Q_OS_LINUX
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <unordered_map>
#include "src/net/linux_secret_service.h"
#endif
#include <QtCore/QUrl>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#else
#include <openssl/crypto.h>
#endif

namespace OpenMeeting {

#ifdef Q_OS_LINUX
struct CredentialStore::AsyncAdapter {
    struct Pending { bool cancelled = false; };
    std::unique_ptr<QObject> dispatcher = std::make_unique<QObject>();
    std::unordered_map<RequestId, std::shared_ptr<Pending>> requests;
    RequestId next = 0;
    bool closed = false;

    template <typename F>
    RequestId post(QObject *context, F operation) {
        Q_ASSERT(QThread::currentThread() == dispatcher->thread());
        if (!context) context = dispatcher.get();
        if (context->thread() != dispatcher->thread()) return 0;
        const QPointer<QObject> receiver(context);
        auto pending = std::make_shared<Pending>();
        pending->cancelled = closed;
        const auto id = ++next;
        requests.emplace(id, pending);
        QTimer::singleShot(0, dispatcher.get(), [this, id, pending, receiver,
                operation = std::move(operation)]() mutable {
            requests.erase(id);
            if (receiver) operation(pending->cancelled);
        });
        return id;
    }
};

CredentialStore::CredentialStore() : adapter_(std::make_unique<AsyncAdapter>()) {}
#else
CredentialStore::CredentialStore() = default;
#endif
CredentialStore::~CredentialStore() = default;

#ifdef Q_OS_LINUX
CredentialStore::RequestId CredentialStore::loadAsync(const QString &service,
        const QString &account, Interaction, QObject *context, LoadCompletion completion) {
    return adapter_->post(context, [this, service, account, completion = std::move(completion)](bool cancelled) mutable {
        auto result = cancelled ? CredentialLoadResult{CredentialStatus::Cancelled, {}} : load(service, account);
        if (completion) completion(std::move(result));
    });
}

CredentialStore::RequestId CredentialStore::saveAsync(StoredSession session,
        Interaction, QObject *context, StatusCompletion completion) {
    return adapter_->post(context, [this, session = std::move(session), completion = std::move(completion)](bool cancelled) mutable {
        const auto result = cancelled ? CredentialStatus::Cancelled : save(session);
        if (completion) completion(result);
    });
}

CredentialStore::RequestId CredentialStore::clearAsync(QObject *context, StatusCompletion completion) {
    Q_ASSERT(QThread::currentThread() == adapter_->dispatcher->thread());
    // Keep legacy revocation's original same-thread commit boundary.
    // Cancellation may suppress notification, never the logout tombstone.
    const auto result = clear();
    return adapter_->post(context, [result, completion = std::move(completion)](bool cancelled) mutable {
        if (completion) completion(result);
        (void)cancelled;
    });
}

void CredentialStore::cancel(RequestId request) {
    Q_ASSERT(QThread::currentThread() == adapter_->dispatcher->thread());
    if (const auto found = adapter_->requests.find(request); found != adapter_->requests.end())
        found->second->cancelled = true;
}

void CredentialStore::shutdownAsync(QObject *context, std::function<void()> completion) {
    Q_ASSERT(QThread::currentThread() == adapter_->dispatcher->thread());
    adapter_->closed = true;
    for (auto &[id, pending] : adapter_->requests) pending->cancelled = true;
    adapter_->post(context, [completion = std::move(completion)](bool) mutable {
        if (completion) completion();
    });
}
#endif

namespace {
constexpr auto kRecord = "auth/protectedSessionV2";
constexpr auto kDisabled = "auth/restoreDisabled";
constexpr int kMaxPlaintext = 64 * 1024;
constexpr int kMaxCiphertext = 128 * 1024;

void wipe(QByteArray &bytes) {
#ifdef Q_OS_WIN
    if (!bytes.isEmpty()) SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
#else
    if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
#endif
    bytes.clear();
}

bool protect(const QByteArray &input, QByteArray &output, bool encrypt) {
#ifdef Q_OS_WIN
    DATA_BLOB source{static_cast<DWORD>(input.size()),
                     reinterpret_cast<BYTE *>(const_cast<char *>(input.constData()))};
    DATA_BLOB result{};
    const BOOL ok = encrypt
        ? CryptProtectData(&source, L"OpenMeeting session v2", nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &result)
        : CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!ok) return false;
    const auto limit = encrypt ? kMaxCiphertext : kMaxPlaintext;
    const bool bounded = result.cbData <= static_cast<DWORD>(limit);
    if (bounded) output = QByteArray(reinterpret_cast<const char *>(result.pbData), int(result.cbData));
    if (result.pbData) {
        SecureZeroMemory(result.pbData, result.cbData);
        LocalFree(result.pbData);
    }
    return bounded;
#else
    Q_UNUSED(input);
    Q_UNUSED(output);
    Q_UNUSED(encrypt);
    return false; // Secret Service support belongs to the keyring phase.
#endif
}

class SettingsCredentialStore final : public CredentialStore {
public:
    explicit SettingsCredentialStore(QSettings &settings) : settings_(settings) {
        settings_.setFallbacksEnabled(false);
    }

    CredentialStatus clear() override {
        // Commit the tombstone first. A crash during deletion must not restore
        // the old blob. A failed sync is never reported as successful cleanup.
        settings_.setValue(kDisabled, true);
        settings_.sync();
        const bool disabled = settings_.status() == QSettings::NoError;
        settings_.remove(kRecord);
        settings_.remove("auth/password");
        settings_.remove("auth/rememberPassword");
        settings_.remove("auth/autoLogin");
        settings_.remove("user");
        settings_.sync();
        return disabled && settings_.status() == QSettings::NoError
            ? CredentialStatus::Empty : CredentialStatus::CleanupFailed;
    }

    CredentialLoadResult load(const QString &service, const QString &account) override {
        settings_.sync();
        if (settings_.status() != QSettings::NoError) return {CredentialStatus::Unavailable, {}};
        // Detect legacy values without materializing a plaintext secret.
        if (settings_.contains("auth/password") || settings_.contains("user/token") ||
            settings_.contains("auth/rememberPassword") || settings_.contains("auth/autoLogin")) {
            return {clear() == CredentialStatus::Empty
                ? CredentialStatus::Migrated : CredentialStatus::CleanupFailed, {}};
        }
        if (!serviceAllowsCredentialPersistence(service)) {
            // A remembered HTTP session remains dormant during strict startup.
            // SessionManager gates activation and a later --debug launch may resume it.
            return {CredentialStatus::Empty, {}};
        }
        if (settings_.value(kDisabled, true).toBool()) {
            return {settings_.contains(kRecord) ? clear() : CredentialStatus::Empty, {}};
        }
        auto reject = [this]() -> CredentialLoadResult {
            return {clear() == CredentialStatus::Empty
                ? CredentialStatus::InvalidRecord : CredentialStatus::CleanupFailed, {}};
        };
        const auto cipher = settings_.value(kRecord).toByteArray();
        if (cipher.isEmpty() || cipher.size() > kMaxCiphertext) return reject();
        QByteArray plain;
        if (!protect(cipher, plain, false)) return reject();
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(plain, &error);
        wipe(plain);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return reject();
        const auto object = document.object();
        StoredSession record;
        record.service = object.value("service").toString();
        record.account = object.value("account").toString();
        record.user = UserInfo::fromJson(object.value("user").toObject());
        record.autoLogin = object.value("autoLogin").toBool();
        if (object.value("version").toInt() != 2 || !object.value("autoLogin").isBool() ||
            service.isEmpty() || record.service != service || record.account != account ||
            record.account.isEmpty() || record.user.token.isEmpty() || record.user.userId.isEmpty()) {
            return reject();
        }
        return {CredentialStatus::Ready, std::move(record)};
    }

    CredentialStatus save(const StoredSession &record) override {
        if (!serviceAllowsCredentialPersistence(record.service) ||
            canonicalServiceUrl(record.service) != record.service ||
            record.account.isEmpty() || record.user.token.isEmpty() || record.user.userId.isEmpty()) {
            return CredentialStatus::SaveFailed;
        }
        if (clear() != CredentialStatus::Empty) return CredentialStatus::CleanupFailed;
        QJsonObject object;
        object["version"] = 2;
        object["service"] = record.service;
        object["account"] = record.account;
        object["user"] = record.user.toJson();
        object["autoLogin"] = record.autoLogin;
        auto plain = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QByteArray cipher;
        const bool protectedOk = plain.size() <= kMaxPlaintext && protect(plain, cipher, true);
        wipe(plain);
        if (!protectedOk) return CredentialStatus::SaveFailed;
        settings_.setValue(kRecord, cipher);
        settings_.sync();
        if (settings_.status() == QSettings::NoError) {
            settings_.setValue(kDisabled, false);
            settings_.sync();
            if (settings_.status() == QSettings::NoError) return CredentialStatus::Ready;
        }
        return clear() == CredentialStatus::Empty
            ? CredentialStatus::SaveFailed : CredentialStatus::CleanupFailed;
    }
private:
    QSettings &settings_;
};
} // namespace

bool credentialPersistenceSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

QString canonicalServiceUrl(const QString &text) {
    QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment() ||
        (url.scheme() != "http" && url.scheme() != "https")) return {};
    url.setHost(url.host().toLower());
    if ((url.scheme() == "https" && url.port() == 443) ||
        (url.scheme() == "http" && url.port() == 80)) url.setPort(-1);
    auto result = url.toString(QUrl::FullyEncoded);
    // Match the HTTP client's effective base URL without folding path case,
    // encoded separators or distinct API roots.
    if (result.endsWith("//")) return {};
    if (result.endsWith('/')) result.chop(1);
    return result;
}

QString credentialStatusMessage(CredentialStatus status) {
    switch (status) {
    case CredentialStatus::Migrated:
        return QCoreApplication::translate("MeetingUI", "Legacy sign-in credentials have been removed. Please sign in again. Passwords will no longer be saved.");
    case CredentialStatus::InvalidRecord:
        return QCoreApplication::translate("MeetingUI", "Your saved session cannot be used. Please sign in again.");
    case CredentialStatus::Unavailable:
    case CredentialStatus::SaveFailed:
        return QCoreApplication::translate("MeetingUI", "Your session was not saved. You will need to sign in again next time.");
    case CredentialStatus::CleanupFailed:
#ifdef Q_OS_LINUX
        return QCoreApplication::translate("MeetingUI", "Unable to finish clearing saved sign-in data. Unlock the keyring and restore access to settings, then try again.");
#else
        return QCoreApplication::translate("MeetingUI", "Unable to clear local credentials. Old credentials may remain after restarting. Restore write access to settings and try again.");
#endif
    case CredentialStatus::Locked:
        return QCoreApplication::translate("MeetingUI", "Your keyring is locked. Unlock it to restore or save your session; you can continue without saving.");
    case CredentialStatus::Busy:
        return QCoreApplication::translate("MeetingUI", "Saved sign-in settings are busy. Try again, or continue without saving.");
    case CredentialStatus::Denied:
    case CredentialStatus::Cancelled:
        return QCoreApplication::translate("MeetingUI", "Keyring access was cancelled. You can continue without saving your session.");
    case CredentialStatus::TimedOut:
        return QCoreApplication::translate("MeetingUI", "The keyring did not respond. You can continue without saving your session.");
    default: return {};
    }
}

std::unique_ptr<CredentialStore> makeCredentialStore(QSettings &settings) {
#ifdef Q_OS_LINUX
    return makeLinuxCredentialStore(settings);
#else
    return std::make_unique<SettingsCredentialStore>(settings);
#endif
}
} // namespace OpenMeeting
