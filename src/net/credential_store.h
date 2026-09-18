#pragma once

#include "src/net/http_types.h"
#include <QtCore/QSettings>
#include <memory>

namespace OpenMeeting {

enum class CredentialStatus {
    Empty, Ready, Migrated, InvalidRecord, Unavailable, SaveFailed, CleanupFailed
};

struct StoredSession {
    QString service;
    QString account;
    UserInfo user;
    bool autoLogin = false;
};

struct CredentialLoadResult {
    CredentialStatus status = CredentialStatus::Empty;
    StoredSession session;
};

// The owner supplies the storage; tests must never fall back to user settings.
// Implementations are called only on the SessionManager's Qt thread.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual CredentialLoadResult load(const QString &service, const QString &account) = 0;
    virtual CredentialStatus save(const StoredSession &session) = 0;
    virtual CredentialStatus clear() = 0;
};

QString canonicalServiceUrl(const QString &url);
QString credentialStatusMessage(CredentialStatus status);
std::unique_ptr<CredentialStore> makeCredentialStore(QSettings &settings);

} // namespace OpenMeeting
