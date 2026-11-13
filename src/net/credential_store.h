#pragma once

#include "src/net/http_types.h"
#include <QtCore/QSettings>
#include <QtCore/QObject>
#include <functional>
#include <memory>

namespace OpenMeeting {

enum class CredentialStatus {
    Empty, Ready, Migrated, InvalidRecord, Unavailable, SaveFailed, CleanupFailed,
    Cancelled, Busy, Locked, Denied, TimedOut
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

// The owner supplies the storage; injected storage never falls back to user settings.
// Implementations are called only on the SessionManager's Qt thread.
class CredentialStore {
public:
#ifdef Q_OS_LINUX
    using RequestId = quint64;
    enum class Interaction { Background, UserInitiated };
    using LoadCompletion = std::function<void(CredentialLoadResult)>;
    using StatusCompletion = std::function<void(CredentialStatus)>;
#endif

    CredentialStore();
    virtual ~CredentialStore();
    CredentialStore(const CredentialStore &) = delete;
    CredentialStore &operator=(const CredentialStore &) = delete;
    virtual CredentialLoadResult load(const QString &service, const QString &account) = 0;
    virtual CredentialStatus save(const StoredSession &session) = 0;
    virtual CredentialStatus clear() = 0;

#ifdef Q_OS_LINUX
    // Only Linux needs asynchronous Secret Service operations. All entry
    // points, cancellation and completions belong to the Qt owner.
    // Completion is never inline. A destroyed context/store suppresses delivery.
    // The default adapter invokes the legacy store on that same Qt thread;
    // Secret Service overrides it with operations on a separate owner.
    virtual RequestId loadAsync(const QString &service, const QString &account,
        Interaction interaction, QObject *context, LoadCompletion completion);
    virtual RequestId saveAsync(StoredSession session, Interaction interaction,
        QObject *context, StatusCompletion completion);
    // A Linux clear persists the restore-disabled tombstone before returning.
    virtual RequestId clearAsync(QObject *context, StatusCompletion completion);
    virtual void cancel(RequestId request);
    // Stop admitting requests, cancel and drain native work without joining on
    // the Qt thread. The owner keeps its event loop alive until completion.
    virtual void shutdownAsync(QObject *context, std::function<void()> completion);

private:
    struct AsyncAdapter;
    std::unique_ptr<AsyncAdapter> adapter_;
#endif
};

// Whether the default platform store can persist secrets in this build.
bool credentialPersistenceSupported();
QString canonicalServiceUrl(const QString &url);
QString credentialStatusMessage(CredentialStatus status);
std::unique_ptr<CredentialStore> makeCredentialStore(QSettings &settings);

} // namespace OpenMeeting
