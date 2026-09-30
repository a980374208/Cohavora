#include "render/api/render_backend_api.h"
#include "render/api/render_backend_module_info.h"
#if defined(FIXTURE_missing_entry)
LK_RENDER_EXPORT int fixture_not_the_entry(void) { return 0; }
#else
static lk_render_result LK_RENDER_CALL describe(uint32_t size, lk_render_backend_info* out) {
    if (!out || size < sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    lk_render_backend_info info = {sizeof(info), LK_RENDER_BACKEND_DX11, LK_RENDER_SURFACE_WIN32,
#if defined(FIXTURE_create_failure)
        sizeof(void*),
#else
        0, LK_RENDER_FORMAT_BIT(LK_RENDER_I420) | LK_RENDER_FORMAT_BIT(LK_RENDER_NV12) | LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8),
        LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8)}; /* Deliberately invalid pointer size. */
#endif
#if defined(FIXTURE_create_failure)
        LK_RENDER_FORMAT_BIT(LK_RENDER_I420) | LK_RENDER_FORMAT_BIT(LK_RENDER_NV12) | LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8),
        LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8)};
#endif
    *out = info;
    return LK_RENDER_OK;
}
static lk_render_result LK_RENDER_CALL create(const lk_render_create_info* i, lk_render_device** d) { (void)i; *d=0; return LK_RENDER_FATAL; }
static void LK_RENDER_CALL destroy(lk_render_device* d, uint32_t m) { (void)d; (void)m; }
static lk_render_result LK_RENDER_CALL resize(lk_render_device* d,const lk_render_frame_target* t) { (void)d;(void)t;return LK_RENDER_FATAL; }
static lk_render_result LK_RENDER_CALL upload(lk_render_device* d,lk_render_resource_id i,const lk_render_frame_view* f) { (void)d;(void)i;(void)f;return LK_RENDER_FATAL; }
static void LK_RENDER_CALL remove_resource(lk_render_device* d,lk_render_resource_id i) { (void)d;(void)i; }
static lk_render_result LK_RENDER_CALL render(lk_render_device* d,const lk_render_frame_target* t,const lk_render_scene_view* s) { (void)d;(void)t;(void)s;return LK_RENDER_FATAL; }
static lk_render_result LK_RENDER_CALL query(lk_render_device* d,uint32_t i,uint32_t v,uint32_t s,void* o) {
    (void)d;
#if defined(FIXTURE_create_failure)
    if (i == LK_RENDER_EXT_MODULE_INFO && v == LK_RENDER_MODULE_INFO_V1 && o && s >= sizeof(lk_render_module_info_v1)) {
        lk_render_module_info_v1 info = {sizeof(info), LK_RENDER_MODULE_VERSION, LK_RENDER_ABI_V1, 0};
        *(lk_render_module_info_v1*)o = info;
        return LK_RENDER_OK;
    }
#endif
    (void)i;(void)v;(void)s;(void)o;return LK_RENDER_UNSUPPORTED;
}
LK_RENDER_EXPORT lk_render_result LK_RENDER_CALL lk_render_get_api(uint32_t v,uint32_t s,lk_render_api* out) {
    if (v != LK_RENDER_ABI_V1) return LK_RENDER_ABI_MISMATCH;
    if (!out || s < sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    lk_render_api api = {LK_RENDER_ABI_V1,sizeof(api),describe,create,destroy,resize,upload,upload,remove_resource,render,query};
#if defined(FIXTURE_wrong_version)
    api.abi_version = 99;
#elif defined(FIXTURE_missing_function)
    api.render = 0;
#endif
    *out=api;
    return LK_RENDER_OK;
}
#endif
