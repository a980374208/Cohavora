#ifndef LIVEKIT_RENDER_BACKEND_API_H
#define LIVEKIT_RENDER_BACKEND_API_H

#include <stdint.h>
#include <stddef.h>

/* ABI v1: in-process, same architecture, native endian, default 8-byte packing.
 * No Qt, WebRTC, STL, compiler-owned strings, exceptions or allocator ownership
 * cross this boundary. All reserved fields must be zero. Structures are frozen
 * for v1; incompatible changes require a new ABI version. No custom /Zp flags.
 *
 * One module is selected for the process. All instance operations run serially
 * on the host's UI/render owner thread. Capture callbacks NEVER call this API.
 * The module is retained until every instance and function-table user is gone.
 * No module callbacks/threads may outlive an API call. Exceptions must be caught
 * inside a C++ module and translated to a result. Diagnostics use result codes.
 */
#if defined(_WIN32)
#define LK_RENDER_CALL __cdecl
#define LK_RENDER_GL_CALL __stdcall
#if defined(LK_RENDER_MODULE_BUILD)
#define LK_RENDER_EXPORT __declspec(dllexport)
#else
#define LK_RENDER_EXPORT
#endif
#else
#define LK_RENDER_CALL
#define LK_RENDER_GL_CALL
#define LK_RENDER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LK_RENDER_ABI_V1 UINT32_C(1)
#define LK_RENDER_ENTRY_POINT "lk_render_get_api"
typedef int32_t lk_render_result;
enum {
    LK_RENDER_OK = 0,
    LK_RENDER_NOT_READY = 1,
    LK_RENDER_UNSUPPORTED = 2,
    LK_RENDER_INVALID_ARGUMENT = 3,
    LK_RENDER_ABI_MISMATCH = 4,
    LK_RENDER_SURFACE_LOST = 5,
    LK_RENDER_DEVICE_LOST = 6,
    LK_RENDER_OUT_OF_MEMORY = 7,
    LK_RENDER_FATAL = 8
};
enum { LK_RENDER_BACKEND_DX11 = 1, LK_RENDER_BACKEND_OPENGL = 2 };
enum { LK_RENDER_SURFACE_WIN32 = 1, LK_RENDER_SURFACE_HOST_GL = 2 };
enum { LK_RENDER_I420 = 1, LK_RENDER_NV12 = 2, LK_RENDER_RGBA8 = 3,
       LK_RENDER_BGRA8 = 4, LK_RENDER_ARGB8 = 5, LK_RENDER_ABGR8 = 6 };
enum { LK_RENDER_MATRIX_UNSPECIFIED = 0, LK_RENDER_MATRIX_BT601 = 1,
       LK_RENDER_MATRIX_BT709 = 2, LK_RENDER_MATRIX_BT2020_NCL = 3 };
enum { LK_RENDER_RANGE_UNSPECIFIED = 0, LK_RENDER_RANGE_LIMITED = 1,
       LK_RENDER_RANGE_FULL = 2 };
enum { LK_RENDER_ALPHA_OPAQUE = 0, LK_RENDER_ALPHA_PREMULTIPLIED = 1 };
enum { LK_RENDER_DESTROY_RELEASE = 0, LK_RENDER_DESTROY_ABANDON_GL = 1 };
enum { LK_RENDER_EXT_DX11_NATIVE = 1, LK_RENDER_EXT_GL_NATIVE = 2 };
#define LK_RENDER_FORMAT_BIT(format) (UINT64_C(1) << (format))

#pragma pack(push, 8)
typedef struct lk_render_device lk_render_device;
typedef struct lk_render_resource_id {
    uint64_t value;       /* Nonzero, assigned by host; no participant identity. */
    uint64_t generation;  /* Nonzero, changes when the logical stream is replaced. */
} lk_render_resource_id;

typedef struct lk_render_plane_view {
    const uint8_t* data;
    uint64_t byte_count;
    uint32_t stride_bytes;
    uint32_t width_samples;
    uint32_t height_samples;
    uint32_t reserved;
} lk_render_plane_view;

/* Borrowed only for upload_frame(). Upload completes its CPU read before return;
 * module must not retain data pointers. Host keeps the owned source alive.
 * I420: three 8-bit planes, UV ceil(w/2) x ceil(h/2). NV12: Y then interleaved
 * UV (two bytes/sample). Packed format names describe byte order in memory,
 * NOT integer endianness. Positive strides only. Validate overflow and bounds:
 * bytes >= (height-1)*stride + width*bytes_per_sample. Rotation is 0/90/180/270
 * clockwise and applies to video only. Unspecified colour = BT.601 limited.
 * v1 supports SDR 8-bit; BT.2020 is NCL conversion, not HDR/tone mapping.
 */
typedef struct lk_render_frame_view {
    uint32_t struct_size;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    uint32_t rotation_degrees;
    uint32_t color_matrix;
    uint32_t color_range;
    uint32_t alpha_mode;
    uint32_t reserved;
    int64_t timestamp_us;
    lk_render_plane_view planes[3];
} lk_render_frame_view;

typedef struct lk_render_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} lk_render_rect;

/* All rectangles are physical pixels, top-left origin. Commands draw in array
 * order, back to front. Host does fit/letterbox, DPI rounding and hit testing.
 * Resource id {0,0} denotes a solid quad. Resource pixels use the metadata from
 * their last successful upload. Clip is intersected with the target bounds.
 */
typedef struct lk_render_draw_command {
    lk_render_resource_id resource;
    lk_render_rect destination;
    lk_render_rect clip;
    float color[4]; /* Solid RGBA or texture modulation; premultiplied. */
} lk_render_draw_command;

typedef struct lk_render_scene_view {
    uint32_t struct_size;
    uint32_t command_count;
    const lk_render_draw_command* commands; /* Borrowed until render returns. */
    float clear_rgba[4];
} lk_render_scene_view;

typedef void (LK_RENDER_GL_CALL *lk_render_gl_proc)(void);
typedef lk_render_gl_proc (LK_RENDER_CALL *lk_render_get_gl_proc)(void*, const char*);

/* Host-only platform bridges, selected once after describe(). Do not pass a
 * QWidget/QOpenGLContext object as the native handle. No Qt runtime in modules.
 */
typedef struct lk_render_win32_binding {
    uint32_t struct_size;
    uint32_t reserved;
    uintptr_t hwnd;
} lk_render_win32_binding;

typedef struct lk_render_gl_binding {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t context_generation;
    void* host_user;
    lk_render_get_gl_proc get_proc; /* Host storage survives until destroy. */
} lk_render_gl_binding;

typedef struct lk_render_create_info {
    uint32_t struct_size;
    uint32_t surface_kind;
    const void* binding;
    uint32_t binding_size;
    uint32_t max_resources; /* Nonzero hard bound; includes video + overlays. */
    uint64_t surface_generation;
} lk_render_create_info;

/* Host supplies the actual current GL FBO on EVERY frame; never assume zero or
 * cache it across resize. Current context must match create's generation for
 * create/resize/upload/render/destroy(RELEASE). GL module must not swap buffers;
 * Qt presents. DX11 ignores gl_draw_fbo (must be zero) and presents itself.
 * Zero dimensions mean suspended/NOT_READY, not fatal. A new context requires
 * a new instance; surface generation rejects queued work from an old surface.
 */
typedef struct lk_render_frame_target {
    uint32_t struct_size;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t gl_draw_fbo;
    uint64_t surface_generation;
    uint64_t context_generation; /* Zero for DX11. */
} lk_render_frame_target;

typedef struct lk_render_backend_info {
    uint32_t struct_size;
    uint32_t backend_kind;
    uint32_t surface_kind;
    uint32_t pointer_size;
    uint64_t video_formats;
    uint64_t overlay_formats; /* v1 requires premultiplied RGBA8 overlay. */
} lk_render_backend_info;

typedef struct lk_render_api {
    uint32_t abi_version;
    uint32_t struct_size;
    lk_render_result (LK_RENDER_CALL *describe)(uint32_t, lk_render_backend_info*);
    lk_render_result (LK_RENDER_CALL *create)(const lk_render_create_info*, lk_render_device**);
    /* destroy(NULL, ...) is valid. ABANDON_GL frees CPU bookkeeping without GL
     * calls after context loss; never delete old GL names in a new context.
     * destroy must not throw; caller retires all outstanding resource handles.
     */
    void (LK_RENDER_CALL *destroy)(lk_render_device*, uint32_t mode);
    lk_render_result (LK_RENDER_CALL *resize)(lk_render_device*, const lk_render_frame_target*);
    lk_render_result (LK_RENDER_CALL *upload_frame)(lk_render_device*, lk_render_resource_id,
                                                 const lk_render_frame_view*);
    lk_render_result (LK_RENDER_CALL *upload_overlay)(lk_render_device*, lk_render_resource_id,
                                                   const lk_render_frame_view*);
    /* Removal is idempotent for the exact {value,generation}. Host removes old
     * generation before reusing a value, on owner thread; it never forwards
     * late uploads. Failed uploads must not expose a partially updated frame.
     */
    void (LK_RENDER_CALL *remove_resource)(lk_render_device*, lk_render_resource_id);
    lk_render_result (LK_RENDER_CALL *render)(lk_render_device*, const lk_render_frame_target*,
                                           const lk_render_scene_view*);
    /* Optional extensions are not required for v1 display. Unsupported id or
     * version returns UNSUPPORTED without modifying output. Extension structs
     * need their own version/size contract; no private pointers in normal UI.
     */
    lk_render_result (LK_RENDER_CALL *query_extension)(lk_render_device*, uint32_t id,
                                                     uint32_t version, uint32_t size, void* out);
} lk_render_api;
#pragma pack(pop)

typedef lk_render_result (LK_RENDER_CALL *lk_render_get_api_fn)(uint32_t, uint32_t, lk_render_api*);

/* The only exported symbol. Unsupported version -> ABI_MISMATCH; null/short
 * output -> INVALID_ARGUMENT. Failure MUST NOT write output. On success writes
 * exactly sizeof(lk_render_api), leaves any caller tail untouched, sets size and
 * version, and populates ALL function pointers. Caller checks these before use.
 * create sets *out=NULL before attempting allocation and rolls back on failure.
 * No API success is proof of a displayed frame; render OK means submitted only.
 */
LK_RENDER_EXPORT lk_render_result LK_RENDER_CALL lk_render_get_api(
    uint32_t requested_abi_version, uint32_t output_size, lk_render_api* out_api);

#ifdef __cplusplus
}
#endif
#endif
