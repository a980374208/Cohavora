#pragma once
#include "video_canvas.h"
#include "render/backend_module.h"

namespace livekit::render {
// UI scene adapter. DX11 owns a separate native surface and render thread;
// no BackendDevice or driver resource is owned by the widget.
class ModuleVideoCanvas : public VideoCanvas {
public:
    ModuleVideoCanvas(std::shared_ptr<BackendModule>, QWidget* parent = nullptr);
    ~ModuleVideoCanvas() override;
    bool rendererReady() const override { return initialized_ && !failed_; }
    bool rendererPending() const override { return !initialized_ && !failed_; }
    QString backendName() const override;
    QPaintEngine* paintEngine() const override { return nullptr; }
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void requestRender() override;
    void submitFrame(const std::string&, VideoRenderFrame::Ptr) override;
    void removeFrame(const std::string&) override;
    void clearBackend() override;
    void releaseRenderer() override;
    void removeDecorationTexture(const std::string&) override;
    VideoFrameGeometry frameGeometry(const std::string&) const override;
    bool beginFrame(QSize&) override;
    void drawSolid(const VideoTileRect&, float, float, float) override;
    bool drawVideo(const VideoTileRect&) override;
    bool drawDecoration(const VideoTileRect&, const QImage&) override;
    bool endFrame() override;
    bool reportsSubmitAsynchronously() const override { return true; }
private:
    friend class ::ParticipantWindowTestAccess;
    class Surface;
    struct Resource {
        lk_render_resource_id id{};
        VideoRenderFrame::Ptr frame;
        QImage image;
    };
    struct Scene;
    struct Worker;
    lk_render_resource_id NewId();
    void AddCommand(const VideoTileRect&, lk_render_resource_id, float r = 1, float g = 1, float b = 1);
    void startWorker();
    void pollWorker();
    void fail(RenderGpuFailure);
    QWindow* windowSurface() const;
    // Single owner task slot for deterministic tests/readback. No GPU pointer
    // escapes the callback, and production never uses this seam.
    void runOnOwnerForTest(std::function<void(BackendDevice&)>);
    bool scenePresentedForTest() const;
    static bool workersIdleForTest();
protected:
    // GL subclasses share module selection/diagnostics, but own their surface.
    std::shared_ptr<BackendModule> module_;
    lk_render_frame_target target_{};
private:
    Surface* surface_ = nullptr;
    QTimer* watchdog_ = nullptr;
    std::shared_ptr<Worker> worker_;
    uint64_t next_resource_ = 1, submitted_ = 0;
    std::map<std::string, Resource> videos_, overlays_;
    std::shared_ptr<Scene> building_;
    bool initialized_ = false, failed_ = false;
};
} // namespace livekit::render
