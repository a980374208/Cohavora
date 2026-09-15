#pragma once

#include "dx11_types.h"
#include "src/render/owned_i420_frame.h"
#include <map>
#include <mutex>
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

    // 跨线程投递：当 WebRTC 解码出新帧时调用（线程安全）
    void PostUserFrame(const std::string& identity, const livekit::VideoFrame& frame);

    // UI/render thread submits the Router-owned remote frame. This is the
    // production DX11 path: no RGBA conversion and no staging readback.
    void PostI420Frame(const std::string& identity, render::OwnedI420Frame::Ptr frame);

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

    struct PendingFrame {
        VideoFrame legacy_frame;
        render::OwnedI420Frame::Ptr i420_frame;
    };

private:
    mutable std::mutex frame_mutex_;
    // 每路只保留一个待上传帧。生产者只在此表中替换，不接触 D3D 资源。
    std::map<std::string, PendingFrame> pending_frames_;

    // 渲染线程持有的 GPU 资源表
    std::map<std::string, UserGpuResource> gpu_resources_;
};

} // namespace dx11
} // namespace livekit
