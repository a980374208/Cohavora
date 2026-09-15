#pragma once

#include <QtWidgets/QWidget>
#include <QtCore/QTimer>
#include <QtCore/QString>
#include "dx11_renderer.h"
#include "dx11_texture_pool.h"
#include "src/render/owned_i420_frame.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace livekit {
namespace dx11 {

class Dx11VideoCanvas : public QWidget {
    Q_OBJECT
public:
    explicit Dx11VideoCanvas(QWidget* parent = nullptr);
    ~Dx11VideoCanvas() override;

    // 投递参会人原始视频帧 (线程安全，支持直接从 WebRTC 回调线程传入)
    void updateFrame(const std::string& identity, const livekit::VideoFrame& frame);

    // Router 到 GPU 的产品路径。只能由 UI/render 线程调用；帧已经是
    // OwnedI420Frame，因此不会保存 WebRTC buffer 裸指针，也不会转 QImage。
    void updateI420Frame(const std::string& identity, render::OwnedI420Frame::Ptr frame);

    static bool IsHardwareBackendAllowed();
    bool rendererReady() const;

    // 参会人离开或关闭视频
    void removeUser(const std::string& identity);
    void clearUsers();

    // 更新网格视口坐标排布
    void setTilesLayout(const std::vector<TileRect>& tiles);

    // 当前用户是否有活跃可渲染的硬件纹理
    bool hasVideo(const std::string& identity) const;

    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    // Initialization, resize or Present failed. The owner must switch the
    // mutually-exclusive VideoRenderSession backend to QtCpu on the UI thread.
    void rendererUnavailable();
    void tileDoubleClicked(const QString& identity);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void render();
    bool EnsureRenderer();
    void NotifyRendererUnavailable();
    TileRect FitTileToFrame(const TileRect& tile, const UserGpuResource& resource) const;

private:
    Dx11Renderer renderer_;
    Dx11TexturePool texture_pool_;

    std::mutex layout_mutex_;
    std::vector<TileRect> tiles_;

    QTimer* fps_timer_ = nullptr;
    std::atomic<bool> frame_dirty_{false};
    bool renderer_unavailable_emitted_{false};
};

} // namespace dx11
} // namespace livekit
