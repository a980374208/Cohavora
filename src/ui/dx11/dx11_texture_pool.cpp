#include "dx11_texture_pool.h"
#include <iostream>

namespace livekit {
namespace dx11 {

Dx11TexturePool::Dx11TexturePool() = default;

Dx11TexturePool::~Dx11TexturePool() {
    Clear();
}

void Dx11TexturePool::PostFrame(const std::string& identity, render::VideoRenderFrame::Ptr frame) {
    if (!identity.empty() && frame) pending_frames_[identity] = std::move(frame);
}

void Dx11TexturePool::RemoveUser(const std::string& identity) {
    pending_frames_.erase(identity);
    gpu_resources_.erase(identity);
}

void Dx11TexturePool::Clear() {
    pending_frames_.clear();
    gpu_resources_.clear();
}

bool Dx11TexturePool::HasUserVideo(const std::string& identity) const {
    auto it = gpu_resources_.find(identity);
    return (it != gpu_resources_.end() && it->second.srv_count > 0);
}

const UserGpuResource* Dx11TexturePool::GetUserResource(const std::string& identity) const {
    auto it = gpu_resources_.find(identity);
    if (it != gpu_resources_.end() && it->second.srv_count > 0) {
        return &it->second;
    }
    return nullptr;
}

bool Dx11TexturePool::EnsureGpuTexture(ID3D11Device* device, UserGpuResource& res, PixelFormatType format, int width, int height) {
    if (res.format == format && res.width == width && res.height == height && res.srv_count > 0) {
        return true;
    }

    res.Release();
    res.format = format;
    res.width = width;
    res.height = height;

    auto Create2DTexture = [device](int w, int h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = fmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
        return SUCCEEDED(hr);
    };

    if (format == PixelFormatType::I420) {
        // Y (w x h), U/V use ceil division for odd WebRTC frame sizes.
        const int chroma_width = (width + 1) / 2;
        const int chroma_height = (height + 1) / 2;
        if (!Create2DTexture(width, height, DXGI_FORMAT_R8_UNORM, res.textures[0], res.srvs[0])) return false;
        if (!Create2DTexture(chroma_width, chroma_height, DXGI_FORMAT_R8_UNORM, res.textures[1], res.srvs[1])) return false;
        if (!Create2DTexture(chroma_width, chroma_height, DXGI_FORMAT_R8_UNORM, res.textures[2], res.srvs[2])) return false;
        res.srv_count = 3;
        return true;
    } else if (format == PixelFormatType::NV12) {
        // Y (w x h), UV uses ceil division and two components per sample.
        const int chroma_width = (width + 1) / 2;
        const int chroma_height = (height + 1) / 2;
        if (!Create2DTexture(width, height, DXGI_FORMAT_R8_UNORM, res.textures[0], res.srvs[0])) return false;
        if (!Create2DTexture(chroma_width, chroma_height, DXGI_FORMAT_R8G8_UNORM, res.textures[1], res.srvs[1])) return false;
        res.srv_count = 2;
        return true;
    } else if (format == PixelFormatType::RGBA) {
        if (!Create2DTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, res.textures[0], res.srvs[0])) return false;
        res.srv_count = 1;
        return true;
    }

    return false;
}

void Dx11TexturePool::UploadPendingFrames(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return;
    auto pending = std::move(pending_frames_);
    pending_frames_.clear();
    for (const auto& [id, frame] : pending) {
        const auto& view = frame->view();
        const auto format = view.format == LK_RENDER_I420 ? PixelFormatType::I420
            : view.format == LK_RENDER_NV12 ? PixelFormatType::NV12 : PixelFormatType::RGBA;
        auto& resource = gpu_resources_[id];
        if (!EnsureGpuTexture(device, resource, format, int(view.width), int(view.height))) continue;
        for (uint32_t plane = 0; plane < view.plane_count; ++plane) {
            context->UpdateSubresource(resource.textures[plane].Get(), 0, nullptr,
                view.planes[plane].data, view.planes[plane].stride_bytes, 0);
        }
        resource.rotation = static_cast<VideoRotation>(view.rotation_degrees);
        resource.color_space = frame->colorSpace();
    }
}

} // namespace dx11
} // namespace livekit
