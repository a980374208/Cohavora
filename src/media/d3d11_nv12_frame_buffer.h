#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/shared_desktop_frame.h"

namespace livekit {

class D3d11Nv12TexturePool;
class D3d11VideoProcessorConverter;

constexpr std::uint32_t kD3d11TextureDesktopFrameCapturerIdBit = 0x80000000u;

// Native GPU NV12 VideoFrameBuffer (type() == VideoFrameBuffer::Type::kNative).
// Holds a reference-counted lease on a bounded pool of shared D3D11 NV12
// textures produced by the GPU color/scale processor, eliminating raw screen
// frame CPU readback and re-upload on the normal hardware encoding path while
// providing lazy on-demand staging readback for ToNV12()/ToI420() when software
// fallback or CPU consumers access the frame.
class D3d11Nv12FrameBuffer : public webrtc::VideoFrameBuffer {
public:
    struct Diagnostics {
        std::uint64_t gpu_frames_created = 0;
        std::uint64_t gpu_scales_performed = 0;
        std::uint64_t cpu_readbacks_performed = 0;
        std::uint64_t pool_exhausted_drops = 0;
    };

    static Diagnostics GetDiagnostics() noexcept;
    static void ResetDiagnostics() noexcept;

    D3d11Nv12FrameBuffer(
        int width,
        int height,
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        HANDLE shared_handle,
        LUID adapter_luid,
        std::shared_ptr<D3d11Nv12TexturePool> owner_pool,
        std::size_t slot_index,
        std::uint64_t pool_epoch,
        std::shared_ptr<std::mutex> device_context_mutex);

    ~D3d11Nv12FrameBuffer() override;

    Type type() const override { return Type::kNative; }
    int width() const override { return width_; }
    int height() const override { return height_; }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
    const webrtc::I420BufferInterface* GetI420() const override;

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
        webrtc::ArrayView<Type> types) override;

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
        int offset_x,
        int offset_y,
        int crop_width,
        int crop_height,
        int scaled_width,
        int scaled_height) override;

    webrtc::scoped_refptr<webrtc::NV12BufferInterface> ToNV12() const;

    ID3D11Device* device() const noexcept { return device_.Get(); }
    ID3D11Texture2D* texture() const noexcept { return texture_.Get(); }
    HANDLE shared_handle() const noexcept { return shared_handle_; }
    LUID adapter_luid() const noexcept { return adapter_luid_; }
    std::shared_ptr<std::mutex> device_context_mutex() const noexcept {
        return device_context_mutex_;
    }

    // Resolves only live buffers registered by this implementation, never by
    // probing another kNative implementation's object layout. The caller must
    // retain its input buffer while using the borrowed result.
    static const D3d11Nv12FrameBuffer* FromVideoFrameBuffer(
        const webrtc::VideoFrameBuffer* buffer) noexcept;

private:
    const int width_;
    const int height_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    const HANDLE shared_handle_;
    const LUID adapter_luid_;
    std::shared_ptr<D3d11Nv12TexturePool> owner_pool_;
    const std::size_t slot_index_;
    const std::uint64_t pool_epoch_;
    std::shared_ptr<std::mutex> device_context_mutex_;

    mutable std::mutex readback_mutex_;
    mutable webrtc::scoped_refptr<webrtc::NV12Buffer> cached_nv12_;
    mutable webrtc::scoped_refptr<webrtc::I420BufferInterface> cached_i420_;
};

// Bounded 4-slot pool of shared DXGI_FORMAT_NV12 textures for hardware encoding.
// A slot remains pinned while any D3d11Nv12FrameBuffer holds a reference to it.
// On resize or Clear(), the pool epoch advances so in-flight buffers retire
// their old textures cleanly upon release without overwriting active frames.
class D3d11Nv12TexturePool final : public std::enable_shared_from_this<D3d11Nv12TexturePool> {
public:
    static constexpr std::size_t kPoolSize = 4;

    D3d11Nv12TexturePool(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        LUID adapter_luid,
        std::shared_ptr<std::mutex> device_context_mutex);
    ~D3d11Nv12TexturePool();

    webrtc::scoped_refptr<D3d11Nv12FrameBuffer> Acquire(int width, int height);
    void ReleaseSlot(std::size_t slot_index, std::uint64_t epoch) noexcept;
    void Clear() noexcept;

    ID3D11Device* device() const noexcept { return device_.Get(); }
    LUID adapter_luid() const noexcept { return adapter_luid_; }
    std::shared_ptr<std::mutex> device_context_mutex() const noexcept {
        return device_context_mutex_;
    }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HANDLE shared_handle = nullptr;
        int width = 0;
        int height = 0;
        bool in_use = false;
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    const LUID adapter_luid_;
    std::shared_ptr<std::mutex> device_context_mutex_;

    std::mutex mutex_;
    std::uint64_t epoch_ = 1;
    int width_ = 0;
    int height_ = 0;
    Slot slots_[kPoolSize];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int staging_width_ = 0;
    int staging_height_ = 0;

    friend class D3d11Nv12FrameBuffer;
    bool ReadbackToNV12(ID3D11Texture2D* texture, int width, int height, webrtc::NV12Buffer* dst);
};

// D3D11 VideoProcessor wrapper performing GPU BGRA->NV12 color conversion and
// scaling (or NV12->NV12 scaling) into a bounded D3d11Nv12TexturePool.
class D3d11VideoProcessorConverter final {
public:
    D3d11VideoProcessorConverter(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        LUID adapter_luid,
        std::shared_ptr<std::mutex> device_context_mutex = std::make_shared<std::mutex>());
    ~D3d11VideoProcessorConverter();

    // Converts a GPU BGRA (or NV12) texture and scales to (dst_width, dst_height)
    // in a single VideoProcessorBlt pass, returning a pooled D3d11Nv12FrameBuffer.
    webrtc::scoped_refptr<D3d11Nv12FrameBuffer> ConvertAndScale(
        ID3D11Texture2D* src_texture,
        int src_width,
        int src_height,
        int dst_width,
        int dst_height,
        int offset_x = 0,
        int offset_y = 0);

    void Clear() noexcept;

    ID3D11Device* device() const noexcept { return device_.Get(); }
    LUID adapter_luid() const noexcept { return adapter_luid_; }
    std::shared_ptr<std::mutex> device_context_mutex() const noexcept {
        return device_context_mutex_;
    }

private:
    bool EnsureProcessorLocked(int in_w, int in_h, int out_w, int out_h);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bindable_input_;
    int bindable_in_w_ = 0;
    int bindable_in_h_ = 0;
    DXGI_FORMAT bindable_in_format_ = DXGI_FORMAT_UNKNOWN;
    const LUID adapter_luid_;
    std::shared_ptr<std::mutex> device_context_mutex_;
    std::shared_ptr<D3d11Nv12TexturePool> pool_;
    int enum_in_w_ = 0;
    int enum_in_h_ = 0;
    int enum_out_w_ = 0;
    int enum_out_h_ = 0;
    bool initialized_ = false;
    bool supported_ = true;
};

// DesktopFrame subclass carrying a captured GPU BGRA texture alongside a lazy
// CPU staging fallback for callers that inspect DesktopFrame::data().
class D3d11TextureDesktopFrame final : public webrtc::DesktopFrame {
public:
    D3d11TextureDesktopFrame(
        webrtc::DesktopSize size,
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        LUID adapter_luid,
        std::shared_ptr<std::mutex> device_context_mutex);

    ~D3d11TextureDesktopFrame() override;

    ID3D11Device* device() const noexcept { return device_.Get(); }
    ID3D11Texture2D* texture() const noexcept { return texture_.Get(); }
    LUID adapter_luid() const noexcept { return adapter_luid_; }
    std::shared_ptr<std::mutex> device_context_mutex() const noexcept {
        return device_context_mutex_;
    }

    bool cpu_populated() const noexcept {
        std::lock_guard<std::mutex> lock(cpu_mutex_);
        return cpu_populated_;
    }

    void ResetCpuPopulated() noexcept {
        std::lock_guard<std::mutex> lock(cpu_mutex_);
        cpu_populated_ = false;
    }

    // Populates the CPU buffer from the GPU texture if a caller explicitly
    // requests CPU BGRA pixels (e.g. GDI/CPU fallback or diagnostic pixel check).
    const std::uint8_t* EnsureCpuData() const;

    // Accepts this owned frame or SharedDesktopFrame wrappers that retain it.
    // Wrappers preserve the registered pixel allocation and geometry; no
    // derived layout is inspected. Keep the input owner alive while using the
    // borrowed result. Cropped or unrelated frames are not accepted.
    static const D3d11TextureDesktopFrame* FromDesktopFrame(
        webrtc::DesktopFrame* frame) noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    const LUID adapter_luid_;
    std::shared_ptr<std::mutex> device_context_mutex_;
    mutable std::mutex cpu_mutex_;
    mutable std::unique_ptr<std::uint8_t[]> cpu_data_;
    mutable bool cpu_populated_ = false;
};

// Capture-worker-owned 2-slot GPU BGRA texture pool + SharedDesktopFrame wrapper.
class D3d11DesktopFrameTexturePool final {
public:
    D3d11DesktopFrameTexturePool() = default;
    ~D3d11DesktopFrameTexturePool() { Clear(); }

    std::unique_ptr<webrtc::SharedDesktopFrame> CaptureTexture(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        LUID adapter_luid,
        const std::shared_ptr<std::mutex>& device_context_mutex,
        ID3D11Texture2D* source_texture,
        int width,
        int height,
        bool populate_cpu_mirror = false);

    void Clear() noexcept {
        for (auto& slot : slots_) {
            slot.frame.reset();
            slot.texture_frame = nullptr;
        }
    }

private:
    struct Slot {
        std::unique_ptr<webrtc::SharedDesktopFrame> frame;
        // Borrowed from frame's underlying owner, assigned only when creating
        // that owner. Pool reuse never needs a DesktopFrame downcast.
        D3d11TextureDesktopFrame* texture_frame = nullptr;
    };
    Slot slots_[2];
};

bool QueryD3d11AdapterLuid(ID3D11Device* device, LUID* out_luid) noexcept;

} // namespace livekit
