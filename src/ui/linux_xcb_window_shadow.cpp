#include "linux_xcb_window_shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <qpa/qplatformnativeinterface.h>

#include <xcb/shape.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <utility>

namespace MeetingUI {
namespace {

template <typename T>
using Reply = std::unique_ptr<T, decltype(&std::free)>;

template <typename T>
Reply<T> ownReply(T *reply) {
    return Reply<T>(reply, &std::free);
}

bool requestSucceeded(xcb_connection_t *connection, xcb_void_cookie_t cookie) {
    return !ownReply(xcb_request_check(connection, cookie));
}

void assertUiThread() {
    Q_ASSERT(!QCoreApplication::instance() ||
        QThread::currentThread() == QCoreApplication::instance()->thread());
}

struct WindowTree {
    xcb_window_t root = XCB_WINDOW_NONE;
    xcb_window_t frame = XCB_WINDOW_NONE;
};

WindowTree windowTree(xcb_connection_t *connection, xcb_window_t body) {
    auto current = body;
    // A framed window is generally two levels deep. Bound malformed or
    // concurrently changing ancestry instead of trusting the server tree.
    for (int depth = 0; depth != 32; ++depth) {
        auto reply = ownReply(xcb_query_tree_reply(
            connection, xcb_query_tree(connection, current), nullptr));
        if (!reply || reply->parent == XCB_WINDOW_NONE || reply->parent == current) return {};
        if (reply->parent == reply->root) return {reply->root, current};
        current = reply->parent;
    }
    return {};
}

bool hasCompositor(xcb_connection_t *connection, xcb_window_t root) {
    int screenNumber = 0;
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (; screens.rem && screens.data->root != root; xcb_screen_next(&screens)) ++screenNumber;
    if (!screens.rem) return false;
    const auto name = QByteArray("_NET_WM_CM_S") + QByteArray::number(screenNumber);
    auto atom = ownReply(xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, true, name.size(), name.constData()), nullptr));
    if (!atom || atom->atom == XCB_ATOM_NONE) return false;
    auto owner = ownReply(xcb_get_selection_owner_reply(connection,
        xcb_get_selection_owner(connection, atom->atom), nullptr));
    return owner && owner->owner != XCB_WINDOW_NONE;
}

} // namespace

struct LinuxXcbWindowShadow::Impl {
    explicit Impl(std::function<void()> callback) : scheduleSync(std::move(callback)) {}

    void releaseFrameEvents() {
        if (connection && frame != XCB_WINDOW_NONE && frameMaskAdded &&
            !QCoreApplication::closingDown()) {
            auto attributes = ownReply(xcb_get_window_attributes_reply(connection,
                xcb_get_window_attributes(connection, frame), nullptr));
            if (attributes) {
                // Preserve any event classes selected since installation.
                const uint32_t mask = attributes->your_event_mask & ~frameMaskAdded;
                requestSucceeded(connection, xcb_change_window_attributes_checked(
                    connection, frame, XCB_CW_EVENT_MASK, &mask));
            }
        }
        frame = XCB_WINDOW_NONE;
        frameMaskAdded = 0;
    }

    bool observeFrame(xcb_window_t nextFrame) {
        if (frame == nextFrame) return true;
        releaseFrameEvents();
        if (nextFrame != body) {
            auto attributes = ownReply(xcb_get_window_attributes_reply(connection,
                xcb_get_window_attributes(connection, nextFrame), nullptr));
            if (!attributes) return false;
            const uint32_t missing = XCB_EVENT_MASK_STRUCTURE_NOTIFY & ~attributes->your_event_mask;
            const uint32_t mask = attributes->your_event_mask | missing;
            if (missing && !requestSucceeded(connection, xcb_change_window_attributes_checked(
                connection, nextFrame, XCB_CW_EVENT_MASK, &mask))) return false;
            frameMaskAdded = missing;
        }
        frame = nextFrame;
        return true;
    }

    std::function<void()> scheduleSync;
    xcb_connection_t *connection = nullptr;
    xcb_window_t body = XCB_WINDOW_NONE;
    xcb_window_t frame = XCB_WINDOW_NONE;
    uint32_t frameMaskAdded = 0;
    bool mapped = false;
};

LinuxXcbWindowShadow::LinuxXcbWindowShadow(std::function<void()> scheduleSync)
    : impl_(std::make_unique<Impl>(std::move(scheduleSync))) {
    assertUiThread();
    if (auto *application = QCoreApplication::instance()) application->installNativeEventFilter(this);
}

LinuxXcbWindowShadow::~LinuxXcbWindowShadow() {
    if (auto *application = QCoreApplication::instance()) application->removeNativeEventFilter(this);
    reset();
}

bool LinuxXcbWindowShadow::configure(QWindow &body, QWindow &shadow) {
    assertUiThread();
    if (QGuiApplication::platformName() != QLatin1String("xcb") ||
        !body.handle() || !shadow.handle()) return false;
    auto *native = QGuiApplication::platformNativeInterface();
    auto *connection = native ? static_cast<xcb_connection_t *>(
        native->nativeResourceForWindow(QByteArrayLiteral("connection"), &body)) : nullptr;
    if (!connection || xcb_connection_has_error(connection)) {
        reset();
        return false;
    }
    const auto bodyId = static_cast<xcb_window_t>(body.winId());
    if (impl_->connection != connection || impl_->body != bodyId) {
        reset();
        impl_->connection = connection;
        impl_->body = bodyId;
    }
    auto attributes = ownReply(xcb_get_window_attributes_reply(connection,
        xcb_get_window_attributes(connection, bodyId), nullptr));
    impl_->mapped = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    if (!impl_->mapped) return false;
    const auto tree = windowTree(connection, bodyId);
    if (tree.frame == XCB_WINDOW_NONE || !impl_->observeFrame(tree.frame) ||
        !hasCompositor(connection, tree.root)) return false;
    const auto *shape = xcb_get_extension_data(connection, &xcb_shape_id);
    if (!shape || !shape->present) return false;
    const auto shadowId = static_cast<xcb_window_t>(shadow.winId());
    // An empty INPUT shape is distinct from a default/unset region. No part of
    // this independent native window may receive a pointer event or resize hit.
    if (!requestSucceeded(connection, xcb_shape_rectangles_checked(connection,
        XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
        shadowId, 0, 0, 0, nullptr))) return false;
    // Use the WM's root-level frame, not the reparented client XID: the shadow
    // and sibling must have the same parent for ConfigureWindow to be valid.
    const uint32_t stacking[] = {tree.frame, XCB_STACK_MODE_BELOW};
    if (!requestSucceeded(connection, xcb_configure_window_checked(connection, shadowId,
        XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, stacking))) return false;
    xcb_flush(connection);
    return true;
}

bool LinuxXcbWindowShadow::bodyMapped() const {
    return impl_->mapped;
}

void LinuxXcbWindowShadow::reset() {
    assertUiThread();
    impl_->releaseFrameEvents();
    impl_->connection = nullptr;
    impl_->body = XCB_WINDOW_NONE;
    impl_->mapped = false;
}

bool LinuxXcbWindowShadow::nativeEventFilter(const QByteArray &eventType, void *message, long *) {
    if (eventType != QByteArrayLiteral("xcb_generic_event_t") || !message || !impl_->connection) return false;
    const auto *event = static_cast<const xcb_generic_event_t *>(message);
    const auto type = event->response_type & ~0x80;
    xcb_window_t window = XCB_WINDOW_NONE;
    switch (type) {
    case XCB_CONFIGURE_NOTIFY:
        window = reinterpret_cast<const xcb_configure_notify_event_t *>(event)->window;
        break;
    case XCB_REPARENT_NOTIFY:
        window = reinterpret_cast<const xcb_reparent_notify_event_t *>(event)->window;
        break;
    case XCB_MAP_NOTIFY:
        window = reinterpret_cast<const xcb_map_notify_event_t *>(event)->window;
        break;
    case XCB_UNMAP_NOTIFY:
        window = reinterpret_cast<const xcb_unmap_notify_event_t *>(event)->window;
        break;
    case XCB_DESTROY_NOTIFY:
        window = reinterpret_cast<const xcb_destroy_notify_event_t *>(event)->window;
        break;
    default:
        return false;
    }
    if (window != impl_->body && window != impl_->frame) return false;
    if (type == XCB_UNMAP_NOTIFY || type == XCB_DESTROY_NOTIFY) impl_->mapped = false;
    if (impl_->scheduleSync) impl_->scheduleSync();
    return false;
}

} // namespace MeetingUI
