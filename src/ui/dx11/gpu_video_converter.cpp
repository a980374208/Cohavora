#include "gpu_video_converter.h"
#include "dx11_types.h"
#include "dx11_shaders.h"
#include <atomic>
#include <iostream>

namespace livekit {
namespace dx11 {

namespace {
#if defined(LIVEKIT_DX11_TESTING)
std::atomic<bool> g_force_initialization_failure{false};
#endif
} // namespace

GpuVideoConverter& GpuVideoConverter::Instance() {
    static GpuVideoConverter instance;
    return instance;
}

GpuVideoConverter::GpuVideoConverter() {
    Initialize();
}

GpuVideoConverter::~GpuVideoConverter() {
    Cleanup();
}

bool GpuVideoConverter::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    return InitializeLocked();
}

bool GpuVideoConverter::InitializeLocked() {
    if (initialized_) return true;

    // A failed prior initialization may have left partially-created COM
    // objects. Always begin from a known-empty state before retrying.
    CleanupLocked();

#if defined(LIVEKIT_DX11_TESTING)
    if (g_force_initialization_failure.load(std::memory_order_relaxed)) {
        std::cerr << "[GpuVideoConverter] Forced initialization failure for test" << std::endl;
        return false;
    }
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        &featureLevel,
        context_.GetAddressOf()
    );

    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &featureLevel,
            context_.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        std::cerr << "[GpuVideoConverter] D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
        CleanupLocked();
        return false;
    }

    if (!EnsurePipeline()) {
        CleanupLocked();
        return false;
    }

    initialized_ = true;
    std::cout << "[GpuVideoConverter] Initialized GPU hardware color-space converter successfully!" << std::endl;
    return true;
}

void GpuVideoConverter::Cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    CleanupLocked();
}

void GpuVideoConverter::CleanupLocked() {
    initialized_ = false;
    if (context_) {
        context_->ClearState();
    }
    ResetFrameResourcesLocked();
    vertex_buffer_.Reset();
    sampler_state_.Reset();
    rasterizer_state_.Reset();
    vs_.Reset();
    input_layout_.Reset();
    ps_i420_.Reset();
    ps_nv12_.Reset();
    context_.Reset();
    device_.Reset();
}

void GpuVideoConverter::ResetFrameResourcesLocked() {
    for (int i = 0; i < 3; ++i) {
        input_tex_[i].Reset();
        input_srv_[i].Reset();
    }
    rt_texture_.Reset();
    rtv_.Reset();
    staging_texture_.Reset();
    current_width_ = 0;
    current_height_ = 0;
    current_type_ = VideoBufferType::RGBA;
}

#if defined(LIVEKIT_DX11_TESTING)
void GpuVideoConverter::SetForceInitializationFailureForTesting(bool enabled) {
    g_force_initialization_failure.store(enabled, std::memory_order_relaxed);
}
#endif

bool GpuVideoConverter::EnsurePipeline() {
    // 1. Vertex Shader & Input Layout
    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(kVertexShaderSource, "main", "vs_5_0", vsBlob.GetAddressOf())) return false;
    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs_.GetAddressOf());
    if (FAILED(hr)) return false;

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

    // 2. Pixel Shaders
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kPixelShaderI420Source, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_i420_.GetAddressOf());
    if (FAILED(hr)) return false;

    psBlob.Reset();
    if (!CompileShader(kPixelShaderNV12Source, "main", "ps_5_0", psBlob.GetAddressOf())) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_nv12_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 3. Quad 顶点
    Vertex vertices[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f }
    };
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(sizeof(vertices));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    hr = device_->CreateBuffer(&bd, &initData, vertex_buffer_.GetAddressOf());
    if (FAILED(hr)) return false;

    // 4. 采样器与光栅化
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device_->CreateSamplerState(&sampDesc, sampler_state_.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;
    hr = device_->CreateRasterizerState(&rastDesc, rasterizer_state_.GetAddressOf());
    return SUCCEEDED(hr);
}

bool GpuVideoConverter::EnsureBuffers(int width, int height, VideoBufferType type) {
    if (width == current_width_ && height == current_height_ && type == current_type_ && rtv_) {
        return true;
    }

    ResetFrameResourcesLocked();
    const auto fail = [this]() {
        ResetFrameResourcesLocked();
        return false;
    };

    auto Create2DTexture = [this](int w, int h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = fmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device_->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
        return SUCCEEDED(hr);
    };

    if (type == VideoBufferType::I420 || type == VideoBufferType::I420A) {
        if (!Create2DTexture(width, height, DXGI_FORMAT_R8_UNORM, input_tex_[0], input_srv_[0])) return fail();
        if (!Create2DTexture(width / 2, height / 2, DXGI_FORMAT_R8_UNORM, input_tex_[1], input_srv_[1])) return fail();
        if (!Create2DTexture(width / 2, height / 2, DXGI_FORMAT_R8_UNORM, input_tex_[2], input_srv_[2])) return fail();
    } else if (type == VideoBufferType::NV12) {
        if (!Create2DTexture(width, height, DXGI_FORMAT_R8_UNORM, input_tex_[0], input_srv_[0])) return fail();
        if (!Create2DTexture(width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM, input_tex_[1], input_srv_[1])) return fail();
    } else {
        return fail();
    }

    // 渲染目标 (RGBA)
    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = static_cast<UINT>(width);
    rtDesc.Height = static_cast<UINT>(height);
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&rtDesc, nullptr, rt_texture_.GetAddressOf());
    if (FAILED(hr)) return fail();

    hr = device_->CreateRenderTargetView(rt_texture_.Get(), nullptr, rtv_.GetAddressOf());
    if (FAILED(hr)) return fail();

    // Staging 纹理 (用于极速 CPU 读取)
    D3D11_TEXTURE2D_DESC stDesc = rtDesc;
    stDesc.BindFlags = 0;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&stDesc, nullptr, staging_texture_.GetAddressOf());
    if (FAILED(hr)) return fail();

    current_width_ = width;
    current_height_ = height;
    current_type_ = type;
    return true;
}

QImage GpuVideoConverter::ConvertToQImage(const livekit::VideoFrame& frame) {
    const int w = frame.width();
    const int h = frame.height();
    if (w <= 0 || h <= 0 || !frame.data()) return QImage();

    // RGBA 直通
    if (frame.type() == VideoBufferType::RGBA) {
        return QImage(frame.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ && !InitializeLocked()) {
        return QImage();
    }

    if (!EnsureBuffers(w, h, frame.type())) {
        return QImage();
    }

    // 1. 上传 YUV 分量至显存
    const uint8_t* src = frame.data();
    if (frame.type() == VideoBufferType::I420 || frame.type() == VideoBufferType::I420A) {
        const uint8_t* y_plane = src;
        const uint8_t* u_plane = y_plane + (w * h);
        const uint8_t* v_plane = u_plane + ((w / 2) * (h / 2));

        context_->UpdateSubresource(input_tex_[0].Get(), 0, nullptr, y_plane, static_cast<UINT>(w), 0);
        context_->UpdateSubresource(input_tex_[1].Get(), 0, nullptr, u_plane, static_cast<UINT>(w / 2), 0);
        context_->UpdateSubresource(input_tex_[2].Get(), 0, nullptr, v_plane, static_cast<UINT>(w / 2), 0);

        context_->PSSetShader(ps_i420_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = { input_srv_[0].Get(), input_srv_[1].Get(), input_srv_[2].Get() };
        context_->PSSetShaderResources(0, 3, srvs);
    } else if (frame.type() == VideoBufferType::NV12) {
        const uint8_t* y_plane = src;
        const uint8_t* uv_plane = y_plane + (w * h);

        context_->UpdateSubresource(input_tex_[0].Get(), 0, nullptr, y_plane, static_cast<UINT>(w), 0);
        context_->UpdateSubresource(input_tex_[1].Get(), 0, nullptr, uv_plane, static_cast<UINT>(w), 0);

        context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { input_srv_[0].Get(), input_srv_[1].Get() };
        context_->PSSetShaderResources(0, 2, srvs);
    } else {
        return QImage();
    }

    // 2. 绑定目标并由 GPU 硬件着色器秒级转码
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<FLOAT>(w), static_cast<FLOAT>(h), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[1] = { rtv_.Get() };
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertex_buffer_.GetAddressOf(), &stride, &offset);
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->RSSetState(rasterizer_state_.Get());

    ID3D11SamplerState* samplers[1] = { sampler_state_.Get() };
    context_->PSSetSamplers(0, 1, samplers);

    // 触发 GPU 绘制
    context_->Draw(4, 0);

    // 解绑 SRV
    ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 3, nullSrvs);

    // 3. 极速拷贝至 Staging 纹理并映射读取
    context_->CopyResource(staging_texture_.Get(), rt_texture_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return QImage();

    QImage result(static_cast<const uchar*>(mapped.pData), w, h, static_cast<int>(mapped.RowPitch), QImage::Format_RGBA8888);
    QImage copyImg = result.copy();
    context_->Unmap(staging_texture_.Get(), 0);

    return copyImg;
}

} // namespace dx11
} // namespace livekit
