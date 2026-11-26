#include "d3d11_nv12_frame_buffer.h"

#include <algorithm>
#include <cstring>
#include <list>
#include <unordered_map>
#include <utility>

#include "api/make_ref_counted.h"
#include "libyuv/convert_argb.h"
#include "libyuv/rotate_argb.h"

namespace livekit {

namespace {

struct NativeFrameRegistry {
    std::mutex mutex;
    std::unordered_map<const webrtc::VideoFrameBuffer*, const D3d11Nv12FrameBuffer*> nv12;
    // SharedDesktopFrame::Wrap/Share retain the original DesktopFrame and
    // preserve its data pointer. The owned pixel allocation identifies that
    // original across any number of wrappers without guessing their type.
    std::unordered_map<const std::uint8_t*, const D3d11TextureDesktopFrame*> desktop;
};

NativeFrameRegistry& FrameRegistry() {
    // GPU frames may retire after other process statics during shutdown.
    // Entries are still removed by frame destructors; only the empty registry
    // itself has process lifetime, avoiding static destruction order hazards.
    static auto* const registry = new NativeFrameRegistry;
    return *registry;
}

std::atomic<std::uint64_t> g_next_texture_id{1};
std::atomic<std::uint64_t> g_direct_capture_conversions{0};
std::atomic<std::uint64_t> g_native_preview_imports{0};
std::atomic<std::uint64_t> g_gpu_frames_created{0};
std::atomic<std::uint64_t> g_gpu_scales_performed{0};
std::atomic<std::uint64_t> g_cpu_readbacks_performed{0};
std::atomic<std::uint64_t> g_pool_exhausted_drops{0};

bool IsValidVideoProcessorRotation(D3D11_VIDEO_PROCESSOR_ROTATION rotation) noexcept {
    switch (rotation) {
    case D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY:
    case D3D11_VIDEO_PROCESSOR_ROTATION_90:
    case D3D11_VIDEO_PROCESSOR_ROTATION_180:
    case D3D11_VIDEO_PROCESSOR_ROTATION_270:
        return true;
    default:
        return false;
    }
}

bool ToLibyuvRotation(D3D11_VIDEO_PROCESSOR_ROTATION rotation,
                      libyuv::RotationMode* out_mode) noexcept {
    if (!out_mode) return false;
    switch (rotation) {
    case D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY:
        *out_mode = libyuv::kRotate0;
        return true;
    case D3D11_VIDEO_PROCESSOR_ROTATION_90:
        *out_mode = libyuv::kRotate90;
        return true;
    case D3D11_VIDEO_PROCESSOR_ROTATION_180:
        *out_mode = libyuv::kRotate180;
        return true;
    case D3D11_VIDEO_PROCESSOR_ROTATION_270:
        *out_mode = libyuv::kRotate270;
        return true;
    default:
        return false;
    }
}

bool CreateConfiguredVideoProcessor(
    ID3D11VideoDevice* video_device,
    int in_w,
    int in_h,
    int out_w,
    int out_h,
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator>* out_enumerator,
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor>* out_processor,
    bool* out_rotation_supported,
    bool* out_format_unsupported) {
    if (out_rotation_supported) *out_rotation_supported = false;
    if (out_format_unsupported) *out_format_unsupported = false;
    if (!video_device || in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0 ||
        !out_enumerator || !out_processor) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputFrameRate.Numerator = 60;
    content_desc.InputFrameRate.Denominator = 1;
    content_desc.InputWidth = static_cast<UINT>(in_w);
    content_desc.InputHeight = static_cast<UINT>(in_h);
    content_desc.OutputFrameRate.Numerator = 60;
    content_desc.OutputFrameRate.Denominator = 1;
    content_desc.OutputWidth = static_cast<UINT>(out_w);
    content_desc.OutputHeight = static_cast<UINT>(out_h);
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    if (FAILED(video_device->CreateVideoProcessorEnumerator(&content_desc, enumerator.GetAddressOf())) ||
        !enumerator) {
        return false;
    }

    UINT bgra_flags = 0;
    UINT nv12_flags = 0;
    if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_flags)) ||
        (bgra_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12_flags)) ||
        (nv12_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        if (out_format_unsupported) *out_format_unsupported = true;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    if (FAILED(enumerator->GetVideoProcessorCaps(&caps))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(video_device->CreateVideoProcessor(enumerator.Get(), 0, processor.GetAddressOf())) ||
        !processor) {
        return false;
    }

    if (out_rotation_supported) {
        *out_rotation_supported =
            (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0;
    }
    *out_enumerator = std::move(enumerator);
    *out_processor = std::move(processor);
    return true;
}

} // namespace

class D3d11Nv12ScaleCache final {
public:
    D3d11Nv12ScaleCache(Microsoft::WRL::ComPtr<ID3D11Device> device,
                       LUID adapter_luid,
                       std::shared_ptr<std::mutex> device_context_mutex)
        : device_(std::move(device)),
          adapter_luid_(adapter_luid),
          device_context_mutex_(std::move(device_context_mutex)) {}

    std::shared_ptr<D3d11VideoProcessorConverter> Get(
        int crop_width, int crop_height, int output_width, int output_height) {
        const Dimensions dimensions{crop_width, crop_height, output_width, output_height};
        std::shared_ptr<D3d11VideoProcessorConverter> retired;
        std::lock_guard lock(mutex_);
        const auto entry = std::find_if(entries_.begin(), entries_.end(),
            [&dimensions](const Entry& current) { return current.dimensions == dimensions; });
        if (entry != entries_.end()) {
            entries_.splice(entries_.begin(), entries_, entry);
            return entry->converter;
        }

        auto converter = std::make_shared<D3d11VideoProcessorConverter>(
            device_, adapter_luid_, device_context_mutex_);
        entries_.push_front({dimensions, converter});
        if (entries_.size() > kMaxConfigurations) {
            // Release the evicted converter and its pool after unlocking.
            retired = std::move(entries_.back().converter);
            entries_.pop_back();
        }
        return converter;
    }

private:
    struct Dimensions {
        int crop_width;
        int crop_height;
        int output_width;
        int output_height;
        bool operator==(const Dimensions&) const = default;
    };
    struct Entry {
        Dimensions dimensions;
        std::shared_ptr<D3d11VideoProcessorConverter> converter;
    };

    static constexpr std::size_t kMaxConfigurations = 4;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    const LUID adapter_luid_;
    std::shared_ptr<std::mutex> device_context_mutex_;
    std::mutex mutex_;
    std::list<Entry> entries_;
};

bool QueryD3d11AdapterLuid(ID3D11Device* device, LUID* out_luid) noexcept {
    if (!device || !out_luid) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(dxgi_device.GetAddressOf()))) ||
        !dxgi_device) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) || !adapter) {
        return false;
    }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    *out_luid = desc.AdapterLuid;
    return true;
}

bool DxgiModeRotationToVideoProcessorRotation(
    DXGI_MODE_ROTATION dxgi_rotation,
    D3D11_VIDEO_PROCESSOR_ROTATION* out_rotation) noexcept {
    if (!out_rotation) return false;
    switch (dxgi_rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        *out_rotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
        return true;
    case DXGI_MODE_ROTATION_ROTATE90:
        *out_rotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
        return true;
    case DXGI_MODE_ROTATION_ROTATE180:
        *out_rotation = D3D11_VIDEO_PROCESSOR_ROTATION_180;
        return true;
    case DXGI_MODE_ROTATION_ROTATE270:
        *out_rotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;
        return true;
    default:
        return false;
    }
}

void RotatedDimensions(
    int raw_width,
    int raw_height,
    D3D11_VIDEO_PROCESSOR_ROTATION rotation,
    int* out_width,
    int* out_height) noexcept {
    const bool swap = rotation == D3D11_VIDEO_PROCESSOR_ROTATION_90 ||
                      rotation == D3D11_VIDEO_PROCESSOR_ROTATION_270;
    if (out_width) *out_width = swap ? raw_height : raw_width;
    if (out_height) *out_height = swap ? raw_width : raw_height;
}

D3d11Nv12FrameBuffer::Diagnostics D3d11Nv12FrameBuffer::GetDiagnostics() noexcept {
    return {
        g_gpu_frames_created.load(std::memory_order_relaxed),
        g_gpu_scales_performed.load(std::memory_order_relaxed),
        g_cpu_readbacks_performed.load(std::memory_order_relaxed),
        g_pool_exhausted_drops.load(std::memory_order_relaxed),
        g_direct_capture_conversions.load(std::memory_order_relaxed),
        g_native_preview_imports.load(std::memory_order_relaxed),
    };
}

void D3d11Nv12FrameBuffer::ResetDiagnostics() noexcept {
    g_gpu_frames_created.store(0, std::memory_order_relaxed);
    g_gpu_scales_performed.store(0, std::memory_order_relaxed);
    g_cpu_readbacks_performed.store(0, std::memory_order_relaxed);
    g_pool_exhausted_drops.store(0, std::memory_order_relaxed);
    g_direct_capture_conversions.store(0, std::memory_order_relaxed);
    g_native_preview_imports.store(0, std::memory_order_relaxed);
}

D3d11Nv12FrameBuffer::D3d11Nv12FrameBuffer(
    int width,
    int height,
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    HANDLE shared_handle,
    LUID adapter_luid,
    std::shared_ptr<D3d11Nv12TexturePool> owner_pool,
    std::size_t slot_index,
    std::uint64_t pool_epoch,
    std::shared_ptr<std::mutex> device_context_mutex)
    : width_(width),
      height_(height),
      device_(std::move(device)),
      texture_(std::move(texture)),
      shared_handle_(shared_handle),
      adapter_luid_(adapter_luid),
      owner_pool_(std::move(owner_pool)),
      slot_index_(slot_index),
      pool_epoch_(pool_epoch),
      device_context_mutex_(std::move(device_context_mutex)) {
    auto& registry = FrameRegistry();
    {
        std::lock_guard lock(registry.mutex);
        registry.nv12.emplace(this, this);
    }
    g_gpu_frames_created.fetch_add(1, std::memory_order_relaxed);
}

D3d11Nv12FrameBuffer::~D3d11Nv12FrameBuffer() {
    auto& registry = FrameRegistry();
    {
        std::lock_guard lock(registry.mutex);
        registry.nv12.erase(this);
    }
    if (owner_pool_) {
        owner_pool_->ReleaseSlot(slot_index_, pool_epoch_);
    }
}

const D3d11Nv12FrameBuffer* D3d11Nv12FrameBuffer::FromVideoFrameBuffer(
    const webrtc::VideoFrameBuffer* buffer) noexcept {
    if (!buffer || buffer->type() != Type::kNative) {
        return nullptr;
    }
    auto& registry = FrameRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.nv12.find(buffer);
    return found != registry.nv12.end() ? found->second : nullptr;
}

bool D3d11Nv12FrameBuffer::PreparePreview(lk_render_dx11_texture_view& view) const {
    if (!owner_pool_ || !shared_handle_ || !device_context_mutex_) return false;
    auto& pool = *owner_pool_;
    uint64_t allocation = 0;
    {
        std::lock_guard lock(pool.mutex_);
        if (pool_epoch_ != pool.epoch_ || slot_index_ >= pool.kPoolSize) return false;
        auto& slot = pool.slots_[slot_index_];
        unsigned reserved = 0;
        for (const auto& s : pool.slots_) {
            if (s.preview_reserved || std::any_of(s.readers.begin(), s.readers.end(),
                [](const auto& r) { return r.fence && r.fence->GetCompletedValue() < r.value; })) ++reserved;
        }
        if (!slot.preview_reserved && reserved >= 2) return false;
        slot.preview_reserved = true;
        allocation = slot.allocation_id;
    }
    std::lock_guard sync(pool.preview_sync_mutex_);
    if (pool.preview_sync_unavailable_) return false;
    std::lock_guard context(*device_context_mutex_);
    if (!pool.preview_ready_) {
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
        device_->GetImmediateContext(immediate.GetAddressOf());
        if (FAILED(device_.As(&device5)) || !immediate ||
            FAILED(immediate.As(&pool.preview_context_)) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&pool.preview_ready_))) ||
            FAILED(pool.preview_ready_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &pool.preview_ready_handle_))) {
            pool.preview_sync_unavailable_ = true;
            return false;
        }
    }
    const auto sequence = ++pool.preview_sequence_;
    if (FAILED(pool.preview_context_->Signal(pool.preview_ready_.Get(), sequence))) return false;
    pool.preview_context_->Flush();
    view = {sizeof(view), uint32_t(width_), uint32_t(height_), adapter_luid_.LowPart,
        adapter_luid_.HighPart, 0, reinterpret_cast<uintptr_t>(shared_handle_),
        reinterpret_cast<uintptr_t>(pool.preview_ready_handle_), sequence, allocation};
    return true;
}

void D3d11Nv12FrameBuffer::RecordPreviewRead(const lk_render_dx11_retirement& ticket) const {
    auto& pool = *owner_pool_;
    std::lock_guard lock(pool.mutex_);
    if (pool_epoch_ != pool.epoch_ || slot_index_ >= pool.kPoolSize) return;
    auto& slot = pool.slots_[slot_index_];
    if (!ticket.fence || !ticket.value) {
        slot.discard_texture = true;
        return;
    }
    g_native_preview_imports.fetch_add(1, std::memory_order_relaxed);
    auto* fence = reinterpret_cast<ID3D11Fence*>(ticket.fence);
    for (auto& read : slot.readers) {
        if (read.fence.Get() == fence) {
            read.value = (std::max)(read.value, ticket.value);
            return;
        }
    }
    for (auto& read : slot.readers) {
        if (!read.fence || read.fence->GetCompletedValue() >= read.value) {
            read.fence = fence;
            read.value = ticket.value;
            return;
        }
    }
    slot.discard_texture = true;
}

std::shared_ptr<const D3d11Nv12FrameBuffer> D3d11Nv12FrameBuffer::TryLeaseForEncoder() const {
    if (!owner_pool_ || owner_pool_->encoder_lease_active_.exchange(true, std::memory_order_acq_rel))
        return {};
    return {this, [pool = owner_pool_, owner = webrtc::scoped_refptr<const D3d11Nv12FrameBuffer>(this)]
        (const D3d11Nv12FrameBuffer*) mutable {
            owner = nullptr;
            pool->encoder_lease_active_.store(false, std::memory_order_release);
        }};
}

webrtc::scoped_refptr<webrtc::NV12BufferInterface> D3d11Nv12FrameBuffer::ToNV12() const {
    std::lock_guard<std::mutex> lock(readback_mutex_);
    if (cached_nv12_) {
        return cached_nv12_;
    }
    auto nv12 = webrtc::NV12Buffer::Create(width_, height_);
    if (owner_pool_ && owner_pool_->ReadbackToNV12(texture_.Get(), width_, height_, nv12.get())) {
        g_cpu_readbacks_performed.fetch_add(1, std::memory_order_relaxed);
        cached_nv12_ = nv12;
        return cached_nv12_;
    }
    // Fallback if pool is unavailable: direct temporary staging texture readback
    if (device_ && texture_) {
        std::unique_lock<std::mutex> ctx_lock;
        if (device_context_mutex_) {
            ctx_lock = std::unique_lock<std::mutex>(*device_context_mutex_);
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())) && staging) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
            device_->GetImmediateContext(ctx.GetAddressOf());
            if (ctx) {
                ctx->CopyResource(staging.Get(), texture_.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                    const auto* src_y = static_cast<const std::uint8_t*>(mapped.pData);
                    for (int r = 0; r < height_; ++r) {
                        std::memcpy(nv12->MutableDataY() + r * nv12->StrideY(),
                                    src_y + r * mapped.RowPitch,
                                    width_);
                    }
                    const auto* src_uv = src_y + static_cast<std::size_t>(mapped.RowPitch) * desc.Height;
                    const int uv_h = (height_ + 1) / 2;
                    const int uv_w_bytes = ((width_ + 1) / 2) * 2;
                    for (int r = 0; r < uv_h; ++r) {
                        std::memcpy(nv12->MutableDataUV() + r * nv12->StrideUV(),
                                    src_uv + r * mapped.RowPitch,
                                    uv_w_bytes);
                    }
                    ctx->Unmap(staging.Get(), 0);
                    g_cpu_readbacks_performed.fetch_add(1, std::memory_order_relaxed);
                    cached_nv12_ = nv12;
                    return cached_nv12_;
                }
            }
        }
    }
    nv12->InitializeData();
    cached_nv12_ = nv12;
    return cached_nv12_;
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> D3d11Nv12FrameBuffer::ToI420() {
    auto nv12 = ToNV12();
    std::lock_guard<std::mutex> lock(readback_mutex_);
    if (!cached_i420_ && nv12) {
        cached_i420_ = nv12->ToI420();
    }
    return cached_i420_;
}

const webrtc::I420BufferInterface* D3d11Nv12FrameBuffer::GetI420() const {
    auto nv12 = ToNV12();
    std::lock_guard<std::mutex> lock(readback_mutex_);
    if (!cached_i420_ && nv12) {
        cached_i420_ = nv12->ToI420();
    }
    return cached_i420_.get();
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3d11Nv12FrameBuffer::GetMappedFrameBuffer(
    webrtc::ArrayView<Type> types) {
    for (Type t : types) {
        if (t == Type::kNV12) {
            return ToNV12();
        }
    }
    for (Type t : types) {
        if (t == Type::kI420) {
            return ToI420();
        }
    }
    return nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3d11Nv12FrameBuffer::CropAndScale(
    int offset_x,
    int offset_y,
    int crop_width,
    int crop_height,
    int scaled_width,
    int scaled_height) {
    const int even_w = (std::max)(2, scaled_width & ~1);
    const int even_h = (std::max)(2, scaled_height & ~1);
    if (offset_x == 0 && offset_y == 0 && crop_width == width_ && crop_height == height_ &&
        even_w == width_ && even_h == height_) {
        return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(this);
    }
    if (device_ && texture_ && crop_width > 0 && crop_height > 0) {
        auto cache = scale_cache_.lock();
        if (!cache && owner_pool_) cache = owner_pool_->GetScaleCache(pool_epoch_);
        auto scaler = cache
            ? cache->Get(crop_width, crop_height, even_w, even_h)
            : std::make_shared<D3d11VideoProcessorConverter>(device_, adapter_luid_, device_context_mutex_);
        auto scaled_gpu = scaler->ConvertAndScale(
            texture_.Get(), crop_width, crop_height, even_w, even_h, offset_x, offset_y);
        if (scaled_gpu) {
            // Derived frames share the cache while the capture pool owns it.
            scaled_gpu->scale_cache_ = cache;
            g_gpu_scales_performed.fetch_add(1, std::memory_order_relaxed);
            return scaled_gpu;
        }
    }
    auto nv12 = ToNV12();
    if (nv12) {
        return nv12->CropAndScale(offset_x, offset_y, crop_width, crop_height, even_w, even_h);
    }
    return nullptr;
}

// ============================================================================
// D3d11Nv12TexturePool
// ============================================================================

D3d11Nv12TexturePool::D3d11Nv12TexturePool(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    LUID adapter_luid,
    std::shared_ptr<std::mutex> device_context_mutex)
    : device_(std::move(device)),
      adapter_luid_(adapter_luid),
      device_context_mutex_(std::move(device_context_mutex)) {}

D3d11Nv12TexturePool::~D3d11Nv12TexturePool() {
    Clear();
    if (preview_ready_handle_) CloseHandle(preview_ready_handle_);
}

void D3d11Nv12TexturePool::Clear() noexcept {
    std::shared_ptr<D3d11Nv12ScaleCache> retired_cache;
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    retired_cache = std::move(scale_cache_);
    width_ = 0;
    height_ = 0;
    for (auto& slot : slots_) {
        slot = {};
    }
    staging_.Reset();
    staging_width_ = 0;
    staging_height_ = 0;
}

std::shared_ptr<D3d11Nv12ScaleCache> D3d11Nv12TexturePool::GetScaleCache(std::uint64_t epoch) {
    std::lock_guard lock(mutex_);
    if (epoch != epoch_ || !device_context_mutex_) return nullptr;
    if (!scale_cache_) {
        scale_cache_ = std::make_shared<D3d11Nv12ScaleCache>(device_, adapter_luid_, device_context_mutex_);
    }
    return scale_cache_;
}

void D3d11Nv12TexturePool::ReleaseSlot(std::size_t slot_index, std::uint64_t epoch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < kPoolSize && epoch == epoch_) {
        slots_[slot_index].in_use = false;
        slots_[slot_index].preview_reserved = false;
    }
}

webrtc::scoped_refptr<D3d11Nv12FrameBuffer> D3d11Nv12TexturePool::Acquire(int width, int height) {
    const int even_w = (std::max)(2, width & ~1);
    const int even_h = (std::max)(2, height & ~1);
    if (!device_ || even_w <= 0 || even_h <= 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (width_ != even_w || height_ != even_h) {
        ++epoch_;
        width_ = even_w;
        height_ = even_h;
        for (auto& slot : slots_) {
            slot = {};
        }
    }

    for (std::size_t i = 0; i < kPoolSize; ++i) {
        auto& slot = slots_[i];
        if (slot.in_use) continue;
        bool gpu_reading = false;
        for (auto& read : slot.readers) {
            if (!read.fence) continue;
            const auto completed = read.fence->GetCompletedValue();
            if (completed == UINT64_MAX) slot.discard_texture = true;
            if (completed >= read.value) read = {};
            else gpu_reading = true;
        }
        if (slot.discard_texture) slot = {}; // Retire this allocation, never overwrite it.
        else if (gpu_reading) continue;

        if (!slot.texture || slot.width != even_w || slot.height != even_h) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(even_w);
            desc.Height = static_cast<UINT>(even_h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, tex.GetAddressOf())) || !tex) {
                return nullptr;
            }
            HANDLE shared_handle = nullptr;
            Microsoft::WRL::ComPtr<IDXGIResource> dxgi_res;
            if (SUCCEEDED(tex.As(&dxgi_res)) && dxgi_res) {
                dxgi_res->GetSharedHandle(&shared_handle);
            }
            slot.texture = std::move(tex);
            slot.shared_handle = shared_handle;
            slot.allocation_id = g_next_texture_id.fetch_add(1, std::memory_order_relaxed);
            slot.width = even_w;
            slot.height = even_h;
        }

        slot.in_use = true;
        return webrtc::make_ref_counted<D3d11Nv12FrameBuffer>(
            even_w,
            even_h,
            device_,
            slot.texture,
            slot.shared_handle,
            adapter_luid_,
            shared_from_this(),
            i,
            epoch_,
            device_context_mutex_);
    }

    g_pool_exhausted_drops.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

bool D3d11Nv12TexturePool::ReadbackToNV12(
    ID3D11Texture2D* texture,
    int width,
    int height,
    webrtc::NV12Buffer* dst) {
    if (!device_ || !texture || !dst || width <= 0 || height <= 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!staging_ || staging_width_ != width || staging_height_ != height) {
            staging_.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            desc.Width = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf())) || !staging_) {
                return false;
            }
            staging_width_ = width;
            staging_height_ = height;
        }
        staging = staging_;
    }

    std::unique_lock<std::mutex> ctx_lock;
    if (device_context_mutex_) {
        ctx_lock = std::unique_lock<std::mutex>(*device_context_mutex_);
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(ctx.GetAddressOf());
    if (!ctx) return false;

    D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    const auto* src_y = static_cast<const std::uint8_t*>(mapped.pData);
    for (int r = 0; r < height; ++r) {
        std::memcpy(dst->MutableDataY() + r * dst->StrideY(),
                    src_y + r * mapped.RowPitch,
                    width);
    }
    const auto* src_uv = src_y + static_cast<std::size_t>(mapped.RowPitch) * height;
    const int uv_h = (height + 1) / 2;
    const int uv_w_bytes = ((width + 1) / 2) * 2;
    for (int r = 0; r < uv_h; ++r) {
        std::memcpy(dst->MutableDataUV() + r * dst->StrideUV(),
                    src_uv + r * mapped.RowPitch,
                    uv_w_bytes);
    }
    ctx->Unmap(staging.Get(), 0);
    return true;
}

// ============================================================================
// D3d11VideoProcessorConverter
// ============================================================================

D3d11VideoProcessorConverter::D3d11VideoProcessorConverter(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    LUID adapter_luid,
    std::shared_ptr<std::mutex> device_context_mutex)
    : device_(std::move(device)),
      adapter_luid_(adapter_luid),
      device_context_mutex_(std::move(device_context_mutex)),
      pool_(std::make_shared<D3d11Nv12TexturePool>(device_, adapter_luid_, device_context_mutex_)) {
    if (device_) {
        device_->GetImmediateContext(context_.GetAddressOf());
    }
}

D3d11VideoProcessorConverter::~D3d11VideoProcessorConverter() {
    Clear();
}

void D3d11VideoProcessorConverter::Clear() noexcept {
    if (pool_) {
        pool_->Clear();
    }
    bindable_input_.Reset();
    bindable_in_w_ = 0;
    bindable_in_h_ = 0;
    bindable_in_format_ = DXGI_FORMAT_UNKNOWN;
    processor_.Reset();
    enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    enum_in_w_ = 0;
    enum_in_h_ = 0;
    enum_out_w_ = 0;
    enum_out_h_ = 0;
    rotation_supported_ = false;
}

bool D3d11VideoProcessorConverter::ProbeSupport(
    ID3D11Device* device,
    int src_width,
    int src_height,
    int dst_width,
    int dst_height,
    D3D11_VIDEO_PROCESSOR_ROTATION rotation) {
    if (!device || src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0 ||
        !IsValidVideoProcessorRotation(rotation)) {
        return false;
    }
    const int even_dst_w = (std::max)(2, dst_width & ~1);
    const int even_dst_h = (std::max)(2, dst_height & ~1);

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                      reinterpret_cast<void**>(video_device.GetAddressOf()))) ||
        !video_device || !context ||
        FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext),
                                       reinterpret_cast<void**>(video_context.GetAddressOf()))) ||
        !video_context) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    bool rotation_supported = false;
    if (!CreateConfiguredVideoProcessor(
            video_device.Get(), src_width, src_height, even_dst_w, even_dst_h,
            &enumerator, &processor, &rotation_supported, nullptr)) {
        return false;
    }
    if (rotation != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY && !rotation_supported) {
        return false;
    }
    return true;
}

bool D3d11VideoProcessorConverter::EnsureProcessorLocked(int in_w, int in_h, int out_w, int out_h) {
    if (!supported_ || !device_ || !context_) return false;
    if (!initialized_) {
        initialized_ = true;
        if (FAILED(device_.As(&video_device_)) || !video_device_ ||
            FAILED(context_.As(&video_context_)) || !video_context_) {
            supported_ = false;
            return false;
        }
    }

    if (processor_ && enumerator_ &&
        enum_in_w_ == in_w && enum_in_h_ == in_h &&
        enum_out_w_ == out_w && enum_out_h_ == out_h) {
        return true;
    }

    processor_.Reset();
    enumerator_.Reset();
    rotation_supported_ = false;

    bool format_unsupported = false;
    if (!CreateConfiguredVideoProcessor(
            video_device_.Get(), in_w, in_h, out_w, out_h,
            &enumerator_, &processor_, &rotation_supported_, &format_unsupported)) {
        if (format_unsupported) {
            supported_ = false;
        }
        return false;
    }

    video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    // Input sRGB/BGRA full range -> Output BT.709 / studio nominal range NV12
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs{};
    input_cs.Usage = 0;
    input_cs.RGB_Range = 0; // Full range [0, 255]
    input_cs.YCbCr_Matrix = 1; // BT.709
    input_cs.YCbCr_xvYCC = 0;
    input_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs{};
    output_cs.Usage = 0;
    output_cs.RGB_Range = 0;
    output_cs.YCbCr_Matrix = 1; // BT.709 (matches libyuv ARGBToNV12 / WebRTC NV12 defaults)
    output_cs.YCbCr_xvYCC = 0;
    output_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_cs);

    enum_in_w_ = in_w;
    enum_in_h_ = in_h;
    enum_out_w_ = out_w;
    enum_out_h_ = out_h;
    return true;
}

webrtc::scoped_refptr<D3d11Nv12FrameBuffer> D3d11VideoProcessorConverter::ConvertAndScale(
    ID3D11Texture2D* src_texture,
    int src_width,
    int src_height,
    int dst_width,
    int dst_height,
    int offset_x,
    int offset_y,
    bool direct_input_only,
    D3D11_VIDEO_PROCESSOR_ROTATION rotation) {
    if (!src_texture || src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0 ||
        !pool_ || !IsValidVideoProcessorRotation(rotation)) {
        return nullptr;
    }
    const int even_dst_w = (std::max)(2, dst_width & ~1);
    const int even_dst_h = (std::max)(2, dst_height & ~1);

    auto dst_buffer = pool_->Acquire(even_dst_w, even_dst_h);
    if (!dst_buffer) {
        return nullptr;
    }

    std::unique_lock<std::mutex> ctx_lock;
    if (device_context_mutex_) {
        ctx_lock = std::unique_lock<std::mutex>(*device_context_mutex_);
    }

    D3D11_TEXTURE2D_DESC src_desc{};
    src_texture->GetDesc(&src_desc);

    if (!EnsureProcessorLocked(src_width, src_height, even_dst_w, even_dst_h)) {
        return nullptr;
    }
    if (rotation != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY && !rotation_supported_) {
        return nullptr;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view_desc{};
    in_view_desc.FourCC = 0;
    in_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_view_desc.Texture2D.MipSlice = 0;
    in_view_desc.Texture2D.ArraySlice = 0;

    int eff_offset_x = offset_x;
    int eff_offset_y = offset_y;
    ID3D11Texture2D* input_tex_for_view = src_texture;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> in_view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            input_tex_for_view, enumerator_.Get(), &in_view_desc, in_view.GetAddressOf())) ||
        !in_view) {
        if (direct_input_only) return nullptr;
        if (!bindable_input_ || bindable_in_w_ != src_width || bindable_in_h_ != src_height ||
            bindable_in_format_ != src_desc.Format) {
            bindable_input_.Reset();
            D3D11_TEXTURE2D_DESC bdesc{};
            bdesc.Width = static_cast<UINT>(src_width);
            bdesc.Height = static_cast<UINT>(src_height);
            bdesc.MipLevels = 1;
            bdesc.ArraySize = 1;
            bdesc.Format = src_desc.Format;
            bdesc.SampleDesc = {1, 0};
            bdesc.Usage = D3D11_USAGE_DEFAULT;
            bdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            bdesc.CPUAccessFlags = 0;
            bdesc.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&bdesc, nullptr, bindable_input_.GetAddressOf())) ||
                !bindable_input_) {
                return nullptr;
            }
            bindable_in_w_ = src_width;
            bindable_in_h_ = src_height;
            bindable_in_format_ = src_desc.Format;
        }
        D3D11_BOX box{
            static_cast<UINT>(offset_x),
            static_cast<UINT>(offset_y),
            0,
            static_cast<UINT>(offset_x + src_width),
            static_cast<UINT>(offset_y + src_height),
            1};
        context_->CopySubresourceRegion(
            bindable_input_.Get(), 0, 0, 0, 0, src_texture, 0, &box);
        eff_offset_x = 0;
        eff_offset_y = 0;
        input_tex_for_view = bindable_input_.Get();
        if (FAILED(video_device_->CreateVideoProcessorInputView(
                input_tex_for_view, enumerator_.Get(), &in_view_desc, in_view.GetAddressOf())) ||
            !in_view) {
            return nullptr;
        }
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
    out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_view_desc.Texture2D.MipSlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> out_view;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(
            dst_buffer->texture(), enumerator_.Get(), &out_view_desc, out_view.GetAddressOf())) ||
        !out_view) {
        return nullptr;
    }

    // Configure stream color space according to source format (BGRA vs NV12)
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs{};
    input_cs.Usage = 0;
    if (src_desc.Format == DXGI_FORMAT_NV12) {
        input_cs.RGB_Range = 0;
        input_cs.YCbCr_Matrix = 1;
        input_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    } else {
        input_cs.RGB_Range = 0;
        input_cs.YCbCr_Matrix = 1;
        input_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    }
    video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_cs);

    RECT src_rect{
        static_cast<LONG>(eff_offset_x),
        static_cast<LONG>(eff_offset_y),
        static_cast<LONG>(eff_offset_x + src_width),
        static_cast<LONG>(eff_offset_y + src_height)};
    RECT dst_rect{0, 0, static_cast<LONG>(even_dst_w), static_cast<LONG>(even_dst_h)};
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src_rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst_rect);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &dst_rect);
    if (rotation != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY) {
        video_context_->VideoProcessorSetStreamRotation(processor_.Get(), 0, TRUE, rotation);
    } else if (rotation_supported_) {
        video_context_->VideoProcessorSetStreamRotation(
            processor_.Get(), 0, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream_data{};
    stream_data.Enable = TRUE;
    stream_data.OutputIndex = 0;
    stream_data.InputFrameOrField = 0;
    stream_data.PastFrames = 0;
    stream_data.FutureFrames = 0;
    stream_data.pInputSurface = in_view.Get();

    if (FAILED(video_context_->VideoProcessorBlt(
            processor_.Get(), out_view.Get(), 0, 1, &stream_data))) {
        return nullptr;
    }

    // Flush the command buffer so cross-device shared resource consumers see the completed blit.
    context_->Flush();
    return dst_buffer;
}

// ============================================================================
// D3d11TextureDesktopFrame & D3d11DesktopFrameTexturePool
// ============================================================================

D3d11TextureDesktopFrame::D3d11TextureDesktopFrame(
    webrtc::DesktopSize size,
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    LUID adapter_luid,
    std::shared_ptr<std::mutex> device_context_mutex,
    D3D11_VIDEO_PROCESSOR_ROTATION rotation,
    int texture_width,
    int texture_height)
    : webrtc::DesktopFrame(
          size,
          size.width() * kBytesPerPixel,
          webrtc::FOURCC_ARGB,
          new std::uint8_t[
              static_cast<std::size_t>(std::max(1, size.width())) *
              static_cast<std::size_t>(std::max(1, size.height())) * kBytesPerPixel],
          nullptr),
      device_(std::move(device)),
      texture_(std::move(texture)),
      adapter_luid_(adapter_luid),
      device_context_mutex_(std::move(device_context_mutex)),
      rotation_(IsValidVideoProcessorRotation(rotation) ? rotation
                                                        : D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY),
      texture_width_(texture_ ? (texture_width > 0 ? texture_width : size.width()) : 0),
      texture_height_(texture_ ? (texture_height > 0 ? texture_height : size.height()) : 0),
      cpu_data_(data_) {
    set_capturer_id(kD3d11TextureDesktopFrameCapturerIdBit);
    auto& registry = FrameRegistry();
    std::lock_guard lock(registry.mutex);
    registry.desktop.emplace(data_, this);
}

D3d11TextureDesktopFrame::~D3d11TextureDesktopFrame() {
    auto& registry = FrameRegistry();
    std::lock_guard lock(registry.mutex);
    registry.desktop.erase(data_);
}

const D3d11TextureDesktopFrame* D3d11TextureDesktopFrame::FromDesktopFrame(
    webrtc::DesktopFrame* frame) noexcept {
    if (!frame || (frame->capturer_id() & kD3d11TextureDesktopFrameCapturerIdBit) == 0) {
        return nullptr;
    }
    auto& registry = FrameRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.desktop.find(frame->data());
    if (found == registry.desktop.end()) return nullptr;
    const auto* candidate = found->second;
    if (!frame->size().equals(candidate->size()) || frame->stride() != candidate->stride() ||
        frame->pixel_format() != candidate->pixel_format()) return nullptr;
    return candidate;
}

const std::uint8_t* D3d11TextureDesktopFrame::EnsureCpuData() const {
    std::lock_guard<std::mutex> lock(cpu_mutex_);
    if (!cpu_populated_ && prepared_) {
        const auto pixels = prepared_->ToNV12();
        cpu_populated_ = pixels && libyuv::NV12ToARGBMatrix(pixels->DataY(), pixels->StrideY(),
            pixels->DataUV(), pixels->StrideUV(), cpu_data_.get(), stride(),
            &libyuv::kYuvH709Constants, size().width(), size().height()) == 0;
        return cpu_populated_ ? data_ : nullptr;
    }
    if (cpu_populated_) {
        return data_;
    }
    if (!device_ || !texture_) {
        return nullptr;
    }
    const int raw_w = texture_width_ > 0 ? texture_width_ : size().width();
    const int raw_h = texture_height_ > 0 ? texture_height_ : size().height();
    int expected_w = 0;
    int expected_h = 0;
    RotatedDimensions(raw_w, raw_h, rotation_, &expected_w, &expected_h);
    libyuv::RotationMode libyuv_rotation = libyuv::kRotate0;
    if (raw_w <= 0 || raw_h <= 0 || expected_w != size().width() || expected_h != size().height() ||
        !ToLibyuvRotation(rotation_, &libyuv_rotation)) {
        return nullptr;
    }
    std::unique_lock<std::mutex> ctx_lock;
    if (device_context_mutex_) {
        ctx_lock = std::unique_lock<std::mutex>(*device_context_mutex_);
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture_->GetDesc(&desc);
    desc.Width = static_cast<UINT>(raw_w);
    desc.Height = static_cast<UINT>(raw_h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())) && staging) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        device_->GetImmediateContext(ctx.GetAddressOf());
        if (ctx) {
            D3D11_BOX box{0, 0, 0, static_cast<UINT>(raw_w), static_cast<UINT>(raw_h), 1};
            ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture_.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                if (rotation_ == D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY) {
                    for (int r = 0; r < raw_h; ++r) {
                        std::memcpy(cpu_data_.get() + r * stride(),
                                    static_cast<const std::uint8_t*>(mapped.pData) + r * mapped.RowPitch,
                                    static_cast<std::size_t>(raw_w) * kBytesPerPixel);
                    }
                    cpu_populated_ = true;
                } else {
                    cpu_populated_ = libyuv::ARGBRotate(
                        static_cast<const std::uint8_t*>(mapped.pData),
                        static_cast<int>(mapped.RowPitch),
                        cpu_data_.get(),
                        stride(),
                        raw_w,
                        raw_h,
                        libyuv_rotation) == 0;
                }
                ctx->Unmap(staging.Get(), 0);
            }
        }
    }
    return cpu_populated_ ? data_ : nullptr;
}

std::unique_ptr<webrtc::SharedDesktopFrame> D3d11DesktopFrameTexturePool::CaptureTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    LUID adapter_luid,
    const std::shared_ptr<std::mutex>& device_context_mutex,
    ID3D11Texture2D* source_texture,
    int width,
    int height,
    bool populate_cpu_mirror,
    bool prepare_nv12,
    D3D11_VIDEO_PROCESSOR_ROTATION rotation) {
    if (!device || !context || !source_texture || width <= 0 || height <= 0 ||
        !IsValidVideoProcessorRotation(rotation)) {
        return nullptr;
    }
    int out_width = 0;
    int out_height = 0;
    RotatedDimensions(width, height, rotation, &out_width, &out_height);
    const webrtc::DesktopSize target_size(out_width, out_height);
    for (auto& slot : slots_) {
        if (slot.frame && slot.frame->IsShared()) {
            continue;
        }
        D3d11TextureDesktopFrame* tex_frame = nullptr;
        if (slot.frame && slot.frame->size().equals(target_size)) {
            tex_frame = slot.texture_frame;
        }
        if (tex_frame) tex_frame->prepared_ = nullptr;
        if (prepare_nv12 && !(out_width & 1) && !(out_height & 1)) {
            if (!converter_ || converter_->device() != device)
                converter_ = std::make_unique<D3d11VideoProcessorConverter>(
                    device, adapter_luid, device_context_mutex);
            // This command is submitted and flushed on the capture device while
            // the WGC frame / DXGI duplication surface still belongs to us.
            if (auto native = converter_->ConvertAndScale(source_texture, width, height,
                    out_width, out_height, 0, 0, true, rotation)) {
                if (!tex_frame || tex_frame->device() != device) {
                    auto owned = std::make_unique<D3d11TextureDesktopFrame>(target_size, device,
                        nullptr, adapter_luid, device_context_mutex);
                    tex_frame = owned.get();
                    slot.frame = webrtc::SharedDesktopFrame::Wrap(std::move(owned));
                    slot.texture_frame = tex_frame;
                }
                g_direct_capture_conversions.fetch_add(1, std::memory_order_relaxed);
                tex_frame->texture_.Reset();
                tex_frame->texture_width_ = 0;
                tex_frame->texture_height_ = 0;
                tex_frame->rotation_ = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
                tex_frame->prepared_ = std::move(native);
                tex_frame->ResetCpuPopulated();
                if (populate_cpu_mirror && !tex_frame->EnsureCpuData()) return nullptr;
                auto shared = slot.frame->Share();
                shared->set_capturer_id(slot.frame->capturer_id() | kD3d11TextureDesktopFrameCapturerIdBit);
                return shared;
            }
        }
        if (!tex_frame || tex_frame->device() != device || !tex_frame->texture() ||
            tex_frame->texture_width() != width || tex_frame->texture_height() != height) {
            D3D11_TEXTURE2D_DESC desc{};
            source_texture->GetDesc(&desc);
            desc.Width = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> gpu_copy;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, gpu_copy.GetAddressOf())) || !gpu_copy) {
                return nullptr;
            }
            auto owned_frame = std::make_unique<D3d11TextureDesktopFrame>(
                target_size, device, std::move(gpu_copy), adapter_luid,
                device_context_mutex, rotation, width, height);
            tex_frame = owned_frame.get();
            slot.frame = webrtc::SharedDesktopFrame::Wrap(std::move(owned_frame));
            slot.texture_frame = tex_frame;
        } else {
            tex_frame->rotation_ = rotation;
        }

        {
            std::unique_lock<std::mutex> ctx_lock;
            if (device_context_mutex) {
                ctx_lock = std::unique_lock<std::mutex>(*device_context_mutex);
            }
            D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
            context->CopySubresourceRegion(
                tex_frame->texture(), 0, 0, 0, 0, source_texture, 0, &box);
        }
        tex_frame->ResetCpuPopulated();
        if (populate_cpu_mirror && (!tex_frame->EnsureCpuData() || !tex_frame->cpu_populated())) {
            return nullptr;
        }
        auto shared = slot.frame->Share();
        shared->set_capturer_id(slot.frame->capturer_id() | kD3d11TextureDesktopFrameCapturerIdBit);
        return shared;
    }
    return nullptr;
}

} // namespace livekit
