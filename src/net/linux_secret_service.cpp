#include <libsecret/secret.h>
#include "src/net/linux_secret_service.h"
#include <QtCore/QUuid>
#include <openssl/crypto.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace OpenMeeting {

SecretBytes::SecretBytes(QByteArray bytes) : bytes_(std::move(bytes)) {}
SecretBytes::~SecretBytes() {
    if (!bytes_.isEmpty()) OPENSSL_cleanse(bytes_.data(), static_cast<size_t>(bytes_.size()));
}

namespace {
constexpr int kMaxSecretBytes = 64 * 1024;
constexpr unsigned kTimeoutSeconds = 30;
constexpr size_t kMaxPending = 32;
const SecretSchema kSchema = {"org.cohavora.Credentials.v2", SECRET_SCHEMA_NONE,
    {{"record", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"version", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, {}}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
const SecretSchema kTestSchema = {"org.cohavora.Credentials.Test.v2", SECRET_SCHEMA_NONE,
    {{"record", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"version", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, {}}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

// High-level libsecret calls may discover a locked collection after our first
// check. Deny prompts inside libsecret itself for background loads/saves and all
// deletion requests, so that this race cannot produce an unsolicited dialog.
typedef struct { SecretService parent; } CohavoraSecretService;
typedef struct { SecretServiceClass parent; } CohavoraSecretServiceClass;
G_DEFINE_TYPE(CohavoraSecretService, cohavora_secret_service, SECRET_TYPE_SERVICE)

GVariant *denySyncPrompt(SecretService *, SecretPrompt *, GCancellable *,
        const GVariantType *, GError **error) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Synchronous keyring prompts are disabled");
    return nullptr;
}

void controlledPrompt(SecretService *service, SecretPrompt *prompt,
        const GVariantType *type, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    auto *task = g_task_new(service, cancel, callback, data);
    if (!g_object_get_data(G_OBJECT(service), "cohavora-allow-prompt")) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Keyring interaction is disabled");
        g_object_unref(task);
        return;
    }
    secret_prompt_perform(prompt, nullptr, type, cancel,
        [](GObject *source, GAsyncResult *result, gpointer opaque) {
            auto *task = G_TASK(opaque);
            GError *error = nullptr;
            auto *value = secret_prompt_perform_finish(SECRET_PROMPT(source), result, &error);
            if (error) g_task_return_error(task, error);
            else g_task_return_pointer(task, value, reinterpret_cast<GDestroyNotify>(g_variant_unref));
            g_object_unref(task);
        }, task);
}

GVariant *controlledPromptFinish(SecretService *, GAsyncResult *result, GError **error) {
    return static_cast<GVariant *>(g_task_propagate_pointer(G_TASK(result), error));
}

void cohavora_secret_service_class_init(CohavoraSecretServiceClass *klass) {
    auto *service = SECRET_SERVICE_CLASS(klass);
    service->prompt_sync = denySyncPrompt;
    service->prompt_async = controlledPrompt;
    service->prompt_finish = controlledPromptFinish;
}
void cohavora_secret_service_init(CohavoraSecretService *) {}

struct Owner;
struct Operation final {
    enum class Kind { Load, Save, Erase };
    std::shared_ptr<Owner> owner;
    SecretServiceTransport::RequestId id = 0;
    Kind kind = Kind::Load;
    CredentialStore::Interaction interaction = CredentialStore::Interaction::Background;
    QStringList records;
    int index = 0;
    QStringList erased;
    std::shared_ptr<SecretBytes> payload;
    SecretServiceTransport::Completion completion;
    GCancellable *cancel = g_cancellable_new();
    SecretService *service = nullptr;
    SecretCollection *collection = nullptr;
    SecretItem *item = nullptr;
    GSource *timeout = nullptr;
    gulong ownerHandler = 0;
    std::string uniqueOwner;
    bool timedOut = false;
    bool ownerLost = false;
    bool finished = false;
    bool createSubmitted = false;
    bool createAcknowledged = false;
    SecretServiceDiagnostics diagnostics;

    ~Operation() {
        if (timeout) { g_source_destroy(timeout); g_source_unref(timeout); }
        if (service && ownerHandler) g_signal_handler_disconnect(service, ownerHandler);
        g_clear_object(&item);
        g_clear_object(&collection);
        g_clear_object(&service);
        g_clear_object(&cancel);
    }
};

std::shared_ptr<Operation> takeOperation(gpointer data) {
    std::unique_ptr<std::shared_ptr<Operation>> holder(static_cast<std::shared_ptr<Operation> *>(data));
    return std::move(*holder);
}
gpointer keepOperation(const std::shared_ptr<Operation> &op) {
    return new std::shared_ptr<Operation>(op);
}

struct Owner final : std::enable_shared_from_this<Owner> {
    explicit Owner(SecretServiceNamespace space) : schema(space == SecretServiceNamespace::ContractTest ? &kTestSchema : &kSchema) {}
    const SecretSchema *schema;
    GMainContext *context = g_main_context_new();
    GMainLoop *loop = g_main_loop_new(context, false);
    std::map<SecretServiceTransport::RequestId, std::shared_ptr<Operation>> operations;
    std::atomic<size_t> queued{0};
    bool stopping = false;

    ~Owner() { g_main_loop_unref(loop); g_main_context_unref(context); }

    void maybeQuit() {
        if (stopping && operations.empty() && queued.load(std::memory_order_acquire) == 0)
            g_main_loop_quit(loop);
    }
    void enqueue(std::function<void()> command) {
        struct Command { std::shared_ptr<Owner> owner; std::function<void()> invoke; };
        auto *work = new Command{shared_from_this(), std::move(command)};
        queued.fetch_add(1, std::memory_order_release);
        auto *source = g_idle_source_new();
        g_source_set_priority(source, G_PRIORITY_DEFAULT);
        g_source_set_callback(source, [](gpointer data) -> gboolean {
            auto &work = *static_cast<Command *>(data);
            work.invoke();
            work.owner->queued.fetch_sub(1, std::memory_order_acq_rel);
            work.owner->maybeQuit();
            return G_SOURCE_REMOVE;
        }, work, [](gpointer data) { delete static_cast<Command *>(data); });
        g_source_attach(source, context);
        g_source_unref(source);
    }
};

CredentialStatus errorStatus(const std::shared_ptr<Operation> &op, const GError *error) {
    if (op->timedOut) return CredentialStatus::TimedOut;
    if (op->ownerLost) return CredentialStatus::Unavailable;
    if (g_cancellable_is_cancelled(op->cancel)) return CredentialStatus::Cancelled;
    if (!error) return CredentialStatus::Unavailable;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        return op->interaction == CredentialStore::Interaction::UserInitiated
            ? CredentialStatus::Denied : CredentialStatus::Locked;
    if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) return CredentialStatus::Locked;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
            g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT) ||
            g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY)) return CredentialStatus::TimedOut;
    return CredentialStatus::Unavailable;
}

void finish(const std::shared_ptr<Operation> &op, CredentialStatus status,
        std::shared_ptr<SecretBytes> value = {}) {
    if (op->finished) return;
    op->finished = true;
    if (op->timeout) { g_source_destroy(op->timeout); g_source_unref(op->timeout); op->timeout = nullptr; }
    if (op->service && op->ownerHandler) {
        g_signal_handler_disconnect(op->service, op->ownerHandler);
        op->ownerHandler = 0;
    }
    op->owner->operations.erase(op->id);
    auto completion = std::move(op->completion);
    op->payload.reset();
    const auto creation = op->createAcknowledged ? SecretServiceResult::Creation::Acknowledged :
        op->createSubmitted ? SecretServiceResult::Creation::Uncertain : SecretServiceResult::Creation::NotSubmitted;
    if (completion) completion({status, std::move(value), op->erased, op->diagnostics, creation});
    op->owner->maybeQuit();
}

bool interrupted(const std::shared_ptr<Operation> &op) {
    if (!g_cancellable_is_cancelled(op->cancel)) return false;
    finish(op, errorStatus(op, nullptr));
    return true;
}

bool failError(const std::shared_ptr<Operation> &op, GError *error) {
    if (!error && !g_cancellable_is_cancelled(op->cancel)) return false;
    finish(op, errorStatus(op, error));
    g_clear_error(&error);
    return true;
}

void findRecord(const std::shared_ptr<Operation> &op);
void readOrDelete(const std::shared_ptr<Operation> &op);
void createRecord(const std::shared_ptr<Operation> &op);

void unlockTarget(const std::shared_ptr<Operation> &op, GObject *target) {
    if (op->interaction != CredentialStore::Interaction::UserInitiated || op->kind == Operation::Kind::Erase) {
        finish(op, CredentialStatus::Locked);
        return;
    }
    GList one{target, nullptr, nullptr};
    secret_service_unlock(op->service, &one, op->cancel,
        [](GObject *, GAsyncResult *result, gpointer data) {
            auto op = takeOperation(data);
            GError *error = nullptr;
            GList *unlocked = nullptr;
            secret_service_unlock_finish(op->service, result, &unlocked, &error);
            g_list_free_full(unlocked, g_object_unref);
            if (failError(op, error)) return;
            const bool save = op->kind == Operation::Kind::Save;
            const bool locked = save ? secret_collection_get_locked(op->collection) : secret_item_get_locked(op->item);
            if (locked) { finish(op, CredentialStatus::Denied); return; }
            if (save) createRecord(op); else readOrDelete(op);
        }, keepOperation(op));
}

void readOrDelete(const std::shared_ptr<Operation> &op) {
    if (interrupted(op)) return;
    if (secret_item_get_locked(op->item)) { unlockTarget(op, G_OBJECT(op->item)); return; }
    if (op->kind == Operation::Kind::Erase) {
        secret_item_delete(op->item, op->cancel,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                auto op = takeOperation(data);
                GError *error = nullptr;
                const bool deleted = secret_item_delete_finish(SECRET_ITEM(source), result, &error);
                if (failError(op, error)) return;
                if (!deleted) { finish(op, CredentialStatus::CleanupFailed); return; }
                op->erased.push_back(op->records[op->index++]);
                g_clear_object(&op->item);
                findRecord(op);
            }, keepOperation(op));
    } else {
        secret_item_load_secret(op->item, op->cancel,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                auto op = takeOperation(data);
                GError *error = nullptr;
                const bool loaded = secret_item_load_secret_finish(SECRET_ITEM(source), result, &error);
                if (failError(op, error)) return;
                if (!loaded) { finish(op, CredentialStatus::Unavailable); return; }
                auto *value = secret_item_get_secret(op->item);
                gsize length = 0;
                const auto *bytes = value ? secret_value_get(value, &length) : nullptr;
                const auto *contentType = value ? secret_value_get_content_type(value) : nullptr;
                op->diagnostics = {value != nullptr, bytes != nullptr,
                    contentType && std::strcmp(contentType, "application/json") == 0,
                    static_cast<int>(std::min<gsize>(length, kMaxSecretBytes + 1))};
                // GNOME Keyring does not preserve the requested MIME type.
                // Preserve the bytes; the Qt record owner validates JSON,
                // version, required types and service/account binding. MIME
                // is diagnostic metadata, never the authentication boundary.
                if (!bytes || !length || length > kMaxSecretBytes) {
                    if (value) secret_value_unref(value);
                    finish(op, CredentialStatus::InvalidRecord);
                    return;
                }
                auto secret = std::make_shared<SecretBytes>(QByteArray(bytes, static_cast<int>(length)));
                secret_value_unref(value);
                finish(op, CredentialStatus::Ready, std::move(secret));
            }, keepOperation(op));
    }
}

void findRecord(const std::shared_ptr<Operation> &op) {
    if (interrupted(op)) return;
    if (op->index == op->records.size()) { finish(op, CredentialStatus::Empty); return; }
    const auto id = op->records[op->index].toUtf8();
    auto *attributes = secret_attributes_build(op->owner->schema, "record", id.constData(), "version", "2", nullptr);
    secret_service_search(op->service, op->owner->schema, attributes, SECRET_SEARCH_ALL, op->cancel,
        [](GObject *, GAsyncResult *result, gpointer data) {
            auto op = takeOperation(data);
            GError *error = nullptr;
            auto *items = secret_service_search_finish(op->service, result, &error);
            if (error || g_cancellable_is_cancelled(op->cancel)) {
                g_list_free_full(items, g_object_unref);
                failError(op, error);
                return;
            }
            // UUIDs are never reused. Multiple matches are an invalid record,
            // not permission to enumerate/delete a broader namespace.
            if (items && items->next) {
                g_list_free_full(items, g_object_unref);
                finish(op, CredentialStatus::InvalidRecord);
                return;
            }
            if (!items) {
                if (op->kind == Operation::Kind::Erase) {
                    op->erased.push_back(op->records[op->index++]);
                    findRecord(op);
                } else finish(op, CredentialStatus::Empty);
                return;
            }
            op->item = SECRET_ITEM(g_object_ref(items->data));
            g_list_free_full(items, g_object_unref);
            readOrDelete(op);
        }, keepOperation(op));
    g_hash_table_unref(attributes);
}

void createRecord(const std::shared_ptr<Operation> &op) {
    if (interrupted(op)) return;
    if (secret_collection_get_locked(op->collection)) { unlockTarget(op, G_OBJECT(op->collection)); return; }
    if (!op->payload || op->payload->bytes().isEmpty() || op->payload->bytes().size() > kMaxSecretBytes) {
        finish(op, CredentialStatus::SaveFailed);
        return;
    }
    const auto id = op->records.front().toUtf8();
    auto *attributes = secret_attributes_build(op->owner->schema, "record", id.constData(), "version", "2", nullptr);
    auto *value = secret_value_new(op->payload->bytes().constData(), op->payload->bytes().size(), "application/json");
    op->createSubmitted = true;
    secret_item_create(op->collection, op->owner->schema, attributes, "Cohavora saved sign-in session", value,
        SECRET_ITEM_CREATE_NONE, op->cancel,
        [](GObject *, GAsyncResult *result, gpointer data) {
            auto op = takeOperation(data);
            GError *error = nullptr;
            auto *item = secret_item_create_finish(result, &error);
            op->createAcknowledged = item != nullptr;
            if (item) g_object_unref(item);
            if (failError(op, error)) return;
            finish(op, item ? CredentialStatus::Ready : CredentialStatus::SaveFailed);
        }, keepOperation(op));
    secret_value_unref(value);
    g_hash_table_unref(attributes);
}

void openService(const std::shared_ptr<Operation> &op) {
    if (interrupted(op)) return;
    secret_service_open(cohavora_secret_service_get_type(), nullptr, SECRET_SERVICE_OPEN_SESSION, op->cancel,
        [](GObject *, GAsyncResult *result, gpointer data) {
            auto op = takeOperation(data);
            GError *error = nullptr;
            op->service = secret_service_open_finish(result, &error);
            if (failError(op, error)) return;
            if (!op->service) { finish(op, CredentialStatus::Unavailable); return; }
            auto *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(op->service));
            if (!owner || !*owner) {
                g_free(owner);
                finish(op, CredentialStatus::Unavailable);
                return;
            }
            op->uniqueOwner = owner;
            g_free(owner);
            op->ownerHandler = g_signal_connect(op->service, "notify::g-name-owner",
                G_CALLBACK(+[](GObject *source, GParamSpec *, gpointer data) {
                    auto *op = static_cast<Operation *>(data);
                    auto *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(source));
                    const bool changed = !owner || op->uniqueOwner != owner;
                    g_free(owner);
                    if (changed) { op->ownerLost = true; g_cancellable_cancel(op->cancel); }
                }), op.get());
            const bool interactive = op->interaction == CredentialStore::Interaction::UserInitiated &&
                op->kind != Operation::Kind::Erase;
            g_object_set_data(G_OBJECT(op->service), "cohavora-allow-prompt", interactive ? GUINT_TO_POINTER(1) : nullptr);
            if (op->kind != Operation::Kind::Save) { findRecord(op); return; }
            secret_collection_for_alias(op->service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, op->cancel,
                [](GObject *, GAsyncResult *result, gpointer data) {
                    auto op = takeOperation(data);
                    GError *error = nullptr;
                    op->collection = secret_collection_for_alias_finish(result, &error);
                    if (failError(op, error)) return;
                    // Do not create or silently substitute a session collection.
                    if (!op->collection) { finish(op, CredentialStatus::Unavailable); return; }
                    createRecord(op);
                }, keepOperation(op));
        }, keepOperation(op));
}

class LibsecretTransport final : public SecretServiceTransport {
public:
    explicit LibsecretTransport(SecretServiceNamespace space) : owner_(std::make_shared<Owner>(space)), join_(std::make_shared<Join>()) {
        auto owner = owner_;
        auto join = join_;
        // Create the reaper before the native owner. Shutdown/destruction then
        // needs no new native thread and never joins on the application thread.
        std::thread([owner, join]() mutable {
            {
                std::unique_lock lock(join->mutex);
                join->retire.wait(lock, [&] { return join->retiring; });
            }
            if (join->native.joinable()) join->native.join();
            std::vector<std::function<void()>> callbacks;
            {
                std::lock_guard lock(join->mutex);
                join->finished = true;
                callbacks.swap(join->callbacks);
            }
            for (auto &callback : callbacks) callback();
        }).detach();
        try {
            join_->native = std::thread([owner] {
                g_main_context_push_thread_default(owner->context);
                g_main_loop_run(owner->loop);
                g_main_context_pop_thread_default(owner->context);
            });
        } catch (...) {
            { std::lock_guard lock(join_->mutex); join_->retiring = true; }
            join_->retire.notify_one();
            throw;
        }
    }
    ~LibsecretTransport() override { shutdown({}); }

    RequestId load(QString id, CredentialStore::Interaction interaction, Completion completion) override {
        return submit(Operation::Kind::Load, {std::move(id)}, {}, interaction, std::move(completion));
    }
    RequestId save(QString id, std::shared_ptr<SecretBytes> secret,
            CredentialStore::Interaction interaction, Completion completion) override {
        return submit(Operation::Kind::Save, {std::move(id)}, std::move(secret), interaction, std::move(completion));
    }
    RequestId erase(QStringList ids, Completion completion) override {
        return submit(Operation::Kind::Erase, std::move(ids), {}, CredentialStore::Interaction::Background, std::move(completion));
    }
    void cancel(RequestId id) override {
        std::lock_guard lock(admission_);
        if (closed_) return;
        auto owner = owner_;
        owner->enqueue([owner, id] {
            const auto found = owner->operations.find(id);
            if (found != owner->operations.end()) g_cancellable_cancel(found->second->cancel);
        });
    }
    void shutdown(std::function<void()> completion) override {
        std::unique_lock lock(admission_);
        bool finished = false;
        {
            std::lock_guard joinLock(join_->mutex);
            finished = join_->finished;
            if (!finished && completion) join_->callbacks.push_back(std::move(completion));
        }
        if (finished) { lock.unlock(); if (completion) completion(); return; }
        if (closed_) return;
        closed_ = true;
        auto owner = owner_;
        owner->enqueue([owner] {
            owner->stopping = true;
            std::vector<std::shared_ptr<Operation>> pending;
            pending.reserve(owner->operations.size());
            for (const auto &[id, op] : owner->operations) pending.push_back(op);
            for (const auto &op : pending) g_cancellable_cancel(op->cancel);
        });
        { std::lock_guard joinLock(join_->mutex); join_->retiring = true; }
        join_->retire.notify_one();
    }
private:
    struct Join {
        std::mutex mutex;
        std::condition_variable retire;
        std::thread native;
        bool retiring = false;
        bool finished = false;
        std::vector<std::function<void()>> callbacks;
    };
    std::shared_ptr<Owner> owner_;
    std::shared_ptr<Join> join_;
    std::mutex admission_;
    RequestId next_ = 0;
    bool closed_ = false;

    RequestId submit(Operation::Kind kind, QStringList records, std::shared_ptr<SecretBytes> payload,
            CredentialStore::Interaction interaction, Completion completion) {
        std::lock_guard lock(admission_);
        if (closed_) return 0;
        const auto id = ++next_;
        auto owner = owner_;
        owner->enqueue([owner, id, kind, records = std::move(records), payload = std::move(payload),
                interaction, completion = std::move(completion)]() mutable {
            if (owner->stopping || owner->operations.size() >= kMaxPending) {
                if (completion) completion({owner->stopping ? CredentialStatus::Cancelled : CredentialStatus::Busy, {}, {}});
                return;
            }
            auto op = std::make_shared<Operation>();
            op->owner = owner;
            op->id = id;
            op->kind = kind;
            op->records = std::move(records);
            op->payload = std::move(payload);
            op->interaction = interaction;
            op->completion = std::move(completion);
            owner->operations.emplace(id, op);
            op->timeout = g_timeout_source_new_seconds(kTimeoutSeconds);
            g_source_set_callback(op->timeout, [](gpointer data) -> gboolean {
                auto *op = static_cast<Operation *>(data);
                op->timedOut = true;
                g_cancellable_cancel(op->cancel);
                return G_SOURCE_REMOVE;
            }, op.get(), nullptr);
            g_source_attach(op->timeout, owner->context);
            if (op->records.size() > 32 ||
                    (kind != Operation::Kind::Erase && op->records.size() != 1)) {
                finish(op, CredentialStatus::InvalidRecord);
                return;
            }
            for (const auto &record : op->records) {
                const auto uuid = QUuid(record);
                if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != record) {
                    finish(op, CredentialStatus::InvalidRecord);
                    return;
                }
            }
            if (op->records.isEmpty()) finish(op, CredentialStatus::Empty);
            else openService(op);
        });
        return id;
    }
};
} // namespace

std::unique_ptr<SecretServiceTransport> makeSecretServiceTransport(SecretServiceNamespace space) {
    return std::make_unique<LibsecretTransport>(space);
}
} // namespace OpenMeeting
