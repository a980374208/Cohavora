#include "render/api/render_backend_api.h"
#include <array>
#include <cstring>
#include <iostream>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

static_assert(std::is_standard_layout_v<lk_render_api>);
static_assert(std::is_trivially_copyable_v<lk_render_frame_view>);
static_assert(sizeof(lk_render_resource_id) == 16);
static_assert(sizeof(lk_render_frame_target) == 32);
static_assert(sizeof(lk_render_draw_command) == 64);

int main(int argc, char** argv) {
    CHECK(argc == 2);
#if defined(_WIN32)
    // CTest passes the absolute fixture path; no renderer search/fallback policy.
    auto module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    CHECK(module);
    auto entry = reinterpret_cast<lk_render_get_api_fn>(GetProcAddress(module, LK_RENDER_ENTRY_POINT));
#else
    auto module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(module);
    auto entry = reinterpret_cast<lk_render_get_api_fn>(dlsym(module, LK_RENDER_ENTRY_POINT));
#endif
    CHECK(entry);
    struct Guarded { lk_render_api api; std::array<unsigned char, 32> tail; } output;
    std::memset(&output, 0xa5, sizeof(output));
    std::array<unsigned char, sizeof(output)> before{};
    std::memcpy(before.data(), &output, sizeof(output));
    CHECK(entry(99, sizeof(output), &output.api) == LK_RENDER_ABI_MISMATCH);
    CHECK(std::memcmp(before.data(), &output, sizeof(output)) == 0);
    CHECK(entry(1, sizeof(lk_render_api) - 1, &output.api) == LK_RENDER_INVALID_ARGUMENT);
    CHECK(std::memcmp(before.data(), &output, sizeof(output)) == 0);
    CHECK(entry(1, sizeof(lk_render_api), nullptr) == LK_RENDER_INVALID_ARGUMENT);
    CHECK(entry(1, sizeof(output), &output.api) == LK_RENDER_OK);
    for (auto byte : output.tail) CHECK(byte == 0xa5);
    auto& api = output.api;
    CHECK(api.abi_version == 1 && api.struct_size == sizeof(api));
    CHECK(api.describe && api.create && api.destroy && api.resize && api.upload_frame &&
          api.upload_overlay && api.remove_resource && api.render && api.query_extension);
    lk_render_backend_info info{};
    CHECK(api.describe(sizeof(info), &info) == LK_RENDER_OK);
    CHECK(info.pointer_size == sizeof(void*) && info.surface_kind == LK_RENDER_SURFACE_HOST_GL);
    lk_render_device* device = reinterpret_cast<lk_render_device*>(uintptr_t(1));
    CHECK(api.create(nullptr, &device) == LK_RENDER_INVALID_ARGUMENT && device == nullptr);
    lk_render_create_info create{};
    create.struct_size = sizeof(create);
    create.surface_generation = 17;
    CHECK(api.create(&create, &device) == LK_RENDER_OK && device);
    lk_render_frame_target target{};
    target.struct_size = sizeof(target);
    target.surface_generation = 17;
    CHECK(api.resize(device, &target) == LK_RENDER_NOT_READY);
    target.pixel_width = 641; target.pixel_height = 359;
    CHECK(api.resize(device, &target) == LK_RENDER_OK);
    target.surface_generation = 16;
    CHECK(api.resize(device, &target) == LK_RENDER_SURFACE_LOST);
    uint64_t sentinel = UINT64_C(0xfedcba9876543210);
    CHECK(api.query_extension(device, 99, 1, sizeof(sentinel), &sentinel) == LK_RENDER_UNSUPPORTED);
    CHECK(sentinel == UINT64_C(0xfedcba9876543210));
    api.destroy(device, LK_RENDER_DESTROY_RELEASE);
    api.destroy(nullptr, LK_RENDER_DESTROY_ABANDON_GL);
    output.api = {}; // Retire function-table users before unloading the fixture.
#if defined(_WIN32)
    CHECK(FreeLibrary(module));
#else
    CHECK(dlclose(module) == 0);
#endif
    std::cout << "PASS C module/C++ consumer: ABI negotiation, guarded output, opaque lifetime, stale surface\n";
}
