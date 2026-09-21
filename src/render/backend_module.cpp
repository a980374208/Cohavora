#include "backend_module.h"
#include <mutex>
#include <exception>
#include <optional>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace livekit::render {
namespace {
struct ModuleIdentity {
    uint64_t volume = 0;
    uint64_t file = 0;
    bool operator==(const ModuleIdentity& other) const noexcept {
        return volume == other.volume && file == other.file;
    }
    bool operator!=(const ModuleIdentity& other) const noexcept { return !(*this == other); }
};
struct Selection {
    std::mutex mutex;
    std::filesystem::path path;
    ModuleIdentity identity;
    uint32_t backend = 0;
    std::weak_ptr<BackendModule> module;
};
Selection& Selected() { static Selection selection; return selection; }
void CloseLibrary(void* handle) {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}
std::optional<ModuleIdentity> IdentityOf(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(handle, &info) != FALSE;
    CloseHandle(handle);
    if (!valid || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    return ModuleIdentity{info.dwVolumeSerialNumber,
        (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
    return ModuleIdentity{uint64_t(info.st_dev), uint64_t(info.st_ino)};
#endif
}
std::filesystem::path LoadedPath(void* library, lk_render_get_api_fn entry) {
#if defined(_WIN32)
    std::vector<wchar_t> path(512);
    for (;;) {
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(library), path.data(), DWORD(path.size()));
        if (!length) return {};
        if (length < path.size()) return std::filesystem::path(std::wstring(path.data(), length));
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(entry), &info) || !info.dli_fname) return {};
    return std::filesystem::path(info.dli_fname);
#endif
}
bool ValidApi(const lk_render_api& api) {
    return api.abi_version == LK_RENDER_ABI_V1 && api.struct_size == sizeof(api) &&
        api.describe && api.create && api.destroy && api.resize && api.upload_frame &&
        api.upload_overlay && api.remove_resource && api.render && api.query_extension;
}
}
uint32_t BackendModule::DefaultBackend() {
#if defined(_WIN32)
    return LK_RENDER_BACKEND_DX11;
#else
    return LK_RENDER_BACKEND_OPENGL;
#endif
}
std::filesystem::path BackendModule::DefaultPath(const std::filesystem::path& directory) {
    return PathForBackend(directory, DefaultBackend());
}
std::filesystem::path BackendModule::PathForBackend(const std::filesystem::path& directory, uint32_t backend) {
    if (backend != LK_RENDER_BACKEND_DX11 && backend != LK_RENDER_BACKEND_OPENGL) return {};
#if defined(_WIN32)
    return directory / "renderers" / (backend == LK_RENDER_BACKEND_DX11 ? "cohavora-render-dx11.dll" : "cohavora-render-opengl.dll");
#elif defined(__APPLE__)
    return backend == LK_RENDER_BACKEND_OPENGL ? directory / "renderers" / "libcohavora-render-opengl.dylib" : std::filesystem::path{};
#else
    return backend == LK_RENDER_BACKEND_OPENGL ? directory / "renderers" / "libcohavora-render-opengl.so" : std::filesystem::path{};
#endif
}
std::filesystem::path BackendModule::LegacyPathForBackend(
        const std::filesystem::path& directory, uint32_t backend) {
    if (backend != LK_RENDER_BACKEND_DX11 && backend != LK_RENDER_BACKEND_OPENGL) return {};
#if defined(_WIN32)
    return directory / "renderers" / (backend == LK_RENDER_BACKEND_DX11
        ? "livekit-render-dx11.dll" : "livekit-render-opengl.dll");
#elif defined(__APPLE__)
    return backend == LK_RENDER_BACKEND_OPENGL
        ? directory / "renderers" / "liblivekit-render-opengl.dylib" : std::filesystem::path{};
#else
    return backend == LK_RENDER_BACKEND_OPENGL
        ? directory / "renderers" / "liblivekit-render-opengl.so" : std::filesystem::path{};
#endif
}
std::shared_ptr<BackendModule> BackendModule::LoadFromDirectory(
        const std::filesystem::path& directory, uint32_t backend, ModuleLoadError& error) {
    const auto current = PathForBackend(directory, backend);
    std::error_code existsError;
    const bool currentExists = !current.empty() && std::filesystem::exists(current, existsError);
    if (currentExists || existsError) return Load(current, backend, error);
    return Load(LegacyPathForBackend(directory, backend), backend, error);
}
std::shared_ptr<BackendModule> BackendModule::Load(const std::filesystem::path& path,
        uint32_t backend, ModuleLoadError& error) {
    error = ModuleLoadError::InvalidPath;
    if (!path.is_absolute() || (backend != LK_RENDER_BACKEND_DX11 && backend != LK_RENDER_BACKEND_OPENGL)) return {};
    error = ModuleLoadError::OpenFailed;
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) return {};
    auto& selection = Selected();
    std::lock_guard lock(selection.mutex);
    if (selection.backend) {
        if (selection.backend != backend) {
            error = ModuleLoadError::SelectionLocked;
            return {};
        }
        if (auto current = selection.module.lock()) {
            if (selection.path == canonical) { error = ModuleLoadError::None; return current; }
            const auto requested = IdentityOf(canonical);
            if (requested && *requested == selection.identity) { error = ModuleLoadError::None; return current; }
            error = ModuleLoadError::SelectionLocked;
            return {};
        }
    }
    const auto requestedIdentity = IdentityOf(canonical);
    if (!requestedIdentity) { error = ModuleLoadError::OpenFailed; return {}; }
    if (selection.backend && *requestedIdentity != selection.identity) {
        error = ModuleLoadError::SelectionLocked;
        return {};
    }
    // OS loader reference counts serialize unload/reload of the same identity.
    // Device owners retain the module independently of this weak process cache.
    std::unique_ptr<BackendModule> candidate(new BackendModule);
#if defined(_WIN32)
    candidate->library_ = LoadLibraryExW(canonical.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto entry = candidate->library_ ? reinterpret_cast<lk_render_get_api_fn>(
        GetProcAddress(static_cast<HMODULE>(candidate->library_), LK_RENDER_ENTRY_POINT)) : nullptr;
#else
    candidate->library_ = dlopen(canonical.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto entry = candidate->library_ ? reinterpret_cast<lk_render_get_api_fn>(dlsym(candidate->library_, LK_RENDER_ENTRY_POINT)) : nullptr;
#endif
    error = candidate->library_ ? ModuleLoadError::EntryMissing : ModuleLoadError::OpenFailed;
    if (!entry) return {};
    const auto loadedIdentity = IdentityOf(LoadedPath(candidate->library_, entry));
    error = ModuleLoadError::IdentityMismatch;
    if (!loadedIdentity || *loadedIdentity != *requestedIdentity) return {};
    error = ModuleLoadError::InvalidAbi;
    if (entry(LK_RENDER_ABI_V1, sizeof(candidate->api_), &candidate->api_) != LK_RENDER_OK || !ValidApi(candidate->api_)) return {};
    error = ModuleLoadError::InvalidCapabilities;
    auto& info = candidate->info_;
    if (candidate->api_.describe(sizeof(info), &info) != LK_RENDER_OK || info.struct_size != sizeof(info) ||
        info.pointer_size != sizeof(void*) || info.backend_kind != backend ||
        info.surface_kind != (backend == LK_RENDER_BACKEND_DX11 ? LK_RENDER_SURFACE_WIN32 : LK_RENDER_SURFACE_HOST_GL)) return {};
    constexpr auto required = LK_RENDER_FORMAT_BIT(LK_RENDER_I420) | LK_RENDER_FORMAT_BIT(LK_RENDER_NV12) |
        LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8);
    if ((info.video_formats & required) != required || !(info.overlay_formats & LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8))) return {};
    auto result = std::shared_ptr<BackendModule>(std::move(candidate));
    selection.path = canonical;
    selection.identity = *requestedIdentity;
    selection.backend = backend;
    selection.module = result;
    error = ModuleLoadError::None;
    return result;
}
BackendModule::~BackendModule() { CloseLibrary(library_); }
std::unique_ptr<BackendDevice> BackendModule::Create(const lk_render_create_info& info, lk_render_result& result) {
    auto owner = std::unique_ptr<BackendDevice>(new BackendDevice(shared_from_this()));
    result = api_.create(&info, &owner->device_);
    if (result != LK_RENDER_OK || !owner->device_) {
        if (result == LK_RENDER_OK) result = LK_RENDER_FATAL;
        return {}; // Also rolls back a nonconforming module that returned a handle on failure.
    }
    return owner;
}
BackendDevice::BackendDevice(std::shared_ptr<BackendModule> module)
    : module_(std::move(module)), owner_(std::this_thread::get_id()) {}
BackendDevice::~BackendDevice() { Reset(); }
void BackendDevice::Reset(uint32_t mode) noexcept {
    if (!OnOwner()) std::terminate(); // Never call a graphics driver on an arbitrary destructor thread.
    if (device_) module_->api_.destroy(device_, mode);
    device_ = nullptr;
    module_.reset();
}
lk_render_result BackendDevice::Resize(const lk_render_frame_target& target) {
    return OnOwner() && device_ ? module_->api_.resize(device_, &target) : LK_RENDER_INVALID_ARGUMENT;
}
lk_render_result BackendDevice::Upload(lk_render_resource_id id, const lk_render_frame_view& frame, bool overlay) {
    if (!OnOwner() || !device_) return LK_RENDER_INVALID_ARGUMENT;
    return (overlay ? module_->api_.upload_overlay : module_->api_.upload_frame)(device_, id, &frame);
}
void BackendDevice::Remove(lk_render_resource_id id) {
    if (!OnOwner()) std::terminate();
    if (device_) module_->api_.remove_resource(device_, id);
}
lk_render_result BackendDevice::Render(const lk_render_frame_target& target, const lk_render_scene_view& scene) {
    return OnOwner() && device_ ? module_->api_.render(device_, &target, &scene) : LK_RENDER_INVALID_ARGUMENT;
}
lk_render_result BackendDevice::QueryExtension(uint32_t id, uint32_t version, uint32_t size, void* out) {
    return OnOwner() && device_ ? module_->api_.query_extension(device_, id, version, size, out) : LK_RENDER_INVALID_ARGUMENT;
}
} // namespace livekit::render
