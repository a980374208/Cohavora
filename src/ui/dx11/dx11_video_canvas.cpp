#include "dx11_video_canvas.h"
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <windows.h>

namespace livekit::dx11 {
Dx11VideoCanvas::Dx11VideoCanvas(QWidget* parent) : render::VideoCanvas(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setUpdatesEnabled(true);
}
Dx11VideoCanvas::~Dx11VideoCanvas() {
    stopRendering();
    clearBackend();
    renderer_.Cleanup();
}
bool Dx11VideoCanvas::IsHardwareBackendAllowed() { return GetSystemMetrics(SM_REMOTESESSION) == 0; }
bool Dx11VideoCanvas::rendererReady() const { return renderer_.is_initialized(); }
void Dx11VideoCanvas::submitFrame(const std::string& key, render::VideoRenderFrame::Ptr frame) {
    texture_pool_.PostFrame(key, std::move(frame));
}
void Dx11VideoCanvas::removeFrame(const std::string& key) { texture_pool_.RemoveUser(key); }
void Dx11VideoCanvas::clearBackend() { texture_pool_.Clear(); decoration_textures_.clear(); }
void Dx11VideoCanvas::removeDecorationTexture(const std::string& key) { decoration_textures_.erase(key); }
render::VideoFrameGeometry Dx11VideoCanvas::frameGeometry(const std::string& key) const {
    const auto* res = texture_pool_.GetUserResource(key);
    return res ? render::VideoFrameGeometry{res->width, res->height, uint32_t(res->rotation), true}
               : render::VideoFrameGeometry{};
}
void Dx11VideoCanvas::showEvent(QShowEvent* e) { QWidget::showEvent(e); render(); }
void Dx11VideoCanvas::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); render(); }
void Dx11VideoCanvas::paintEvent(QPaintEvent*) { render(); }
bool Dx11VideoCanvas::beginFrame(QSize& physical) {
    if (!EnsureRenderer()) return false;
    RECT client{};
    if (!GetClientRect(reinterpret_cast<HWND>(winId()), &client)) return false;
    physical = QSize(client.right - client.left, client.bottom - client.top);
    if (physical.isEmpty()) return true; // Minimized/suspended, not device failure.
    if (!renderer_.Resize(physical.width(), physical.height())) return false;
    texture_pool_.UploadPendingFrames(renderer_.device(), renderer_.context());
    return renderer_.BeginFrame();
}
void Dx11VideoCanvas::drawSolid(const render::VideoTileRect& tile, float r, float g, float b) {
    renderer_.SetViewport(tile.x, tile.y, tile.width, tile.height);
    renderer_.DrawSolidQuad(r, g, b);
}
void Dx11VideoCanvas::drawVideo(const render::VideoTileRect& tile) {
    const auto* res = texture_pool_.GetUserResource(tile.identity);
    if (!res) return;
    renderer_.SetViewport(tile.x, tile.y, tile.width, tile.height);
    renderer_.SetRotation(res->rotation);
    if (res->format == PixelFormatType::I420 || res->format == PixelFormatType::NV12)
        renderer_.SetYuvColorSpace(res->color_space);
    ID3D11ShaderResourceView* srvs[3] = {res->srvs[0].Get(), res->srvs[1].Get(), res->srvs[2].Get()};
    renderer_.DrawQuad(res->format, srvs, res->srv_count);
}
bool Dx11VideoCanvas::drawDecoration(const render::VideoTileRect& tile, const QImage& image) {
    auto& decoration = decoration_textures_[tile.identity];
    if (!decoration.texture || decoration.pixels != image.size()) {
        decoration.srv.Reset(); decoration.texture.Reset(); decoration.cacheKey = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width(); desc.Height = image.height();
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
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
bool Dx11VideoCanvas::endFrame() { return renderer_.EndFrame(true); }
bool Dx11VideoCanvas::EnsureRenderer() {
    return renderer_.is_initialized() || (IsHardwareBackendAllowed() && width() > 0 && height() > 0 &&
        renderer_.Initialize(reinterpret_cast<HWND>(winId()), width(), height()));
}
} // namespace livekit::dx11
