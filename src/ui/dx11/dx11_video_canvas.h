#pragma once
#include "ui/render/video_canvas.h"
#include "dx11_renderer.h"
#include "dx11_texture_pool.h"

namespace livekit::dx11 {
class Dx11VideoCanvas final : public render::VideoCanvas {
public:
    explicit Dx11VideoCanvas(QWidget* parent = nullptr);
    ~Dx11VideoCanvas() override;
    static bool IsHardwareBackendAllowed();
    bool rendererReady() const override;
    QString backendName() const override { return QStringLiteral("DX11"); }
    QPaintEngine* paintEngine() const override { return nullptr; }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void submitFrame(const std::string&, render::VideoRenderFrame::Ptr) override;
    void removeFrame(const std::string&) override;
    void clearBackend() override;
    void releaseRenderer() override { clearBackend(); renderer_.Cleanup(); }
    void removeDecorationTexture(const std::string&) override;
    render::VideoFrameGeometry frameGeometry(const std::string&) const override;
    bool beginFrame(QSize&) override;
    void drawSolid(const render::VideoTileRect&, float, float, float) override;
    bool drawVideo(const render::VideoTileRect&) override;
    bool drawDecoration(const render::VideoTileRect&, const QImage&) override;
    bool endFrame() override;
    const char* submitMeasurementPoint() const override {
        return "legacy_dx11_present";
    }
private:
    friend class ::ParticipantWindowTestAccess;
    bool EnsureRenderer();
    Dx11Renderer renderer_;
    Dx11TexturePool texture_pool_;
    struct DecorationTexture {
        QSize pixels;
        qint64 cacheKey = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::map<std::string, DecorationTexture> decoration_textures_;
};
} // namespace livekit::dx11
