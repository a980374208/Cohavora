#pragma once
#include "module_video_canvas.h"

class QWindow;
namespace livekit::render {
// UI owns layout/input and immutable snapshots. The render owner alone creates,
// uses and destroys the context/device and presents; UI never waits for a driver.
class GlVideoCanvas final : public ModuleVideoCanvas {
    Q_OBJECT
public:
    GlVideoCanvas(std::shared_ptr<BackendModule>, QWidget* parent = nullptr);
    ~GlVideoCanvas() override;
    bool rendererReady() const override { return initialized_ && !failed_; }
    bool rendererPending() const override { return !initialized_ && !failed_; }
    QString backendName() const override { return QStringLiteral("OpenGL"); }
    QString failureReason() const { return failure_reason_; }
    QString driverDescription() const { return driver_description_; }
    uint64_t presentedFrames() const { return presented_; }
signals:
    void framePresented();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void requestRender() override;
    void releaseRenderer() override;
    void submitFrame(const std::string&, VideoRenderFrame::Ptr) override;
    void removeFrame(const std::string&) override;
    void clearBackend() override;
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
    QWindow* windowSurface() const;
    void startWorker();
    void pollWorker();
    void fail(RenderGpuFailure);
    void addCommand(const VideoTileRect&, lk_render_resource_id, float = 1, float = 1, float = 1);
    // Private deterministic fault seam; no environment switch or ABI extension.
    void setBeforePresentForTest(std::function<void()>);
    static bool workersIdleForTest();
    bool scenePresentedForTest() const;
    Surface* surface_ = nullptr;
    QTimer* watchdog_ = nullptr;
    std::shared_ptr<Worker> worker_;
    std::map<std::string, Resource> videos_, overlays_;
    std::shared_ptr<Scene> building_;
    uint64_t next_resource_ = 1, presented_ = 0, submitted_ = 0;
    bool initialized_ = false, failed_ = false;
    bool waiting_for_worker_slot_ = false;
    QString failure_reason_, driver_description_;
};
}
