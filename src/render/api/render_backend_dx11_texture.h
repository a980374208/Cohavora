#pragma once
#include "render_backend_api.h"

// Optional, same-process ABI. Handles are borrowed during import. The host
// retains the source allocation until the returned retirement fence completes.
#define LK_RENDER_EXT_DX11_TEXTURE UINT32_C(4)
#define LK_RENDER_DX11_TEXTURE_V1 UINT32_C(1)
#pragma pack(push, 8)
typedef struct lk_render_dx11_texture_view {
    uint32_t struct_size, width, height, adapter_low;
    int32_t adapter_high;
    uint32_t reserved;
    uintptr_t texture_handle, ready_fence_handle;
    uint64_t ready_value, allocation_id;
} lk_render_dx11_texture_view;
typedef struct lk_render_dx11_retirement {
    uintptr_t fence; /* borrowed ID3D11Fence; valid until resource removal */
    uint64_t value;
} lk_render_dx11_retirement;
typedef struct lk_render_dx11_texture_api {
    uint32_t struct_size, reserved;
    lk_render_device* device;
    lk_render_result (LK_RENDER_CALL *import_nv12)(lk_render_device*, lk_render_resource_id,
        const lk_render_dx11_texture_view*, lk_render_dx11_retirement*);
} lk_render_dx11_texture_api;
#pragma pack(pop)
