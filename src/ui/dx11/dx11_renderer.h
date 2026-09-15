#pragma once

#include "dx11_types.h"
#include "dx11_color_conversion.h"
#include <memory>
#include <mutex>

namespace livekit {
namespace dx11 {

class Dx11Renderer {
public:
    Dx11Renderer();
    ~Dx11Renderer();

    Dx11Renderer(const Dx11Renderer&) = delete;
    Dx11Renderer& operator=(const Dx11Renderer&) = delete;

    bool Initialize(HWND hwnd, int width, int height);
    void Cleanup();

#if defined(LIVEKIT_DX11_TESTING)
    static void SetForceInitializationFailureForTesting(bool enabled);
#endif

    bool Resize(int width, int height);

    bool BeginFrame(float r = 0.0706f, float g = 0.0784f, float b = 0.1020f); // #12141a
    void SetViewport(int x, int y, int width, int height);
    void SetYuvColorSpace(const render::RenderColorSpace& color_space);
    void SetRotation(VideoRotation rotation);
    void DrawQuad(PixelFormatType format, ID3D11ShaderResourceView* const* srvs, UINT count);
    void DrawSolidQuad(float r, float g, float b, float a = 1.0f);
    bool EndFrame(bool vsync = true);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    bool is_initialized() const { return initialized_; }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    // Callers must hold render_mutex_. These helpers make all failure paths
    // non-recursive with respect to the public locking API.
    bool InitializeLocked(HWND hwnd, int width, int height);
    void CleanupLocked();
    bool CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    bool CreateRenderTarget();
    bool CreateShadersAndPipeline();

private:
    HWND hwnd_{nullptr};
    int width_{0};
    int height_{0};
    bool initialized_{false};
    std::mutex render_mutex_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_view_;

    // 管线状态与着色器
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;

    ComPtr<ID3D11PixelShader> ps_i420_;
    ComPtr<ID3D11PixelShader> ps_nv12_;
    ComPtr<ID3D11PixelShader> ps_rgba_;
    ComPtr<ID3D11PixelShader> ps_solid_color_;

    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_;
    ComPtr<ID3D11Buffer> yuv_conversion_buffer_;
    ComPtr<ID3D11Buffer> frame_transform_buffer_;
    ComPtr<ID3D11Buffer> solid_color_buffer_;
};

} // namespace dx11
} // namespace livekit
