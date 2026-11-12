#include "src/net/linux_secret_service.h"
#include "src/net/service_endpoint_policy.h"
#include "src/core/session_shutdown_service.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryFile>
#include <QtCore/QLockFile>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace OpenMeeting {
#ifdef COHAVORA_KEYRING_CONTRACT_TESTING
// Defined only by the private contract target. Product builds have no callback
// hook or extra API at this interprocess transaction boundary.
void credentialStoreContractBeforeRevoke(bool legacy);
#endif
namespace {
constexpr auto kDisabled = "auth/restoreDisabled";
constexpr auto kActive = "auth/keyringV2/active";
constexpr auto kPending = "auth/keyringV2/pending";
constexpr auto kRevision = "auth/keyringV2/revision";
constexpr auto kTransaction = "auth/keyringV2/transaction";
constexpr auto kJournal = "auth/keyringV2/journal";
constexpr auto kCreating = "auth/keyringV2/creating";
constexpr auto kService = "auth/keyringV2/service";
constexpr auto kAccount = "auth/keyringV2/account";
constexpr int kJournalLimit = 32;
constexpr int kMaxPlaintext = 64 * 1024;
constexpr qint64 kMaxMetadata = 128 * 1024;

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validUuid(const QString &value) {
    const QUuid id(value);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == value;
}

struct Metadata {
    quint64 revision = 0;
    QString transaction;
    QString active;
    QString pending;
    QStringList journal;
    QVariantMap creating;
    QString service;
    QString account;
    bool disabled = true;
    bool legacyPending = false;
    bool legacyHandled = false;
    bool sourceMalformed = false;
};

bool sameBinding(const Metadata &left, const Metadata &right) {
    return left.revision == right.revision && left.transaction == right.transaction &&
        left.active == right.active && left.pending == right.pending && left.service == right.service &&
        left.account == right.account && left.disabled == right.disabled;
}

std::shared_ptr<SecretBytes> encode(const StoredSession &session) {
    if (session.service.isEmpty() || canonicalServiceUrl(session.service) != session.service ||
            !serviceAllowsCredentialPersistence(session.service) || session.account.isEmpty() ||
            session.user.userId.isEmpty() || session.user.token.isEmpty()) return {};
    QJsonObject object;
    object["version"] = 2;
    object["service"] = session.service;
    object["account"] = session.account;
    object["user"] = session.user.toJson();
    object["autoLogin"] = session.autoLogin;
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaxPlaintext) {
        auto wipeOnExit = std::make_shared<SecretBytes>(std::move(bytes));
        return {};
    }
    return std::make_shared<SecretBytes>(std::move(bytes));
}

CredentialLoadResult decode(const SecretBytes &secret, const QString &service, const QString &account) {
    if (secret.bytes().isEmpty() || secret.bytes().size() > kMaxPlaintext) return {CredentialStatus::InvalidRecord, {}};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(secret.bytes(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {CredentialStatus::InvalidRecord, {}};
    const auto object = document.object();
    const auto user = object.value("user");
    if (!object.value("version").isDouble() || object.value("version").toDouble() != 2 ||
            !object.value("service").isString() || !object.value("account").isString() ||
            !object.value("autoLogin").isBool() || !user.isObject() ||
            !user.toObject().value("token").isString() || !user.toObject().value("userID").isString())
        return {CredentialStatus::InvalidRecord, {}};
    StoredSession session;
    session.service = object.value("service").toString();
    session.account = object.value("account").toString();
    session.autoLogin = object.value("autoLogin").toBool();
    session.user = UserInfo::fromJson(user.toObject());
    if (service.isEmpty() || canonicalServiceUrl(service) != service ||
            session.service != service || session.account != account || account.isEmpty() ||
            session.user.token.isEmpty() || session.user.userId.isEmpty()) return {CredentialStatus::InvalidRecord, {}};
    return {CredentialStatus::Ready, std::move(session)};
}

class LinuxCredentialStore final : public QObject, public CredentialStore {
public:
    LinuxCredentialStore(QSettings &settings, std::unique_ptr<SecretServiceTransport> transport)
        : settings_(settings), transport_(std::move(transport)) {
        gate_ = QtCallbackGate<LinuxCredentialStore>::Create(this);
        const auto path = settings_.fileName();
        if (!path.isEmpty() && QDir().mkpath(QFileInfo(path).absolutePath())) {
            ownerLock_ = std::make_shared<QLockFile>(ownerLockPath(ownerId_));
            ownerLock_->setStaleLockTime(0);
            if (!ownerLock_->tryLock(0)) ownerLock_.reset();
        }
        if (!transport_) transport_ = makeSecretServiceTransport();
    }
    ~LinuxCredentialStore() override {
        gate_->Revoke();
        auto lease = std::move(ownerLock_);
        transport_->shutdown([lease = std::move(lease)] {});
    }

    // Linux production uses the asynchronous API. These entry points never
    // wait for D-Bus and cannot claim that an asynchronous secret write ended.
    CredentialLoadResult load(const QString &, const QString &) override { return {CredentialStatus::Unavailable, {}}; }
    CredentialStatus save(const StoredSession &) override { return CredentialStatus::SaveFailed; }
    CredentialStatus clear() override {
        invalidate();
        Metadata snapshot;
        const auto status = revoke(snapshot);
        if (status != CredentialStatus::Empty) return status;
        if (snapshot.journal.isEmpty()) return snapshot.legacyPending ? CredentialStatus::CleanupFailed : CredentialStatus::Empty;
        eraseRetired(snapshot.journal, {});
        return CredentialStatus::CleanupFailed;
    }

    RequestId loadAsync(const QString &service, const QString &account,
            Interaction interaction, QObject *context, LoadCompletion completion) override {
        assertOwner();
        auto request = pending(context);
        request->load = std::move(completion);
        if (closed_) { queueFinish(request, CredentialStatus::Cancelled); return request->id; }
        if (!ownerLock_) { queueFinish(request, CredentialStatus::Unavailable); return request->id; }
        Metadata snapshot;
        auto status = readMetadata(snapshot);
        if (status != CredentialStatus::Ready) { queueFinish(request, status); return request->id; }
        Metadata legacyRetired;
        status = revoke(legacyRetired, &snapshot, true);
        if (status == CredentialStatus::Empty) {
            eraseRetired(legacyRetired.journal, [gate = gate_, request](CredentialStatus cleanup) {
                gate->Post([request, cleanup](LinuxCredentialStore *self) {
                    // Legacy source keys were deliberately retained. This is
                    // a revoked migration, not proof of plaintext deletion.
                    self->finishRequest(request, CredentialStatus::CleanupFailed);
                });
            });
            return request->id;
        }
        if (status != CredentialStatus::Ready) { queueFinish(request, status); return request->id; }
        if (!serviceAllowsCredentialPersistence(service) || account.isEmpty()) {
            queueFinish(request, CredentialStatus::Empty); return request->id;
        }
        if (snapshot.disabled || snapshot.active.isEmpty()) {
            queueFinish(request, snapshot.legacyPending ? CredentialStatus::CleanupFailed : CredentialStatus::Empty);
            // Pending and retired UUIDs are never used for restoration. Cleanup
            // is precise and cannot unlock a collection or affect the UI result.
            auto retired = snapshot.journal;
            retired.removeAll(snapshot.pending);
            if (!retired.isEmpty()) eraseRetired(std::move(retired), {});
            return request->id;
        }
        if (snapshot.service != service || snapshot.account != account) {
            queueFinish(request, CredentialStatus::Empty); return request->id;
        }
        request->snapshot = snapshot;
        request->transportId = transport_->load(snapshot.active, interaction,
            [gate = gate_, request, service, account](SecretServiceResult result) mutable {
                gate->Post([request, service, account, result = std::move(result)](LinuxCredentialStore *self) mutable {
                    self->completeLoad(request, service, account, std::move(result));
                });
            });
        if (!request->transportId) queueFinish(request, CredentialStatus::Unavailable);
        return request->id;
    }

    RequestId saveAsync(StoredSession session, Interaction interaction,
            QObject *context, StatusCompletion completion) override {
        assertOwner();
        auto request = pending(context);
        request->status = std::move(completion);
        if (closed_) { queueFinish(request, CredentialStatus::Cancelled); return request->id; }
        if (!ownerLock_) { queueFinish(request, CredentialStatus::Unavailable); return request->id; }
        auto value = encode(session);
        if (!value) { queueFinish(request, CredentialStatus::SaveFailed); return request->id; }
        invalidate(request);
        request->generation = generation_;
        request->record = uuid();
        auto status = prepareSave(session, request->record, request->snapshot);
        if (status != CredentialStatus::Ready) { queueFinish(request, status); return request->id; }
        request->transportId = transport_->save(request->record, std::move(value), interaction,
            [gate = gate_, request](SecretServiceResult result) mutable {
                gate->Post([request, result = std::move(result)](LinuxCredentialStore *self) mutable {
                    self->completeSave(request, std::move(result));
                });
            });
        if (!request->transportId) discardSave(request, CredentialStatus::Unavailable);
        return request->id;
    }

    RequestId clearAsync(QObject *context, StatusCompletion completion) override {
        assertOwner();
        auto request = pending(context);
        request->status = std::move(completion);
        request->clearing = true;
        // Revocation is synchronous and precedes any D-Bus operation or callback.
        invalidate(request);
        request->generation = generation_;
        Metadata snapshot;
        const auto status = revoke(snapshot);
        if (status != CredentialStatus::Empty) { queueFinish(request, CredentialStatus::CleanupFailed); return request->id; }
        eraseRetired(snapshot.journal, [gate = gate_, request, legacyPending = snapshot.legacyPending](CredentialStatus cleanup) {
            gate->Post([request, cleanup, legacyPending](LinuxCredentialStore *self) {
                self->finishRequest(request, legacyPending ? CredentialStatus::CleanupFailed : cleanup);
            });
        });
        return request->id;
    }

    void cancel(RequestId id) override {
        assertOwner();
        const auto found = requests_.find(id);
        if (found == requests_.end()) return;
        auto &request = found->second;
        request->cancelled = true;
        // Cancellation never cancels already committed logout revocation or
        // exact cleanup; otherwise an interrupted clear could strand secrets.
        if (request->transportId && !request->clearing) transport_->cancel(request->transportId);
    }

    void shutdownAsync(QObject *context, std::function<void()> completion) override {
        assertOwner();
        closed_ = true;
        for (auto &[id, request] : requests_) request->cancelled = true;
        const auto receiver = QtCallbackGate<QObject>::Create(context ? context : this);
        auto lease = std::move(ownerLock_);
        transport_->shutdown([receiver, lease = std::move(lease), completion = std::move(completion)]() mutable {
            // This only retires the local producer. It is not a remote Create
            // ACK; uncertain UUID references survive even after this lease.
            lease.reset();
            if (receiver) receiver->Post([completion = std::move(completion)](QObject *) mutable {
                if (completion) completion();
            });
        });
    }

private:
    struct Pending {
        RequestId id = 0;
        quint64 generation = 0;
        SecretServiceTransport::RequestId transportId = 0;
        QPointer<QObject> context;
        LoadCompletion load;
        StatusCompletion status;
        Metadata snapshot;
        QString record;
        bool cancelled = false;
        bool clearing = false;
        bool finished = false;
    };
    QSettings &settings_;
    std::unique_ptr<SecretServiceTransport> transport_;
    std::shared_ptr<QtCallbackGate<LinuxCredentialStore>> gate_;
    std::map<RequestId, std::shared_ptr<Pending>> requests_;
    RequestId next_ = 0;
    quint64 generation_ = 0;
    bool closed_ = false;
    const QString ownerId_ = uuid();
    std::shared_ptr<QLockFile> ownerLock_;

    QString ownerLockPath(const QString &owner) const {
        return settings_.fileName() + QStringLiteral(".cohavora-keyring-owner-") + owner + QStringLiteral(".lock");
    }
    bool creatorAlive(const QString &owner) const {
        if (owner == ownerId_ && ownerLock_) return true;
        QLockFile probe(ownerLockPath(owner));
        probe.setStaleLockTime(0);
        if (!probe.tryLock(0)) return true; // Ambiguity fails closed.
        return false;
    }

    void assertOwner() const { Q_ASSERT(QThread::currentThread() == thread()); }
    std::shared_ptr<Pending> pending(QObject *context) {
        assertOwner();
        auto request = std::make_shared<Pending>();
        request->id = ++next_;
        request->generation = generation_;
        request->context = context ? context : this;
        Q_ASSERT(request->context->thread() == thread());
        requests_.emplace(request->id, request);
        return request;
    }
    void invalidate(const std::shared_ptr<Pending> &except = {}) {
        assertOwner();
        ++generation_;
        for (auto &[id, request] : requests_) {
            if (request == except || request->clearing) continue;
            request->cancelled = true;
            if (request->transportId) transport_->cancel(request->transportId);
        }
    }
    void finishRequest(const std::shared_ptr<Pending> &request, CredentialStatus status, StoredSession session = {}) {
        assertOwner();
        if (request->finished) return;
        request->finished = true;
        requests_.erase(request->id);
        if (request->cancelled && status != CredentialStatus::CleanupFailed) {
            status = CredentialStatus::Cancelled;
            session = {};
        }
        if (!request->context) return;
        auto load = std::move(request->load);
        auto callback = std::move(request->status);
        if (load) load({status, std::move(session)});
        else if (callback) callback(status);
    }
    void queueFinish(const std::shared_ptr<Pending> &request, CredentialStatus status) {
        gate_->Post([request, status](LinuxCredentialStore *self) { self->finishRequest(request, status); });
    }

    QString metadataPath() const {
        return settings_.fileName() + QStringLiteral(".cohavora-keyring-v2.json");
    }
    // Never call sync()/setValue()/remove() on the shared application settings.
    // Qt's own <settings>.lock uses an unbounded wait even when an unrelated
    // settings writer does not participate in our credentials lock.
    CredentialStatus importMetadata(Metadata &metadata) {
        const auto source = settings_.fileName();
        if (QFileInfo(source).isSymLink()) return CredentialStatus::Unavailable;
        QLockFile sourceLock(source + QStringLiteral(".lock"));
        if (!sourceLock.tryLock(0)) return CredentialStatus::Busy;
        const auto legacy = settings_.property("cohavoraLegacyKeyringMetadata").toMap();
        const auto legacyPath = legacy.value("source_path").toString();
        std::unique_ptr<QLockFile> legacyLock;
        if (!legacyPath.isEmpty() && legacyPath != source) {
            if (QFileInfo(legacyPath).isSymLink()) return CredentialStatus::Unavailable;
            // A fresh install has no legacy namespace directory yet. Keep the
            // same source-lock boundary instead of treating its absence as busy.
            if (!QDir().mkpath(QFileInfo(legacyPath).absolutePath())) return CredentialStatus::Unavailable;
            legacyLock = std::make_unique<QLockFile>(legacyPath + QStringLiteral(".lock"));
            if (!legacyLock->tryLock(0)) return CredentialStatus::Busy;
        }
        if (settings_.status() != QSettings::NoError) return CredentialStatus::Unavailable;
        bool ok = true;
        metadata.revision = settings_.value(kRevision, qulonglong(0)).toULongLong(&ok);
        metadata.transaction = settings_.value(kTransaction).toString();
        metadata.journal = settings_.value(kJournal).toStringList();
        metadata.creating = settings_.value(kCreating).toMap();
        // Only already-loaded, nonsecret whitelist values are imported. The
        // source is retained, so a stale snapshot cannot discard its only UUID
        // reference. No imported record can be restored without a fresh login.
        for (const auto key : {kActive, kPending}) {
            const auto id = settings_.value(key).toString();
            if (!id.isEmpty() && !metadata.journal.contains(id)) metadata.journal.push_back(id);
        }
        metadata.legacyPending = settings_.contains("auth/password") ||
            settings_.contains("auth/protectedSessionV2") || settings_.contains("user/token") ||
            settings_.contains("auth/rememberPassword") || settings_.contains("auth/autoLogin");
        metadata.sourceMalformed = !ok;
        metadata.legacyPending = metadata.legacyPending || legacy.value("has_legacy_secret_fields").toBool();
        // The integration layer passes only nonsecret metadata from the old
        // application namespace. Never splice bindings or activate either one.
        const auto oldJournal = legacy.value(kJournal).toStringList();
        for (const auto &id : oldJournal)
            if (!metadata.journal.contains(id)) metadata.journal.push_back(id);
        for (const auto key : {kActive, kPending}) {
            const auto id = legacy.value(key).toString();
            if (!id.isEmpty() && !metadata.journal.contains(id)) metadata.journal.push_back(id);
        }
        const auto oldCreating = legacy.value(kCreating).toMap();
        for (auto it = oldCreating.cbegin(); it != oldCreating.cend(); ++it) {
            if (metadata.creating.contains(it.key()) && metadata.creating.value(it.key()) != it.value())
                metadata.sourceMalformed = true;
            else metadata.creating.insert(it.key(), it.value());
        }
        metadata.disabled = true;
        return persist(metadata) ? CredentialStatus::Ready : CredentialStatus::Unavailable;
    }
    CredentialStatus readFile(Metadata &metadata) {
        const auto encoded = QFile::encodeName(metadataPath());
        const int descriptor = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) return errno == ENOENT ? importMetadata(metadata) : CredentialStatus::Unavailable;
        struct stat info {};
        if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
                (info.st_mode & 0777) != 0600 || info.st_size < 0 || info.st_size > kMaxMetadata) {
            ::close(descriptor); return CredentialStatus::Unavailable;
        }
        QFile file;
        if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(descriptor); return CredentialStatus::Unavailable;
        }
        const auto bytes = file.read(kMaxMetadata + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > kMaxMetadata) return CredentialStatus::Unavailable;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() ||
                document.toJson(QJsonDocument::Compact) != bytes) return CredentialStatus::InvalidRecord;
        // Internal canonical encoding only: this also rejects duplicate object
        // keys (QJsonDocument would otherwise silently retain the last one).
        const auto object = document.object();
        // Revision is a decimal string: a JSON double cannot preserve uint64.
        if (object.size() != 13 || !object.value("version").isDouble() || object.value("version").toDouble() != 1 ||
                !object.value("revision").isString() || !object.value("journal").isArray() ||
                !object.value("creating").isObject()) return CredentialStatus::InvalidRecord;
        for (const auto key : {"transaction", "active", "pending", "service", "account"})
            if (!object.value(key).isString()) return CredentialStatus::InvalidRecord;
        for (const auto key : {"disabled", "legacyPending", "legacyHandled", "sourceMalformed"})
            if (!object.value(key).isBool()) return CredentialStatus::InvalidRecord;
        bool ok = true;
        const auto revision = object.value("revision").toString();
        metadata.revision = revision.toULongLong(&ok);
        if (!ok || QString::number(metadata.revision) != revision) return CredentialStatus::InvalidRecord;
        metadata.transaction = object.value("transaction").toString();
        metadata.active = object.value("active").toString();
        metadata.pending = object.value("pending").toString();
        metadata.service = object.value("service").toString();
        metadata.account = object.value("account").toString();
        metadata.disabled = object.value("disabled").toBool();
        metadata.legacyPending = object.value("legacyPending").toBool();
        metadata.legacyHandled = object.value("legacyHandled").toBool();
        metadata.sourceMalformed = object.value("sourceMalformed").toBool();
        for (const auto value : object.value("journal").toArray()) {
            if (!value.isString()) return CredentialStatus::InvalidRecord;
            metadata.journal.push_back(value.toString());
        }
        const auto creating = object.value("creating").toObject();
        for (auto it = creating.begin(); it != creating.end(); ++it) {
            if (!it.value().isString()) return CredentialStatus::InvalidRecord;
            metadata.creating.insert(it.key(), it.value().toString());
        }
        return CredentialStatus::Ready;
    }
    template <typename F>
    CredentialStatus locked(F operation, bool revokeOnInvalid = false) {
        assertOwner();
        const auto path = settings_.fileName();
        if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) return CredentialStatus::Unavailable;
        QLockFile lock(path + QStringLiteral(".cohavora-credentials.lock"));
        if (!lock.tryLock(0)) return CredentialStatus::Busy;
        Metadata metadata;
        const auto read = readFile(metadata);
        // Structural/read errors cannot be mistaken for an empty store or
        // overwritten: doing so could erase the only pending cleanup journal.
        if (read != CredentialStatus::Ready) return read;
        bool valid = !metadata.sourceMalformed && (!metadata.revision || validUuid(metadata.transaction)) &&
                (metadata.pending.isEmpty() || validUuid(metadata.pending)) &&
                (metadata.active.isEmpty() || validUuid(metadata.active)) &&
                metadata.journal.size() <= kJournalLimit && metadata.creating.size() <= kJournalLimit;
        QStringList unique;
        for (const auto &id : metadata.journal) {
            if (!validUuid(id) || unique.contains(id)) valid = false;
            unique.push_back(id);
        }
        for (auto it = metadata.creating.cbegin(); it != metadata.creating.cend(); ++it) {
            if (!validUuid(it.key()) || !validUuid(it.value().toString()) || !metadata.journal.contains(it.key()))
                valid = false;
        }
        if ((!metadata.active.isEmpty() && !metadata.journal.contains(metadata.active)) ||
                (!metadata.pending.isEmpty() && (!metadata.journal.contains(metadata.pending) ||
                    !metadata.active.isEmpty() || !metadata.disabled))) valid = false;
        if (!valid) {
            if (revokeOnInvalid) {
                // Preserve even corrupt journal entries; never broaden cleanup.
                metadata.disabled = true;
                metadata.active.clear();
                metadata.pending.clear();
                metadata.transaction = uuid();
                metadata.revision = 1;
                metadata.service.clear();
                metadata.account.clear();
                metadata.legacyHandled = true;
                persist(metadata);
                return CredentialStatus::CleanupFailed;
            }
            return CredentialStatus::InvalidRecord;
        }
        return operation(metadata);
    }
    CredentialStatus readMetadata(Metadata &snapshot) {
        return locked([&](const Metadata &metadata) { snapshot = metadata; return CredentialStatus::Ready; });
    }
    bool persist(const Metadata &metadata) {
        const auto path = metadataPath();
        const auto encoded = QFile::encodeName(path);
        struct stat info {};
        if (::lstat(encoded.constData(), &info) == 0) {
            if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0600) return false;
        } else if (errno != ENOENT) return false;
        QJsonArray journal;
        for (const auto &id : metadata.journal) journal.push_back(id);
        QJsonObject creating;
        for (auto it = metadata.creating.cbegin(); it != metadata.creating.cend(); ++it)
            creating.insert(it.key(), it.value().toString());
        const QJsonObject object{{"version", 1}, {"revision", QString::number(metadata.revision)},
            {"transaction", metadata.transaction}, {"active", metadata.active}, {"pending", metadata.pending},
            {"journal", journal}, {"creating", creating}, {"service", metadata.service}, {"account", metadata.account},
            {"disabled", metadata.disabled}, {"legacyPending", metadata.legacyPending},
            {"legacyHandled", metadata.legacyHandled}, {"sourceMalformed", metadata.sourceMalformed}};
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (bytes.size() > kMaxMetadata) return false;
        // QSaveFile follows an existing symlink. A fresh 0600 temporary file +
        // atomic POSIX rename never opens/truncates the destination through one.
        QTemporaryFile temporary(path + QStringLiteral(".tmp-XXXXXX"));
        if (!temporary.open() || !temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                temporary.write(bytes) != bytes.size() || !temporary.flush() || ::fsync(temporary.handle()) != 0) return false;
        const auto temporaryName = QFile::encodeName(temporary.fileName());
        temporary.close();
        if (::rename(temporaryName.constData(), encoded.constData()) != 0) return false;
        const auto parent = QFile::encodeName(QFileInfo(path).absolutePath());
        const int directory = ::open(parent.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        if (directory < 0) return false;
        const bool synced = ::fsync(directory) == 0;
        ::close(directory);
        return synced;
    }
    bool advance(Metadata &metadata) {
        if (metadata.revision == std::numeric_limits<quint64>::max()) return false;
        ++metadata.revision;
        metadata.transaction = uuid();
        metadata.disabled = true;
        metadata.active.clear();
        metadata.pending.clear();
        return true;
    }
    CredentialStatus prepareSave(const StoredSession &session, const QString &record, Metadata &snapshot) {
        return locked([&](Metadata &metadata) {
            if (metadata.journal.size() >= kJournalLimit) return CredentialStatus::Busy;
            if (!advance(metadata)) return CredentialStatus::SaveFailed;
            metadata.journal.push_back(record);
            metadata.creating.insert(record, ownerId_);
            metadata.pending = record;
            metadata.service = session.service;
            metadata.account = session.account;
            metadata.legacyHandled = true;
            if (!persist(metadata)) return CredentialStatus::CleanupFailed;
            snapshot = metadata;
            return CredentialStatus::Ready;
        });
    }
    CredentialStatus revoke(Metadata &snapshot, const Metadata *expected = nullptr, bool legacyOnly = false) {
        const auto status = locked([&](Metadata &metadata) {
            // Compare and revoke under the same interprocess lock. A stale
            // invalid/empty/legacy read must not revoke another instance's save.
            if (expected && !sameBinding(metadata, *expected)) return CredentialStatus::Cancelled;
            if (legacyOnly && (!metadata.legacyPending || metadata.legacyHandled)) return CredentialStatus::Ready;
#ifdef COHAVORA_KEYRING_CONTRACT_TESTING
            if (expected) credentialStoreContractBeforeRevoke(legacyOnly);
#endif
            if (!advance(metadata)) {
                metadata.disabled = true;
                metadata.active.clear();
                metadata.pending.clear();
                metadata.transaction = uuid();
                persist(metadata);
                return CredentialStatus::CleanupFailed;
            }
            metadata.service.clear();
            metadata.account.clear();
            metadata.legacyHandled = true;
            if (!persist(metadata)) return CredentialStatus::CleanupFailed;
            snapshot = metadata;
            return CredentialStatus::Empty;
        }, expected == nullptr);
        if (expected) return status;
        return status == CredentialStatus::Empty ? status : CredentialStatus::CleanupFailed;
    }
    bool reconcileErased(const QStringList &ids) {
        if (ids.isEmpty()) return true;
        return locked([&](Metadata &metadata) {
            bool uncertain = false;
            for (const auto &id : ids) {
                if ((!metadata.disabled && metadata.active == id) || metadata.pending == id)
                    return CredentialStatus::CleanupFailed;
                if (metadata.creating.contains(id)) {
                    // A local cancelled callback, an expired producer lease,
                    // or a not-found/delete response is not the missing remote
                    // Create ACK. Keep the only precise cleanup reference.
                    uncertain = true;
                    continue;
                }
                metadata.journal.removeAll(id);
            }
            // This only drops already deleted inactive references. It cannot
            // replace a transaction or invalidate another instance's save.
            return persist(metadata) && !uncertain ? CredentialStatus::Ready : CredentialStatus::CleanupFailed;
        }) == CredentialStatus::Ready;
    }
    void eraseRetired(QStringList ids, StatusCompletion completion) {
        if (ids.isEmpty()) {
            if (completion) gate_->Post([completion = std::move(completion)](LinuxCredentialStore *) mutable {
                completion(CredentialStatus::Empty);
            });
            return;
        }
        Metadata current;
        if (readMetadata(current) != CredentialStatus::Ready) {
            if (completion) gate_->Post([completion = std::move(completion)](LinuxCredentialStore *) mutable {
                completion(CredentialStatus::CleanupFailed);
            });
            return;
        }
        if (!current.disabled) ids.removeAll(current.active);
        ids.removeAll(current.pending);
        bool deferred = false;
        for (auto it = current.creating.cbegin(); it != current.creating.cend(); ++it) {
            if (ids.contains(it.key()) && creatorAlive(it.value().toString())) {
                ids.removeAll(it.key());
                deferred = true;
            }
        }
        if (ids.isEmpty()) {
            if (completion) gate_->Post([completion = std::move(completion), deferred](LinuxCredentialStore *) mutable {
                completion(deferred ? CredentialStatus::CleanupFailed : CredentialStatus::Empty);
            });
            return;
        }
        auto terminal = std::make_shared<StatusCompletion>(std::move(completion));
        const auto transportId = transport_->erase(std::move(ids),
            [gate = gate_, terminal, deferred](SecretServiceResult result) mutable {
                gate->Post([result = std::move(result), terminal, deferred](LinuxCredentialStore *self) mutable {
                    const bool reconciled = self->reconcileErased(result.erased);
                    auto completion = std::move(*terminal);
                    if (completion) completion(result.status == CredentialStatus::Empty && reconciled && !deferred
                        ? CredentialStatus::Empty : CredentialStatus::CleanupFailed);
                });
            });
        if (!transportId && *terminal) gate_->Post([terminal](LinuxCredentialStore *) mutable {
            auto completion = std::move(*terminal);
            if (completion) completion(CredentialStatus::CleanupFailed);
        });
    }
    void completeLoad(const std::shared_ptr<Pending> &request, const QString &service,
            const QString &account, SecretServiceResult result) {
        if (request->cancelled || request->generation != generation_) {
            finishRequest(request, CredentialStatus::Cancelled); return;
        }
        auto loaded = result.status == CredentialStatus::Ready
            ? (result.secret ? decode(*result.secret, service, account) : CredentialLoadResult{CredentialStatus::InvalidRecord, {}})
            : CredentialLoadResult{result.status, {}};
        result.secret.reset();
        if (loaded.status == CredentialStatus::InvalidRecord || loaded.status == CredentialStatus::Empty) {
            Metadata retired;
            const auto revoked = revoke(retired, &request->snapshot);
            if (revoked != CredentialStatus::Empty) {
                finishRequest(request, revoked); return;
            }
            eraseRetired(retired.journal, [gate = gate_, request](CredentialStatus status) {
                gate->Post([request, status](LinuxCredentialStore *self) {
                    self->finishRequest(request, status == CredentialStatus::Empty
                        ? CredentialStatus::InvalidRecord : CredentialStatus::CleanupFailed);
                });
            });
            return;
        }
        if (loaded.status != CredentialStatus::Ready) { finishRequest(request, loaded.status); return; }
        // The recheck and activation callback share one Qt turn. No extra
        // queued hop may appear between this check and SessionManager commit.
        const auto status = locked([&](const Metadata &current) {
            return sameBinding(current, request->snapshot) && !current.disabled
                ? CredentialStatus::Ready : CredentialStatus::Cancelled;
        });
        if (status == CredentialStatus::Ready) finishRequest(request, status, std::move(loaded.session));
        else finishRequest(request, status);
    }
    void completeSave(const std::shared_ptr<Pending> &request, SecretServiceResult result) {
        // Only an explicit item ACK, or proof that Create was never submitted,
        // can settle creation. Local drain/join cannot settle a remote effect.
        const bool settled = result.creation == SecretServiceResult::Creation::Acknowledged ||
            (result.creation == SecretServiceResult::Creation::NotSubmitted && result.status != CredentialStatus::Ready);
        if (settled) {
            const auto acknowledged = locked([&](Metadata &current) {
                if (current.creating.value(request->record).toString() == ownerId_)
                    current.creating.remove(request->record);
                return persist(current) ? CredentialStatus::Ready : CredentialStatus::CleanupFailed;
            });
            if (acknowledged != CredentialStatus::Ready) { finishRequest(request, CredentialStatus::CleanupFailed); return; }
        }
        if (result.status == CredentialStatus::Ready && result.creation != SecretServiceResult::Creation::Acknowledged)
            result.status = CredentialStatus::CleanupFailed;
        if (result.status != CredentialStatus::Ready || request->cancelled || request->generation != generation_) {
            discardSave(request, request->cancelled || request->generation != generation_
                ? CredentialStatus::Cancelled : result.status);
            return;
        }
        const auto committed = locked([&](Metadata &current) {
            if (!sameBinding(current, request->snapshot) || !current.journal.contains(request->record))
                return CredentialStatus::Cancelled;
            current.active = request->record;
            current.pending.clear();
            current.disabled = false;
            if (persist(current)) return CredentialStatus::Ready;
            // Best effort to retain the disabled transaction after an atomic
            // commit/directory-sync failure; no QSettings destructor retries.
            current.active.clear();
            current.disabled = true;
            persist(current);
            return CredentialStatus::CleanupFailed;
        });
        if (committed != CredentialStatus::Ready) { discardSave(request, committed); return; }
        auto retired = request->snapshot.journal;
        retired.removeAll(request->record);
        if (!retired.isEmpty()) eraseRetired(std::move(retired), {});
        finishRequest(request, CredentialStatus::Ready);
    }
    void discardSave(const std::shared_ptr<Pending> &request, CredentialStatus status) {
        // A failed/cancelled create can still have reached the service. Erase
        // only its unique UUID; its journal reference persists until success.
        const auto retired = locked([&](Metadata &current) {
            if (current.pending != request->record || !sameBinding(current, request->snapshot))
                return CredentialStatus::Ready;
            if (!advance(current) || !persist(current)) return CredentialStatus::CleanupFailed;
            return CredentialStatus::Ready;
        });
        if (retired != CredentialStatus::Ready) { finishRequest(request, CredentialStatus::CleanupFailed); return; }
        eraseRetired({request->record}, [gate = gate_, request, status](CredentialStatus cleanup) {
            gate->Post([request, status, cleanup](LinuxCredentialStore *self) {
                self->finishRequest(request, cleanup == CredentialStatus::Empty ? status : CredentialStatus::CleanupFailed);
            });
        });
    }
};
} // namespace

std::unique_ptr<CredentialStore> makeLinuxCredentialStore(QSettings &settings,
        std::unique_ptr<SecretServiceTransport> transport) {
    return std::make_unique<LinuxCredentialStore>(settings, std::move(transport));
}
} // namespace OpenMeeting
