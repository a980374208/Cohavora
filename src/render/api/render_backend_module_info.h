#ifndef LIVEKIT_RENDER_BACKEND_MODULE_INFO_H
#define LIVEKIT_RENDER_BACKEND_MODULE_INFO_H

#include "render_backend_api.h"

#define LK_RENDER_EXT_MODULE_INFO UINT32_C(3)
#define LK_RENDER_MODULE_INFO_V1 UINT32_C(1)
#define LK_RENDER_MODULE_VERSION UINT32_C(1)

#pragma pack(push, 8)
typedef struct lk_render_module_info_v1 {
    uint32_t struct_size;
    uint32_t module_version;
    uint32_t abi_version;
    uint32_t reserved;
} lk_render_module_info_v1;
#pragma pack(pop)

#endif
