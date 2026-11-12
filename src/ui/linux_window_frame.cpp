#include "linux_window_frame.h"
#include "app_theme.h"
#include "linux_xcb_window_shadow.h"

#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QtMath>
#include <QtGui/QCursor>
#include <QtGui/QBackingStore>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPlatformSurfaceEvent>
#include <QtGui/QSurfaceFormat>
#include <QtGui/QWindow>
#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylanddecorationfactory_p.h>
#include <QtWaylandClient/private/qwaylandsubsurface_p.h>
#include <QtWaylandClient/private/qwaylandsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace MeetingUI {
namespace {

constexpr int kShadowMargin = 24;

bool usesBundledClientDecoration(QtWaylandClient::QWaylandWindow &window) {
    if (!window.decoration()) return false;
    // Match Qt 5's selection rule. Other decoration plugins may already draw
    // shadows, so only supplement the known, bundled bradient implementation.
    const auto keys = QtWaylandClient::QWaylandDecorationFactory::keys();
    auto key = QString::fromLocal8Bit(qgetenv("QT_WAYLAND_DECORATION"));
    if (!keys.contains(key)) key = keys.value(0);
    return key == QLatin1String("bradient");
}

// Fixed-size rounded-rectangle distance field, stretched only through its flat
// middle. Window resizes copy nine patches; they never blur a full-size window.
QImage shadowImage(QSize size, qreal dpr, int radius, bool active) {
    const int core = radius + 8;
    const int cap = kShadowMargin + core;
    const int extent = 2 * cap + 1;
    QImage tile(QSize(qCeil(extent * dpr), qCeil(extent * dpr)), QImage::Format_ARGB32_Premultiplied);
    const auto distance = [=](qreal x, qreal y, qreal offset) {
        const qreal center = extent / 2.;
        const qreal half = core + .5;
        const qreal dx = std::abs(x - center) - half + radius;
        const qreal dy = std::abs(y - center - offset) - half + radius;
        return std::hypot(std::max(dx, 0.), std::max(dy, 0.)) + std::min(std::max(dx, dy), 0.) - radius;
    };
    for (int y = 0; y < tile.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(tile.scanLine(y));
        for (int x = 0; x < tile.width(); ++x) {
            const auto px = (x + .5) / dpr, py = (y + .5) / dpr;
            const auto d = std::max(0., distance(px, py, 4));
            const auto near = std::max(0., distance(px, py, 1));
            const auto ambient = std::max(0., std::exp(-d*d/162.) - std::exp(-400./162.));
            const auto contact = std::exp(-near*near/8.);
            const int alpha = distance(px, py, 0) < 0 ? 0 : qBound(0,
                qRound(255 * ((active ? .25 : .13) * ambient + (active ? .18 : .08) * contact)), 255);
            row[x] = qRgba(0, 0, 0, alpha);
        }
    }
    QImage image(QSize(qCeil(size.width()*dpr), qCeil(size.height()*dpr)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const int sx = qRound(cap*dpr), sy = sx;
    const int dx = std::min(sx, image.width()/2), dy = std::min(sy, image.height()/2);
    const int sourceX[] = {0, sx, tile.width()-sx, tile.width()};
    const int sourceY[] = {0, sy, tile.height()-sy, tile.height()};
    const int destX[] = {0, dx, image.width()-dx, image.width()};
    const int destY[] = {0, dy, image.height()-dy, image.height()};
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int y = 0; y != 3; ++y) for (int x = 0; x != 3; ++x) {
        painter.drawImage(QRect(destX[x], destY[y], destX[x+1]-destX[x], destY[y+1]-destY[y]), tile,
            QRect(sourceX[x], sourceY[y], sourceX[x+1]-sourceX[x], sourceY[y+1]-sourceY[y]));
    }
    painter.end();
    image.setDevicePixelRatio(dpr);
    return image;
}

class ShadowWindow final : public QWindow {
public:
    ShadowWindow(QWindow &body, bool wayland) : QWindow(wayland ? &body : nullptr), store_(this) {
        setObjectName(QStringLiteral("linuxWindowShadow"));
        setProperty("linuxWindowShadowOwner", QVariant::fromValue(static_cast<QObject *>(&body)));
        setSurfaceType(QSurface::RasterSurface);
        Qt::WindowFlags flags = wayland ? Qt::WindowFlags(Qt::SubWindow) :
            Qt::WindowFlags(Qt::Tool | Qt::BypassWindowManagerHint);
        setFlags(flags | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        QSurfaceFormat format;
        format.setAlphaBufferSize(8);
        setFormat(format);
        connect(this, &QWindow::screenChanged, this, [this] {
            if (!image_.isNull()) updateImage(radius_, active_);
        });
    }

    void updateImage(int radius, bool active) {
        const auto dpr = devicePixelRatio();
        if (imageSize_ == size() && dpr_ == dpr && radius_ == radius && active_ == active && !image_.isNull()) return;
        imageSize_ = size(); dpr_ = dpr; radius_ = radius; active_ = active;
        image_ = shadowImage(size(), dpr, radius, active);
        ++revision_;
        requestUpdate();
    }
    const QImage &image() const { return image_; }
    quint64 revision() const { return revision_; }
    bool nativeConfigured() const { return configured_; }
    void setNativeConfigured(bool value) { configured_ = value; }

    bool configureWayland(QWindow &body) {
        if (configured_) return true;
        // Qt 5.15 has no public ShadowsOnly margins or subsurface stacking
        // API. Use the project's exact QtWayland ABI, without changing xdg
        // window geometry (which remains the real, margin-free body size).
        auto *native = static_cast<QtWaylandClient::QWaylandWindow *>(handle());
        auto *parent = static_cast<QtWaylandClient::QWaylandWindow *>(body.handle());
        if (!native || !parent || !native->subSurfaceWindow() || !native->waylandSurface()) return false;
        native->subSurfaceWindow()->place_below(parent->wlSurface());
        auto *empty = native->display()->createRegion(QRegion());
        native->waylandSurface()->set_input_region(empty);
        wl_region_destroy(empty);
        // Each subsurface receives output enter/leave independently. Queue
        // behind Qt's own scale update rather than trusting the body screen.
        connect(native->waylandSurface(), &QtWaylandClient::QWaylandSurface::screensChanged,
            this, [this] {
                if (!image_.isNull()) updateImage(radius_, active_);
                requestUpdate();
            }, Qt::QueuedConnection);
        // Subsurface position/stacking is applied with the parent commit.
        parent->commit();
        configured_ = true;
        return true;
    }

protected:
    bool event(QEvent *event) override {
        // QtWayland unmaps by resetting its wl_surface while retaining the
        // QPlatformWindow. Reapply stacking/input after every subsequent show.
        if (event->type() == QEvent::Hide) configured_ = false;
        if (event->type() == QEvent::PlatformSurface &&
            static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            configured_ = false;
        if (event->type() == QEvent::UpdateRequest) { render(); return true; }
        return QWindow::event(event);
    }
    void exposeEvent(QExposeEvent *) override { render(); }

private:
    void render() {
        if (!isExposed() || image_.isNull()) return;
        updateImage(radius_, active_);
        store_.resize(size());
        const QRegion region(QRect(QPoint(), size()));
        store_.beginPaint(region);
        QPainter painter(store_.paintDevice());
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPoint(), image_);
        painter.end();
        store_.endPaint();
        store_.flush(region);
    }

    bool configured_ = false;
    QBackingStore store_;
    QImage image_;
    QSize imageSize_;
    qreal dpr_ = 0;
    int radius_ = 0;
    bool active_ = false;
    quint64 revision_ = 0;
};

class CornerOverlay final : public QWidget {
public:
    explicit CornerOverlay(QWidget &window) : QWidget(&window) {
        setObjectName(QStringLiteral("linuxWindowCornerOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
    }

    void setRadius(int radius) {
        if (radius_ == radius) return;
        radius_ = radius;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (radius_ <= 0) return;
        // This last raster child shares the top-level backing store. Clearing
        // the outside also clips opaque title/footer children at the corners;
        // a QWidget mask alone only changes input/opaque regions on Wayland.
        QPainterPath outside;
        outside.setFillRule(Qt::OddEvenFill);
        outside.addRect(QRectF(rect()));
        outside.addRoundedRect(QRectF(rect()), radius_, radius_);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.fillPath(outside, Qt::black);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        auto border = AppTheme::color(AppTheme::ColorRole::Border);
        border.setAlpha(window()->isActiveWindow() ? 180 : 115);
        painter.setPen(QPen(border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), radius_-.5, radius_-.5);
    }

private:
    int radius_ = 0;
};

class LinuxWindowFrame final : public QObject {
public:
    LinuxWindowFrame(QWidget &window, LinuxWindowFrameOptions options, bool clientDecorationOnly = false)
        : QObject(&window), window_(&window), options_(std::move(options)), clientDecorationOnly_(clientDecorationOnly) {
        setObjectName(QStringLiteral("linuxWindowFrame"));
        options_.radius = std::max(0, options_.radius);
        options_.resizeMargin = std::max(0, options_.resizeMargin);
        if (!clientDecorationOnly_) {
            window.setAttribute(Qt::WA_TranslucentBackground);
            overlay_ = new CornerOverlay(window);
        }
        qApp->installEventFilter(this);
        AppTheme::observeChanges(*this, [this] { if (overlay_) overlay_->update(); });
        sync();
    }

    ~LinuxWindowFrame() override {
        if (qApp) qApp->removeEventFilter(this);
        delete shadow_.data();
        if (window_) restoreCursor();
    }

    LinuxWindowFrameDiagnostics diagnostics() const {
        auto result = diagnostics_;
        result.installed = true;
        result.isWayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        result.active = window_ && window_->isActiveWindow();
        result.radius = options_.radius;
        result.shadowMargin = kShadowMargin;
        result.clientDecorationShadowFallback = clientDecorationFallback_;
        if (clientDecorationOnly_ && nativeParent_) result.nativeFrameMargins = nativeParent_->frameMargins();
        if (shadow_) {
            result.shadowVisible = shadow_->isVisible();
            result.nativeShadowConfigured = shadow_->nativeConfigured();
            result.shadowGeometry = shadow_->geometry();
            result.shadowImage = shadow_->image();
            result.shadowRevision = shadow_->revision();
        }
        return result;
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (!window_) return false;
        if (object == nativeParent_) {
            if (event->type() == QEvent::PlatformSurface &&
                static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                delete shadow_.data();
                xcbShadow_.reset();
            } else if (event->type() == QEvent::Expose || event->type() == QEvent::PlatformSurface ||
                       event->type() == QEvent::ScreenChangeInternal) {
                scheduleSync();
            }
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(object);
        // Popups/dialogs are separate windows even when QObject-parented here.
        // Native video input is forwarded by its surface to its QWidget host.
        if (!widget || widget == overlay_ || widget->window() != window_) return false;

        if (widget == window_) {
            switch (event->type()) {
            case QEvent::Show:
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::WindowStateChange:
            case QEvent::WinIdChange:
            case QEvent::WindowActivate:
            case QEvent::WindowDeactivate:
            case QEvent::ScreenChangeInternal:
                sync();
                scheduleSync();
                break;
            case QEvent::Hide:
                if (shadow_) shadow_->hide();
                restoreCursor();
                break;
            default:
                break;
            }
        }
        // Qt's client-side/native decoration retains all title, pointer and
        // resize handling; supplementing its shadow must not consume input.
        if (clientDecorationOnly_) return false;
        switch (event->type()) {
        case QEvent::ChildAdded:
        case QEvent::ChildPolished:
        case QEvent::ZOrderChange:
            // A new/raised control can cover a corner; merge these changes and
            // do not recursively raise from inside its ZOrderChange delivery.
            scheduleSync();
            break;
        case QEvent::Leave:
            if (widget == cursorWidget_) restoreCursor();
            break;
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            return handleMouse(*widget, *static_cast<QMouseEvent *>(event));
        default:
            break;
        }
        return false;
    }

private:
    bool normalWindow() const {
        return window_ && !window_->isMaximized() && !window_->isFullScreen();
    }

    void sync() {
        if (!window_) return;
        if (!overlay_) { syncShadow(); return; }
        const bool rounded = normalWindow() && options_.radius > 0;
        const bool changed = rounded != rounded_;
        rounded_ = rounded;
        overlay_->setRadius(rounded ? options_.radius : 0);
        overlay_->setGeometry(window_->rect());
        overlay_->setVisible(rounded && window_->isVisible());
        if (rounded) overlay_->raise();
        overlay_->update();
        syncShadow();
        if (changed) {
            // Redraw the underlying corner children too when restoring alpha
            // after a state change; hiding the eraser cannot restore pixels.
            window_->update();
            for (auto *child : window_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
                if (child != overlay_) child->update();
            }
            restoreCursor();
        }
    }

    void syncShadow() {
        auto *body = window_->windowHandle();
        if (nativeParent_ != body) {
            delete shadow_.data();
            xcbShadow_.reset();
            nativeParent_ = body;
        }
        const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        const bool xcb = QGuiApplication::platformName() == QLatin1String("xcb");
        clientDecorationFallback_ = false;
        if (clientDecorationOnly_) {
            // An ordinary QDialog can still have a Qt-drawn title bar on
            // GNOME Wayland. Its stock Qt 5 bradient decoration has no outer
            // shadow. A server-side decoration instead belongs to the WM.
            if (wayland && body && body->handle()) {
                auto *native = static_cast<QtWaylandClient::QWaylandWindow *>(body->handle());
                clientDecorationFallback_ = usesBundledClientDecoration(*native);
                if (clientDecorationFallback_) options_.radius = std::max(0, body->frameMargins().left());
            }
            if (!clientDecorationFallback_) { if (shadow_) shadow_->hide(); return; }
        }
        const bool visible = (wayland || xcb) && normalWindow() && window_->isVisible() &&
            !window_->isMinimized() && body && body->handle() && body->isExposed();
        if (!visible) { if (shadow_) shadow_->hide(); return; }
        if (!shadow_) {
            shadow_ = new ShadowWindow(*body, wayland);
            if (xcb) xcbShadow_ = std::make_unique<LinuxXcbWindowShadow>([this] { scheduleSync(); });
        }
        const auto origin = wayland ? QPoint() : window_->mapToGlobal(QPoint());
        const auto frame = clientDecorationOnly_ ? body->frameMargins() : QMargins();
        // QtWayland adds the parent's decoration margins to a child position.
        // Include the existing title/border in the shadow, keeping the actual
        // wl_subsurface offset at (-margin, -margin) around the full surface.
        shadow_->setGeometry(QRect(origin - QPoint(kShadowMargin + frame.left(), kShadowMargin + frame.top()),
            window_->size() + QSize(kShadowMargin*2 + frame.left() + frame.right(),
                kShadowMargin*2 + frame.top() + frame.bottom())));
        shadow_->create();
        // Qt creates the wl_subsurface role on show, not QWindow::create.
        // There is no attached buffer yet, so configure input/stacking before
        // the first raster paint can map the shadow.
        if (wayland) shadow_->show();
        const bool configured = wayland ? shadow_->configureWayland(*body) : xcbShadow_->configure(*body, *shadow_);
        shadow_->setNativeConfigured(configured);
        shadow_->updateImage(options_.radius, window_->isActiveWindow());
        shadow_->setVisible(configured && (!xcb || xcbShadow_->bodyMapped()));
    }

    void scheduleSync() {
        if (syncPending_) return;
        syncPending_ = true;
        QTimer::singleShot(0, this, [this] {
            syncPending_ = false;
            sync();
        });
    }

    Qt::Edges resizeEdges(QPoint point) const {
        if (!normalWindow() || !window_->rect().contains(point)) return {};
        const int margin = options_.resizeMargin;
        Qt::Edges edges;
        if (window_->minimumWidth() < window_->maximumWidth()) {
            if (point.x() < margin) edges |= Qt::LeftEdge;
            else if (point.x() >= window_->width() - margin) edges |= Qt::RightEdge;
        }
        if (window_->minimumHeight() < window_->maximumHeight()) {
            if (point.y() < margin) edges |= Qt::TopEdge;
            else if (point.y() >= window_->height() - margin) edges |= Qt::BottomEdge;
        }
        return edges;
    }

    void restoreCursor() {
        if (cursorWidget_) {
            if (cursorWasExplicit_) cursorWidget_->setCursor(previousCursor_);
            else cursorWidget_->unsetCursor();
        }
        cursorWidget_.clear();
    }

    void resizeCursor(QWidget &widget, Qt::Edges edges) {
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
            shape = Qt::SizeFDiagCursor;
        } else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
            shape = Qt::SizeBDiagCursor;
        } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
            shape = Qt::SizeHorCursor;
        } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
            shape = Qt::SizeVerCursor;
        }
        if (cursorWidget_ != &widget) {
            restoreCursor();
            cursorWidget_ = &widget;
            cursorWasExplicit_ = widget.testAttribute(Qt::WA_SetCursor);
            previousCursor_ = widget.cursor();
        }
        widget.setCursor(shape);
    }

    bool handleMouse(QWidget &widget, QMouseEvent &event) {
        if (!window_->isVisible() || window_->isMinimized()) return false;
        const QPoint point = window_->mapFromGlobal(event.globalPos());
        const auto edges = resizeEdges(point);
        if (event.type() == QEvent::MouseMove) {
            if (!edges || event.buttons() != Qt::NoButton) {
                restoreCursor();
                return false;
            }
            resizeCursor(widget, edges);
            return true;
        }
        if (event.button() != Qt::LeftButton) return false;
        auto *handle = window_->windowHandle();
        if (!handle) return false;
        if (edges && event.type() == QEvent::MouseButtonPress) {
            ++diagnostics_.resizeRequests;
            diagnostics_.lastResizeEdges = edges;
            diagnostics_.lastResizeAccepted = handle->startSystemResize(edges);
            event.accept();
            return true;
        }
        if (window_->isFullScreen() || !options_.captionHitTest || !options_.captionHitTest(point)) return false;
        if (event.type() == QEvent::MouseButtonDblClick &&
            window_->windowFlags().testFlag(Qt::WindowMaximizeButtonHint)) {
            if (window_->isMaximized()) window_->showNormal();
            else window_->showMaximized();
            event.accept();
            return true;
        }
        if (event.type() == QEvent::MouseButtonPress) {
            ++diagnostics_.moveRequests;
            diagnostics_.lastMoveAccepted = handle->startSystemMove();
            event.accept();
            return true;
        }
        return false;
    }

    QPointer<QWidget> window_;
    QPointer<CornerOverlay> overlay_;
    QPointer<QWindow> nativeParent_;
    QPointer<ShadowWindow> shadow_;
    std::unique_ptr<LinuxXcbWindowShadow> xcbShadow_;
    LinuxWindowFrameDiagnostics diagnostics_;
    LinuxWindowFrameOptions options_;
    bool clientDecorationOnly_ = false;
    bool clientDecorationFallback_ = false;
    QPointer<QWidget> cursorWidget_;
    QCursor previousCursor_;
    bool cursorWasExplicit_ = false;
    bool syncPending_ = false;
    bool rounded_ = false;
};

} // namespace

void installLinuxWindowFrame(QWidget &window, LinuxWindowFrameOptions options) {
    Q_ASSERT(qApp && QThread::currentThread() == window.thread());
    // Native decorated windows keep their platform decoration and shadow.
    // Only an owner explicitly choosing a custom title bar gets this fallback.
    if (!window.windowFlags().testFlag(Qt::FramelessWindowHint)) return;
    Q_ASSERT(window.isWindow() && !window.testAttribute(Qt::WA_WState_Created));
    new LinuxWindowFrame(window, std::move(options));
}

void installLinuxClientDecorationShadow(QWidget &window) {
    Q_ASSERT(qApp && QThread::currentThread() == window.thread());
    Q_ASSERT(window.isWindow() && !window.testAttribute(Qt::WA_WState_Created));
    if (window.windowFlags().testFlag(Qt::FramelessWindowHint)) return;
    // The Linux SDK bundles the Qt 5 bradient decoration (3 px corner radius).
    new LinuxWindowFrame(window, {.radius = 3}, true);
}

LinuxWindowFrameDiagnostics inspectLinuxWindowFrame(const QWidget &window) {
    Q_ASSERT(qApp && QThread::currentThread() == window.thread());
    for (auto *child : window.children()) {
        if (auto *frame = dynamic_cast<LinuxWindowFrame *>(child)) return frame->diagnostics();
    }
    return {};
}

} // namespace MeetingUI
