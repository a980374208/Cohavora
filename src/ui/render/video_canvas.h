#pragma once

#include <QtWidgets/QWidget>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtGui/QImage>
#include "render/video_layout.h"
#include "render/video_render_frame.h"
#include "render/render_diagnostics.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class ParticipantWindowTestAccess;

namespace livekit::render {
// Shared Qt host: layout, decorations, input and render scheduling. All calls
// are owner/UI-thread-only. Capture callbacks must use a frame mailbox instead.
// No native GPU handles in this interface; modules receive borrowed C ABI views.
class VideoCanvas : public QWidget {
    Q_OBJECT
public:
    explicit VideoCanvas(QWidget* parent = nullptr);
    ~VideoCanvas() override;
    virtual bool rendererReady() const = 0;
    virtual bool rendererPending() const { return false; }
    virtual QString backendName() const = 0;
    const RenderDiagnostics& renderDiagnostics() const noexcept { return *diagnostics_; }
    void updateFrame(const std::string& key, VideoRenderFrame::Ptr frame);
    void updateI420Frame(const std::string& key, OwnedI420Frame::Ptr frame) {
        updateFrame(key, VideoRenderFrame::FromI420(std::move(frame)));
    }
    void removeUser(const std::string& key);
    void clearUsers();
    void shutdownRenderer();
    bool hasVideo(const std::string& key) const { return frameGeometry(key).available; }
    void setTilesLayout(const std::vector<VideoTileRect>& tiles);
    using DecorationPainter = std::function<QImage(const QSize&, bool, bool)>;
    void setTileDecoration(const std::string& key, DecorationPainter painter, const QRect& pinRect);
    void updateTilePresentation(const std::string& key, bool hasVideo);
    void setStageOverlay(QWidget* widget);
signals:
    void rendererInitialized();
    void rendererUnavailable();
    void tileDoubleClicked(const QString& key);
    void tilePinRequested(const QString& key);
protected:
    void render();
    virtual void requestRender() { render(); }
    bool eventFilter(QObject*, QEvent*) override;
    bool event(QEvent*) override;
    void notifyRendererUnavailable();
    void stopRendering();
    void initializeModuleDiagnostics(RenderBackend backend, uint32_t abiVersion);
    void markRendererActive(QString driverDescription, uint32_t moduleVersion);
    void markRendererFailure(RenderGpuFailure failure, RenderFallbackReason fallback);
    virtual void submitFrame(const std::string&, VideoRenderFrame::Ptr) = 0;
    virtual void removeFrame(const std::string&) = 0;
    virtual void clearBackend() = 0;
    virtual void releaseRenderer() = 0;
    virtual void removeDecorationTexture(const std::string&) = 0;
    virtual VideoFrameGeometry frameGeometry(const std::string&) const = 0;
    virtual bool beginFrame(QSize& physicalSize) = 0;
    virtual void drawSolid(const VideoTileRect&, float r, float g, float b) = 0;
    virtual bool drawVideo(const VideoTileRect&) = 0;
    virtual bool drawDecoration(const VideoTileRect&, const QImage&) = 0;
    virtual bool endFrame() = 0;
    virtual bool reportsSubmitAsynchronously() const { return false; }
    virtual const char* submitMeasurementPoint() const {
        return "gpu_present";
    }
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    friend class ::ParticipantWindowTestAccess;
    std::string hitTest(const QPoint&, bool pinOnly = false) const;
    void updateHovered(const QPoint&);
    struct Decoration {
        DecorationPainter painter;
        QRect pinRect;
        qint64 cacheKey = 0;
    };
    QPointer<QWidget> stage_overlay_;
    QImage stage_image_;
    bool stage_dirty_ = true, painting_stage_ = false;
    std::vector<VideoTileRect> tiles_;
    std::map<std::string, Decoration> decorations_;
    std::map<std::string, VideoRenderFrame::Ptr> telemetry_frames_;
    std::string hovered_tile_, pressed_pin_;
    QTimer* fps_timer_ = nullptr;
    std::atomic<bool> frame_dirty_{false};
    bool renderer_unavailable_emitted_ = false;
    std::unique_ptr<RenderDiagnostics> diagnostics_;
    void updateRenderExpectations();
};

VideoCanvas* CreateVideoCanvas(QWidget* parent, RenderDiagnostics* initialDiagnostics = nullptr);
// Explicit application-directory injection for isolated loader/fallback tests.
VideoCanvas* CreateVideoCanvasFromDirectory(QWidget* parent, const QString& applicationDirectory,
    RenderDiagnostics* initialDiagnostics = nullptr);
} // namespace livekit::render
