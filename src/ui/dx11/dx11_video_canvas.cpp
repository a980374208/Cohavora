#include "dx11_video_canvas.h"
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace livekit {
namespace dx11 {

Dx11VideoCanvas::Dx11VideoCanvas(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setUpdatesEnabled(true);

    // The UI thread owns the D3D immediate context. Producers only replace a
    // mailbox entry; this timer performs upload, draw and Present at most once
    // per tick without any cross-thread D3D call.
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(16);
    connect(fps_timer_, &QTimer::timeout, this, [this]() {
        if (frame_dirty_.exchange(false, std::memory_order_acq_rel)) {
            render();
        }
    });
    fps_timer_->start();
}

Dx11VideoCanvas::~Dx11VideoCanvas() {
    if (fps_timer_) {
        fps_timer_->stop();
    }
    texture_pool_.Clear();
    renderer_.Cleanup();
}

void Dx11VideoCanvas::updateFrame(const std::string& identity, const livekit::VideoFrame& frame) {
    texture_pool_.PostUserFrame(identity, frame);
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::updateI420Frame(const std::string& identity, render::OwnedI420Frame::Ptr frame) {
    texture_pool_.PostI420Frame(identity, std::move(frame));
    frame_dirty_.store(true, std::memory_order_release);
}

bool Dx11VideoCanvas::IsHardwareBackendAllowed() {
#if defined(Q_OS_WIN)
    return GetSystemMetrics(SM_REMOTESESSION) == 0;
#else
    return false;
#endif
}

bool Dx11VideoCanvas::rendererReady() const {
    return renderer_.is_initialized();
}

void Dx11VideoCanvas::removeUser(const std::string& identity) {
    texture_pool_.RemoveUser(identity);
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::clearUsers() {
    texture_pool_.Clear();
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::setTilesLayout(const std::vector<TileRect>& tiles) {
    std::lock_guard<std::mutex> lock(layout_mutex_);
    tiles_ = tiles;
    frame_dirty_.store(true, std::memory_order_release);
}

bool Dx11VideoCanvas::hasVideo(const std::string& identity) const {
    return texture_pool_.HasUserVideo(identity);
}

void Dx11VideoCanvas::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    render();
}

void Dx11VideoCanvas::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (renderer_.is_initialized()) {
        if (!renderer_.Resize(width(), height())) {
            NotifyRendererUnavailable();
            return;
        }
    }
    render();
}

void Dx11VideoCanvas::paintEvent(QPaintEvent* e) {
    Q_UNUSED(e);
    render();
}

void Dx11VideoCanvas::render() {
    if (!isVisible() || width() <= 0 || height() <= 0) return;

    if (!EnsureRenderer()) {
        return;
    }

    // 1. 上传所有用户的待处理帧至显存
    texture_pool_.UploadPendingFrames(renderer_.device(), renderer_.context());

    // 2. 清屏 (#12141a) 并准备渲染上下文
    if (!renderer_.BeginFrame()) {
        NotifyRendererUnavailable();
        return;
    }

    // 3. 按照网格排布批量绘制活跃用户的 Quad
    std::vector<TileRect> tiles;
    {
        std::lock_guard<std::mutex> lock(layout_mutex_);
        tiles = tiles_;
    }

    for (const auto& tile : tiles) {
        if (tile.width <= 0 || tile.height <= 0) continue;

        // Keep tile chrome in the same native surface: a speaking border and
        // black letterbox background never rely on a Qt child overlay.
        renderer_.SetViewport(tile.x, tile.y, tile.width, tile.height);
        if (tile.isSpeaking) {
            renderer_.DrawSolidQuad(0.08f, 0.72f, 0.46f);
        } else {
            renderer_.DrawSolidQuad(0.16f, 0.18f, 0.23f);
        }

        if (!tile.hasVideo) continue;

        const UserGpuResource* res = texture_pool_.GetUserResource(tile.identity);
        if (res && res->srv_count > 0) {
            TileRect content = tile;
            content.x += 2;
            content.y += 2;
            content.width = std::max(1, content.width - 4);
            content.height = std::max(1, content.height - 4);
            renderer_.SetViewport(content.x, content.y, content.width, content.height);
            renderer_.DrawSolidQuad(0.0f, 0.0f, 0.0f);

            const TileRect fitted = FitTileToFrame(content, *res);
            renderer_.SetViewport(fitted.x, fitted.y, fitted.width, fitted.height);
            renderer_.SetRotation(res->rotation);
            if (res->format == PixelFormatType::I420 || res->format == PixelFormatType::NV12) {
                renderer_.SetYuvColorSpace(res->color_space);
            }

            ID3D11ShaderResourceView* srvs[3] = {
                res->srvs[0].Get(),
                res->srvs[1].Get(),
                res->srvs[2].Get()
            };
            renderer_.DrawQuad(res->format, srvs, res->srv_count);
        }
    }

    // 4. 提交呈现
    if (!renderer_.EndFrame(true)) {
        NotifyRendererUnavailable();
    }
}

bool Dx11VideoCanvas::EnsureRenderer() {
    if (renderer_.is_initialized()) {
        return true;
    }
    if (!IsHardwareBackendAllowed() || width() <= 0 || height() <= 0 ||
        !renderer_.Initialize(reinterpret_cast<HWND>(winId()), width(), height())) {
        NotifyRendererUnavailable();
        return false;
    }
    return true;
}

void Dx11VideoCanvas::NotifyRendererUnavailable() {
    if (renderer_unavailable_emitted_) {
        return;
    }
    renderer_unavailable_emitted_ = true;
    emit rendererUnavailable();
}

TileRect Dx11VideoCanvas::FitTileToFrame(const TileRect& tile, const UserGpuResource& resource) const {
    int source_width = resource.width;
    int source_height = resource.height;
    if (resource.rotation == VideoRotation::VIDEO_ROTATION_90 ||
        resource.rotation == VideoRotation::VIDEO_ROTATION_270) {
        std::swap(source_width, source_height);
    }
    if (source_width <= 0 || source_height <= 0) {
        return tile;
    }

    const double scale = std::min(static_cast<double>(tile.width) / source_width,
                                  static_cast<double>(tile.height) / source_height);
    const int width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(source_height * scale)));

    TileRect fitted = tile;
    fitted.x += (tile.width - width) / 2;
    fitted.y += (tile.height - height) / 2;
    fitted.width = width;
    fitted.height = height;
    return fitted;
}

void Dx11VideoCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    std::vector<TileRect> tiles;
    {
        std::lock_guard<std::mutex> lock(layout_mutex_);
        tiles = tiles_;
    }
    for (const auto& tile : tiles) {
        if (QRect(tile.x, tile.y, tile.width, tile.height).contains(e->pos())) {
            emit tileDoubleClicked(QString::fromStdString(tile.identity));
            e->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(e);
}

} // namespace dx11
} // namespace livekit
