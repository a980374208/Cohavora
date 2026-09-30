#ifndef LIVEKIT_DX11_TEST_HOOKS_H
#define LIVEKIT_DX11_TEST_HOOKS_H

#include "render/api/render_backend_api.h"

/* Private extension implemented only by the dedicated DX11 test module. It
 * never changes the frozen render API or adds a production runtime switch. */
#define LK_RENDER_EXT_DX11_TEST_HOOKS UINT32_C(0x7fff0011)
#define LK_RENDER_DX11_TEST_HOOKS_V1 UINT32_C(1)

#pragma pack(push, 8)
typedef int32_t (LK_RENDER_CALL *lk_render_dx11_before_present)(void* context);
typedef void (LK_RENDER_CALL *lk_render_dx11_before_destroy)(void* context);
typedef struct lk_render_dx11_test_hooks_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    void* context;
    /* Runs at the actual Present boundary on the device owner. S_OK proceeds
     * to Present; a failing HRESULT skips Present and injects that failure. */
    lk_render_dx11_before_present before_present;
    lk_render_dx11_before_destroy before_destroy;
} lk_render_dx11_test_hooks_v1;

typedef struct lk_render_dx11_test_control_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    lk_render_device* device;
    /* Owner-thread only; nullptr clears hooks. The module copies the POD, but
     * the test retains context until device destruction (including quarantine).
     * This borrowed control must not outlive the owning BackendDevice. */
    lk_render_result (LK_RENDER_CALL *set_hooks)(
        lk_render_device*, const lk_render_dx11_test_hooks_v1*);
} lk_render_dx11_test_control_v1;
#pragma pack(pop)

#endif
