#include "dx11_video_canvas.h"
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QCursor>
#include <QtCore/QThread>

#include <algorithm>
#include <cmath>
#include <utility>

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
    setMouseTracking(true);

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
    decorations_.clear();
    hovered_tile_.clear();
    pressed_pin_.clear();
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::setTilesLayout(const std::vector<TileRect>& tiles) {
    Q_ASSERT(QThread::currentThread() == thread());
    std::lock_guard<std::mutex> lock(layout_mutex_);
    tiles_ = tiles;
    for (auto it = decorations_.begin(); it != decorations_.end();) {
        const bool present = std::any_of(tiles.begin(), tiles.end(),
            [&](const auto& tile) { return tile.identity == it->first; });
        if (!present) it = decorations_.erase(it);
        else ++it;
    }
    // A layout change must not transfer an in-flight click to a new card.
    pressed_pin_.clear();
    hovered_tile_ = underMouse() ? hitTest(mapFromGlobal(QCursor::pos())) : std::string();
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::setTileDecoration(const std::string& identity,
        DecorationPainter painter, const QRect& pinRect) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& decoration = decorations_[identity];
    decoration.painter = std::move(painter);
    decoration.pinRect = pinRect;
    frame_dirty_.store(true, std::memory_order_release);
}

void Dx11VideoCanvas::updateTilePresentation(const std::string& identity, bool hasVideo) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto& tile : tiles_) {
        if (tile.identity == identity) tile.hasVideo = hasVideo;
    }
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
    // DXGI presents to physical HWND pixels. Match its backbuffer to that
    // surface on every draw, including hidden resize and DPI transitions;
    // otherwise DXGI can independently stretch the two logical dimensions.
    RECT client{};
    if (!GetClientRect(reinterpret_cast<HWND>(winId()), &client)) return;
    const int pixelWidth = client.right - client.left;
    const int pixelHeight = client.bottom - client.top;
    if (pixelWidth <= 0 || pixelHeight <= 0) return;
    if (!renderer_.Resize(pixelWidth, pixelHeight)) {
        NotifyRendererUnavailable();
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

    for (auto tile : tiles) {
        const auto pixelX = [&](int x) { return static_cast<int>(std::lround(x * double(pixelWidth) / width())); };
        const auto pixelY = [&](int y) { return static_cast<int>(std::lround(y * double(pixelHeight) / height())); };
        const int right = pixelX(tile.x + tile.width);
        const int bottom = pixelY(tile.y + tile.height);
        tile.x = pixelX(tile.x);
        tile.y = pixelY(tile.y);
        tile.width = right - tile.x;
        tile.height = bottom - tile.y;
        if (tile.width <= 0 || tile.height <= 0) continue;

        // Keep tile chrome in the same native surface: a speaking border and
        // black letterbox background never rely on a Qt child overlay.
        renderer_.SetViewport(tile.x, tile.y, tile.width, tile.height);
        if (tile.isSpeaking && !decorations_.count(tile.identity)) {
            renderer_.DrawSolidQuad(0.08f, 0.72f, 0.46f);
        } else {
            renderer_.DrawSolidQuad(0.16f, 0.18f, 0.23f);
        }

        const UserGpuResource* res = texture_pool_.GetUserResource(tile.identity);
        const bool hasFrame = res && res->srv_count > 0;
        if (tile.hasVideo && hasFrame) {
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
        if (!DrawDecoration(tile, hasFrame)) {
            NotifyRendererUnavailable();
            return;
        }
    }

    // 4. 提交呈现
    if (!renderer_.EndFrame(true)) {
        NotifyRendererUnavailable();
    }
}

bool Dx11VideoCanvas::DrawDecoration(const TileRect& tile, bool hasFrame) {
    auto it = decorations_.find(tile.identity);
    if (it == decorations_.end() || !it->second.painter) return true;
    auto& decoration = it->second;
    const auto image = decoration.painter(QSize(tile.width, tile.height), hasFrame,
        hovered_tile_ == tile.identity);
    if (image.isNull()) return true; // The guarded Qt card has been retired.
    if (!decoration.texture || decoration.pixels != image.size()) {
        decoration.srv.Reset();
        decoration.texture.Reset();
        decoration.cacheKey = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width();
        desc.Height = image.height();
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(renderer_.device()->CreateTexture2D(&desc, nullptr, decoration.texture.GetAddressOf())) ||
            FAILED(renderer_.device()->CreateShaderResourceView(decoration.texture.Get(), nullptr,
                decoration.srv.GetAddressOf()))) return false;
        decoration.pixels = image.size();
    }
    if (decoration.cacheKey != image.cacheKey()) {
        renderer_.context()->UpdateSubresource(decoration.texture.Get(), 0, nullptr,
            image.constBits(), image.bytesPerLine(), 0);
        decoration.cacheKey = image.cacheKey();
    }
    renderer_.SetViewport(tile.x, tile.y, tile.width, tile.height);
    renderer_.DrawPremultipliedOverlay(decoration.srv.Get());
    return true;
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

std::string Dx11VideoCanvas::hitTest(const QPoint& point, bool pinOnly) const {
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

void Dx11VideoCanvas::updateHovered(const QPoint& point) {
    const auto hovered = hitTest(point);
    if (hovered_tile_ != hovered) {
        hovered_tile_ = hovered;
        frame_dirty_.store(true, std::memory_order_release);
    }
    setCursor(hitTest(point, true).empty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void Dx11VideoCanvas::mouseMoveEvent(QMouseEvent* e) {
    updateHovered(e->pos());
    QWidget::mouseMoveEvent(e);
}

void Dx11VideoCanvas::mousePressEvent(QMouseEvent* e) {
    updateHovered(e->pos());
    pressed_pin_ = e->button() == Qt::LeftButton ? hitTest(e->pos(), true) : std::string();
    QWidget::mousePressEvent(e);
}

void Dx11VideoCanvas::mouseReleaseEvent(QMouseEvent* e) {
    const auto pressed = std::exchange(pressed_pin_, {});
    if (e->button() == Qt::LeftButton && !pressed.empty() && hitTest(e->pos(), true) == pressed) {
        emit tilePinRequested(QString::fromStdString(pressed));
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void Dx11VideoCanvas::leaveEvent(QEvent* e) {
    hovered_tile_.clear();
    frame_dirty_.store(true, std::memory_order_release);
    QWidget::leaveEvent(e);
}

} // namespace dx11
} // namespace livekit
