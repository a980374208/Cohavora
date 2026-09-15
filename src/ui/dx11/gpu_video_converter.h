#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <QtGui/QImage>
#include <mutex>
#include <memory>
#include "src/rtc/video_frame.h"

namespace livekit {
namespace dx11 {

using Microsoft::WRL::ComPtr;

class GpuVideoConverter {
public:
    static GpuVideoConverter& Instance();

    GpuVideoConverter();
    ~GpuVideoConverter();

    GpuVideoConverter(const GpuVideoConverter&) = delete;
    GpuVideoConverter& operator=(const GpuVideoConverter&) = delete;

    bool Initialize();
    void Cleanup();

#if defined(LIVEKIT_DX11_TESTING)
    // Test-only failure injection. The production target never defines this
    // symbol, so device creation remains entirely platform driven.
    static void SetForceInitializationFailureForTesting(bool enabled);
#endif

    // 核心接口：通过 GPU Pixel Shader 硬件秒级完成 YUV420P / NV12 / RGBA 色彩矩阵转换与映射
    QImage ConvertToQImage(const livekit::VideoFrame& frame);

private:
    // Callers must hold mutex_. Keeping the locking boundary explicit avoids
    // recursive std::mutex acquisition on fallback/error paths.
    bool InitializeLocked();
    void CleanupLocked();
    void ResetFrameResourcesLocked();
    bool EnsurePipeline();
    bool EnsureBuffers(int width, int height, VideoBufferType type);

private:
    std::mutex mutex_;
    bool initialized_{false};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    // 着色器与固定管线状态
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11PixelShader> ps_i420_;
    ComPtr<ID3D11PixelShader> ps_nv12_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_;
    ComPtr<ID3D11Buffer> vertex_buffer_;

    // 离屏渲染与读取纹理
    int current_width_{0};
    int current_height_{0};
    VideoBufferType current_type_{VideoBufferType::RGBA};

    // 输入 Y, U, V
    ComPtr<ID3D11Texture2D> input_tex_[3];
    ComPtr<ID3D11ShaderResourceView> input_srv_[3];

    // 渲染目标 (RGBA)
    ComPtr<ID3D11Texture2D> rt_texture_;
    ComPtr<ID3D11RenderTargetView> rtv_;

    // Staging 纹理 (用于从显存极速读取转换好的 RGBA 连续像素)
    ComPtr<ID3D11Texture2D> staging_texture_;
};

} // namespace dx11
} // namespace livekit
