#include "video_canvas.h"
#include "src/ui/native_child_geometry.h"
#include "src/ui/app_theme.h"
#include <QtGui/QMouseEvent>
#include <QtGui/QCursor>
#include <QtGui/QPainter>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolTip>
#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace livekit::render {
VideoCanvas::VideoCanvas(QWidget* parent)
    : QWidget(parent), diagnostics_(std::make_unique<RenderDiagnostics>()) {
    // Keep the stage/chrome in Qt's shared backing store. The native canvas
    // still gets its own HWND, positioned relative to the nearest native parent.
    new MeetingUI::NativeChildGeometry(*this);
    setMouseTracking(true);
    tool_tip_timer_ = new QTimer(this);
    tool_tip_timer_->setSingleShot(true);
    connect(tool_tip_timer_, &QTimer::timeout, this, &VideoCanvas::showTileToolTip);
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(16);
    connect(fps_timer_, &QTimer::timeout, this, [this] {
        if (frame_dirty_.exchange(false, std::memory_order_acq_rel)) requestRender();
    });
    fps_timer_->start();
    MeetingUI::AppTheme::observeChanges(*this, [this] {
        // A static or empty stage must repaint without waiting for a video frame.
        stage_dirty_ = true;
        frame_dirty_.store(true, std::memory_order_release);
        update();
    });
}
VideoCanvas::~VideoCanvas() {
    stopRendering();
    for (const auto& [_, frame] : telemetry_frames_) {
        frame->SetRenderExpected(false, RenderExpectationReason::BindingEnded);
    }
}
void VideoCanvas::stopRendering() {
    fps_timer_->stop();
    cancelTileToolTip();
}
void VideoCanvas::shutdownRenderer() {
    Q_ASSERT(QThread::currentThread() == thread());
    stopRendering();
    renderer_unavailable_emitted_ = true;
    clearUsers();
    releaseRenderer();
    if (diagnostics_->fallback_reason == RenderFallbackReason::None &&
        diagnostics_->actual_backend != RenderBackend::None) {
        diagnostics_->actual_backend = RenderBackend::None;
    }
}
void VideoCanvas::updateFrame(const std::string& key, VideoRenderFrame::Ptr frame) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!frame || key.empty() || renderer_unavailable_emitted_) return;
    const auto previous = telemetry_frames_.find(key);
    if (previous != telemetry_frames_.end() && previous->second != frame &&
        !previous->second->renderMetadata().same_binding_as(
            frame->renderMetadata())) {
        previous->second->SetRenderExpected(
            false, RenderExpectationReason::BindingEnded);
    }
    telemetry_frames_[key] = frame;
    submitFrame(key, std::move(frame));
    updateRenderExpectations();
    frame_dirty_.store(true, std::memory_order_release);
}
void VideoCanvas::setVideoDestination(const std::string& key, const QRectF& rect) {
    if (video_destinations_[key] == rect) return;
    video_destinations_[key] = rect;
    frame_dirty_ = true;
}
void VideoCanvas::removeUser(const std::string& key) {
    video_destinations_.erase(key);
    Q_ASSERT(QThread::currentThread() == thread());
    if (const auto frame = telemetry_frames_.find(key);
        frame != telemetry_frames_.end()) {
        frame->second->SetRenderExpected(
            false, RenderExpectationReason::BindingEnded);
        telemetry_frames_.erase(frame);
    }
    removeFrame(key);
    removeDecorationTexture(key);
    decorations_.erase(key);
    if (hovered_tile_ == key) {
        hovered_tile_.clear();
        updateTileToolTip();
    }
    frame_dirty_.store(true, std::memory_order_release);
}
void VideoCanvas::clearUsers() {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelTileToolTip();
    for (const auto& [_, frame] : telemetry_frames_) {
        frame->SetRenderExpected(false, RenderExpectationReason::BindingEnded);
    }
    telemetry_frames_.clear();
    video_destinations_.clear();
    clearBackend();
    decorations_.clear(); hovered_tile_.clear(); pressed_pin_.clear();
    updateTileToolTip();
    frame_dirty_.store(true, std::memory_order_release);
}
void VideoCanvas::notifyRendererUnavailable() {
    if (renderer_unavailable_emitted_) return;
    if (diagnostics_->gpu_failure == RenderGpuFailure::None) {
        markRendererFailure(RenderGpuFailure::UnknownRendererFailure,
            RenderFallbackReason::GpuRuntimeFailed);
    }
    renderer_unavailable_emitted_ = true;
    updateRenderExpectations();
    stopRendering();
    emit rendererUnavailable();
}
void VideoCanvas::initializeModuleDiagnostics(RenderBackend backend, uint32_t abiVersion) {
    diagnostics_->requested_backend = backend;
    diagnostics_->actual_backend = RenderBackend::QtCpu;
    diagnostics_->abi_version = abiVersion;
}
void VideoCanvas::markRendererActive(QString driverDescription, uint32_t moduleVersion) {
    auto next = *diagnostics_;
    next.actual_backend = next.requested_backend;
    next.module_version = moduleVersion;
    next.driver_description = SanitizeRenderDriverDescription(std::move(driverDescription));
    next.gpu_failure = RenderGpuFailure::None;
    next.fallback_reason = RenderFallbackReason::None;
    if (next == *diagnostics_) return;
    *diagnostics_ = std::move(next);
}
void VideoCanvas::markRendererFailure(RenderGpuFailure failure, RenderFallbackReason fallback) {
    auto next = *diagnostics_;
    next.actual_backend = RenderBackend::QtCpu;
    next.gpu_failure = failure;
    next.fallback_reason = fallback;
    if (next == *diagnostics_) return;
    *diagnostics_ = std::move(next);
}
std::array<float, 4> VideoCanvas::stageClearColor() const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto color = MeetingUI::AppTheme::color(MeetingUI::AppTheme::ColorRole::Stage);
    return {float(color.redF()), float(color.greenF()), float(color.blueF()), 1.0f};
}
void VideoCanvas::render() {
    Q_ASSERT(QThread::currentThread() == thread());
    updateRenderExpectations();
    if (renderer_unavailable_emitted_ || !isVisible() || width() <= 0 || height() <= 0) return;
    QSize physical;
    if (!beginFrame(physical)) { notifyRendererUnavailable(); return; }
    if (physical.isEmpty()) return;
    last_physical_size_ = physical;
    using Role = MeetingUI::AppTheme::ColorRole;
    const auto tileColor = MeetingUI::AppTheme::color(Role::Tile);
    const auto letterboxColor = MeetingUI::AppTheme::color(Role::Letterbox);
    const auto speakingColor = MeetingUI::AppTheme::color(Role::Success);
    std::vector<VideoRenderFrame::Ptr> submitted_frames;
    for (const auto& logical : tiles_) {
        auto tile = ScaleVideoTile(logical, width(), height(), physical.width(), physical.height());
        if (tile.width <= 0 || tile.height <= 0 ||
            (tile.clip && (tile.clip->width <= 0 || tile.clip->height <= 0))) continue;
        const auto background = tile.isSpeaking && !decorations_.count(tile.identity)
            ? speakingColor : tileColor;
        drawSolid(tile, float(background.redF()), float(background.greenF()), float(background.blueF()));
        const auto geometry = frameGeometry(tile.identity);
        if (tile.hasVideo && geometry.available) {
            auto content = tile;
            if (!video_destinations_.count(tile.identity)) {
                content.x += 2; content.y += 2;
                content.width = std::max(1, content.width - 4);
                content.height = std::max(1, content.height - 4);
            }
            drawSolid(content, float(letterboxColor.redF()), float(letterboxColor.greenF()),
                float(letterboxColor.blueF()));
            auto destination = FitVideoTile(content, geometry);
            if (const auto found = video_destinations_.find(tile.identity); found != video_destinations_.end()) {
                const auto& rect = found->second;
                // Round only in physical pixels: at 200% DPI an odd source
                // width must not become one logical pixel (two pixels) wider.
                const double sx = double(physical.width()) / width();
                const double sy = double(physical.height()) / height();
                destination = {tile.identity, qRound(rect.x()*sx), qRound(rect.y()*sy),
                    qRound(rect.width()*sx), qRound(rect.height()*sy), false, 0, true};
            }
            if (drawVideo(destination)) {
                if (const auto frame = telemetry_frames_.find(tile.identity);
                    frame != telemetry_frames_.end() && frame->second) {
                    submitted_frames.push_back(frame->second);
                }
            }
        }
        auto decoration = decorations_.find(tile.identity);
        if (decoration != decorations_.end() && decoration->second.painter) {
            const auto image = decoration->second.painter(QSize(tile.width, tile.height),
                geometry.available, hovered_tile_ == tile.identity);
            if (!image.isNull()) {
                if (!drawDecoration(tile, image)) { notifyRendererUnavailable(); return; }
                decoration->second.cacheKey = image.cacheKey();
            }
        }
    }
    if (stage_overlay_ && stage_overlay_->isVisible() && !stage_overlay_->size().isEmpty()) {
        const qreal dpr = qreal(physical.width()) / width();
        const QSize size(qRound(stage_overlay_->width()*dpr), qRound(stage_overlay_->height()*dpr));
        if (stage_dirty_ || stage_image_.size() != size) {
            stage_image_ = QImage(size, QImage::Format_RGBA8888_Premultiplied);
            stage_image_.setDevicePixelRatio(dpr); stage_image_.fill(Qt::transparent);
            painting_stage_ = true;
            stage_overlay_->render(&stage_image_, QPoint(), QRegion(), QWidget::DrawChildren);
            painting_stage_ = false; stage_dirty_ = false;
        }
        const auto pos = mapFromGlobal(stage_overlay_->mapToGlobal(QPoint()));
        VideoTileRect logical{"\x01stage-overlay", pos.x(), pos.y(), stage_overlay_->width(), stage_overlay_->height()};
        const auto tile = ScaleVideoTile(logical, width(), height(), physical.width(), physical.height());
        if (!drawDecoration(tile, stage_image_)) { notifyRendererUnavailable(); return; }
    }
    if (!endFrame()) {
        notifyRendererUnavailable();
        return;
    }
    if (!reportsSubmitAsynchronously()) {
        const auto submitted_at = std::chrono::steady_clock::now();
        for (const auto& frame : submitted_frames) {
            frame->NotifyRendered(submitMeasurementPoint(), submitted_at);
        }
    }
}
void VideoCanvas::setStageOverlay(QWidget* widget) {
    if (stage_overlay_) stage_overlay_->removeEventFilter(this);
    stage_overlay_ = widget;
    if (widget) widget->installEventFilter(this);
    stage_dirty_ = true; frame_dirty_.store(true, std::memory_order_release);
}
bool VideoCanvas::eventFilter(QObject* object, QEvent* event) {
    if (object == stage_overlay_ && !painting_stage_) {
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
            event->type() == QEvent::Move || event->type() == QEvent::Resize) cancelTileToolTip();
        switch (event->type()) {
        case QEvent::Show: case QEvent::Hide: case QEvent::Move: case QEvent::Resize:
        case QEvent::UpdateRequest: case QEvent::Paint: case QEvent::StyleChange:
            stage_dirty_ = true; frame_dirty_.store(true, std::memory_order_release); break;
        default: break;
        }
    }
    return QWidget::eventFilter(object, event);
}

bool VideoCanvas::event(QEvent* event) {
    // A GPU surface forwards mouse events without making this QWidget Qt's
    // natural tooltip target. Both surface variants use the owner timer below.
    if (event->type() == QEvent::ToolTip) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::ParentChange ||
        event->type() == QEvent::WindowStateChange || event->type() == QEvent::Wheel ||
        event->type() == QEvent::KeyPress) cancelTileToolTip();
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::Hide) {
        hovered_tile_.clear();
        updateTileToolTip();
    }
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowStateChange:
    case QEvent::ParentChange:
        updateRenderExpectations();
        break;
    default:
        break;
    }
    return result;
}

void VideoCanvas::updateRenderExpectations() {
    const bool minimized = window() && window()->isMinimized();
    const bool surface_visible = isVisible() && !minimized &&
        width() > 0 && height() > 0 && !renderer_unavailable_emitted_;
    std::set<std::string> visible_keys;
    if (surface_visible) {
        for (const auto& tile : tiles_) {
            auto visible = QRect(tile.x, tile.y, tile.width, tile.height).intersected(rect());
            if (tile.clip) visible = visible.intersected(
                QRect(tile.clip->x, tile.clip->y, tile.clip->width, tile.clip->height));
            if (tile.hasVideo && !visible.isEmpty()) {
                visible_keys.insert(tile.identity);
            }
        }
    }
    for (const auto& [key, frame] : telemetry_frames_) {
        const bool expected = visible_keys.contains(key);
        frame->SetRenderExpected(
            expected,
            expected ? RenderExpectationReason::SurfaceVisible
                     : minimized ? RenderExpectationReason::WindowMinimized
                                 : RenderExpectationReason::SurfaceHidden);
    }
}
void VideoCanvas::setTilesLayout(const std::vector<VideoTileRect>& tiles) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool trackingPointer = underMouse() || !hovered_tile_.empty();
    const auto hoverBounds = [](const auto& layout, const std::string& key) {
        const auto found = std::find_if(layout.begin(), layout.end(),
            [&](const auto& tile) { return tile.identity == key; });
        if (found == layout.end()) return std::pair<QRect, std::optional<VideoClipRect>>{};
        return std::pair{QRect(found->x, found->y, found->width, found->height), found->clip};
    };
    const auto previousHover = hovered_tile_;
    const auto previousBounds = hoverBounds(tiles_, previousHover);
    tiles_ = tiles;
    for (auto it = decorations_.begin(); it != decorations_.end();) {
        const bool present = std::any_of(tiles.begin(), tiles.end(),
            [&](const auto& tile) { return tile.identity == it->first; });
        if (!present) { removeDecorationTexture(it->first); it = decorations_.erase(it); }
        else ++it;
    }
    // A layout change must not transfer an in-flight click to a new card.
    pressed_pin_.clear();
    hovered_tile_ = trackingPointer ? hitTest(mapFromGlobal(QCursor::pos())) : std::string();
    if (previousHover != hovered_tile_ || previousBounds != hoverBounds(tiles_, hovered_tile_))
        cancelTileToolTip();
    updateTileToolTip();
    // Presentation-only refreshes must not starve a stationary pointer's
    // pending tooltip or dismiss the current person's unchanged name.
    if (trackingPointer && !tool_tip_timer_->isActive() && visible_tool_tip_.isEmpty())
        scheduleTileToolTip(mapFromGlobal(QCursor::pos()));
    frame_dirty_.store(true, std::memory_order_release);
    updateRenderExpectations();
}

void VideoCanvas::setTileDecoration(const std::string& identity,
        DecorationPainter painter, const QRect& pinRect, ToolTipProvider toolTip) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& decoration = decorations_[identity];
    decoration.painter = std::move(painter);
    decoration.toolTip = std::move(toolTip);
    decoration.pinRect = pinRect;
    if (hovered_tile_ == identity) updateTileToolTip();
    frame_dirty_.store(true, std::memory_order_release);
}

void VideoCanvas::updateTilePresentation(const std::string& identity, bool hasVideo) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto& tile : tiles_) {
        if (tile.identity == identity) tile.hasVideo = hasVideo;
    }
    if (hovered_tile_ == identity) updateTileToolTip();
    frame_dirty_.store(true, std::memory_order_release);
}

void VideoCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    cancelTileToolTip();
    const auto hit = hitTest(e->pos());
    if (e->button() == Qt::LeftButton && !hit.empty()) {
        pressed_pin_ = hitTest(e->pos(), true);
        // Rapidly reopening More must still activate on release; only the
        // video body interprets a double click as a layout lock toggle.
        if (pressed_pin_.empty()) emit tileDoubleClicked(QString::fromStdString(hit));
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

std::string VideoCanvas::hitTest(const QPoint& point, bool pinOnly) const {
    // All layout, metadata and input operations belong to the UI thread.
    // Draw back-to-front; hit-test front-to-back, in logical Qt coordinates.
    if (!rect().contains(point)) return {};
    for (auto it = tiles_.rbegin(); it != tiles_.rend(); ++it) {
        if (!QRect(it->x, it->y, it->width, it->height).contains(point)) continue;
        if (it->clip && !QRect(it->clip->x, it->clip->y,
            it->clip->width, it->clip->height).contains(point)) continue;
        if (!pinOnly) return it->identity;
        const auto decoration = decorations_.find(it->identity);
        if (decoration != decorations_.end() &&
            decoration->second.pinRect.contains(point - QPoint(it->x, it->y))) return it->identity;
        return {}; // An upper card also occludes the controls of a lower card.
    }
    return {};
}

void VideoCanvas::updateHovered(const QPoint& point) {
    const auto hovered = hitTest(point);
    if (hovered_tile_ != hovered) {
        hovered_tile_ = hovered;
        frame_dirty_.store(true, std::memory_order_release);
    }
    updateTileToolTip();
    setCursor(hitTest(point, true).empty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void VideoCanvas::updateTileToolTip() {
    const auto found = decorations_.find(hovered_tile_);
    auto next = hovered_tile_ != remote_input_key_ && found != decorations_.end() && found->second.toolTip
        ? found->second.toolTip() : QString();
    if (next == QStringLiteral("<qt></qt>")) next.clear();
    if (next == toolTip()) return;
    cancelTileToolTip();
    setToolTip(next);
    if (!next.isEmpty()) scheduleTileToolTip(mapFromGlobal(QCursor::pos()));
}

void VideoCanvas::cancelTileToolTip() {
    if (tool_tip_timer_) tool_tip_timer_->stop();
    tool_tip_key_.clear();
    if ((!visible_tool_tip_.isEmpty() && QToolTip::text() == visible_tool_tip_) ||
        (!toolTip().isEmpty() && QToolTip::text() == toolTip())) QToolTip::hideText();
    visible_tool_tip_.clear();
}

bool VideoCanvas::canShowTileToolTip(const QPoint& point) const {
    if (!isVisible() || !isEnabled() || !window() || !window()->isActiveWindow() ||
        window()->isMinimized() || renderer_unavailable_emitted_ || toolTip().isEmpty() ||
        hovered_tile_.empty() || hovered_tile_ == remote_input_key_ ||
        !rect().contains(point) || hitTest(point) != hovered_tile_ ||
        QApplication::mouseButtons() != Qt::NoButton || QApplication::activePopupWidget()) return false;
    return !stage_overlay_ || !stage_overlay_->isVisible() ||
        !stage_overlay_->rect().contains(stage_overlay_->mapFromGlobal(mapToGlobal(point)));
}

void VideoCanvas::scheduleTileToolTip(const QPoint& point) {
    if (!canShowTileToolTip(point)) {
        cancelTileToolTip();
        return;
    }
    if (!visible_tool_tip_.isEmpty() && visible_tool_tip_ == toolTip() && QToolTip::isVisible()) return;
    cancelTileToolTip();
    tool_tip_key_ = hovered_tile_;
    const int delay = style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay, nullptr, this);
    tool_tip_timer_->start(delay >= 0 ? delay : 700);
}

void VideoCanvas::showTileToolTip() {
    const auto key = std::exchange(tool_tip_key_, {});
    const auto point = mapFromGlobal(QCursor::pos());
    if (key.empty() || key != hovered_tile_ || !canShowTileToolTip(point)) {
        cancelTileToolTip();
        return;
    }
    const auto found = decorations_.find(key);
    const auto text = found != decorations_.end() && found->second.toolTip
        ? found->second.toolTip() : QString();
    if (text != toolTip()) {
        updateTileToolTip();
        return;
    }
    const auto tile = std::find_if(tiles_.begin(), tiles_.end(),
        [&](const auto& value) { return value.identity == key; });
    if (tile == tiles_.end()) return;
    visible_tool_tip_ = text;
    auto visible = QRect(tile->x, tile->y, tile->width, tile->height).intersected(rect());
    if (tile->clip) visible = visible.intersected(
        QRect(tile->clip->x, tile->clip->y, tile->clip->width, tile->clip->height));
    QToolTip::showText(QCursor::pos(), text, this, visible);
}

void VideoCanvas::setRemoteInputKey(std::string key) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (remote_input_key_ == key) return;
    cancelTileToolTip();
    remote_input_key_ = std::move(key);
    updateTileToolTip();
    frame_dirty_.store(true, std::memory_order_release);
}

QRectF VideoCanvas::videoContentRect(const std::string& key) const {
    if (width() <= 0 || height() <= 0 || last_physical_size_.isEmpty()) return {};
    const auto frame = frameGeometry(key);
    if (!frame.available || frame.rotation != 0) return {};
    for (const auto& logical : tiles_) if (logical.identity == key && logical.hasVideo) {
        auto content = ScaleVideoTile(logical,width(),height(),last_physical_size_.width(),last_physical_size_.height());
        content.x += 2; content.y += 2; content.width -= 4; content.height -= 4;
        if (content.width <= 0 || content.height <= 0) return {};
        content = FitVideoTile(content,frame);
        const double sx = double(width())/last_physical_size_.width();
        const double sy = double(height())/last_physical_size_.height();
        return {content.x*sx,content.y*sy,content.width*sx,content.height*sy};
    }
    return {};
}

void VideoCanvas::mouseMoveEvent(QMouseEvent* e) {
    updateHovered(e->pos());
    scheduleTileToolTip(e->pos());
    QWidget::mouseMoveEvent(e);
}

void VideoCanvas::mousePressEvent(QMouseEvent* e) {
    updateHovered(e->pos());
    cancelTileToolTip();
    pressed_pin_ = e->button() == Qt::LeftButton ? hitTest(e->pos(), true) : std::string();
    QWidget::mousePressEvent(e);
}

void VideoCanvas::mouseReleaseEvent(QMouseEvent* e) {
    const auto pressed = std::exchange(pressed_pin_, {});
    if (e->button() == Qt::LeftButton && !pressed.empty() && hitTest(e->pos(), true) == pressed) {
        emit tilePinRequested(QString::fromStdString(pressed));
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void VideoCanvas::leaveEvent(QEvent* e) {
    cancelTileToolTip();
    hovered_tile_.clear();
    updateTileToolTip();
    frame_dirty_.store(true, std::memory_order_release);
    QWidget::leaveEvent(e);
}

} // namespace livekit::render
