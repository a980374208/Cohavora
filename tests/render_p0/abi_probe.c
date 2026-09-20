/* Contract fixture, NOT a renderer. C module + C++ consumer exercise the real
 * shared-library boundary without linking Qt or the production application. */
#include "render/api/render_backend_api.h"
#include <stdlib.h>
#include <string.h>

struct lk_render_device { uint64_t surface_generation; };

static lk_render_result LK_RENDER_CALL describe(uint32_t size, lk_render_backend_info* out) {
    lk_render_backend_info info = {0};
    if (!out || size < sizeof(info)) return LK_RENDER_INVALID_ARGUMENT;
    info.struct_size = sizeof(info);
    info.backend_kind = LK_RENDER_BACKEND_OPENGL;
    info.surface_kind = LK_RENDER_SURFACE_HOST_GL;
    info.pointer_size = sizeof(void*);
    /* Zero video capabilities: this fixture deliberately does not render. */
    memcpy(out, &info, sizeof(info));
    return LK_RENDER_OK;
}
static lk_render_result LK_RENDER_CALL create(const lk_render_create_info* info, lk_render_device** out) {
    if (!out) return LK_RENDER_INVALID_ARGUMENT;
    *out = NULL;
    if (!info || info->struct_size != sizeof(*info) || !info->surface_generation)
        return LK_RENDER_INVALID_ARGUMENT;
    *out = (lk_render_device*)malloc(sizeof(lk_render_device));
    if (!*out) return LK_RENDER_OUT_OF_MEMORY;
    (*out)->surface_generation = info->surface_generation;
    return LK_RENDER_OK;
}
static void LK_RENDER_CALL destroy(lk_render_device* device, uint32_t mode) {
    (void)mode;
    free(device);
}
static lk_render_result LK_RENDER_CALL resize(lk_render_device* device, const lk_render_frame_target* target) {
    if (!device || !target || target->struct_size != sizeof(*target)) return LK_RENDER_INVALID_ARGUMENT;
    if (device->surface_generation != target->surface_generation) return LK_RENDER_SURFACE_LOST;
    return target->pixel_width && target->pixel_height ? LK_RENDER_OK : LK_RENDER_NOT_READY;
}
static lk_render_result LK_RENDER_CALL upload(lk_render_device* device, lk_render_resource_id id,
                                            const lk_render_frame_view* frame) {
    (void)device; (void)id; (void)frame;
    return LK_RENDER_UNSUPPORTED;
}
static void LK_RENDER_CALL remove_resource(lk_render_device* device, lk_render_resource_id id) {
    (void)device; (void)id;
}
static lk_render_result LK_RENDER_CALL render(lk_render_device* device, const lk_render_frame_target* target,
                                            const lk_render_scene_view* scene) {
    (void)device; (void)target; (void)scene;
    return LK_RENDER_UNSUPPORTED;
}
static lk_render_result LK_RENDER_CALL extension(lk_render_device* device, uint32_t id,
                                                uint32_t version, uint32_t size, void* out) {
    (void)device; (void)id; (void)version; (void)size; (void)out;
    return LK_RENDER_UNSUPPORTED;
}
LK_RENDER_EXPORT lk_render_result LK_RENDER_CALL lk_render_get_api(uint32_t version, uint32_t size,
                                                                 lk_render_api* out) {
    lk_render_api api = {0};
    if (version != LK_RENDER_ABI_V1) return LK_RENDER_ABI_MISMATCH;
    if (!out || size < sizeof(api)) return LK_RENDER_INVALID_ARGUMENT;
    api.abi_version = LK_RENDER_ABI_V1;
    api.struct_size = sizeof(api);
    api.describe = describe;
    api.create = create;
    api.destroy = destroy;
    api.resize = resize;
    api.upload_frame = upload;
    api.upload_overlay = upload;
    api.remove_resource = remove_resource;
    api.render = render;
    api.query_extension = extension;
    memcpy(out, &api, sizeof(api));
    return LK_RENDER_OK;
}
