#pragma once

#include "dx11_types.h"
#include "src/render/video_render_frame.h"
#include <map>
#include <memory>
#include <optional>

namespace livekit {
namespace dx11 {

struct UserGpuResource {
    PixelFormatType format{PixelFormatType::Unknown};
    int width{0};
    int height{0};
    VideoRotation rotation{VideoRotation::VIDEO_ROTATION_0};
    render::RenderColorSpace color_space{};

    // I420: Y, U, V; NV12: Y, UV; RGBA: Texture
    ComPtr<ID3D11Texture2D> textures[3];
    ComPtr<ID3D11ShaderResourceView> srvs[3];
    UINT srv_count{0};

    void Release() {
        for (int i = 0; i < 3; ++i) {
            textures[i].Reset();
            srvs[i].Reset();
        }
        srv_count = 0;
        format = PixelFormatType::Unknown;
        width = 0;
        height = 0;
        rotation = VideoRotation::VIDEO_ROTATION_0;
        color_space = {};
    }
};

class Dx11TexturePool {
public:
    Dx11TexturePool();
    ~Dx11TexturePool();

    // Owner/UI thread only; capture threads submit to the host mailbox.
    void PostFrame(const std::string& identity, render::VideoRenderFrame::Ptr frame);

    // 参会人离开或关闭摄像头时清理
    void RemoveUser(const std::string& identity);
    void Clear();

    // 渲染线程调用：检查并上传脏帧至 GPU 显存
    void UploadPendingFrames(ID3D11Device* device, ID3D11DeviceContext* context);

    // 获取特定用户已就绪的 GPU 资源
    const UserGpuResource* GetUserResource(const std::string& identity) const;

    bool HasUserVideo(const std::string& identity) const;

private:
    bool EnsureGpuTexture(ID3D11Device* device, UserGpuResource& res, PixelFormatType format, int width, int height);

    // Per-stream latest frame, only populated by the bounded host bindings.
    std::map<std::string, render::VideoRenderFrame::Ptr> pending_frames_;

    // 渲染线程持有的 GPU 资源表
    std::map<std::string, UserGpuResource> gpu_resources_;
};

} // namespace dx11
} // namespace livekit
