#include "video_canvas.h"
#include <QtGui/QMouseEvent>
#include <QtGui/QCursor>
#include <QtGui/QPainter>
#include <QtCore/QThread>
#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace livekit::render {
VideoCanvas::VideoCanvas(QWidget* parent)
    : QWidget(parent), diagnostics_(std::make_unique<RenderDiagnostics>()) {
    setMouseTracking(true);
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(16);
    connect(fps_timer_, &QTimer::timeout, this, [this] {
        if (frame_dirty_.exchange(false, std::memory_order_acq_rel)) requestRender();
    });
    fps_timer_->start();
}
VideoCanvas::~VideoCanvas() {
    stopRendering();
    for (const auto& [_, frame] : telemetry_frames_) {
        frame->SetRenderExpected(false, RenderExpectationReason::BindingEnded);
    }
}
void VideoCanvas::stopRendering() { fps_timer_->stop(); }
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
void VideoCanvas::removeUser(const std::string& key) {
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
    frame_dirty_.store(true, std::memory_order_release);
}
void VideoCanvas::clearUsers() {
    Q_ASSERT(QThread::currentThread() == thread());
    for (const auto& [_, frame] : telemetry_frames_) {
        frame->SetRenderExpected(false, RenderExpectationReason::BindingEnded);
    }
    telemetry_frames_.clear();
    clearBackend();
    decorations_.clear(); hovered_tile_.clear(); pressed_pin_.clear();
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
void VideoCanvas::render() {
    Q_ASSERT(QThread::currentThread() == thread());
    updateRenderExpectations();
    if (renderer_unavailable_emitted_ || !isVisible() || width() <= 0 || height() <= 0) return;
    QSize physical;
    if (!beginFrame(physical)) { notifyRendererUnavailable(); return; }
    if (physical.isEmpty()) return;
    std::vector<VideoRenderFrame::Ptr> submitted_frames;
    for (const auto& logical : tiles_) {
        auto tile = ScaleVideoTile(logical, width(), height(), physical.width(), physical.height());
        if (tile.width <= 0 || tile.height <= 0) continue;
        if (tile.isSpeaking && !decorations_.count(tile.identity)) drawSolid(tile, 0.08f, 0.72f, 0.46f);
        else drawSolid(tile, 0.16f, 0.18f, 0.23f);
        const auto geometry = frameGeometry(tile.identity);
        if (tile.hasVideo && geometry.available) {
            auto content = tile;
            content.x += 2; content.y += 2;
            content.width = std::max(1, content.width - 4);
            content.height = std::max(1, content.height - 4);
            drawSolid(content, 0, 0, 0);
            if (drawVideo(FitVideoTile(content, geometry))) {
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
    const bool result = QWidget::event(event);
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
            if (tile.hasVideo && tile.width > 0 && tile.height > 0) {
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
    tiles_ = tiles;
    for (auto it = decorations_.begin(); it != decorations_.end();) {
        const bool present = std::any_of(tiles.begin(), tiles.end(),
            [&](const auto& tile) { return tile.identity == it->first; });
        if (!present) { removeDecorationTexture(it->first); it = decorations_.erase(it); }
        else ++it;
    }
    // A layout change must not transfer an in-flight click to a new card.
    pressed_pin_.clear();
    hovered_tile_ = underMouse() ? hitTest(mapFromGlobal(QCursor::pos())) : std::string();
    frame_dirty_.store(true, std::memory_order_release);
    updateRenderExpectations();
}

void VideoCanvas::setTileDecoration(const std::string& identity,
        DecorationPainter painter, const QRect& pinRect) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& decoration = decorations_[identity];
    decoration.painter = std::move(painter);
    decoration.pinRect = pinRect;
    frame_dirty_.store(true, std::memory_order_release);
}

void VideoCanvas::updateTilePresentation(const std::string& identity, bool hasVideo) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto& tile : tiles_) {
        if (tile.identity == identity) tile.hasVideo = hasVideo;
    }
    frame_dirty_.store(true, std::memory_order_release);
}

void VideoCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    const auto hit = hitTest(e->pos());
    if (e->button() == Qt::LeftButton && !hit.empty()) {
        pressed_pin_.clear();
        // The first click already activated the Pin button.
        if (hitTest(e->pos(), true).empty()) emit tileDoubleClicked(QString::fromStdString(hit));
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

std::string VideoCanvas::hitTest(const QPoint& point, bool pinOnly) const {
    // All layout, metadata and input operations belong to the UI thread.
    // Draw back-to-front; hit-test front-to-back, in logical Qt coordinates.
    for (auto it = tiles_.rbegin(); it != tiles_.rend(); ++it) {
        if (!QRect(it->x, it->y, it->width, it->height).contains(point)) continue;
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
    setCursor(hitTest(point, true).empty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void VideoCanvas::mouseMoveEvent(QMouseEvent* e) {
    updateHovered(e->pos());
    QWidget::mouseMoveEvent(e);
}

void VideoCanvas::mousePressEvent(QMouseEvent* e) {
    updateHovered(e->pos());
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
    hovered_tile_.clear();
    frame_dirty_.store(true, std::memory_order_release);
    QWidget::leaveEvent(e);
}

} // namespace livekit::render
