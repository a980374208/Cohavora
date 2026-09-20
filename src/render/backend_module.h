#pragma once
#include "render/api/render_backend_api.h"
#include <filesystem>
#include <memory>
#include <thread>

namespace livekit::render {
enum class ModuleLoadError {
    None,
    InvalidPath,
    OpenFailed,
    EntryMissing,
    IdentityMismatch,
    InvalidAbi,
    InvalidCapabilities,
    SelectionLocked,
};
class BackendDevice;

// Host implementation only. Neither this C++ owner nor its allocator crosses
// the DLL boundary. Only one successful module identity is selected per process.
class BackendModule final : public std::enable_shared_from_this<BackendModule> {
public:
    static uint32_t DefaultBackend();
    static std::filesystem::path PathForBackend(const std::filesystem::path&, uint32_t backend);
    static std::filesystem::path DefaultPath(const std::filesystem::path& applicationDirectory);
    static std::shared_ptr<BackendModule> Load(const std::filesystem::path& absolutePath,
        uint32_t expectedBackend, ModuleLoadError& error);
    ~BackendModule();
    BackendModule(const BackendModule&) = delete;
    BackendModule& operator=(const BackendModule&) = delete;
    uint32_t abiVersion() const noexcept { return api_.abi_version; }
    const lk_render_backend_info& info() const noexcept { return info_; }
    std::unique_ptr<BackendDevice> Create(const lk_render_create_info&, lk_render_result& result);
private:
    friend class BackendDevice;
    BackendModule() = default;
    void* library_ = nullptr;
    lk_render_api api_{};
    lk_render_backend_info info_{};
};

// Nonmovable, owner-thread-only device. No exposed function table can outlive
// its DLL. For GL, the host must make the original context current for Reset,
// or explicitly Reset(ABANDON_GL) after losing that context.
class BackendDevice final {
public:
    ~BackendDevice();
    BackendDevice(const BackendDevice&) = delete;
    BackendDevice& operator=(const BackendDevice&) = delete;
    lk_render_result Resize(const lk_render_frame_target&);
    lk_render_result Upload(lk_render_resource_id, const lk_render_frame_view&, bool overlay = false);
    void Remove(lk_render_resource_id);
    lk_render_result Render(const lk_render_frame_target&, const lk_render_scene_view&);
    lk_render_result QueryExtension(uint32_t id, uint32_t version, uint32_t size, void* out);
    void Reset(uint32_t mode = LK_RENDER_DESTROY_RELEASE) noexcept;
private:
    friend class BackendModule;
    explicit BackendDevice(std::shared_ptr<BackendModule> module);
    bool OnOwner() const noexcept { return owner_ == std::this_thread::get_id(); }
    std::shared_ptr<BackendModule> module_;
    lk_render_device* device_ = nullptr;
    std::thread::id owner_;
};
} // namespace livekit::render
