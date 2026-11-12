#include "wayland_screencast.h"
#include "pipewire_capture.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libyuv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>

namespace livekit::linux_capture {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequest = "org.freedesktop.portal.Request";
constexpr auto kSession = "org.freedesktop.portal.Session";
constexpr int kMethodTimeoutMs = 3000;
std::atomic<std::uint64_t> delivered_frames{0};
std::atomic<std::uint64_t> capture_failures{0};
std::atomic<const char*> failure_reason{"none"};

std::string Token() {
    char *uuid = g_uuid_string_random();
    std::string result = "cohavora_";
    for (const char *p = uuid; *p; ++p) if (*p != '-') result += *p;
    g_free(uuid);
    return result;
}

bool HasEnvironment(const char *name) {
    const auto *value = std::getenv(name);
    return value && *value;
}

bool IsPermissionDenied(const GError *error) {
    if (!error) return false;
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)) return true;
    char *remote = g_dbus_error_get_remote_error(error);
    const bool denied = remote &&
        (std::strcmp(remote, "org.freedesktop.DBus.Error.AccessDenied") == 0 ||
         std::strcmp(remote, "org.freedesktop.DBus.Error.PermissionDenied") == 0 ||
         std::strcmp(remote, "org.freedesktop.portal.Error.PermissionDenied") == 0);
    g_free(remote);
    return denied;
}

struct Variant {
    GVariant *value = nullptr;
    ~Variant() { if (value) g_variant_unref(value); }
};
}

struct WaylandScreencast::Run final : std::enable_shared_from_this<Run> {
    enum class RequestStage { None, Create, Select, Start };
    struct Method {
        std::shared_ptr<Run> run;
        std::function<void(GVariant*, GUnixFDList*)> result;
        bool cleanup = false;
    };
    struct Request {
        RequestStage stage = RequestStage::None;
        std::uint64_t serial = 0;
        std::string predicted;
        std::string actual;
        std::map<std::string, GVariant*> early;
        void Clear() {
            for (auto &[path, value] : early) g_variant_unref(value);
            early.clear();
            stage = RequestStage::None;
            predicted.clear(); actual.clear();
        }
        ~Request() { Clear(); }
    } request;

    Run(DesktopSource source, FrameCallback frame, EndCallback ended, StatusCallback status,
        ScreenShareQuality quality, std::uint64_t revision, QualityCallback quality_callback)
        : source(std::move(source)), frame(std::move(frame)), ended(std::move(ended)),
          status(std::move(status)), quality(quality), revision(revision),
          quality_callback(std::move(quality_callback)), context(g_main_context_new()),
          cancellable(g_cancellable_new()) {}

    ~Run() {
        if (remote_fd >= 0) ::close(remote_fd);
        if (connection) g_object_unref(connection);
        g_object_unref(cancellable);
        g_main_context_unref(context);
    }

    void Main() noexcept {
        g_main_context_push_thread_default(context);
        try {
            Notify(DesktopCapturePhase::AwaitingPermission);
            connection_deadline = Clock::now() + std::chrono::seconds(10);
            auto *tick = g_timeout_source_new(5);
            g_source_set_callback(tick, [](gpointer data) -> gboolean {
                auto *self = static_cast<Run*>(data);
                try { self->Tick(); }
                catch (...) { self->Finish(DesktopCapturePhase::Failed, "capture_conversion_failed"); }
                return G_SOURCE_CONTINUE;
            }, this, nullptr);
            timer = tick;
            g_source_attach(timer, context);
            if (stopping.load()) {
                // Stop can win immediately after the facade publishes this run.
            } else if (!WaylandCaptureSupported() || source.id != 0) {
                Finish(DesktopCapturePhase::Failed, "wayland_environment_unavailable");
            } else {
                const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
                ++pending;
                auto *owned = new std::shared_ptr<Run>(shared_from_this());
                g_dbus_connection_new_for_address(address,
                    static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                    nullptr, cancellable,
                    [](GObject*, GAsyncResult *result, gpointer data) {
                        std::unique_ptr<std::shared_ptr<Run>> owned(static_cast<std::shared_ptr<Run>*>(data));
                        auto self = *owned;
                        --self->pending;
                        GError *error = nullptr;
                        auto *connection = g_dbus_connection_new_for_address_finish(result, &error);
                        if (error) g_error_free(error);
                        if (!connection) {
                            if (self->Accepting()) self->Finish(DesktopCapturePhase::Failed, "portal_bus_unavailable");
                            return;
                        }
                        self->connection = connection;
                        self->connection_deadline = {};
                        if (self->Accepting()) self->Connected();
                    }, owned);
            }
            while (Accepting()) g_main_context_iteration(context, true);
        } catch (...) { Finish(DesktopCapturePhase::Failed, "portal_setup_failed"); }
        Cleanup();
        g_main_context_pop_thread_default(context);
    }

    bool Accepting() const { return !stopping.load(std::memory_order_acquire) && !finished; }

    void Stop() {
        stopping.store(true, std::memory_order_release);
        g_cancellable_cancel(cancellable);
        g_main_context_wakeup(context);
    }

    void Notify(DesktopCapturePhase phase) {
        if (stopping.load()) return;
        try { if (status) status(phase); } catch (...) {}
    }

    void Finish(DesktopCapturePhase phase, const char *reason) {
        if (finished || stopping.load()) return;
        finished = true;
        if (phase != DesktopCapturePhase::Cancelled) {
            capture_failures.fetch_add(1, std::memory_order_relaxed);
            failure_reason.store(reason, std::memory_order_relaxed);
        }
        Notify(phase);
        try { if (!stopping.load() && ended) ended(); } catch (...) {}
        g_main_context_wakeup(context);
    }

    void Call(const char *destination, const char *path, const char *interface, const char *method,
              GVariant *parameters, const GVariantType *reply_type,
              std::function<void(GVariant*, GUnixFDList*)> callback, bool cleanup = false) {
        ++pending;
        auto *owned = new Method{shared_from_this(), std::move(callback), cleanup};
        g_dbus_connection_call_with_unix_fd_list(connection, destination, path, interface, method,
            parameters, reply_type, G_DBUS_CALL_FLAGS_NO_AUTO_START, kMethodTimeoutMs, nullptr,
            cleanup ? nullptr : cancellable,
            [](GObject *object, GAsyncResult *result, gpointer data) {
                std::unique_ptr<Method> owned(static_cast<Method*>(data));
                auto self = owned->run;
                --self->pending;
                GError *error = nullptr;
                GUnixFDList *fds = nullptr;
                Variant reply{g_dbus_connection_call_with_unix_fd_list_finish(
                    G_DBUS_CONNECTION(object), &fds, result, &error)};
                const bool denied = IsPermissionDenied(error);
                if (error) g_error_free(error);
                if (denied && self->Accepting() && !owned->cleanup)
                    self->Finish(DesktopCapturePhase::Denied, "portal_permission_denied");
                if (self->Accepting() || owned->cleanup) {
                    try { owned->result(reply.value, fds); }
                    catch (...) { self->Finish(DesktopCapturePhase::Failed, "portal_response_invalid"); }
                }
                if (fds) g_object_unref(fds);
            }, owned);
    }

    void Connected() {
        const char *unique = g_dbus_connection_get_unique_name(connection);
        if (!unique || unique[0] != ':') { Finish(DesktopCapturePhase::Failed, "portal_bus_invalid"); return; }
        sender = unique + 1;
        std::replace(sender.begin(), sender.end(), '.', '_');
        request_prefix = std::string(kPath) + "/request/" + sender + "/";
        session_prefix = std::string(kPath) + "/session/" + sender + "/";
        closed_handler = g_signal_connect(connection, "closed", G_CALLBACK(+[](
                GDBusConnection*, gboolean, GError*, gpointer data) {
            auto *self = static_cast<Run*>(data);
            if (self->Accepting()) self->Finish(DesktopCapturePhase::Failed, "portal_bus_closed");
        }), this);
        owner_subscription = g_dbus_connection_signal_subscribe(connection, "org.freedesktop.DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", kService,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant *params, gpointer data) {
                auto *self = static_cast<Run*>(data);
                const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
                g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
                if (!self->owner.empty() && self->owner != new_owner && self->Accepting())
                    self->Finish(DesktopCapturePhase::Failed, "portal_owner_changed");
            }, this, nullptr);
        FindOwner(false);
    }

    void FindOwner(bool activated) {
        auto self = shared_from_this();
        Call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
            g_variant_new("(s)", kService), G_VARIANT_TYPE("(s)"),
            [self, activated](GVariant *reply, GUnixFDList*) {
                if (!reply) {
                    if (activated) { self->Finish(DesktopCapturePhase::Failed, "portal_service_unavailable"); return; }
                    self->Call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName",
                        g_variant_new("(su)", kService, 0u), G_VARIANT_TYPE("(u)"),
                        [self](GVariant *reply, GUnixFDList*) {
                            if (reply) self->FindOwner(true);
                            else self->Finish(DesktopCapturePhase::Failed, "portal_service_unavailable");
                        });
                    return;
                }
                const char *owner = nullptr;
                g_variant_get(reply, "(&s)", &owner);
                if (!owner || owner[0] != ':') { self->Finish(DesktopCapturePhase::Failed, "portal_owner_invalid"); return; }
                self->owner = owner;
                self->ReadCapabilities();
            });
    }

    void ReadCapabilities() {
        auto self = shared_from_this();
        Call(owner.c_str(), kPath, "org.freedesktop.DBus.Properties", "GetAll",
            g_variant_new("(s)", kScreenCast), G_VARIANT_TYPE("(a{sv})"),
            [self](GVariant *reply, GUnixFDList*) {
                if (!reply) { self->Finish(DesktopCapturePhase::Failed, "portal_capabilities_unavailable"); return; }
                GVariant *properties = nullptr;
                g_variant_get(reply, "(@a{sv})", &properties);
                Variant values{properties};
                std::uint32_t sources = 0, cursors = 0;
                if (!g_variant_lookup(properties, "version", "u", &self->portal_version) || self->portal_version < 1 ||
                    !g_variant_lookup(properties, "AvailableSourceTypes", "u", &sources) ||
                    !(sources & (self->source.kind == DesktopSourceKind::Screen ? 1u : 2u))) {
                    self->Finish(DesktopCapturePhase::Failed, "portal_source_unsupported"); return;
                }
                if (self->portal_version >= 2) {
                    if (!g_variant_lookup(properties, "AvailableCursorModes", "u", &cursors)) {
                        self->Finish(DesktopCapturePhase::Failed, "portal_cursor_unsupported"); return;
                    }
                    self->cursor_mode = (cursors & 2) ? 2 : ((cursors & 1) ? 1 : 0);
                    if (!self->cursor_mode) { self->Finish(DesktopCapturePhase::Failed, "portal_cursor_unsupported"); return; }
                }
                self->response_subscription = g_dbus_connection_signal_subscribe(self->connection, self->owner.c_str(),
                    kRequest, "Response", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                    [](GDBusConnection*, const gchar*, const gchar *path, const gchar*, const gchar*, GVariant *params, gpointer data) {
                        static_cast<Run*>(data)->Response(path, params);
                    }, self.get(), nullptr);
                self->CreateSession();
            });
    }

    void BeginRequest(RequestStage stage, const char *method, GVariant *parameters, const std::string &token) {
        request.Clear();
        request.stage = stage;
        const auto serial = ++request.serial;
        request.predicted = request_prefix + token;
        auto self = shared_from_this();
        Call(owner.c_str(), kPath, kScreenCast, method, parameters, G_VARIANT_TYPE("(o)"),
            [self, serial](GVariant *reply, GUnixFDList*) {
                if (self->request.serial != serial || self->request.stage == RequestStage::None) return;
                if (!reply) { self->Finish(DesktopCapturePhase::Failed, "portal_request_failed"); return; }
                const char *path = nullptr;
                g_variant_get(reply, "(&o)", &path);
                if (!path || !std::string(path).starts_with(self->request_prefix)) {
                    self->Finish(DesktopCapturePhase::Failed, "portal_request_handle_invalid"); return;
                }
                self->request.actual = path;
                const auto found = self->request.early.find(path);
                if (found != self->request.early.end()) {
                    Variant result{g_variant_ref(found->second)};
                    self->CompleteResponse(result.value);
                }
            });
    }

    void CreateSession() {
        const auto token = Token();
        const auto session_token = Token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token.c_str()));
        BeginRequest(RequestStage::Create, "CreateSession", g_variant_new("(a{sv})", &options), token);
    }

    void SelectSources() {
        const auto token = Token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(source.kind == DesktopSourceKind::Screen ? 1 : 2));
        g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(false));
        if (portal_version >= 2)
            g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(cursor_mode));
        if (portal_version >= 4)
            g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(0));
        BeginRequest(RequestStage::Select, "SelectSources", g_variant_new("(oa{sv})", session.c_str(), &options), token);
    }

    void StartSession() {
        const auto token = Token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        // Empty parent is explicitly allowed by the portal protocol. Native
        // window handles never cross the session/control ownership boundary.
        BeginRequest(RequestStage::Start, "Start", g_variant_new("(osa{sv})", session.c_str(), "", &options), token);
    }

    void Response(const char *path, GVariant *params) {
        if (!Accepting() || request.stage == RequestStage::None || !path ||
            !std::string(path).starts_with(request_prefix) ||
            !g_variant_is_of_type(params, G_VARIANT_TYPE("(ua{sv})"))) return;
        if (!request.actual.empty()) {
            if (request.actual == path) CompleteResponse(params);
            return;
        }
        if (!request.early.contains(path)) {
            if (request.early.size() >= 4) { Finish(DesktopCapturePhase::Failed, "portal_response_overflow"); return; }
            request.early.emplace(path, g_variant_ref(params));
        }
    }

    void CompleteResponse(GVariant *params) {
        std::uint32_t response = 2;
        GVariant *properties = nullptr;
        g_variant_get(params, "(u@a{sv})", &response, &properties);
        Variant values{properties};
        const auto stage = request.stage;
        request.Clear();
        if (response == 1) { Finish(DesktopCapturePhase::Cancelled, "portal_cancelled"); return; }
        if (response != 0) { Finish(DesktopCapturePhase::Failed, "portal_request_failed"); return; }
        if (stage == RequestStage::Create) {
            Variant handle{g_variant_lookup_value(properties, "session_handle", nullptr)};
            if (!handle.value || (!g_variant_is_of_type(handle.value, G_VARIANT_TYPE_STRING) &&
                !g_variant_is_of_type(handle.value, G_VARIANT_TYPE_OBJECT_PATH))) {
                Finish(DesktopCapturePhase::Failed, "portal_session_handle_invalid"); return;
            }
            const char *path = g_variant_get_string(handle.value, nullptr);
            if (!g_variant_is_object_path(path) || !std::string(path).starts_with(session_prefix)) {
                Finish(DesktopCapturePhase::Failed, "portal_session_handle_invalid"); return;
            }
            session = path;
            session_subscription = g_dbus_connection_signal_subscribe(connection, owner.c_str(), kSession,
                "Closed", session.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
                    auto *self = static_cast<Run*>(data);
                    if (self->Accepting()) self->Finish(DesktopCapturePhase::Failed, "portal_session_closed");
                }, this, nullptr);
            SelectSources();
        } else if (stage == RequestStage::Select) {
            StartSession();
        } else if (stage == RequestStage::Start) {
            Variant streams{g_variant_lookup_value(properties, "streams", G_VARIANT_TYPE("a(ua{sv})"))};
            if (!streams.value || g_variant_n_children(streams.value) != 1) {
                Finish(DesktopCapturePhase::Failed, "portal_stream_count_invalid"); return;
            }
            Variant entry{g_variant_get_child_value(streams.value, 0)};
            GVariant *metadata = nullptr;
            g_variant_get(entry.value, "(u@a{sv})", &node, &metadata);
            Variant info{metadata};
            std::uint32_t source_type = source.kind == DesktopSourceKind::Screen ? 1 : 2;
            const auto expected_type = source_type;
            g_variant_lookup(metadata, "source_type", "u", &source_type);
            if (!node || node == std::numeric_limits<std::uint32_t>::max() || source_type != expected_type) {
                Finish(DesktopCapturePhase::Failed, "portal_stream_identity_invalid"); return;
            }
            OpenRemote();
        }
    }

    void OpenRemote() {
        auto self = shared_from_this();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        Call(owner.c_str(), kPath, kScreenCast, "OpenPipeWireRemote", g_variant_new("(oa{sv})", session.c_str(), &options),
            G_VARIANT_TYPE("(h)"), [self](GVariant *reply, GUnixFDList *fds) {
                if (!reply || !fds) { self->Finish(DesktopCapturePhase::Failed, "portal_remote_fd_missing"); return; }
                int index = -1;
                g_variant_get(reply, "(h)", &index);
                GError *error = nullptr;
                const int fd = g_unix_fd_list_get(fds, index, &error);
                if (error) g_error_free(error);
                if (fd < 0) { self->Finish(DesktopCapturePhase::Failed, "portal_remote_fd_invalid"); return; }
                self->remote_fd = fd;
                ScreenShareQuality quality;
                { std::lock_guard lock(self->quality_mutex); quality = self->quality; }
                if (!self->pipewire.Start(self->node, fd, quality.fps)) {
                    self->Finish(DesktopCapturePhase::Failed, "pipewire_connect_failed"); return;
                }
                self->setup_deadline = Clock::now() + std::chrono::seconds(5);
            });
    }

    void Tick() {
        if (!Accepting()) return;
        if (connection_deadline != Clock::time_point{} && Clock::now() >= connection_deadline) {
            Finish(DesktopCapturePhase::Failed, "portal_bus_timeout"); return;
        }
        if (pipewire.failure() != PipeWireFailure::None) {
            Finish(DesktopCapturePhase::Failed, "pipewire_stream_failed"); return;
        }
        if (!ready && setup_deadline != Clock::time_point{}) {
            if (pipewire.ready()) {
                ready = true;
                Notify(DesktopCapturePhase::Ready);
            } else if (Clock::now() >= setup_deadline) {
                Finish(DesktopCapturePhase::Failed, "pipewire_setup_timeout"); return;
            }
        }
        if (!ready) return;
        auto pixels = pipewire.TakeFrame();
        if (!pixels) return;
        ScreenShareQuality requested;
        std::uint64_t requested_revision;
        QualityCallback applied;
        {
            std::lock_guard lock(quality_mutex);
            requested = quality;
            requested_revision = revision;
            applied = quality_callback;
        }
        pipewire.SetFrameRate(requested.fps);
        auto i420 = ConvertCpuFrame(*pixels);
        auto planes = i420.planeInfos();
        const auto profile = ResolveScreenShareProfile(i420.width(), i420.height(), requested, requested_revision);
        if (!profile) { Finish(DesktopCapturePhase::Failed, "capture_profile_unsupported"); return; }
        if (profile->width != i420.width() || profile->height != i420.height()) {
            auto scaled = VideoFrame::create(profile->width, profile->height, VideoBufferType::I420);
            auto output = scaled.planeInfos();
            planes = i420.planeInfos();
            if (libyuv::I420Scale(
                    reinterpret_cast<const std::uint8_t*>(planes[0].data_ptr), planes[0].stride,
                    reinterpret_cast<const std::uint8_t*>(planes[1].data_ptr), planes[1].stride,
                    reinterpret_cast<const std::uint8_t*>(planes[2].data_ptr), planes[2].stride, i420.width(), i420.height(),
                    reinterpret_cast<std::uint8_t*>(output[0].data_ptr), output[0].stride,
                    reinterpret_cast<std::uint8_t*>(output[1].data_ptr), output[1].stride,
                    reinterpret_cast<std::uint8_t*>(output[2].data_ptr), output[2].stride, scaled.width(), scaled.height(),
                    libyuv::kFilterBox) != 0) {
                Finish(DesktopCapturePhase::Failed, "capture_scaling_failed"); return;
            }
            i420 = std::move(scaled);
        }
        if (!Accepting()) return;
        if (frame) frame(i420);
        if (!Accepting()) return;
        if (applied) applied(*profile);
        delivered_frames.fetch_add(1, std::memory_order_relaxed);
    }

    void CloseObject(const std::string &path, const char *interface) {
        if (path.empty() || owner.empty() || !connection || g_dbus_connection_is_closed(connection)) return;
        Call(owner.c_str(), path.c_str(), interface, "Close", nullptr, nullptr,
            [](GVariant*, GUnixFDList*) {}, true);
    }

    void Cleanup() noexcept {
        if (timer) { g_source_destroy(timer); g_source_unref(timer); timer = nullptr; }
        pipewire.Stop();
        if (remote_fd >= 0) { ::close(remote_fd); remote_fd = -1; }
        if (connection) {
            if (response_subscription) g_dbus_connection_signal_unsubscribe(connection, response_subscription);
            if (session_subscription) g_dbus_connection_signal_unsubscribe(connection, session_subscription);
            if (owner_subscription) g_dbus_connection_signal_unsubscribe(connection, owner_subscription);
            if (closed_handler) g_signal_handler_disconnect(connection, closed_handler);
            CloseObject(request.predicted, kRequest);
            if (request.actual != request.predicted) CloseObject(request.actual, kRequest);
            CloseObject(session, kSession);
        }
        request.Clear();
        g_cancellable_cancel(cancellable);
        // Every async operation has a cancellation source and a bounded method
        // deadline. Drain its completion on the context that owns it, then
        // disconnect the private bus name (also revoking pending portal work).
        while (pending) g_main_context_iteration(context, true);
        if (connection && !g_dbus_connection_is_closed(connection)) {
            ++pending;
            g_dbus_connection_close(connection, nullptr,
                [](GObject *object, GAsyncResult *result, gpointer data) {
                    auto *self = static_cast<Run*>(data);
                    GError *error = nullptr;
                    g_dbus_connection_close_finish(G_DBUS_CONNECTION(object), result, &error);
                    if (error) g_error_free(error);
                    --self->pending;
                }, this);
            while (pending) g_main_context_iteration(context, true);
        }
        while (g_main_context_iteration(context, false)) {}
    }

    DesktopSource source;
    FrameCallback frame;
    EndCallback ended;
    StatusCallback status;
    std::mutex quality_mutex;
    ScreenShareQuality quality;
    std::uint64_t revision;
    QualityCallback quality_callback;
    GMainContext *context = nullptr;
    GCancellable *cancellable = nullptr;
    GDBusConnection *connection = nullptr;
    GSource *timer = nullptr;
    std::atomic<bool> stopping{false};
    bool finished = false;
    bool ready = false;
    unsigned pending = 0;
    guint owner_subscription = 0, response_subscription = 0, session_subscription = 0;
    gulong closed_handler = 0;
    std::string sender, owner, request_prefix, session_prefix, session;
    std::uint32_t portal_version = 0, cursor_mode = 1, node = 0;
    int remote_fd = -1;
    PipeWireCapture pipewire;
    Clock::time_point connection_deadline{}, setup_deadline{};
};

WaylandScreencast::WaylandScreencast() = default;
WaylandScreencast::~WaylandScreencast() { Stop(); }
void WaylandScreencast::Start(DesktopSource source, FrameCallback frame, EndCallback ended) {
    StartWithStatus(std::move(source), std::move(frame), std::move(ended), {});
}
void WaylandScreencast::StartWithStatus(DesktopSource source, FrameCallback frame, EndCallback ended, StatusCallback status) {
    Stop();
    std::lock_guard lock(mutex_);
    auto run = std::make_shared<Run>(std::move(source), std::move(frame), std::move(ended), std::move(status),
        quality_, revision_, quality_callback_);
    run_ = run;
    worker_ = std::thread([run] { run->Main(); });
}
void WaylandScreencast::Stop() {
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        if (run_) run_->Stop();
        worker = std::move(worker_);
        run_.reset();
    }
    if (worker.joinable()) worker.join();
}
bool WaylandScreencast::SetQuality(ScreenShareQuality quality, std::uint64_t revision, QualityCallback applied) {
    if (!quality.valid()) return false;
    std::lock_guard lock(mutex_);
    quality_ = quality;
    revision_ = revision;
    quality_callback_ = std::move(applied);
    if (run_) {
        std::lock_guard quality_lock(run_->quality_mutex);
        run_->quality = quality_;
        run_->revision = revision_;
        run_->quality_callback = quality_callback_;
        g_main_context_wakeup(run_->context);
    }
    return true;
}

bool WaylandCaptureSupported() {
    const char *session = std::getenv("XDG_SESSION_TYPE");
    return session && std::strcmp(session, "wayland") == 0 && HasEnvironment("WAYLAND_DISPLAY") &&
        HasEnvironment("XDG_RUNTIME_DIR") && HasEnvironment("DBUS_SESSION_BUS_ADDRESS");
}
DesktopCaptureObservation ObserveWaylandCapture() {
    DesktopCaptureObservation observation;
    observation.frames = delivered_frames.load(std::memory_order_relaxed);
    if (observation.frames) observation.backend = "wayland_portal_pipewire_cpu";
    observation.failures = capture_failures.load(std::memory_order_relaxed);
    observation.failure_reason = failure_reason.load(std::memory_order_relaxed);
    return observation;
}

} // namespace livekit::linux_capture
