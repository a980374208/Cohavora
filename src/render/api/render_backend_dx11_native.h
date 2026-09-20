#ifndef LIVEKIT_RENDER_BACKEND_DX11_NATIVE_H
#define LIVEKIT_RENDER_BACKEND_DX11_NATIVE_H
#include "render_backend_api.h"
#define LK_RENDER_DX11_NATIVE_V1 UINT32_C(1)
#pragma pack(push, 8)
/* Optional diagnostics bridge; not used for normal presentation. Borrowed COM
 * pointers, no AddRef/Release transfer. Valid only while the owning device lives.
 * Query must use exact version and at least sizeof(output); failure writes nothing.
 */
typedef struct lk_render_dx11_native_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uintptr_t device;
    uintptr_t context;
} lk_render_dx11_native_v1;
#pragma pack(pop)
#endif
