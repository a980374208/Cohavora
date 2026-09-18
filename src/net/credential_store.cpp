#include "src/net/credential_store.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>
#include <windows.h>
#include <dpapi.h>

namespace OpenMeeting {
namespace {
constexpr auto kRecord = "auth/protectedSessionV2";
constexpr auto kDisabled = "auth/restoreDisabled";
constexpr int kMaxPlaintext = 64 * 1024;
constexpr int kMaxCiphertext = 128 * 1024;

void wipe(QByteArray &bytes) {
    if (!bytes.isEmpty()) SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
    bytes.clear();
}

bool protect(const QByteArray &input, QByteArray &output, bool encrypt) {
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
        if (clear() != CredentialStatus::Empty) return CredentialStatus::CleanupFailed;
        if (record.service.isEmpty() || canonicalServiceUrl(record.service) != record.service ||
            record.account.isEmpty() || record.user.token.isEmpty() || record.user.userId.isEmpty()) {
            return CredentialStatus::SaveFailed;
        }
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
        return QString::fromUtf8("已清理旧版登录凭据，请重新登录。密码将不再保存。");
    case CredentialStatus::InvalidRecord:
        return QString::fromUtf8("已保存的登录状态无法使用，请重新登录。");
    case CredentialStatus::Unavailable:
    case CredentialStatus::SaveFailed:
        return QString::fromUtf8("登录状态未保存，下次启动需要重新登录。");
    case CredentialStatus::CleanupFailed:
        return QString::fromUtf8("本地凭据清理失败，无法保证重启后旧凭据已移除。请恢复配置写入权限后重试。");
    default: return {};
    }
}

std::unique_ptr<CredentialStore> makeCredentialStore(QSettings &settings) {
    return std::make_unique<SettingsCredentialStore>(settings);
}
} // namespace OpenMeeting
