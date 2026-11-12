#pragma once

#include "src/net/credential_store.h"
#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <functional>
#include <memory>

namespace OpenMeeting {

// Only immutable request data crosses the GLib owner boundary. Secrets are
// never logged or written to a scratch file. Shared ownership avoids incidental
// copies in queued completions; the final owner wipes the allocation.
class SecretBytes final {
public:
    explicit SecretBytes(QByteArray bytes);
    ~SecretBytes();
    SecretBytes(const SecretBytes &) = delete;
    SecretBytes &operator=(const SecretBytes &) = delete;
    const QByteArray &bytes() const { return bytes_; }
private:
    QByteArray bytes_;
};

struct SecretServiceDiagnostics {
    bool valuePresent = false;
    bool bytesPresent = false;
    bool jsonContentType = false;
    int boundedPayloadBytes = 0; // At most the record limit plus one.
};

struct SecretServiceResult {
    enum class Creation { NotSubmitted, Acknowledged, Uncertain };
    CredentialStatus status = CredentialStatus::Unavailable;
    std::shared_ptr<SecretBytes> secret;
    QStringList erased; // Exact UUIDs successfully erased, including absent items.
    SecretServiceDiagnostics diagnostics;
    Creation creation = Creation::NotSubmitted;
};

// Operation callbacks run on the GLib owner, never on Qt. A transport has one
// terminal completion per request, including cancel; shutdown drains callbacks.
// This seam also permits isolated protocol/metadata tests without user keyrings.
class SecretServiceTransport {
public:
    using RequestId = quint64;
    using Completion = std::function<void(SecretServiceResult)>;
    virtual ~SecretServiceTransport() = default;
    virtual RequestId load(QString id, CredentialStore::Interaction interaction,
        Completion completion) = 0;
    virtual RequestId save(QString id, std::shared_ptr<SecretBytes> secret,
        CredentialStore::Interaction interaction, Completion completion) = 0;
    virtual RequestId erase(QStringList ids, Completion completion) = 0;
    virtual void cancel(RequestId request) = 0;
    // Nonblocking; completion is called after the GLib worker has been joined
    // by an independent reaper, not the serial media cleanup worker.
    // An already joined transport may notify inline; the Qt facade queues it.
    virtual void shutdown(std::function<void()> completion) = 0;
};

enum class SecretServiceNamespace { Product, ContractTest };
std::unique_ptr<SecretServiceTransport> makeSecretServiceTransport(
    SecretServiceNamespace space = SecretServiceNamespace::Product);
std::unique_ptr<CredentialStore> makeLinuxCredentialStore(QSettings &settings,
    std::unique_ptr<SecretServiceTransport> transport = {});

} // namespace OpenMeeting
