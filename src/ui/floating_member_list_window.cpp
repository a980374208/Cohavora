#include "floating_member_list_window.h"

#include "app_icons.h"
#include "app_theme.h"
#include "meeting_layout_controls.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>

#include <algorithm>
#include <set>
#include <utility>

#ifdef Q_OS_LINUX
#include "linux_window_frame.h"
#endif

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace MeetingUI {
namespace {

constexpr int kOuterMarginX = 4;
constexpr int kOuterMarginTop = 4;
constexpr int kOuterMarginBottom = 4;
constexpr int kTitleBarHeight = 34;
constexpr int kPagerBarHeight = 34;
constexpr int kMinWindowWidth = 180;
constexpr int kMaxWindowWidth = 520;
constexpr int kMinExpandedHeight = 180;
constexpr int kDefaultWindowWidth = 240;
constexpr int kDefaultWindowHeight = 520;
constexpr int kCollapsedWindowHeight = kOuterMarginTop + kTitleBarHeight + kOuterMarginBottom;

QString roomText(const char* source) {
    return QCoreApplication::translate("MeetingUI::MeetingRoomWindow", source);
}

QIcon makePinIcon(bool active) {
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor color = AppTheme::color(
        active ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextSecondary);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(active ? color : Qt::NoBrush);
    // Upright thumbtack silhouette in a 16x16 logical box
    QPainterPath head;
    head.moveTo(5.5, 3.5);
    head.lineTo(10.5, 3.5);
    head.lineTo(9.8, 7.2);
    head.lineTo(11.5, 9.2);
    head.lineTo(4.5, 9.2);
    head.lineTo(6.2, 7.2);
    head.closeSubpath();
    p.drawPath(head);
    p.drawLine(QPointF(8.0, 9.4), QPointF(8.0, 13.2));
    return QIcon(pixmap);
}

} // namespace

class FloatingListViewport final : public QWidget {
public:
    explicit FloatingListViewport(FloatingMemberListWindow* owner)
        : QWidget(owner), _owner(owner) {
        setObjectName(QStringLiteral("floatingMemberListViewport"));
        setMouseTracking(true);
        setMinimumSize(1, 1);
    }

    QString hoveredKey() const { return _hoveredKey; }

    void clearInteractionState() {
        _hoveredKey.clear();
        _pressedPinKey.clear();
        unsetCursor();
    }

    void syncHoverFromCursor() {
        if (!isVisible()) return;
        updateHoveredAt(mapFromGlobal(QCursor::pos()));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), AppTheme::color(AppTheme::ColorRole::Stage));
        if (!_owner || _owner->_gpu) return;
        for (const auto& slot : _owner->_slots) {
            if (slot.rect.isEmpty() || !slot.cpuPainter) continue;
            slot.cpuPainter(painter, slot.rect, _hoveredKey == slot.renderKey);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        updateHoveredAt(event->pos());
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        if (!_hoveredKey.isEmpty()) {
            _hoveredKey.clear();
            unsetCursor();
            QToolTip::hideText();
            if (_owner && !_owner->_gpu) update();
        }
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (_owner && !_owner->_gpu && event->button() == Qt::LeftButton) {
            _pressedPinKey = hitTest(event->pos(), true);
            if (!_pressedPinKey.isEmpty()) {
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (_owner && !_owner->_gpu && event->button() == Qt::LeftButton && !_pressedPinKey.isEmpty()) {
            const QString released = hitTest(event->pos(), true);
            const QString target = std::exchange(_pressedPinKey, QString());
            if (released == target && !target.isEmpty()) {
                emit _owner->tileMenuRequested(target, _owner->menuAnchorGlobalPos(target));
            }
            event->accept();
            return;
        }
        if (_owner && event->button() == Qt::RightButton) {
            const QString target = hitTest(event->pos(), false);
            if (!target.isEmpty()) {
                emit _owner->tileMenuRequested(target, event->globalPos());
                event->accept();
                return;
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (_owner && !_owner->_gpu && event->button() == Qt::LeftButton) {
            if (!hitTest(event->pos(), true).isEmpty()) {
                event->accept();
                return;
            }
            const QString target = hitTest(event->pos(), false);
            if (!target.isEmpty()) {
                emit _owner->tileDoubleClicked(target);
                event->accept();
                return;
            }
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        if (!_owner) return;
        const QString target = hitTest(event->pos(), false);
        if (!target.isEmpty()) {
            emit _owner->tileMenuRequested(target, event->globalPos());
            event->accept();
            return;
        }
        QWidget::contextMenuEvent(event);
    }

private:
    QString hitTest(const QPoint& pos, bool pinOnly) const {
        if (!_owner) return {};
        for (auto it = _owner->_slots.rbegin(); it != _owner->_slots.rend(); ++it) {
            if (!it->rect.contains(pos)) continue;
            if (pinOnly) {
                const QRect pin = it->pinRect.translated(it->rect.topLeft());
                return pin.contains(pos) ? it->renderKey : QString();
            }
            return it->renderKey;
        }
        return {};
    }

    void updateHoveredAt(const QPoint& pos) {
        if (!_owner || _owner->_gpu) return;
        const QString next = rect().contains(pos) ? hitTest(pos, false) : QString();
        const bool overPin = !next.isEmpty() && !hitTest(pos, true).isEmpty();
        setCursor(overPin ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (_hoveredKey != next) {
            _hoveredKey = next;
            if (next.isEmpty()) {
                QToolTip::hideText();
            } else {
                for (const auto& slot : _owner->_slots) {
                    if (slot.renderKey == next && slot.toolTipProvider) {
                        const QString tip = slot.toolTipProvider();
                        if (!tip.isEmpty()) {
                            QToolTip::showText(mapToGlobal(pos), tip, this, slot.rect);
                        }
                        break;
                    }
                }
            }
            update();
        }
    }

    FloatingMemberListWindow* _owner = nullptr;
    QString _hoveredKey;
    QString _pressedPinKey;
};

FloatingMemberListWindow::FloatingMemberListWindow(bool gpu, bool persistPreferences)
    : QWidget(nullptr, Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint)
    , _gpu(gpu)
    , _persistPreferences(persistPreferences) {
    setObjectName(QStringLiteral("floatingMemberListWindow"));
    setMinimumSize(kMinWindowWidth, kMinExpandedHeight);
    setMaximumWidth(kMaxWindowWidth);
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

#ifdef Q_OS_LINUX
    setWindowFlag(Qt::FramelessWindowHint, true);
    installLinuxWindowFrame(*this, {
        .radius = 10,
        .resizeMargin = 6,
        .captionHitTest = [this](QPoint position) {
            return isTitleDragArea(position);
        },
    });
#endif

    _titleBar = new QWidget(this);
    _titleBar->setObjectName(QStringLiteral("floatingMemberListTitleBar"));
    _titleBar->installEventFilter(this);
    auto* bar = new QHBoxLayout(_titleBar);
    bar->setContentsMargins(8, 2, 4, 2);
    bar->setSpacing(4);

    _iconLabel = new QLabel(_titleBar);
    _iconLabel->setFixedSize(16, 16);
    _iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(_iconLabel);

    _titleLabel = new QLabel(_titleBar);
    _titleLabel->setObjectName(QStringLiteral("floatingMemberListTitle"));
    _titleLabel->setTextFormat(Qt::PlainText);
    _titleLabel->setMinimumWidth(0);
    _titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(_titleLabel, 1);

    const auto makeHeaderButton = [this](const char* name) {
        auto* btn = new QToolButton(_titleBar);
        btn->setObjectName(QString::fromLatin1(name));
        btn->setFixedSize(26, 26);
        btn->setIconSize(QSize(14, 14));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::StrongFocus);
        return btn;
    };

    _onTopButton = makeHeaderButton("floatingMemberListOnTop");
    _onTopButton->setCheckable(true);
    if (_persistPreferences) {
        _onTop = QSettings().value(QStringLiteral("floatingMemberList/onTop"), true).toBool();
    }
    _onTopButton->setChecked(_onTop);
    connect(_onTopButton, &QToolButton::toggled, this, &FloatingMemberListWindow::setAlwaysOnTop);
    bar->addWidget(_onTopButton);

    _collapseButton = makeHeaderButton("floatingMemberListCollapse");
    connect(_collapseButton, &QToolButton::clicked, this, [this] {
        setCollapsed(!_collapsed);
    });
    bar->addWidget(_collapseButton);

    _dockButton = makeHeaderButton("floatingMemberListDock");
    connect(_dockButton, &QToolButton::clicked, this, &FloatingMemberListWindow::dockRequested);
    bar->addWidget(_dockButton);

    _viewport = new FloatingListViewport(this);
    _viewport->installEventFilter(this);

    _emptyLabel = new QLabel(_viewport);
    _emptyLabel->setObjectName(QStringLiteral("floatingMemberListEmpty"));
    _emptyLabel->setAlignment(Qt::AlignCenter);
    _emptyLabel->setWordWrap(true);
    _emptyLabel->setTextFormat(Qt::PlainText);
    _emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    _emptyLabel->hide();

    _pager = new MeetingVideoPager(this);
    _pager->hide();
    _pager->pageRequested = [this](uint32_t page) {
        emit pageRequested(page);
    };

    AppTheme::setStyleVariant(*this, "floating-member-list-window");
    AppTheme::observeChanges(*this, [this] { refreshTheme(); });

    const auto observeScreen = [this](QScreen* target) {
        connect(target, &QScreen::availableGeometryChanged, this, [this] {
            ensureOnScreen();
            emit viewportChanged();
        });
    };
    for (auto* target : QGuiApplication::screens()) observeScreen(target);
    connect(qApp, &QGuiApplication::screenAdded, this, observeScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        QTimer::singleShot(0, this, [this] {
            ensureOnScreen();
            emit viewportChanged();
        });
    });

    if (_persistPreferences) {
        const QRect saved = QSettings().value(QStringLiteral("floatingMemberList/geometry")).toRect();
        if (saved.isValid() && saved.width() >= kMinWindowWidth && saved.height() >= kMinExpandedHeight) {
            const QRect bounded(saved.topLeft(),
                QSize(std::clamp(saved.width(), kMinWindowWidth, kMaxWindowWidth),
                      std::max(kMinExpandedHeight, saved.height())));
            setGeometry(bounded);
            _normalGeometry = bounded;
            _expandedHeight = bounded.height();
            _restoredGeometry = true;
            ensureOnScreen();
        }
    }

    updateChromeText();
    refreshTheme();
    updateContentGeometry();

    if (_gpu) {
        _canvas = livekit::render::CreateVideoCanvas(_viewport);
        if (_canvas) {
            _canvas->setStageOverlay(_emptyLabel);
            _canvas->installEventFilter(this);
            connect(_canvas, &livekit::render::VideoCanvas::rendererUnavailable,
                this, &FloatingMemberListWindow::rendererUnavailable, Qt::QueuedConnection);
            connect(_canvas, &livekit::render::VideoCanvas::rendererInitialized,
                this, [this] { layoutCanvas(); });
            connect(_canvas, &livekit::render::VideoCanvas::tileDoubleClicked,
                this, &FloatingMemberListWindow::tileDoubleClicked);
            connect(_canvas, &livekit::render::VideoCanvas::tilePinRequested,
                this, [this](const QString& key) {
                    emit tileMenuRequested(key, menuAnchorGlobalPos(key));
                });
        } else {
            QTimer::singleShot(0, this, &FloatingMemberListWindow::rendererUnavailable);
        }
    }
}

FloatingMemberListWindow::~FloatingMemberListWindow() {
    savePreferences();
    if (_nativeFilter && qApp) qApp->removeNativeEventFilter(this);
    clearFrames();
    if (_canvas) _canvas->shutdownRenderer();
}

livekit::FloatingStripViewport FloatingMemberListWindow::viewport() const {
    livekit::FloatingStripViewport result;
    const int contentTop = kOuterMarginTop + kTitleBarHeight;
    const int contentW = std::max(0, width() - 2 * kOuterMarginX);
    const int sourceHeight = _collapsed ? _expandedHeight : height();
    const int contentH = _collapsed
        ? 0
        : std::max(0, sourceHeight - contentTop - kOuterMarginBottom);
    result.rect = {0, 0, contentW, contentH};
    result.paging_bar_height = kPagerBarHeight;
    result.device_pixel_ratio = devicePixelRatioF();
    result.visible = isVisible();
    result.minimized = isMinimized();
    result.collapsed = _collapsed;
    return result;
}

bool FloatingMemberListWindow::viewing() const {
    return isVisible() && !isMinimized() && !_collapsed &&
        _viewport && _viewport->width() > 0 && _viewport->height() > 0;
}

void FloatingMemberListWindow::setSlots(std::vector<FloatingMemberSlot> memberSlots, const QString& emptyHint) {
    _slots = std::move(memberSlots);
    _emptyHint = emptyHint;

    const bool showEmpty = _slots.empty() && !_emptyHint.isEmpty() && !_collapsed;
    _emptyLabel->setText(_emptyHint);
    _emptyLabel->setVisible(showEmpty);
    if (showEmpty && _viewport) {
        const int pad = 16;
        _emptyLabel->setGeometry(pad, 0, std::max(1, _viewport->width() - 2 * pad), _viewport->height());
    }

    if (_gpu && _canvas) {
        std::set<std::string> nextKeys;
        std::vector<livekit::render::VideoTileRect> tiles;
        tiles.reserve(_slots.size());
        for (const auto& slot : _slots) {
            if (slot.renderKey.isEmpty() || slot.rect.isEmpty()) continue;
            const std::string key = slot.renderKey.toStdString();
            nextKeys.insert(key);
            tiles.push_back({
                key,
                slot.rect.x(),
                slot.rect.y(),
                slot.rect.width(),
                slot.rect.height(),
                slot.isSpeaking,
                slot.audioLevel,
                slot.hasVideo,
                livekit::render::VideoClipRect{
                    slot.rect.x(), slot.rect.y(), slot.rect.width(), slot.rect.height()}
            });
            _canvas->setTileDecoration(
                key, slot.decorationPainter, slot.pinRect, slot.toolTipProvider);
        }
        for (const auto& previous : _canvasKeys) {
            if (!nextKeys.contains(previous)) {
                _canvas->removeUser(previous);
            }
        }
        _canvasKeys.assign(nextKeys.begin(), nextKeys.end());
        _canvas->setGeometry(_viewport->rect());
        _canvas->setTilesLayout(tiles);
        if (!_collapsed) {
            _canvas->show();
            _canvas->raise();
        }
    } else if (_viewport) {
        _viewport->syncHoverFromCursor();
        _viewport->update();
    }
}

void FloatingMemberListWindow::setMemberCount(int count) {
    const int normalized = std::max(0, count);
    if (_memberCount == normalized) return;
    _memberCount = normalized;
    updateChromeText();
}

void FloatingMemberListWindow::setPage(uint32_t page, uint32_t pageCount) {
    const uint32_t normalizedCount = std::max<uint32_t>(1, pageCount);
    const uint32_t normalizedPage = std::min(page, normalizedCount - 1);
    const bool visibilityChanged = (_pageCount > 1) != (normalizedCount > 1);
    _page = normalizedPage;
    _pageCount = normalizedCount;
    if (_pager) _pager->setPage(_page, _pageCount);
    if (visibilityChanged) {
        updateContentGeometry();
    }
}

void FloatingMemberListWindow::setCollapsed(bool collapsed) {
    if (_collapsed == collapsed) return;
    if (collapsed && !isMinimized() && height() >= kMinExpandedHeight) {
        _expandedHeight = height();
    }
    _collapsed = collapsed;
    if (_collapsed) {
        if (_viewport) _viewport->clearInteractionState();
        clearFrames();
        setMinimumHeight(kCollapsedWindowHeight);
        setMaximumHeight(kCollapsedWindowHeight);
        resize(width(), kCollapsedWindowHeight);
    } else {
        setMinimumHeight(kMinExpandedHeight);
        setMaximumHeight(QWIDGETSIZE_MAX);
        resize(width(), std::max(kMinExpandedHeight, _expandedHeight));
        ensureOnScreen();
    }
    updateChromeText();
    refreshTheme();
    updateContentGeometry();
    emit viewportChanged();
}

void FloatingMemberListWindow::setAlwaysOnTop(bool enabled) {
    _onTop = enabled;
    if (_onTopButton && _onTopButton->isChecked() != enabled) {
        _onTopButton->setChecked(enabled);
    }
    applyTopMostFlag();
    updateChromeText();
    refreshTheme();
    savePreferences();
}

void FloatingMemberListWindow::updateGpuFrame(
        const std::string& renderKey,
        livekit::render::VideoRenderFrame::Ptr frame) {
    if (!_gpu || !_canvas || !_canvas->rendererReady() || !viewing() || !frame || renderKey.empty()) {
        return;
    }
    _canvas->updateFrame(renderKey, std::move(frame));
}

void FloatingMemberListWindow::updateTilePresentation(const std::string& renderKey, bool hasVideo) {
    if (_gpu && _canvas) {
        _canvas->updateTilePresentation(renderKey, hasVideo);
    } else if (_viewport) {
        _viewport->update();
    }
}

void FloatingMemberListWindow::removeUser(const std::string& renderKey) {
    if (_canvas) _canvas->removeUser(renderKey);
    std::erase(_canvasKeys, renderKey);
}

void FloatingMemberListWindow::requestCpuRepaint() {
    if (!_gpu && _viewport && !_collapsed) {
        _viewport->update();
    }
}

void FloatingMemberListWindow::useCpu() {
    if (!_gpu && !_canvas) return;
    _gpu = false;
    _canvasKeys.clear();
    if (_canvas) {
        _canvas->setStageOverlay(nullptr);
        _canvas->shutdownRenderer();
        _canvas->hide();
    }
    if (_viewport) _viewport->update();
}

void FloatingMemberListWindow::clearFrames() {
    _canvasKeys.clear();
    if (_canvas) _canvas->clearUsers();
    if (_viewport) _viewport->update();
}

void FloatingMemberListWindow::placeBeside(const QWidget* hostWindow) {
    if (_restoredGeometry) {
        ensureOnScreen();
        return;
    }
    QScreen* target = hostWindow && hostWindow->screen()
        ? hostWindow->screen()
        : (screen() ? screen() : QGuiApplication::primaryScreen());
    if (!target) return;
    const QRect area = target->availableGeometry();
    const int targetW = std::min(kDefaultWindowWidth, std::max(kMinWindowWidth, area.width() / 4));
    const int hostHeight = hostWindow ? hostWindow->height() : kDefaultWindowHeight;
    const int targetH = std::clamp(hostHeight - 80, 320, std::min(kDefaultWindowHeight, area.height() - 32));
    _expandedHeight = std::max(kMinExpandedHeight, targetH);
    resize(targetW, _collapsed ? kCollapsedWindowHeight : _expandedHeight);

    int x = area.right() - width() - 16;
    int y = area.top() + 48;
    if (hostWindow) {
        const QRect hostFrame = hostWindow->frameGeometry();
        if (hostFrame.right() + 12 + width() <= area.right() + 1) {
            x = hostFrame.right() + 12;
        } else {
            x = std::min(area.right() - width() + 1, hostFrame.right() - width() - 16);
        }
        y = std::clamp(hostFrame.top() + 44, area.top(), std::max(area.top(), area.bottom() - height() + 1));
    }
    move(std::clamp(x, area.left(), std::max(area.left(), area.right() - width() + 1)),
         std::clamp(y, area.top(), std::max(area.top(), area.bottom() - height() + 1)));
    if (!_collapsed) _normalGeometry = geometry();
}

void FloatingMemberListWindow::ensureOnScreen() {
    QScreen* target = QGuiApplication::screenAt(frameGeometry().center());
    if (!target && _persistPreferences) {
        const QString hint = QSettings().value(QStringLiteral("floatingMemberList/screen")).toString();
        for (auto* candidate : QGuiApplication::screens()) {
            if (candidate->name() == hint) {
                target = candidate;
                break;
            }
        }
    }
    if (!target) target = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!target) return;
    const QRect area = target->availableGeometry();
    const int boundedW = std::clamp(width(), kMinWindowWidth, std::min(kMaxWindowWidth, area.width()));
    const int boundedH = _collapsed
        ? kCollapsedWindowHeight
        : std::clamp(height(), kMinExpandedHeight, std::max(kMinExpandedHeight, area.height()));
    if (size() != QSize(boundedW, boundedH)) {
        resize(boundedW, boundedH);
    }
    move(std::clamp(x(), area.left(), std::max(area.left(), area.right() - width() + 1)),
         std::clamp(y(), area.top(), std::max(area.top(), area.bottom() - height() + 1)));
}

bool FloatingMemberListWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _titleBar && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && isTitleDragArea(_titleBar->mapTo(this, mouse->pos()))) {
            if (windowHandle()) windowHandle()->startSystemMove();
            return true;
        }
    }
    if (watched == _titleBar && event->type() == QEvent::MouseButtonDblClick) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && isTitleDragArea(_titleBar->mapTo(this, mouse->pos()))) {
            setCollapsed(!_collapsed);
            return true;
        }
    }
    if (watched == _canvas && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton && _viewport) {
            const QPoint vpPos = _viewport->mapFromGlobal(mouse->globalPos());
            for (auto it = _slots.rbegin(); it != _slots.rend(); ++it) {
                if (it->rect.contains(vpPos)) {
                    emit tileMenuRequested(it->renderKey, mouse->globalPos());
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool FloatingMemberListWindow::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        updateChromeText();
    } else if (event->type() == QEvent::ScreenChangeInternal) {
        QTimer::singleShot(0, this, [this] {
            layoutCanvas();
            emit viewportChanged();
        });
    }
    return QWidget::event(event);
}

void FloatingMemberListWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateContentGeometry();
    updateChromeText();
    if (isVisible() && !isMinimized() && !_collapsed) {
        _expandedHeight = height();
        _normalGeometry = geometry();
    }
    emit viewportChanged();
}

void FloatingMemberListWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    if (isVisible() && !isMinimized()) {
        _normalGeometry = QRect(pos(), QSize(width(), _expandedHeight));
    }
}

void FloatingMemberListWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    setupNativeWindow();
    applyTopMostFlag();
    if (!_screenConnected && windowHandle()) {
        _screenConnected = true;
        connect(windowHandle(), &QWindow::screenChanged, this, [this] {
            layoutCanvas();
            emit viewportChanged();
        });
    }
    updateContentGeometry();
    layoutCanvas();
    emit viewportChanged();
}

void FloatingMemberListWindow::hideEvent(QHideEvent* event) {
    clearFrames();
    QWidget::hideEvent(event);
    emit viewportChanged();
}

void FloatingMemberListWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (!viewing()) clearFrames();
        layoutCanvas();
        emit viewportChanged();
    }
}

void FloatingMemberListWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit dockRequested();
}

void FloatingMemberListWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), AppTheme::color(AppTheme::ColorRole::Window));
    painter.setPen(QPen(AppTheme::color(AppTheme::ColorRole::Border), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void FloatingMemberListWindow::updateChromeText() {
    const QString baseTitle = roomText("Floating list");
    const QString fullTitle = _memberCount > 0
        ? QStringLiteral("%1 (%2)").arg(baseTitle).arg(_memberCount)
        : baseTitle;
    setWindowTitle(fullTitle);
    if (_titleLabel) {
        _titleLabel->setToolTip(fullTitle);
        const int available = std::max(40, _titleLabel->width());
        _titleLabel->setText(_titleLabel->fontMetrics().elidedText(fullTitle, Qt::ElideRight, available));
    }
    if (_onTopButton) {
        const QString tip = _onTop
            ? roomText("Cancel always on top")
            : roomText("Always on top");
        _onTopButton->setToolTip(tip);
        _onTopButton->setAccessibleName(tip);
    }
    if (_collapseButton) {
        const QString tip = _collapsed
            ? roomText("Expand member list")
            : roomText("Collapse member list");
        _collapseButton->setToolTip(tip);
        _collapseButton->setAccessibleName(tip);
    }
    if (_dockButton) {
        const QString tip = roomText("Dock to meeting window");
        _dockButton->setToolTip(tip);
        _dockButton->setAccessibleName(tip);
    }
}

void FloatingMemberListWindow::updateContentGeometry() {
    if (!_titleBar || !_viewport || !_pager) return;
    const int contentW = std::max(0, width() - 2 * kOuterMarginX);
    _titleBar->setGeometry(kOuterMarginX, kOuterMarginTop, contentW, kTitleBarHeight);
    if (_collapsed) {
        _viewport->hide();
        _pager->hide();
        if (_canvas) _canvas->hide();
        return;
    }

    const int contentTop = kOuterMarginTop + kTitleBarHeight;
    const int totalContentH = std::max(0, height() - contentTop - kOuterMarginBottom);
    const bool showPager = _pageCount > 1;
    const int pagerH = showPager ? std::min(kPagerBarHeight, totalContentH) : 0;
    const int viewportH = std::max(0, totalContentH - pagerH);

    _viewport->setGeometry(kOuterMarginX, contentTop, contentW, viewportH);
    _viewport->show();
    if (_emptyLabel) {
        const int pad = 16;
        _emptyLabel->setGeometry(pad, 0, std::max(1, contentW - 2 * pad), viewportH);
        _emptyLabel->setVisible(_slots.empty() && !_emptyHint.isEmpty());
    }
    _pager->setGeometry(kOuterMarginX, contentTop + viewportH, contentW, pagerH);
    _pager->setVisible(showPager && pagerH > 0);
    layoutCanvas();
}

void FloatingMemberListWindow::layoutCanvas() {
    if (!_gpu || !_canvas || !_viewport || _collapsed) return;
    _canvas->setGeometry(_viewport->rect());
    _canvas->show();
    _canvas->raise();
}

void FloatingMemberListWindow::applyTopMostFlag() {
#if defined(Q_OS_WIN)
    if (!isVisible()) return;
    const auto handle = reinterpret_cast<HWND>(winId());
    if (!handle) return;
    // Never call setWindowFlags on a live HWND hosting a DX11 child surface.
    SetWindowPos(handle, _onTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    if (windowFlags().testFlag(Qt::WindowStaysOnTopHint) != _onTop) {
        const bool wasVisible = isVisible();
        setWindowFlag(Qt::WindowStaysOnTopHint, _onTop);
        if (wasVisible) show();
    }
#endif
}

void FloatingMemberListWindow::refreshTheme() {
    if (_iconLabel) {
        _iconLabel->setPixmap(AppTheme::icon(AppTheme::Icon::LayoutFloating).pixmap(16, 16));
    }
    if (_onTopButton) {
        _onTopButton->setIcon(makePinIcon(_onTop));
    }
    if (_collapseButton) {
        _collapseButton->setIcon(AppTheme::icon(
            _collapsed ? AppTheme::Icon::ChevronDown : AppTheme::Icon::Collapse));
    }
    if (_dockButton) {
        _dockButton->setIcon(AppTheme::icon(AppTheme::Icon::Close));
    }
    update();
    if (_titleBar) _titleBar->update();
    if (_viewport) _viewport->update();
}

void FloatingMemberListWindow::savePreferences() {
    if (!_persistPreferences) return;
    QSettings settings;
    if (_normalGeometry.isValid()) {
        settings.setValue(QStringLiteral("floatingMemberList/geometry"), _normalGeometry);
    }
    if (screen()) {
        settings.setValue(QStringLiteral("floatingMemberList/screen"), screen()->name());
    }
    settings.setValue(QStringLiteral("floatingMemberList/onTop"), _onTop);
}

bool FloatingMemberListWindow::isTitleDragArea(const QPoint& windowPos) const {
    if (!_titleBar || !_titleBar->geometry().contains(windowPos)) return false;
    const QPoint local = _titleBar->mapFrom(this, windowPos);
    const QWidget* child = _titleBar->childAt(local);
    return !child || child == _titleLabel || child == _iconLabel;
}

QPoint FloatingMemberListWindow::menuAnchorGlobalPos(const QString& renderKey) const {
    if (!_viewport) return QCursor::pos();
    for (const auto& slot : _slots) {
        if (slot.renderKey == renderKey) {
            return _viewport->mapToGlobal(slot.rect.topLeft() + slot.pinRect.bottomLeft());
        }
    }
    return QCursor::pos();
}

void FloatingMemberListWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
    const auto handle = reinterpret_cast<HWND>(winId());
    if (!handle) return;
    if (!_nativeFilter && qApp) {
        qApp->installNativeEventFilter(this);
        _nativeFilter = true;
    }
    const auto oldStyle = GetWindowLongPtrW(handle, GWL_STYLE);
    const auto style = (oldStyle & ~LONG_PTR(WS_CAPTION | WS_MAXIMIZEBOX)) |
        WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    if (style != oldStyle) {
        SetWindowLongPtrW(handle, GWL_STYLE, style);
    }
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(handle, &margins);
    DWORD preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(handle, 33, &preference, sizeof(preference));
    if (style != oldStyle) {
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

int FloatingMemberListWindow::resizeHit(qintptr position) const {
#if defined(Q_OS_WIN)
    if (isMaximized() || isFullScreen()) return HTCLIENT;
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    RECT client{};
    if (!ScreenToClient(handle, &point) || !GetClientRect(handle, &client) ||
        !PtInRect(&client, point)) {
        return HTCLIENT;
    }
    const int edge = std::max(1, qRound(6 * devicePixelRatioF()));
    const bool left = point.x < edge;
    const bool right = point.x >= client.right - edge;
    if (_collapsed) {
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        return HTCLIENT;
    }
    const bool top = point.y < edge;
    const bool bottom = point.y >= client.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
#else
    Q_UNUSED(position);
#endif
    return 1; // HTCLIENT
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool FloatingMemberListWindow::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
#else
bool FloatingMemberListWindow::nativeEventFilter(const QByteArray&, void* message, long* result) {
#endif
#if defined(Q_OS_WIN)
    const auto* msg = static_cast<MSG*>(message);
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    if (msg && msg->message == WM_NCHITTEST && msg->hwnd != handle &&
        IsChild(handle, msg->hwnd) && resizeHit(msg->lParam) != HTCLIENT) {
        *result = HTTRANSPARENT;
        return true;
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool FloatingMemberListWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#else
bool FloatingMemberListWindow::nativeEvent(const QByteArray& type, void* message, long* result) {
#endif
#if defined(Q_OS_WIN)
    const auto* msg = static_cast<MSG*>(message);
    const auto handle = reinterpret_cast<HWND>(effectiveWinId());
    if (msg && msg->hwnd == handle && msg->message == WM_ERASEBKGND) {
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
        if ((style & WS_THICKFRAME) && !(style & WS_MAXIMIZE) && resizeBorder > 0) {
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
        const QPoint logical(
            qRound(point.x / devicePixelRatioF()),
            qRound(point.y / devicePixelRatioF()));
        if (isTitleDragArea(logical)) {
            *result = HTCAPTION;
        }
        return true;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}

} // namespace MeetingUI
