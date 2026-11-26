#include "render/api/render_backend_api.h"
#include "render/api/render_backend_dx11_native.h"
#include "render/api/render_backend_dx11_texture.h"
#include <d3d11_4.h>
#include <array>
#include "render/api/render_backend_module_info.h"
#include "ui/dx11/dx11_renderer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <thread>

using livekit::dx11::Dx11Renderer;
using livekit::dx11::PixelFormatType;
using Microsoft::WRL::ComPtr;
namespace {
struct GpuFrame {
    uint32_t format = 0, width = 0, height = 0, count = 0, rotation = 0;
    livekit::render::RenderColorSpace color;
    bool overlay = false;
    ComPtr<ID3D11Texture2D> textures[3];
    ComPtr<ID3D11ShaderResourceView> views[3];
};
struct Resource {
    uint64_t generation = 0;
    GpuFrame active, spare;
    struct Imported {
        uint64_t allocation = 0;
        GpuFrame frame;
        ComPtr<ID3D11Fence> ready;
    };
    std::array<Imported, 4> imports{};
    size_t next_import = 0;
    ComPtr<ID3D11Fence> retired;
    uint64_t last_ticket = 0;
    bool imported_active = false;
};
template<class F> lk_render_result Guard(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return LK_RENDER_OUT_OF_MEMORY; }
    catch (...) { return LK_RENDER_FATAL; }
}
constexpr uint64_t kVideoFormats = LK_RENDER_FORMAT_BIT(LK_RENDER_I420) |
    LK_RENDER_FORMAT_BIT(LK_RENDER_NV12) | LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8);
bool ValidColor(const float* rgba) {
    for (int i = 0; i < 4; ++i) if (!std::isfinite(rgba[i]) || rgba[i] < 0 || rgba[i] > 1) return false;
    return true;
}
}
struct lk_render_device {
    Dx11Renderer renderer;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    LUID adapter{};
    std::map<uint64_t, Resource> resources;
    std::thread::id owner = std::this_thread::get_id();
    uint64_t surface_generation = 0;
    uint32_t max_resources = 0;
#if defined(LIVEKIT_DX11_MODULE_INSPECTION)
    lk_render_dx11_inspection_hooks_v1 instrumentation_hooks{};
#endif
};
namespace {
bool OnOwner(const lk_render_device* d) { return d && d->owner == std::this_thread::get_id(); }
lk_render_result CheckTarget(lk_render_device* d, const lk_render_frame_target* t) {
    if (!OnOwner(d) || !t || t->struct_size != sizeof(*t) || t->gl_draw_fbo || t->context_generation) return LK_RENDER_INVALID_ARGUMENT;
    if (t->surface_generation != d->surface_generation) return LK_RENDER_SURFACE_LOST;
    if (!t->pixel_width || !t->pixel_height) return LK_RENDER_NOT_READY;
    if (t->pixel_width > 16384 || t->pixel_height > 16384) return LK_RENDER_INVALID_ARGUMENT;
    if (!d->renderer.is_initialized()) return LK_RENDER_DEVICE_LOST;
    return LK_RENDER_OK;
}
lk_render_result LK_RENDER_CALL Describe(uint32_t size, lk_render_backend_info* out) noexcept {
    if (!out || size < sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    const lk_render_backend_info info{sizeof(info), LK_RENDER_BACKEND_DX11, LK_RENDER_SURFACE_WIN32,
        sizeof(void*), kVideoFormats, LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8)};
    *out = info;
    return LK_RENDER_OK;
}
lk_render_result LK_RENDER_CALL Create(const lk_render_create_info* info, lk_render_device** out) noexcept {
    if (!out) return LK_RENDER_INVALID_ARGUMENT;
    *out = nullptr;
    return Guard([&]() -> lk_render_result {
        if (!info || info->struct_size != sizeof(*info) || info->surface_kind != LK_RENDER_SURFACE_WIN32 ||
            !info->binding || info->binding_size != sizeof(lk_render_win32_binding) ||
            !info->surface_generation || !info->max_resources || info->max_resources > 4096) return LK_RENDER_INVALID_ARGUMENT;
        const auto& binding = *static_cast<const lk_render_win32_binding*>(info->binding);
        if (binding.struct_size != sizeof(binding) || binding.reserved || !binding.hwnd) return LK_RENDER_INVALID_ARGUMENT;
        RECT rect{};
        const auto hwnd = reinterpret_cast<HWND>(binding.hwnd);
        if (!IsWindow(hwnd) || !GetClientRect(hwnd, &rect)) return LK_RENDER_SURFACE_LOST;
        if (rect.right <= 0 || rect.bottom <= 0) return LK_RENDER_NOT_READY;
        auto device = std::make_unique<lk_render_device>();
        device->surface_generation = info->surface_generation;
        device->max_resources = info->max_resources;
        if (!device->renderer.Initialize(hwnd, rect.right, rect.bottom)) return LK_RENDER_DEVICE_LOST;
        auto* native = device->renderer.device();
        native->QueryInterface(IID_PPV_ARGS(&device->device5));
        device->renderer.context()->QueryInterface(IID_PPV_ARGS(&device->context4));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description)))
            device->adapter = description.AdapterLuid;
        *out = device.release();
        return LK_RENDER_OK;
    });
}
bool RetireImported(lk_render_device* d, Resource& r) noexcept {
    if (!r.imported_active) return true;
    if (FAILED(d->context4->Signal(r.retired.Get(), r.last_ticket))) return false;
    d->context4->Flush();
    r.imported_active = false;
    r.active = {};
    return true;
}

void LK_RENDER_CALL Destroy(lk_render_device* d, uint32_t) noexcept {
    if (!d) return;
    if (!OnOwner(d)) std::terminate();
#if defined(LIVEKIT_DX11_MODULE_INSPECTION)
    if (d->instrumentation_hooks.before_destroy) {
        try { d->instrumentation_hooks.before_destroy(d->instrumentation_hooks.context); }
        catch (...) { /* An inspection callback must never unwind across the C ABI. */ }
    }
#endif
    for (auto& [id, resource] : d->resources) RetireImported(d, resource);
    d->resources.clear();
    d->renderer.Cleanup();
    delete d;
}
lk_render_result LK_RENDER_CALL Resize(lk_render_device* d, const lk_render_frame_target* t) noexcept {
    return Guard([&]() -> lk_render_result {
        const auto result = CheckTarget(d, t);
        if (result != LK_RENDER_OK) return result;
        return d->renderer.Resize(int(t->pixel_width), int(t->pixel_height)) ? LK_RENDER_OK : LK_RENDER_DEVICE_LOST;
    });
}
lk_render_result ValidateFrame(const lk_render_frame_view* f, bool overlay) {
    if (!f || f->struct_size != sizeof(*f) || f->reserved || !f->width || !f->height ||
        f->width > 16384 || f->height > 16384 ||
        (f->rotation_degrees != 0 && f->rotation_degrees != 90 && f->rotation_degrees != 180 && f->rotation_degrees != 270) ||
        f->color_matrix > LK_RENDER_MATRIX_BT2020_NCL || f->color_range > LK_RENDER_RANGE_FULL) return LK_RENDER_INVALID_ARGUMENT;
    if (f->format != LK_RENDER_I420 && f->format != LK_RENDER_NV12 && f->format != LK_RENDER_RGBA8) return LK_RENDER_UNSUPPORTED;
    if (overlay ? (f->format != LK_RENDER_RGBA8 || f->alpha_mode != LK_RENDER_ALPHA_PREMULTIPLIED || f->rotation_degrees)
                : f->alpha_mode != LK_RENDER_ALPHA_OPAQUE) return LK_RENDER_INVALID_ARGUMENT;
    const uint32_t count = f->format == LK_RENDER_I420 ? 3 : f->format == LK_RENDER_NV12 ? 2 : 1;
    if (f->plane_count != count) return LK_RENDER_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < 3; ++i) {
        const auto& p = f->planes[i];
        if (i >= count) {
            if (p.data || p.byte_count || p.stride_bytes || p.width_samples || p.height_samples || p.reserved) return LK_RENDER_INVALID_ARGUMENT;
            continue;
        }
        const uint32_t w = i ? (f->width + 1) / 2 : f->width;
        const uint32_t h = i ? (f->height + 1) / 2 : f->height;
        const uint32_t components = f->format == LK_RENDER_RGBA8 ? 4 : (f->format == LK_RENDER_NV12 && i ? 2 : 1);
        const uint64_t row = uint64_t(w) * components;
        if (!p.data || p.reserved || p.width_samples != w || p.height_samples != h || p.stride_bytes < row ||
            p.byte_count < uint64_t(h - 1) * p.stride_bytes + row) return LK_RENDER_INVALID_ARGUMENT;
    }
    return LK_RENDER_OK;
}
lk_render_result Upload(lk_render_device* d, lk_render_resource_id id, const lk_render_frame_view* f, bool overlay) {
    if (!OnOwner(d) || !id.value || !id.generation) return LK_RENDER_INVALID_ARGUMENT;
    auto status = ValidateFrame(f, overlay);
    if (status != LK_RENDER_OK) return status;
    if (!d->renderer.is_initialized()) return LK_RENDER_DEVICE_LOST;
    auto it = d->resources.find(id.value);
    if (it != d->resources.end() && it->second.generation != id.generation) return LK_RENDER_INVALID_ARGUMENT;
    if (it == d->resources.end() && d->resources.size() >= d->max_resources) return LK_RENDER_OUT_OF_MEMORY;
    // Upload into an inactive buffer, commit only after every plane succeeds.
    // Reuse two buffers per stream so unchanged dimensions do not allocate per frame.
    GpuFrame candidate = it == d->resources.end() ? GpuFrame{} : it->second.spare;
    if (candidate.width != f->width || candidate.height != f->height || candidate.format != f->format) {
        candidate = {};
        candidate.width = f->width; candidate.height = f->height; candidate.format = f->format;
        candidate.count = f->plane_count;
        for (uint32_t i = 0; i < f->plane_count; ++i) {
            const auto& p = f->planes[i];
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = p.width_samples; desc.Height = p.height_samples;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = f->format == LK_RENDER_RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM
                : (f->format == LK_RENDER_NV12 && i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const auto device = d->renderer.device();
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, candidate.textures[i].GetAddressOf());
            if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(candidate.textures[i].Get(), nullptr, candidate.views[i].GetAddressOf());
            if (FAILED(hr)) return hr == E_OUTOFMEMORY ? LK_RENDER_OUT_OF_MEMORY : LK_RENDER_DEVICE_LOST;
        }
    }
    for (uint32_t i = 0; i < f->plane_count; ++i) d->renderer.context()->UpdateSubresource(
        candidate.textures[i].Get(), 0, nullptr, f->planes[i].data, f->planes[i].stride_bytes, 0);
    if (FAILED(d->renderer.device()->GetDeviceRemovedReason())) return LK_RENDER_DEVICE_LOST;
    candidate.rotation = f->rotation_degrees; candidate.overlay = overlay;
    candidate.color = {static_cast<livekit::render::RenderColorMatrix>(f->color_matrix),
        static_cast<livekit::render::RenderColorRange>(f->color_range)};
    auto& resource = d->resources[id.value];
    if (!RetireImported(d, resource)) return LK_RENDER_DEVICE_LOST;
    resource.generation = id.generation;
    resource.spare = std::move(resource.active);
    resource.active = std::move(candidate);
    return LK_RENDER_OK;
}
lk_render_result LK_RENDER_CALL ImportNv12(lk_render_device* d, lk_render_resource_id id,
        const lk_render_dx11_texture_view* input, lk_render_dx11_retirement* ticket) noexcept {
    return Guard([&]() -> lk_render_result {
        if (!OnOwner(d) || !id.value || !id.generation || !input || !ticket ||
            input->struct_size != sizeof(*input) || input->reserved || !input->allocation_id ||
            !input->texture_handle || !input->ready_fence_handle || !input->ready_value ||
            !input->width || !input->height || input->width > 16384 || input->height > 16384)
            return LK_RENDER_INVALID_ARGUMENT;
        if (!d->device5 || !d->context4 || input->adapter_low != d->adapter.LowPart ||
            input->adapter_high != d->adapter.HighPart) return LK_RENDER_UNSUPPORTED;
        auto it = d->resources.find(id.value);
        if (it != d->resources.end() && it->second.generation != id.generation) return LK_RENDER_INVALID_ARGUMENT;
        if (it == d->resources.end() && d->resources.size() >= d->max_resources) return LK_RENDER_OUT_OF_MEMORY;
        Resource first;
        auto& r = it == d->resources.end() ? first : it->second;
        auto entry = std::find_if(r.imports.begin(), r.imports.end(),
            [&](const auto& e) { return e.allocation == input->allocation_id; });
        if (entry == r.imports.end()) {
            Resource::Imported next;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(d->device5->OpenSharedResource(reinterpret_cast<HANDLE>(input->texture_handle),
                    IID_PPV_ARGS(&texture))) ||
                FAILED(d->device5->OpenSharedFence(reinterpret_cast<HANDLE>(input->ready_fence_handle),
                    IID_PPV_ARGS(&next.ready)))) return LK_RENDER_UNSUPPORTED;
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != input->width || desc.Height != input->height ||
                desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return LK_RENDER_UNSUPPORTED;
            next.allocation = input->allocation_id;
            auto& frame = next.frame;
            frame.format = LK_RENDER_NV12;
            frame.width = desc.Width;
            frame.height = desc.Height;
            frame.count = 2;
            frame.color = {livekit::render::RenderColorMatrix::Bt709, livekit::render::RenderColorRange::Limited};
            for (unsigned plane = 0; plane < 2; ++plane) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
                srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                if (FAILED(d->device5->CreateShaderResourceView(texture.Get(), &srv, &frame.views[plane])))
                    return LK_RENDER_UNSUPPORTED;
            }
            entry = r.imports.begin() + (r.next_import++ % r.imports.size());
            *entry = std::move(next);
        }
        if (!r.retired && FAILED(d->device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.retired))))
            return LK_RENDER_UNSUPPORTED;
        if (!RetireImported(d, r) || FAILED(d->context4->Wait(entry->ready.Get(), input->ready_value)))
            return LK_RENDER_DEVICE_LOST;
        r.active = entry->frame;
        r.generation = id.generation;
        r.imported_active = true;
        ++r.last_ticket;
        auto* fence = r.retired.Get();
        const auto value = r.last_ticket;
        if (it == d->resources.end()) d->resources.emplace(id.value, std::move(first));
        *ticket = {reinterpret_cast<uintptr_t>(fence), value};
        return LK_RENDER_OK;
    });
}

lk_render_result LK_RENDER_CALL UploadFrame(lk_render_device* d, lk_render_resource_id id, const lk_render_frame_view* f) noexcept {
    return Guard([&] { return Upload(d, id, f, false); });
}
lk_render_result LK_RENDER_CALL UploadOverlay(lk_render_device* d, lk_render_resource_id id, const lk_render_frame_view* f) noexcept {
    return Guard([&] { return Upload(d, id, f, true); });
}
void LK_RENDER_CALL Remove(lk_render_device* d, lk_render_resource_id id) noexcept {
    if (!OnOwner(d)) return;
    const auto it = d->resources.find(id.value);
    if (it != d->resources.end() && it->second.generation == id.generation) {
        RetireImported(d, it->second);
        d->resources.erase(it);
    }
}
lk_render_result LK_RENDER_CALL Render(lk_render_device* d, const lk_render_frame_target* t, const lk_render_scene_view* scene) noexcept {
    return Guard([&]() -> lk_render_result {
        auto status = CheckTarget(d, t);
        if (status != LK_RENDER_OK) return status;
        if (!scene || scene->struct_size != sizeof(*scene) || scene->command_count > 65536 ||
            (scene->command_count && !scene->commands) || !ValidColor(scene->clear_rgba)) return LK_RENDER_INVALID_ARGUMENT;
        // Validate the complete scene before touching the visible target.
        for (uint32_t i = 0; i < scene->command_count; ++i) {
            const auto& c = scene->commands[i];
            if (!ValidColor(c.color) || c.destination.width > 32767 || c.destination.height > 32767 ||
                c.destination.x < -32768 || c.destination.y < -32768 ||
                int64_t(c.destination.x) + c.destination.width > 32767 ||
                int64_t(c.destination.y) + c.destination.height > 32767) return LK_RENDER_INVALID_ARGUMENT;
            if (!c.resource.value) { if (c.resource.generation) return LK_RENDER_INVALID_ARGUMENT; }
            else {
                const auto it = d->resources.find(c.resource.value);
                if (it == d->resources.end() || it->second.generation != c.resource.generation) return LK_RENDER_INVALID_ARGUMENT;
            }
        }
        status = Resize(d, t);
        if (status != LK_RENDER_OK) return status;
        if (!d->renderer.BeginFrame(scene->clear_rgba[0], scene->clear_rgba[1], scene->clear_rgba[2], scene->clear_rgba[3])) return LK_RENDER_DEVICE_LOST;
        for (uint32_t i = 0; i < scene->command_count; ++i) {
            const auto& c = scene->commands[i];
            const auto left = std::clamp<int64_t>(c.clip.x, 0, t->pixel_width);
            const auto top = std::clamp<int64_t>(c.clip.y, 0, t->pixel_height);
            const auto right = std::clamp<int64_t>(int64_t(c.clip.x) + c.clip.width, 0, t->pixel_width);
            const auto bottom = std::clamp<int64_t>(int64_t(c.clip.y) + c.clip.height, 0, t->pixel_height);
            if (left >= right || top >= bottom || !c.destination.width || !c.destination.height) continue;
            d->renderer.SetClip(int(left), int(top), int(right), int(bottom));
            d->renderer.SetViewport(c.destination.x, c.destination.y, c.destination.width, c.destination.height);
            if (!c.resource.value) {
                d->renderer.DrawSolidQuad(c.color[0], c.color[1], c.color[2], c.color[3]);
            } else {
                const auto& f = d->resources.at(c.resource.value).active;
                if (f.overlay) d->renderer.DrawPremultipliedOverlay(f.views[0].Get(), c.color);
                else {
                    d->renderer.SetRotation(static_cast<livekit::VideoRotation>(f.rotation));
                    d->renderer.SetYuvColorSpace(f.color);
                    ID3D11ShaderResourceView* views[]{f.views[0].Get(), f.views[1].Get(), f.views[2].Get()};
                    const auto format = f.format == LK_RENDER_I420 ? PixelFormatType::I420 :
                        f.format == LK_RENDER_NV12 ? PixelFormatType::NV12 : PixelFormatType::RGBA;
                    d->renderer.DrawQuad(format, views, f.count, c.color);
                }
            }
        }
        if (d->renderer.EndFrame(true)) return LK_RENDER_OK;
        const auto failure = d->renderer.last_present_result();
        return failure == DXGI_ERROR_DEVICE_REMOVED || failure == DXGI_ERROR_DEVICE_RESET ||
            failure == DXGI_ERROR_DEVICE_HUNG || failure == DXGI_ERROR_DRIVER_INTERNAL_ERROR
            ? LK_RENDER_DEVICE_LOST : LK_RENDER_FATAL;
    });
}
#if defined(LIVEKIT_DX11_MODULE_INSPECTION)
lk_render_result LK_RENDER_CALL SetInspectionHooks(lk_render_device* d,
        const lk_render_dx11_inspection_hooks_v1* hooks) noexcept {
    if (!OnOwner(d) || (hooks && (hooks->struct_size != sizeof(*hooks) || hooks->reserved)))
        return LK_RENDER_INVALID_ARGUMENT;
    d->instrumentation_hooks = hooks ? *hooks : lk_render_dx11_inspection_hooks_v1{};
    d->renderer.SetPresentHook(d->instrumentation_hooks.before_present, d->instrumentation_hooks.context);
    return LK_RENDER_OK;
}
#endif
lk_render_result LK_RENDER_CALL Query(lk_render_device* d, uint32_t id, uint32_t version, uint32_t size, void* out) noexcept {
#if defined(LIVEKIT_DX11_MODULE_INSPECTION)
    if (id == LK_RENDER_EXT_DX11_INSPECTION_HOOKS && version == LK_RENDER_DX11_INSPECTION_HOOKS_V1) {
        if (!OnOwner(d) || !out || size < sizeof(lk_render_dx11_inspection_control_v1)) return LK_RENDER_INVALID_ARGUMENT;
        *static_cast<lk_render_dx11_inspection_control_v1*>(out) = {
            sizeof(lk_render_dx11_inspection_control_v1), 0, d, SetInspectionHooks};
        return LK_RENDER_OK;
    }
#endif
    if (id == LK_RENDER_EXT_DX11_TEXTURE && version == LK_RENDER_DX11_TEXTURE_V1) {
        if (!OnOwner(d) || !out || size < sizeof(lk_render_dx11_texture_api)) return LK_RENDER_INVALID_ARGUMENT;
        if (!d->context4 || !d->device5) return LK_RENDER_UNSUPPORTED;
        *static_cast<lk_render_dx11_texture_api*>(out) = {sizeof(lk_render_dx11_texture_api), 0, d, ImportNv12};
        return LK_RENDER_OK;
    }
    if (id == LK_RENDER_EXT_MODULE_INFO && version == LK_RENDER_MODULE_INFO_V1) {
        if (!OnOwner(d) || !out) return LK_RENDER_INVALID_ARGUMENT;
        if (size < sizeof(lk_render_module_info_v1)) return LK_RENDER_INVALID_ARGUMENT;
        *static_cast<lk_render_module_info_v1*>(out) = {
            sizeof(lk_render_module_info_v1), LK_RENDER_MODULE_VERSION, LK_RENDER_ABI_V1, 0};
        return LK_RENDER_OK;
    }
    if (id != LK_RENDER_EXT_DX11_NATIVE || version != LK_RENDER_DX11_NATIVE_V1) return LK_RENDER_UNSUPPORTED;
    if (!OnOwner(d) || !out || size < sizeof(lk_render_dx11_native_v1)) return LK_RENDER_INVALID_ARGUMENT;
    if (!d->renderer.is_initialized()) return LK_RENDER_DEVICE_LOST;
    *static_cast<lk_render_dx11_native_v1*>(out) = {sizeof(lk_render_dx11_native_v1), 0,
        reinterpret_cast<uintptr_t>(d->renderer.device()), reinterpret_cast<uintptr_t>(d->renderer.context())};
    return LK_RENDER_OK;
}
}
extern "C" LK_RENDER_EXPORT lk_render_result LK_RENDER_CALL lk_render_get_api(
        uint32_t version, uint32_t size, lk_render_api* out) {
    if (version != LK_RENDER_ABI_V1) return LK_RENDER_ABI_MISMATCH;
    if (!out || size < sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    const lk_render_api api{LK_RENDER_ABI_V1, sizeof(api), Describe, Create, Destroy, Resize,
        UploadFrame, UploadOverlay, Remove, Render, Query};
    *out = api;
    return LK_RENDER_OK;
}
