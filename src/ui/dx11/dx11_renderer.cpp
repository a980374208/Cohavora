#include "dx11_renderer.h"
#include "dx11_shaders.h"
#include <atomic>
#include <cstring>
#include <iostream>

namespace livekit {
namespace dx11 {

namespace {
#if defined(LIVEKIT_DX11_TESTING)
std::atomic<bool> g_force_initialization_failure{false};
#endif

struct FrameTransformConstants {
    int rotation = 0;
    float padding[3]{};
};

struct SolidColorConstants {
    float color[4]{};
};
} // namespace

Dx11Renderer::Dx11Renderer() = default;

Dx11Renderer::~Dx11Renderer() {
    Cleanup();
}

bool Dx11Renderer::Initialize(HWND hwnd, int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    return InitializeLocked(hwnd, width, height);
}

bool Dx11Renderer::InitializeLocked(HWND hwnd, int width, int height) {
    if (initialized_) {
        return true;
    }

    // Retry is supported after any failed initialization. Start with no stale
    // COM state rather than leaving a partially-created renderer behind.
    CleanupLocked();

    if (!hwnd || width <= 0 || height <= 0) {
        std::cerr << "[Dx11Renderer] Invalid init parameters: hwnd=" << hwnd
                  << " w=" << width << " h=" << height << std::endl;
        return false;
    }

#if defined(LIVEKIT_DX11_TESTING)
    if (g_force_initialization_failure.load(std::memory_order_relaxed)) {
        std::cerr << "[Dx11Renderer] Forced initialization failure for test" << std::endl;
        return false;
    }
#endif

    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    if (!CreateDeviceAndSwapChain(hwnd, width, height)) {
        CleanupLocked();
        return false;
    }

    if (!CreateRenderTarget()) {
        CleanupLocked();
        return false;
    }

    if (!CreateShadersAndPipeline()) {
        CleanupLocked();
        return false;
    }

    initialized_ = true;
    std::cout << "[Dx11Renderer] Initialized successfully (" << width << "x" << height << ")" << std::endl;
    return true;
}

void Dx11Renderer::Cleanup() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    CleanupLocked();
}

void Dx11Renderer::CleanupLocked() {
    initialized_ = false;

    if (context_) {
        context_->ClearState();
    }

    render_target_view_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();

    vertex_shader_.Reset();
    input_layout_.Reset();
    ps_i420_.Reset();
    ps_nv12_.Reset();
    ps_rgba_.Reset();
    ps_solid_color_.Reset();
    vertex_buffer_.Reset();
    sampler_state_.Reset();
    rasterizer_state_.Reset();
    yuv_conversion_buffer_.Reset();
    frame_transform_buffer_.Reset();
    solid_color_buffer_.Reset();
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
}

#if defined(LIVEKIT_DX11_TESTING)
void Dx11Renderer::SetForceInitializationFailureForTesting(bool enabled) {
    g_force_initialization_failure.store(enabled, std::memory_order_relaxed);
}
#endif

bool Dx11Renderer::CreateDeviceAndSwapChain(HWND hwnd, int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = static_cast<UINT>(width);
    sd.BufferDesc.Height = static_cast<UINT>(height);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#if defined(_DEBUG)
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels,
        static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
        D3D11_SDK_VERSION,
        &sd,
        swap_chain_.GetAddressOf(),
        device_.GetAddressOf(),
        &featureLevel,
        context_.GetAddressOf()
    );

    if (FAILED(hr) && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        // 重试无 debug 标志 (某些机器可能未装 SDK 调试层)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
            D3D11_SDK_VERSION,
            &sd,
            swap_chain_.GetAddressOf(),
            device_.GetAddressOf(),
            &featureLevel,
            context_.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        std::cerr << "[Dx11Renderer] D3D11CreateDeviceAndSwapChain failed: hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

bool Dx11Renderer::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[Dx11Renderer] Failed to get back buffer from swap chain: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, render_target_view_.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[Dx11Renderer] Failed to create render target view: 0x" << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

bool Dx11Renderer::CreateShadersAndPipeline() {
    // 1. 编译并创建 Vertex Shader
    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(kVertexShaderSource, "main", "vs_5_0", vsBlob.GetAddressOf())) {
        return false;
    }
    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vertex_shader_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 2. 创建 Input Layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    hr = device_->CreateInputLayout(
        layoutDesc,
        static_cast<UINT>(sizeof(layoutDesc) / sizeof(layoutDesc[0])),
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        input_layout_.GetAddressOf()
    );
    if (FAILED(hr)) return false;

    // 3. 编译并创建各格式 Pixel Shader
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kPixelShaderI420Source, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_i420_.GetAddressOf());
    if (FAILED(hr)) return false;

    psBlob.Reset();
    if (!CompileShader(kPixelShaderNV12Source, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_nv12_.GetAddressOf());
    if (FAILED(hr)) return false;

    psBlob.Reset();
    if (!CompileShader(kPixelShaderRGBASource, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_rgba_.GetAddressOf());
    if (FAILED(hr)) return false;

    psBlob.Reset();
    if (!CompileShader(kPixelShaderSolidColorSource, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_solid_color_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 4. 创建 Quad 顶点缓冲 (Triangle Strip 覆盖 [-1, 1])
    Vertex vertices[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f }, // 左上
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f }, // 右上
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f }, // 左下
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f }  // 右下
    };

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(sizeof(vertices));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    hr = device_->CreateBuffer(&bd, &initData, vertex_buffer_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 5. 创建线性采样器 (Linear Clamp)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampDesc, sampler_state_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 6. 光栅化状态 (无背面剔除)
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;
    hr = device_->CreateRasterizerState(&rastDesc, rasterizer_state_.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC conversion_desc = {};
    conversion_desc.Usage = D3D11_USAGE_DYNAMIC;
    conversion_desc.ByteWidth = static_cast<UINT>(sizeof(YuvColorConversion));
    conversion_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    conversion_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&conversion_desc, nullptr, yuv_conversion_buffer_.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC transform_desc = {};
    transform_desc.Usage = D3D11_USAGE_DYNAMIC;
    transform_desc.ByteWidth = static_cast<UINT>(sizeof(FrameTransformConstants));
    transform_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    transform_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&transform_desc, nullptr, frame_transform_buffer_.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC solid_color_desc = {};
    solid_color_desc.Usage = D3D11_USAGE_DYNAMIC;
    solid_color_desc.ByteWidth = static_cast<UINT>(sizeof(SolidColorConstants));
    solid_color_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    solid_color_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&solid_color_desc, nullptr, solid_color_buffer_.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}

bool Dx11Renderer::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!initialized_ || width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_) return true;

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_view_.Reset();

    HRESULT hr = swap_chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        std::cerr << "[Dx11Renderer] ResizeBuffers failed: 0x" << std::hex << hr << std::endl;
        CleanupLocked();
        return false;
    }

    width_ = width;
    height_ = height;
    if (!CreateRenderTarget()) {
        CleanupLocked();
        return false;
    }
    return true;
}

bool Dx11Renderer::BeginFrame(float r, float g, float b) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!initialized_ || !context_ || !render_target_view_) return false;

    float clearColor[4] = { r, g, b, 1.0f };
    context_->ClearRenderTargetView(render_target_view_.Get(), clearColor);

    ID3D11RenderTargetView* rtvs[] = { render_target_view_.Get() };
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    // 绑定基础状态
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertex_buffer_.GetAddressOf(), &stride, &offset);
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->RSSetState(rasterizer_state_.Get());

    ID3D11SamplerState* samplers[] = { sampler_state_.Get() };
    context_->PSSetSamplers(0, 1, samplers);
    return true;
}

void Dx11Renderer::SetViewport(int x, int y, int width, int height) {
    if (!context_) return;
    D3D11_VIEWPORT vp;
    vp.TopLeftX = static_cast<FLOAT>(x);
    vp.TopLeftY = static_cast<FLOAT>(y);
    vp.Width = static_cast<FLOAT>(width);
    vp.Height = static_cast<FLOAT>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
}

void Dx11Renderer::SetYuvColorSpace(const render::RenderColorSpace& color_space) {
    if (!context_ || !yuv_conversion_buffer_) return;

    const auto conversion = MakeYuvColorConversion(color_space);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(yuv_conversion_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &conversion, sizeof(conversion));
    context_->Unmap(yuv_conversion_buffer_.Get(), 0);

    ID3D11Buffer* buffers[] = { yuv_conversion_buffer_.Get() };
    context_->PSSetConstantBuffers(0, 1, buffers);
}

void Dx11Renderer::SetRotation(VideoRotation rotation) {
    if (!context_ || !frame_transform_buffer_) return;

    FrameTransformConstants constants;
    constants.rotation = static_cast<int>(rotation);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(frame_transform_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(frame_transform_buffer_.Get(), 0);

    ID3D11Buffer* buffers[] = { frame_transform_buffer_.Get() };
    context_->VSSetConstantBuffers(1, 1, buffers);
}

void Dx11Renderer::DrawQuad(PixelFormatType format, ID3D11ShaderResourceView* const* srvs, UINT count) {
    if (!context_ || !srvs || count == 0) return;

    switch (format) {
    case PixelFormatType::I420:
        context_->PSSetShader(ps_i420_.Get(), nullptr, 0);
        break;
    case PixelFormatType::NV12:
        context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
        break;
    case PixelFormatType::RGBA:
        context_->PSSetShader(ps_rgba_.Get(), nullptr, 0);
        break;
    default:
        return;
    }

    context_->PSSetShaderResources(0, count, srvs);
    context_->Draw(4, 0);

    // 解绑 SRV，防止管线危险状态冲突
    ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, count, nullSrvs);
}

void Dx11Renderer::DrawSolidQuad(float r, float g, float b, float a) {
    if (!context_ || !ps_solid_color_ || !solid_color_buffer_) return;

    SolidColorConstants constants{{r, g, b, a}};
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(solid_color_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(solid_color_buffer_.Get(), 0);

    ID3D11Buffer* buffers[] = { solid_color_buffer_.Get() };
    context_->PSSetConstantBuffers(2, 1, buffers);
    context_->PSSetShader(ps_solid_color_.Get(), nullptr, 0);
    context_->Draw(4, 0);
}

bool Dx11Renderer::EndFrame(bool vsync) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!initialized_ || !swap_chain_) return false;
    const HRESULT hr = swap_chain_->Present(vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        std::cerr << "[Dx11Renderer] Present failed: 0x" << std::hex << hr << std::endl;
        CleanupLocked();
        return false;
    }
    return true;
}

} // namespace dx11
} // namespace livekit
