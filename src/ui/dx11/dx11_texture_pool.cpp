#include "dx11_texture_pool.h"
#include <iostream>

namespace livekit {
namespace dx11 {

Dx11TexturePool::Dx11TexturePool() = default;

Dx11TexturePool::~Dx11TexturePool() {
    Clear();
}

void Dx11TexturePool::PostUserFrame(const std::string& identity, const livekit::VideoFrame& frame) {
    if (identity.empty() || frame.width() <= 0 || frame.height() <= 0 || !frame.data()) {
        return;
    }
    std::lock_guard<std::mutex> lock(frame_mutex_);
    pending_frames_[identity] = PendingFrame{frame, nullptr};
}

void Dx11TexturePool::PostI420Frame(const std::string& identity, render::OwnedI420Frame::Ptr frame) {
    if (identity.empty() || !frame || frame->width() <= 0 || frame->height() <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(frame_mutex_);
    pending_frames_[identity] = PendingFrame{VideoFrame{}, std::move(frame)};
}

void Dx11TexturePool::RemoveUser(const std::string& identity) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    pending_frames_.erase(identity);
    gpu_resources_.erase(identity);
}

void Dx11TexturePool::Clear() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
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

    // Take the bounded latest-frame mailbox under a short lock. D3D uploads
    // occur after the lock so media producers never wait for the GPU.
    std::map<std::string, PendingFrame> pending_frames;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        pending_frames.swap(pending_frames_);
    }

    for (auto& [id, pending] : pending_frames) {
        const auto& owned = pending.i420_frame;
        const VideoFrame& legacy = pending.legacy_frame;
        const int w = owned ? owned->width() : legacy.width();
        const int h = owned ? owned->height() : legacy.height();
        if (w <= 0 || h <= 0 || (!owned && !legacy.data())) continue;

        const PixelFormatType fmt = owned
            ? PixelFormatType::I420
            : MapBufferTypeToPixelFormat(legacy.type());
        if (fmt == PixelFormatType::Unknown) continue;

        UserGpuResource& res = gpu_resources_[id];
        if (!EnsureGpuTexture(device, res, fmt, w, h)) {
            continue;
        }

        if (fmt == PixelFormatType::I420) {
            if (owned) {
                context->UpdateSubresource(res.textures[0].Get(), 0, nullptr, owned->data_y(), static_cast<UINT>(owned->stride_y()), 0);
                context->UpdateSubresource(res.textures[1].Get(), 0, nullptr, owned->data_u(), static_cast<UINT>(owned->stride_u()), 0);
                context->UpdateSubresource(res.textures[2].Get(), 0, nullptr, owned->data_v(), static_cast<UINT>(owned->stride_v()), 0);
                res.rotation = owned->rotation();
                res.color_space = owned->color_space();
            } else {
                const int chroma_width = (w + 1) / 2;
                const int chroma_height = (h + 1) / 2;
                const uint8_t* y_plane = legacy.data();
                const uint8_t* u_plane = y_plane + (w * h);
                const uint8_t* v_plane = u_plane + (chroma_width * chroma_height);
                context->UpdateSubresource(res.textures[0].Get(), 0, nullptr, y_plane, static_cast<UINT>(w), 0);
                context->UpdateSubresource(res.textures[1].Get(), 0, nullptr, u_plane, static_cast<UINT>(chroma_width), 0);
                context->UpdateSubresource(res.textures[2].Get(), 0, nullptr, v_plane, static_cast<UINT>(chroma_width), 0);
                res.rotation = VideoRotation::VIDEO_ROTATION_0;
                res.color_space = {render::RenderColorMatrix::Bt601, render::RenderColorRange::Limited};
            }
        } else if (fmt == PixelFormatType::NV12) {
            const uint8_t* y_plane = legacy.data();
            const uint8_t* uv_plane = y_plane + (w * h);
            const int chroma_width = (w + 1) / 2;

            context->UpdateSubresource(res.textures[0].Get(), 0, nullptr, y_plane, static_cast<UINT>(w), 0);
            context->UpdateSubresource(res.textures[1].Get(), 0, nullptr, uv_plane, static_cast<UINT>(chroma_width * 2), 0);
            res.rotation = VideoRotation::VIDEO_ROTATION_0;
            res.color_space = {render::RenderColorMatrix::Bt601, render::RenderColorRange::Limited};
        } else if (fmt == PixelFormatType::RGBA) {
            context->UpdateSubresource(res.textures[0].Get(), 0, nullptr, legacy.data(), static_cast<UINT>(w * 4), 0);
            res.rotation = VideoRotation::VIDEO_ROTATION_0;
        }
    }
}

} // namespace dx11
} // namespace livekit
