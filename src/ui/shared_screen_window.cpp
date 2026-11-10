#include "shared_screen_window.h"
#include <algorithm>
#include "app_theme.h"
#include "native_child_geometry.h"
#include "whiteboard/accessible_combo_box.h"
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtGui/QWheelEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QMenu>
#include <QtWidgets/QShortcut>
#include "render/video_canvas.h"
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#endif

namespace MeetingUI {
class SharedScreenFrameView final : public QWidget {
public:
    explicit SharedScreenFrameView(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("sharedScreenViewport"));
        setMinimumSize(1, 1);
    }
    void clear() {
        if (frame) frame->SetRenderExpected(false);
        frame.reset();
        image = {};
        update();
    }
    ShareViewTransform* transform = nullptr;
    QImage image;
    livekit::render::VideoRenderFrame::Ptr frame;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), AppTheme::color(AppTheme::ColorRole::Letterbox));
        if (image.isNull()) return;
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(transform ? transform->destination() : QRectF(rect()), image);
        if (frame) frame->NotifyRendered("qt_cpu_paint");
    }
};

SharedScreenWindow::SharedScreenWindow(bool gpu)
: QWidget(nullptr, Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint), _gpu(gpu) {
    setObjectName(QStringLiteral("sharedScreenWindow"));
    setWindowTitle(tr("Shared screen — Cohavora"));
    setMinimumSize(640, 360);
    resize(960, 600);
    auto* layout = _layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(0);
    _toolbar = new QWidget(this);
    _toolbar->setObjectName(QStringLiteral("sharedScreenTitleBar"));
    _toolbar->installEventFilter(this);
    auto* bar = new QHBoxLayout(_toolbar);
    bar->setContentsMargins(10, 3, 10, 3);
    _name = new QLabel(_toolbar);
    _name->setTextFormat(Qt::PlainText);
    _name->setMinimumWidth(0);
    _name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _name->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(_name, 1);
    _scale = createAccessibleComboBox(_toolbar);
    _scale->setObjectName(QStringLiteral("sharedScreenScale"));
    AppTheme::setStyleVariant(*_scale, "screen-share-picker");
    for (const auto value : {0, 100, 125, 150, 200, 400})
        _scale->addItem(value ? QString::number(value) + "%" : tr("Fit to window"), value);
    _scale->installEventFilter(this);
    connect(_scale, QOverload<int>::of(&QComboBox::activated), this,
        [this](int index) { setZoom(_scale->itemData(index).toDouble()/100); });
    bar->addWidget(_scale);
    _fullscreen = new QPushButton(tr("Full screen"), _toolbar);
    _fullscreen->setObjectName(QStringLiteral("sharedScreenFullscreen"));
    connect(_fullscreen, &QPushButton::clicked, this, &SharedScreenWindow::toggleFullscreen);
    bar->addWidget(_fullscreen);
    _more = new QPushButton(tr("More"), _toolbar);
    _more->setObjectName(QStringLiteral("sharedScreenMore"));
    _menu = new QMenu(this);
    AppTheme::styleMenu(*_menu, AppTheme::Tone::Dark);
    _menu->addAction(tr("Fit window to content"), this, &SharedScreenWindow::fitWindowToFrame);
    auto* top = _menu->addAction(tr("Always on top")); top->setCheckable(true);
    connect(top, &QAction::toggled, this, &SharedScreenWindow::setOnTop);
    auto* pin = _menu->addAction(tr("Keep toolbar expanded")); pin->setCheckable(true);
    _pinnedToolbar = QSettings().value("sharedScreen/pinToolbar", false).toBool();
    pin->setChecked(_pinnedToolbar);
    connect(pin, &QAction::toggled, this, [this](bool checked) {
        _pinnedToolbar = checked; setCompact(false); savePreferences(); noteInteraction();
    });
    _menu->addAction(tr("Minimize"), this, &QWidget::showMinimized);
    connect(_more, &QPushButton::clicked, this, [this] {
        noteInteraction(); _menu->popup(_more->mapToGlobal(QPoint(0, _more->height())));
    });
    connect(_menu, &QMenu::aboutToHide, this, &SharedScreenWindow::noteInteraction);
    bar->addWidget(_more);
    _expand = new QPushButton(tr("Expand toolbar"), _toolbar);
    connect(_expand, &QPushButton::clicked, this, [this] { setCompact(false); noteInteraction(); });
    bar->addWidget(_expand); _expand->hide();
    auto* back = _back = new QPushButton(tr("Return to meeting"), _toolbar);
    back->setObjectName(QStringLiteral("sharedScreenReturn"));
    connect(back, &QPushButton::clicked, this, &SharedScreenWindow::returnRequested);
    bar->addWidget(back);
    layout->addWidget(_toolbar);
    _view = new SharedScreenFrameView(this);
    _view->installEventFilter(this);
    _view->transform = &_transform;
    _view->setMouseTracking(true);
    _view->setFocusPolicy(Qt::StrongFocus);
    auto* row = new QHBoxLayout(); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
    row->addWidget(_view, 1);
    _vertical = new QScrollBar(Qt::Vertical, this); row->addWidget(_vertical);
    layout->addLayout(row, 1);
    _horizontal = new QScrollBar(Qt::Horizontal, this); layout->addWidget(_horizontal);
    const auto scroll = [this] {
        if (_updatingTransform) return;
        const auto size = _transform.displaySize();
        if (!size.isEmpty()) {
            _transform.center = {(_horizontal->value()+_view->width()/2.0)/size.width(),
                (_vertical->value()+_view->height()/2.0)/size.height()};
            updateTransform();
        }
    };
    connect(_horizontal, &QScrollBar::valueChanged, this, scroll);
    connect(_vertical, &QScrollBar::valueChanged, this, scroll);
    _status = new QLabel(tr("Waiting for shared screen…"), this);
    _status->setTextFormat(Qt::PlainText);
    layout->addWidget(_status);
    AppTheme::setStyleVariant(*this, "shared-screen-window");
    AppTheme::observeChanges(*this, [this] { refreshTheme(); });
    refreshTheme();
    _idleTimer = new QTimer(this); _idleTimer->setInterval(5000); _idleTimer->setSingleShot(true);
    connect(_idleTimer, &QTimer::timeout, this, [this] {
        auto* focus = QApplication::focusWidget();
        if (_pinnedToolbar || _toolbar->underMouse() || _menu->isVisible() ||
            QApplication::activePopupWidget() || (focus && _toolbar->isAncestorOf(focus))) {
            noteInteraction(); return;
        }
        if (isFullScreen()) setCompact(true);
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] {
        if (_menu->isVisible()) _menu->close();
        else if (isFullScreen()) toggleFullscreen();
    });
    auto* full = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(full, &QShortcut::activated, this, &SharedScreenWindow::toggleFullscreen);
    for (auto* widget : _toolbar->findChildren<QWidget*>()) widget->installEventFilter(this);
    const auto observeScreen = [this](QScreen* target) {
        connect(target, &QScreen::availableGeometryChanged, this, [this] { ensureOnScreen(); updateTransform(); });
    };
    for (auto* target : QGuiApplication::screens()) observeScreen(target);
    connect(qApp, &QGuiApplication::screenAdded, this, observeScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        QTimer::singleShot(0, this, [this] {
            const bool full = isFullScreen();
            if (full) toggleFullscreen();
            ensureOnScreen(); updateTransform();
            if (full) toggleFullscreen();
        });
    });
    const auto geometry = QSettings().value("sharedScreen/geometry").toRect();
    if (geometry.isValid()) { setGeometry(geometry); _normalGeometry = geometry; _restoredGeometry = true; ensureOnScreen(); }
    updateTransform();
    if (_gpu) {
        _canvas = livekit::render::CreateVideoCanvas(_view);
        if (_canvas) {
            _canvas->installEventFilter(this);
            _canvas->setFocusPolicy(Qt::StrongFocus);
            connect(_canvas, &livekit::render::VideoCanvas::rendererUnavailable,
                this, &SharedScreenWindow::rendererUnavailable, Qt::QueuedConnection);
            connect(_canvas, &livekit::render::VideoCanvas::rendererInitialized,
                this, [this] { layoutCanvas(); });
        } else {
            QTimer::singleShot(0, this, &SharedScreenWindow::rendererUnavailable);
        }
        QTimer::singleShot(3000, this, [this] {
            if (_gpu && (!_canvas || !_canvas->rendererReady())) emit rendererUnavailable();
        });
    }
}
SharedScreenWindow::~SharedScreenWindow() {
    savePreferences();
    if (_nativeFilter) qApp->removeNativeEventFilter(this);
    clearFrame();
    if (_canvas) _canvas->shutdownRenderer();
}
livekit::DetachedShareViewport SharedScreenWindow::viewport() const {
    livekit::DetachedShareViewport result;
    result.rect = {0, 0, _view->width(), _view->height()};
    result.device_pixel_ratio = devicePixelRatioF();
    result.visible = isVisible();
    result.minimized = isMinimized();
    return result;
}
bool SharedScreenWindow::viewing() const {
    return isVisible() && !isMinimized() && _view->width() > 0 && _view->height() > 0;
}
void SharedScreenWindow::setSharingName(const QString& name) {
    setZoom(0);
    _name->setText(tr("%1 is sharing").arg(name));
    _name->setToolTip(_name->text());
    _name->setText(_name->fontMetrics().elidedText(_name->toolTip(), Qt::ElideRight, _name->width()));
}
void SharedScreenWindow::setStatus(const QString& text, bool clear) {
    if (clear) clearFrame();
    _status->setText(text);
}
void SharedScreenWindow::setFrame(const QImage& image, livekit::render::VideoRenderFrame::Ptr frame) {
    if (_gpu || !viewing() || image.isNull()) return;
    updateFrameGeometry(frame ? frame->view().width : image.width(),
        frame ? frame->view().height : image.height(), frame ? frame->view().rotation_degrees : 0);
    _view->image = image;
    _view->frame = std::move(frame);
    if (_view->frame) _view->frame->SetRenderExpected(true);
    ++_receivedFrames;
    updateTransform();
    _view->update();
}
void SharedScreenWindow::setGpuFrame(livekit::render::VideoRenderFrame::Ptr frame) {
    if (!_gpu || !_canvas || !_canvas->rendererReady() || !viewing() || !frame) return;
    const auto& view = frame->view();
    updateFrameGeometry(view.width, view.height, view.rotation_degrees);
    updateTransform();
    _canvas->updateFrame("shared-screen", std::move(frame));
    ++_receivedFrames;
}
void SharedScreenWindow::useCpu() {
    _gpu = false;
    if (_canvas) { _canvas->shutdownRenderer(); _canvas->hide(); }
    _view->update();
}
void SharedScreenWindow::clearFrame() {
    _view->clear();
    _transform.source = {};
    updateTransform();
    if (_canvas) _canvas->clearUsers();
}
void SharedScreenWindow::locate() {
    ensureOnScreen();
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
#if defined(Q_OS_WIN)
    if (GetForegroundWindow() != reinterpret_cast<HWND>(winId())) {
        FLASHWINFO info{sizeof(info), reinterpret_cast<HWND>(winId()), FLASHW_TRAY, 3, 0};
        FlashWindowEx(&info);
    }
#endif
}
void SharedScreenWindow::layoutCanvas() {
    if (!_gpu || !_canvas) return;
    _canvas->setGeometry(_view->rect());
    _canvas->setTilesLayout({{"shared-screen", 0, 0, _view->width(), _view->height(), false, 0, true}});
    _canvas->show();
    updateTransform();
    layoutToolbar();
}
bool SharedScreenWindow::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::KeyPress || event->type() == QEvent::Wheel) noteInteraction();
    if (object == _scale && event->type() == QEvent::Wheel) return true;
    if (object == _name && event->type() == QEvent::Resize)
        _name->setText(_name->fontMetrics().elidedText(_name->toolTip(), Qt::ElideRight, _name->width()));
    const bool content = object == _view || object == _canvas;
    if (content && event->type() == QEvent::Wheel) {
        const auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const auto size = _transform.orientedSource();
            const double fit = size.isEmpty() ? 1 : std::min(
                double(_view->width())/size.width(), double(_view->height())/size.height()) * devicePixelRatioF();
            setZoom((_transform.zoom > 0 ? _transform.zoom : fit) *
                std::pow(1.25, wheel->angleDelta().y()/120.0));
        } else {
            QPointF delta = wheel->pixelDelta();
            if (delta.isNull()) delta = QPointF(wheel->angleDelta())/3.0;
            if (wheel->modifiers() & Qt::ShiftModifier) delta = {delta.y(), delta.x()};
            _transform.pan(delta); updateTransform();
        }
        event->accept(); return true;
    }
    if (content && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            _panning = true; _panPosition = mouse->globalPos();
            static_cast<QWidget*>(object)->setCursor(Qt::ClosedHandCursor);
            event->accept(); return true;
        }
    }
    if (content && event->type() == QEvent::MouseMove && _panning) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        _transform.pan(mouse->globalPos() - _panPosition); _panPosition = mouse->globalPos();
        updateTransform(); event->accept(); return true;
    }
    if (content && event->type() == QEvent::MouseButtonRelease && _panning) {
        _panning = false; static_cast<QWidget*>(object)->unsetCursor(); event->accept(); return true;
    }
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse) {
        _panning = false;
        if (content) static_cast<QWidget*>(object)->unsetCursor();
    }

    if (object == _view && event->type() == QEvent::Resize) {
        layoutCanvas();
        emit viewportChanged();
    }
    if (object == _toolbar && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (!isFullScreen() && mouse->button() == Qt::LeftButton && windowHandle()) windowHandle()->startSystemMove();
    }
    if (object == _toolbar && event->type() == QEvent::MouseButtonDblClick) {
        if (!isFullScreen() && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            isMaximized() ? showNormal() : showMaximized();
        }
    }
    return QWidget::eventFilter(object, event);
}
void SharedScreenWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    setupNativeWindow();
    setOnTop(_onTop);
    if (!_screenConnected && windowHandle()) {
        _screenConnected = true;
        connect(windowHandle(), &QWindow::screenChanged, this, [this] {
            updateTransform(); emit viewportChanged();
        });
    }
    layoutCanvas();
    emit viewportChanged();
}
void SharedScreenWindow::hideEvent(QHideEvent* event) {
    clearFrame();
    QWidget::hideEvent(event);
    emit viewportChanged();
}
void SharedScreenWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::ScreenChangeInternal) {
        if (!viewing()) clearFrame();
        layoutCanvas();
        emit viewportChanged();
    }
}
void SharedScreenWindow::paintEvent(QPaintEvent*) {
    // Fill the complete raster surface; a QWidget subclass does not paint its
    // stylesheet background by default. DX11 paints only its native child.
    QPainter painter(this);
    painter.fillRect(rect(), AppTheme::color(AppTheme::ColorRole::Window));
}
void SharedScreenWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit returnRequested();
}
void SharedScreenWindow::refreshTheme() {
    auto palette = _toolbar->palette();
    palette.setColor(QPalette::Window, AppTheme::color(AppTheme::ColorRole::Window));
    _toolbar->setPalette(palette); _toolbar->setAutoFillBackground(true);
    if (_menu) AppTheme::styleMenu(*_menu, AppTheme::Tone::Dark);
    update(); _view->update();
}
void SharedScreenWindow::setZoom(double zoom) {
    _transform.setZoom(zoom); updateTransform();
}
void SharedScreenWindow::updateFrameGeometry(int width, int height, int rotation) {
    if (_transform.source == QSize(width, height) && _transform.rotation == rotation) return;
    _transform.source = {width, height}; _transform.rotation = rotation;
    updateTransform();
}
void SharedScreenWindow::updateTransform() {
    if (_updatingTransform || !_horizontal || !_vertical) return;
    _updatingTransform = true;
    _transform.viewport = _view->size(); _transform.dpr = devicePixelRatioF(); _transform.clamp();
    const auto size = _transform.displaySize(); const auto rect = _transform.destination();
    const bool scrollable = _transform.zoom > 0 && !size.isEmpty();
    // Keep both bars for fixed scale to avoid layout oscillation near fit size.
    _horizontal->setVisible(scrollable); _vertical->setVisible(scrollable);
    {
        QSignalBlocker h(_horizontal), v(_vertical), z(_scale);
        _horizontal->setRange(0, std::max(0, qRound(size.width() - _view->width())));
        _vertical->setRange(0, std::max(0, qRound(size.height() - _view->height())));
        _horizontal->setPageStep(_view->width()); _vertical->setPageStep(_view->height());
        _horizontal->setValue(qRound(-rect.x())); _vertical->setValue(qRound(-rect.y()));
        const int percent = qRound(_transform.zoom*100);
        int index = _scale->findData(percent);
        if (index < 0) { if (_scale->count() > 6) _scale->removeItem(6); _scale->addItem(QString::number(percent)+"%", percent); index = _scale->count()-1; }
        _scale->setCurrentIndex(index);
    }
    if (_canvas && !rect.isEmpty()) _canvas->setVideoDestination("shared-screen", rect);
    _view->update();
    if (_status && !_transform.source.isEmpty())
        _status->setText(tr("Watching · %1 × %2 · %3").arg(_transform.source.width())
            .arg(_transform.source.height()).arg(_transform.zoom > 0 ?
                QString::number(qRound(_transform.zoom*100))+"%" : tr("Fit to window")));
    _updatingTransform = false;
}
void SharedScreenWindow::layoutToolbar() {
    if (!isFullScreen()) return;
    const int width = std::min(this->width()-16, _compact ? 520 : 980);
    _toolbar->setGeometry((this->width()-width)/2, 8, width, _toolbar->sizeHint().height());
    _toolbar->show(); _toolbar->raise();
}
void SharedScreenWindow::setCompact(bool compact) {
    _compact = compact && isFullScreen();
    _scale->setVisible(!_compact); _more->setVisible(!_compact); _back->setVisible(!_compact);
    _expand->setVisible(_compact); layoutToolbar();
}
void SharedScreenWindow::noteInteraction() {
    if (_idleTimer && isFullScreen()) _idleTimer->start();
}
void SharedScreenWindow::toggleFullscreen() {
    if (isFullScreen()) {
        _idleTimer->stop();
        showNormal();
        _layout->insertWidget(0, _toolbar);
        _layout->setContentsMargins(6, 4, 6, 4); _status->show();
        setCompact(false); _fullscreen->setText(tr("Full screen"));
        if (_beforeFullscreen.isValid()) setGeometry(_beforeFullscreen);
        if (_wasMaximized) showMaximized();
        ensureOnScreen();
    } else {
        _wasMaximized = isMaximized();
        _beforeFullscreen = _wasMaximized ? normalGeometry() : geometry();
        savePreferences();
        // Only the toolbar gains an HWND; the video surface never changes parent.
        if (!_toolbar->testAttribute(Qt::WA_NativeWindow)) {
            new NativeChildGeometry(*_toolbar); _toolbar->setAttribute(Qt::WA_NativeWindow);
        }
        _layout->removeWidget(_toolbar); _layout->setContentsMargins(0, 0, 0, 0);
        _status->hide();
        showFullScreen(); _fullscreen->setText(tr("Exit full screen"));
        setCompact(false); layoutToolbar(); noteInteraction();
    }
    layoutCanvas(); updateTransform(); emit viewportChanged();
}
void SharedScreenWindow::setOnTop(bool enabled) {
    _onTop = enabled;
#if defined(Q_OS_WIN)
    // Do not use setWindowFlags here: that may recreate the active HWND.
    SetWindowPos(reinterpret_cast<HWND>(winId()), enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}
void SharedScreenWindow::fitWindowToFrame() {
    if (_transform.orientedSource().isEmpty()) return;
    if (isFullScreen()) toggleFullscreen();
    if (isMaximized()) showNormal();
    const auto* target = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!target) return;
    const auto available = target->availableGeometry();
    const auto source = QSizeF(_transform.orientedSource()) / devicePixelRatioF();
    const QSize chrome(width()-_view->width(), height()-_view->height());
    const auto limit = QSizeF(std::max(1, available.width()-chrome.width()-24),
        std::max(1, available.height()-chrome.height()-24));
    const double factor = std::min({1.0, limit.width()/source.width(), limit.height()/source.height()});
    resize((source*factor).toSize()+chrome); ensureOnScreen();
}
void SharedScreenWindow::ensureOnScreen() {
    if (isFullScreen()) return;
    QScreen* target = QGuiApplication::screenAt(frameGeometry().center());
    if (!target) {
        const auto hint = QSettings().value("sharedScreen/screen").toString();
        for (auto* candidate : QGuiApplication::screens())
            if (candidate->name() == hint) { target = candidate; break; }
    }
    if (!target) target = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!target) return;
    const auto area = target->availableGeometry();
    const QSize bounded = size().boundedTo(area.size());
    if (size() != bounded) resize(bounded);
    move(std::clamp(x(), area.left(), std::max(area.left(), area.right()-width()+1)),
         std::clamp(y(), area.top(), std::max(area.top(), area.bottom()-height()+1)));
}
void SharedScreenWindow::savePreferences() {
    QSettings settings;
    if (_normalGeometry.isValid()) settings.setValue("sharedScreen/geometry", _normalGeometry);
    if (screen()) settings.setValue("sharedScreen/screen", screen()->name());
    settings.setValue("sharedScreen/pinToolbar", _pinnedToolbar);
}
void SharedScreenWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event); layoutToolbar(); updateTransform();
    if (isVisible() && !isFullScreen() && !isMaximized() && !isMinimized()) _normalGeometry = geometry();
}
void SharedScreenWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    if (isVisible() && !isFullScreen() && !isMaximized() && !isMinimized()) _normalGeometry = geometry();
}
bool SharedScreenWindow::event(QEvent* event) {
    if (event->type() == QEvent::ScreenChangeInternal) {
        QTimer::singleShot(0, this, [this] { updateTransform(); layoutCanvas(); emit viewportChanged(); });
    }
    if (event->type() == QEvent::WindowDeactivate) _panning = false;
    return QWidget::event(event);
}
void SharedScreenWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
    const auto handle = reinterpret_cast<HWND>(winId());
    if (!_nativeFilter) { qApp->installNativeEventFilter(this); _nativeFilter = true; }
    if (isFullScreen()) return;
    const auto old = GetWindowLongPtrW(handle, GWL_STYLE);
    const auto style = (old & ~LONG_PTR(WS_CAPTION)) | WS_THICKFRAME | WS_SYSMENU |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    if (style != old) {
        SetWindowLongPtrW(handle, GWL_STYLE, style);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}
int SharedScreenWindow::resizeHit(qintptr position) const {
#if defined(Q_OS_WIN)
    if (isMaximized() || isFullScreen()) return HTCLIENT;
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    RECT client{};
    if (!ScreenToClient(handle, &point) || !GetClientRect(handle, &client)) return HTCLIENT;
    const int edge = qRound(6 * devicePixelRatioF());
    const bool left = point.x < edge, right = point.x >= client.right - edge;
    const bool top = point.y < edge, bottom = point.y >= client.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
#endif
    return 1; // HTCLIENT
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool SharedScreenWindow::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
#else
bool SharedScreenWindow::nativeEventFilter(const QByteArray&, void* message, long* result) {
#endif
#if defined(Q_OS_WIN)
    const auto* msg = static_cast<MSG*>(message);
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    if (msg && msg->message == WM_NCHITTEST && msg->hwnd != handle &&
        IsChild(handle, msg->hwnd) && resizeHit(msg->lParam) != HTCLIENT) {
        *result = HTTRANSPARENT;
        return true;
    }
#endif
    return false;
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool SharedScreenWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#else
bool SharedScreenWindow::nativeEvent(const QByteArray& type, void* message, long* result) {
#endif
#if defined(Q_OS_WIN)
    const auto* msg = static_cast<MSG*>(message);
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    if (msg && msg->hwnd == handle && msg->message == WM_ERASEBKGND) {
        // The Qt backing store paints the opaque client area in one pass.
        *result = 1;
        return true;
    }
    if (msg && msg->hwnd == handle && msg->message == WM_NCACTIVATE) {
        *result = DefWindowProcW(handle, msg->message, msg->wParam, -1);
        return true;
    }
    if (msg && msg->hwnd == handle && msg->message == WM_NCCALCSIZE && msg->wParam) {
        auto* parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
        const RECT proposed = parameters->rgrc[0];
        DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
        const int resizeBorder = proposed.bottom - parameters->rgrc[0].bottom;
        const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
        if ((style & WS_THICKFRAME) && !(style & WS_MAXIMIZE) &&
            !isFullScreen() && resizeBorder > 0) {
            // Match the meeting window: retain native side borders and a thin
            // physical top/bottom edge rather than taking over all NC pixels.
            const int thinBorder = std::min(resizeBorder, 1);
            parameters->rgrc[0].top = proposed.top + thinBorder;
            parameters->rgrc[0].bottom = proposed.bottom - thinBorder;
        } else {
            parameters->rgrc[0].top = proposed.top + resizeBorder;
        }
        *result = 0;
        return true;
    }
    if (msg && msg->hwnd == handle && msg->message == WM_NCHITTEST) {
        POINT nativePoint{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT client{};
        if (ScreenToClient(handle, &nativePoint) && GetClientRect(handle, &client) &&
            !PtInRect(&client, nativePoint)) {
            *result = DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
            return true;
        }
        *result = resizeHit(msg->lParam);
        if (*result != HTCLIENT) return true;
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(handle, &point);
        const QPoint logical(qRound(point.x / devicePixelRatioF()), qRound(point.y / devicePixelRatioF()));
        if (!isFullScreen() && _toolbar->geometry().contains(logical)) {
            const auto* child = _toolbar->childAt(_toolbar->mapFrom(this, logical));
            if (!child || qobject_cast<const QLabel*>(child)) *result = HTCAPTION;
        }
        return true;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}
} // namespace MeetingUI
