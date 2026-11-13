#include "d3d11_amf_video_encoder.h"
#include "bounded_encoder_worker.h"
#include "h264_level_constraints.h"
#include "telemetry/diagnostic_pipeline.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "media/d3d11_nv12_frame_buffer.h"
#include "api/array_view.h"
#include "api/sequence_checker.h"
#include "api/video/encoded_image.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/h264_profile_level_id.h"
#if defined(RTC_ENABLE_H265)
#include "api/video_codecs/h265_profile_tier_level.h"
#include "common_video/h265/h265_bitstream_parser.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_pps_parser.h"
#include "common_video/h265/h265_sps_parser.h"
#endif
#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/pps_parser.h"
#include "common_video/h264/sps_parser.h"
#include "modules/video_coding/codecs/external/amf_hevc_level_limits.h"
#include "modules/video_coding/codecs/external/amf_video_encoder.h"
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
#include "modules/video_coding/codecs/external/x265_video_encoder.h"
#endif
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/bitstream_reader.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace livekit {

namespace {

// Minimal self-contained AMD AMF SDK C++ ABI declarations matching amfrt64.dll
// (AMF v1.4.36) so product builds do not depend on an external AMF SDK path.
namespace amf_abi {

using amf_int64 = std::int64_t;
using amf_int32 = std::int32_t;
using amf_uint64 = std::uint64_t;
using amf_uint32 = std::uint32_t;
using amf_size = std::size_t;
using amf_bool = bool;
using amf_long = long;
using amf_uint = unsigned int;
using amf_pts = amf_int64;
using amf_flags = amf_uint32;
using amf_handle = void*;
using amf_double = double;
using amf_float = float;

#define LK_AMF_STD_CALL __stdcall
#define LK_AMF_CDECL_CALL __cdecl
#define LK_AMF_NO_VTABLE __declspec(novtable)

constexpr amf_pts kAmfSecond = 10000000L;
constexpr amf_uint64 kAmfFullVersion =
    (static_cast<amf_uint64>(1) << 48ull) |
    (static_cast<amf_uint64>(4) << 32ull) |
    (static_cast<amf_uint64>(36) << 16ull) |
    static_cast<amf_uint64>(0);

struct AMFSize {
    amf_int32 width;
    amf_int32 height;
    bool operator==(const AMFSize& other) const {
        return width == other.width && height == other.height;
    }
};

inline AMFSize AMFConstructSize(amf_int32 width, amf_int32 height) {
    return {width, height};
}

struct AMFRate {
    amf_uint32 num;
    amf_uint32 den;
    bool operator==(const AMFRate& other) const {
        return num == other.num && den == other.den;
    }
};

inline AMFRate AMFConstructRate(amf_uint32 num, amf_uint32 den) {
    return {num, den};
}

struct AMFRect {
    amf_int32 left;
    amf_int32 top;
    amf_int32 right;
    amf_int32 bottom;
};

struct AMFPoint {
    amf_int32 x;
    amf_int32 y;
};

struct AMFRatio {
    amf_uint32 num;
    amf_uint32 den;
};

struct AMFColor {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

struct AMFFloatSize {
    amf_float width;
    amf_float height;
};

struct AMFFloatPoint2D {
    amf_float x;
    amf_float y;
};

struct AMFFloatPoint3D {
    amf_float x;
    amf_float y;
    amf_float z;
};

struct AMFFloatVector4D {
    amf_float x;
    amf_float y;
    amf_float z;
    amf_float w;
};

struct AMFGuid {
    amf_uint32 data1;
     std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data41;
    std::uint8_t data42;
    std::uint8_t data43;
    std::uint8_t data44;
    std::uint8_t data45;
    std::uint8_t data46;
    std::uint8_t data47;
    std::uint8_t data48;
};

enum AMF_RESULT {
    AMF_OK = 0,
    AMF_FAIL,
    AMF_UNEXPECTED,
    AMF_ACCESS_DENIED,
    AMF_INVALID_ARG,
    AMF_OUT_OF_RANGE,
    AMF_OUT_OF_MEMORY,
    AMF_INVALID_POINTER,
    AMF_NO_INTERFACE,
    AMF_NOT_IMPLEMENTED,
    AMF_NOT_SUPPORTED,
    AMF_NOT_FOUND,
    AMF_ALREADY_INITIALIZED,
    AMF_NOT_INITIALIZED,
    AMF_INVALID_FORMAT,
    AMF_WRONG_STATE,
    AMF_FILE_NOT_OPEN,
    AMF_NO_DEVICE,
    AMF_DIRECTX_FAILED,
    AMF_OPENCL_FAILED,
    AMF_GLX_FAILED,
    AMF_XV_FAILED,
    AMF_ALSA_FAILED,
    AMF_EOF,
    AMF_REPEAT,
    AMF_INPUT_FULL,
    AMF_RESOLUTION_CHANGED,
    AMF_RESOLUTION_UPDATED,
    AMF_INVALID_DATA_TYPE,
    AMF_INVALID_RESOLUTION,
    AMF_CODEC_NOT_SUPPORTED,
    AMF_SURFACE_FORMAT_NOT_SUPPORTED,
    AMF_SURFACE_MUST_BE_SHARED,
    AMF_DECODER_NOT_PRESENT,
    AMF_DECODER_SURFACE_ALLOCATION_FAILED,
    AMF_DECODER_NO_FREE_SURFACES,
    AMF_ENCODER_NOT_PRESENT,
    AMF_DEM_ERROR,
    AMF_DEM_PROPERTY_READONLY,
    AMF_DEM_REMOTE_DISPLAY_CREATE_FAILED,
    AMF_DEM_START_ENCODING_FAILED,
    AMF_DEM_QUERY_OUTPUT_FAILED,
    AMF_TAN_CLIPPING_WAS_REQUIRED,
    AMF_TAN_UNSUPPORTED_VERSION,
    AMF_NEED_MORE_INPUT,
    AMF_VULKAN_FAILED,
};

enum AMF_VARIANT_TYPE {
    AMF_VARIANT_EMPTY = 0,
    AMF_VARIANT_BOOL = 1,
    AMF_VARIANT_INT64 = 2,
    AMF_VARIANT_DOUBLE = 3,
    AMF_VARIANT_RECT = 4,
    AMF_VARIANT_SIZE = 5,
    AMF_VARIANT_POINT = 6,
    AMF_VARIANT_RATE = 7,
    AMF_VARIANT_RATIO = 8,
    AMF_VARIANT_COLOR = 9,
    AMF_VARIANT_STRING = 10,
    AMF_VARIANT_WSTRING = 11,
    AMF_VARIANT_INTERFACE = 12,
    AMF_VARIANT_FLOAT = 13,
    AMF_VARIANT_FLOAT_SIZE = 14,
    AMF_VARIANT_FLOAT_POINT2D = 15,
    AMF_VARIANT_FLOAT_POINT3D = 16,
    AMF_VARIANT_FLOAT_VECTOR4D = 17,
};

class LK_AMF_NO_VTABLE AMFInterface {
public:
    virtual amf_long LK_AMF_STD_CALL Acquire() = 0;
    virtual amf_long LK_AMF_STD_CALL Release() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL QueryInterface(
        const AMFGuid& interfaceID,
        void** ppInterface) = 0;
};

template <class T>
class AMFInterfacePtr_T {
public:
    AMFInterfacePtr_T() : p_(nullptr) {}
    AMFInterfacePtr_T(T* p) : p_(p) {
        if (p_) p_->Acquire();
    }
    AMFInterfacePtr_T(AMFInterface* pInterf) : p_(nullptr) {
        if (pInterf) {
            void* raw = nullptr;
            if (pInterf->QueryInterface(T::IID(), &raw) == AMF_OK && raw) {
                p_ = static_cast<T*>(raw);
            }
        }
    }
    AMFInterfacePtr_T(const AMFInterfacePtr_T<T>& other) : p_(other.p_) {
        if (p_) p_->Acquire();
    }
    ~AMFInterfacePtr_T() { Release(); }

    AMFInterfacePtr_T<T>& operator=(T* p) {
        if (p) p->Acquire();
        if (p_) p_->Release();
        p_ = p;
        return *this;
    }
    AMFInterfacePtr_T<T>& operator=(const AMFInterfacePtr_T<T>& other) {
        return operator=(other.p_);
    }

    void Release() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    operator T*() const { return p_; }
    T* operator->() const { return p_; }
    T** operator&() {
        Release();
        return &p_;
    }
    bool operator!() const { return p_ == nullptr; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_;
};

struct AMFVariantStruct {
    AMF_VARIANT_TYPE type = AMF_VARIANT_EMPTY;
    union {
        amf_bool boolValue;
        amf_int64 int64Value;
        amf_double doubleValue;
        char* stringValue;
        wchar_t* wstringValue;
        AMFInterface* pInterface;
        AMFRect rectValue;
        AMFSize sizeValue;
        AMFPoint pointValue;
        AMFRate rateValue;
        AMFRatio ratioValue;
        AMFColor colorValue;
        amf_float floatValue;
        AMFFloatSize floatSizeValue;
        AMFFloatPoint2D floatPoint2DValue;
        AMFFloatPoint3D floatPoint3DValue;
        AMFFloatVector4D floatVector4DValue;
    };
};

struct AMFVariant : public AMFVariantStruct {
    AMFVariant() {
        type = AMF_VARIANT_EMPTY;
        int64Value = 0;
    }
    explicit AMFVariant(amf_bool v) {
        type = AMF_VARIANT_BOOL;
        boolValue = v;
    }
    explicit AMFVariant(amf_int64 v) {
        type = AMF_VARIANT_INT64;
        int64Value = v;
    }
    explicit AMFVariant(amf_int32 v) {
        type = AMF_VARIANT_INT64;
        int64Value = v;
    }
    explicit AMFVariant(amf_uint32 v) {
        type = AMF_VARIANT_INT64;
        int64Value = static_cast<amf_int64>(v);
    }
    explicit AMFVariant(const AMFSize& v) {
        type = AMF_VARIANT_SIZE;
        sizeValue = v;
    }
    explicit AMFVariant(const AMFRate& v) {
        type = AMF_VARIANT_RATE;
        rateValue = v;
    }
    ~AMFVariant() {
        if (type == AMF_VARIANT_INTERFACE && pInterface) {
            pInterface->Release();
            pInterface = nullptr;
        }
        type = AMF_VARIANT_EMPTY;
    }
    AMFVariant(const AMFVariant&) = delete;
    AMFVariant& operator=(const AMFVariant&) = delete;
};

class LK_AMF_NO_VTABLE AMFPropertyStorageObserver {
public:
    virtual void LK_AMF_STD_CALL OnPropertyChanged(const wchar_t* name) = 0;
};

class LK_AMF_NO_VTABLE AMFPropertyStorage : public AMFInterface {
public:
    virtual AMF_RESULT LK_AMF_STD_CALL SetProperty(const wchar_t* name,
                                                   AMFVariantStruct value) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetProperty(const wchar_t* name,
                                                   AMFVariantStruct* pValue) const = 0;
    virtual amf_bool LK_AMF_STD_CALL HasProperty(const wchar_t* name) const = 0;
    virtual amf_size LK_AMF_STD_CALL GetPropertyCount() const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetPropertyAt(amf_size index,
                                                     wchar_t* name,
                                                     amf_size nameSize,
                                                     AMFVariantStruct* pValue) const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Clear() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL AddTo(AMFPropertyStorage* pDest,
                                             amf_bool overwrite,
                                             amf_bool deep) const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CopyTo(AMFPropertyStorage* pDest,
                                              amf_bool deep) const = 0;
    virtual void LK_AMF_STD_CALL AddObserver(AMFPropertyStorageObserver* pObserver) = 0;
    virtual void LK_AMF_STD_CALL RemoveObserver(AMFPropertyStorageObserver* pObserver) = 0;

    AMF_RESULT SetProperty(const wchar_t* name, amf_bool value) {
        AMFVariantStruct v{};
        v.type = AMF_VARIANT_BOOL;
        v.boolValue = value;
        return SetProperty(name, v);
    }
    AMF_RESULT SetProperty(const wchar_t* name, amf_int64 value) {
        AMFVariantStruct v{};
        v.type = AMF_VARIANT_INT64;
        v.int64Value = value;
        return SetProperty(name, v);
    }
    AMF_RESULT SetProperty(const wchar_t* name, const AMFSize& value) {
        AMFVariantStruct v{};
        v.type = AMF_VARIANT_SIZE;
        v.sizeValue = value;
        return SetProperty(name, v);
    }
    AMF_RESULT SetProperty(const wchar_t* name, const AMFRate& value) {
        AMFVariantStruct v{};
        v.type = AMF_VARIANT_RATE;
        v.rateValue = value;
        return SetProperty(name, v);
    }

    AMF_RESULT GetProperty(const wchar_t* name, amf_bool* pValue) const {
        AMFVariant v;
        AMF_RESULT res = GetProperty(name, static_cast<AMFVariantStruct*>(&v));
        if (res == AMF_OK && v.type == AMF_VARIANT_BOOL) {
            *pValue = v.boolValue;
            return AMF_OK;
        }
        if (res == AMF_OK && v.type == AMF_VARIANT_INT64) {
            *pValue = (v.int64Value != 0);
            return AMF_OK;
        }
        return res == AMF_OK ? AMF_INVALID_DATA_TYPE : res;
    }
    AMF_RESULT GetProperty(const wchar_t* name, amf_int64* pValue) const {
        AMFVariant v;
        AMF_RESULT res = GetProperty(name, static_cast<AMFVariantStruct*>(&v));
        if (res == AMF_OK && v.type == AMF_VARIANT_INT64) {
            *pValue = v.int64Value;
            return AMF_OK;
        }
        return res == AMF_OK ? AMF_INVALID_DATA_TYPE : res;
    }
    AMF_RESULT GetProperty(const wchar_t* name, AMFSize* pValue) const {
        AMFVariant v;
        AMF_RESULT res = GetProperty(name, static_cast<AMFVariantStruct*>(&v));
        if (res == AMF_OK && v.type == AMF_VARIANT_SIZE) {
            *pValue = v.sizeValue;
            return AMF_OK;
        }
        return res == AMF_OK ? AMF_INVALID_DATA_TYPE : res;
    }
    AMF_RESULT GetProperty(const wchar_t* name, AMFRate* pValue) const {
        AMFVariant v;
        AMF_RESULT res = GetProperty(name, static_cast<AMFVariantStruct*>(&v));
        if (res == AMF_OK && v.type == AMF_VARIANT_RATE) {
            *pValue = v.rateValue;
            return AMF_OK;
        }
        return res == AMF_OK ? AMF_INVALID_DATA_TYPE : res;
    }
};

enum AMF_PROPERTY_ACCESS_TYPE {
    AMF_PROPERTY_ACCESS_PRIVATE = 0,
    AMF_PROPERTY_ACCESS_READ = 0x1,
    AMF_PROPERTY_ACCESS_WRITE = 0x2,
    AMF_PROPERTY_ACCESS_READ_WRITE = 0x3,
    AMF_PROPERTY_ACCESS_WRITE_RUNTIME = 0x4,
    AMF_PROPERTY_ACCESS_FULL = 0xFF,
};

struct AMFEnumDescriptionEntry {
    int value;
    const wchar_t* name;
};

struct AMFPropertyInfo {
    const wchar_t* name = nullptr;
    const wchar_t* desc = nullptr;
    AMF_VARIANT_TYPE type = AMF_VARIANT_EMPTY;
    amf_uint32 contentType = 0;
    AMFVariantStruct defaultValue{};
    AMFVariantStruct minValue{};
    AMFVariantStruct maxValue{};
    AMF_PROPERTY_ACCESS_TYPE accessType = AMF_PROPERTY_ACCESS_FULL;
    const AMFEnumDescriptionEntry* pEnumDescription = nullptr;

    virtual ~AMFPropertyInfo() {}

    amf_bool LK_AMF_STD_CALL AllowedRead() const {
        return (accessType & AMF_PROPERTY_ACCESS_READ) != 0;
    }
    amf_bool LK_AMF_STD_CALL AllowedWrite() const {
        return (accessType & AMF_PROPERTY_ACCESS_WRITE) != 0;
    }
    amf_bool LK_AMF_STD_CALL AllowedChangeInRuntime() const {
        return (accessType & AMF_PROPERTY_ACCESS_WRITE_RUNTIME) != 0;
    }
};

class LK_AMF_NO_VTABLE AMFPropertyStorageEx : public AMFPropertyStorage {
public:
    virtual amf_size LK_AMF_STD_CALL GetPropertiesInfoCount() const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetPropertyInfo(
        amf_size index,
        const AMFPropertyInfo** ppInfo) const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetPropertyInfo(
        const wchar_t* name,
        const AMFPropertyInfo** ppInfo) const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL ValidateProperty(
        const wchar_t* name,
        AMFVariantStruct value,
        AMFVariantStruct* pOutValidated) const = 0;
};

enum AMF_DATA_TYPE {
    AMF_DATA_BUFFER = 0,
    AMF_DATA_SURFACE = 1,
    AMF_DATA_AUDIO_BUFFER = 2,
    AMF_DATA_USER = 1000,
};

enum AMF_MEMORY_TYPE {
    AMF_MEMORY_UNKNOWN = 0,
    AMF_MEMORY_HOST = 1,
    AMF_MEMORY_DX9 = 2,
    AMF_MEMORY_DX11 = 3,
    AMF_MEMORY_OPENCL = 4,
    AMF_MEMORY_OPENGL = 5,
    AMF_MEMORY_XV = 6,
    AMF_MEMORY_GRALLOC = 7,
    AMF_MEMORY_COMPUTE_FOR_DX9 = 8,
    AMF_MEMORY_COMPUTE_FOR_DX11 = 9,
    AMF_MEMORY_VULKAN = 10,
    AMF_MEMORY_DX12 = 11,
};

enum AMF_DX_VERSION {
    AMF_DX9 = 90,
    AMF_DX9_EX = 91,
    AMF_DX11_0 = 110,
    AMF_DX11_1 = 111,
    AMF_DX12 = 120,
};

enum AMF_SURFACE_FORMAT {
    AMF_SURFACE_UNKNOWN = 0,
    AMF_SURFACE_NV12 = 1,
    AMF_SURFACE_YV12 = 2,
    AMF_SURFACE_BGRA = 3,
    AMF_SURFACE_ARGB = 4,
    AMF_SURFACE_RGBA = 5,
    AMF_SURFACE_GRAY8 = 6,
    AMF_SURFACE_YUV420P = 7,
};

enum AMF_PLANE_TYPE {
    AMF_PLANE_UNKNOWN = 0,
    AMF_PLANE_PACKED = 1,
    AMF_PLANE_Y = 2,
    AMF_PLANE_UV = 3,
    AMF_PLANE_U = 4,
    AMF_PLANE_V = 5,
};

enum AMF_FRAME_TYPE {
    AMF_FRAME_UNKNOWN = -1,
    AMF_FRAME_PROGRESSIVE = 0,
};

class LK_AMF_NO_VTABLE AMFData : public AMFPropertyStorage {
public:
    virtual AMF_MEMORY_TYPE LK_AMF_STD_CALL GetMemoryType() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Duplicate(AMF_MEMORY_TYPE type,
                                                 AMFData** ppData) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Convert(AMF_MEMORY_TYPE type) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Interop(AMF_MEMORY_TYPE type) = 0;
    virtual AMF_DATA_TYPE LK_AMF_STD_CALL GetDataType() = 0;
    virtual amf_bool LK_AMF_STD_CALL IsReusable() = 0;
    virtual void LK_AMF_STD_CALL SetPts(amf_pts pts) = 0;
    virtual amf_pts LK_AMF_STD_CALL GetPts() = 0;
    virtual void LK_AMF_STD_CALL SetDuration(amf_pts duration) = 0;
    virtual amf_pts LK_AMF_STD_CALL GetDuration() = 0;
};
using AMFDataPtr = AMFInterfacePtr_T<AMFData>;

class AMFBuffer;
class LK_AMF_NO_VTABLE AMFBufferObserver {
public:
    virtual void LK_AMF_STD_CALL OnBufferDataRelease(AMFBuffer* pBuffer) = 0;
};

class LK_AMF_NO_VTABLE AMFBuffer : public AMFData {
public:
    static const AMFGuid IID() {
        return {0xb04b7248, 0xb6f0, 0x4321, 0xb6, 0x91, 0xba, 0xa4, 0x74, 0x0f, 0x9f, 0xcb};
    }
    virtual AMF_RESULT LK_AMF_STD_CALL SetSize(amf_size newSize) = 0;
    virtual amf_size LK_AMF_STD_CALL GetSize() = 0;
    virtual void* LK_AMF_STD_CALL GetNative() = 0;
    virtual void LK_AMF_STD_CALL AddObserver(AMFBufferObserver* pObserver) = 0;
    virtual void LK_AMF_STD_CALL RemoveObserver(AMFBufferObserver* pObserver) = 0;
};
using AMFBufferPtr = AMFInterfacePtr_T<AMFBuffer>;

class AMFPlane;
class AMFSurface;
class LK_AMF_NO_VTABLE AMFSurfaceObserver {
public:
    virtual void LK_AMF_STD_CALL OnSurfaceDataRelease(AMFSurface* pSurface) = 0;
};

class LK_AMF_NO_VTABLE AMFSurface : public AMFData {
public:
    static const AMFGuid IID() {
        return {0x3075dbe3, 0x8718, 0x4cfa, 0x86, 0xfb, 0x21, 0x14, 0xc0, 0xa5, 0xa4, 0x51};
    }
    virtual AMF_SURFACE_FORMAT LK_AMF_STD_CALL GetFormat() = 0;
    virtual amf_size LK_AMF_STD_CALL GetPlanesCount() = 0;
    virtual AMFPlane* LK_AMF_STD_CALL GetPlaneAt(amf_size index) = 0;
    virtual AMFPlane* LK_AMF_STD_CALL GetPlane(AMF_PLANE_TYPE type) = 0;
    virtual AMF_FRAME_TYPE LK_AMF_STD_CALL GetFrameType() = 0;
    virtual void LK_AMF_STD_CALL SetFrameType(AMF_FRAME_TYPE type) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL SetCrop(amf_int32 x,
                                               amf_int32 y,
                                               amf_int32 width,
                                               amf_int32 height) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CopySurfaceRegion(AMFSurface* pDest,
                                                         amf_int32 dstX,
                                                         amf_int32 dstY,
                                                         amf_int32 srcX,
                                                         amf_int32 srcY,
                                                         amf_int32 width,
                                                         amf_int32 height) = 0;
    virtual void LK_AMF_STD_CALL AddObserver(AMFSurfaceObserver* pObserver) = 0;
    virtual void LK_AMF_STD_CALL RemoveObserver(AMFSurfaceObserver* pObserver) = 0;
};
using AMFSurfacePtr = AMFInterfacePtr_T<AMFSurface>;

class AMFAudioBuffer;
class AMFCompute;
class AMFComputeDevice;
class AMFComputeFactory;
using AMF_AUDIO_FORMAT = int;

class LK_AMF_NO_VTABLE AMFContext : public AMFPropertyStorage {
public:
    virtual AMF_RESULT LK_AMF_STD_CALL Terminate() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitDX9(void* pDX9Device) = 0;
    virtual void* LK_AMF_STD_CALL GetDX9Device(AMF_DX_VERSION dxVersionRequired = AMF_DX9) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockDX9() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockDX9() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitDX11(
        void* pDX11Device,
        AMF_DX_VERSION dxVersionRequired = AMF_DX11_0) = 0;
    virtual void* LK_AMF_STD_CALL GetDX11Device(
        AMF_DX_VERSION dxVersionRequired = AMF_DX11_0) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockDX11() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockDX11() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitOpenCL(void* pCommandQueue = nullptr) = 0;
    virtual void* LK_AMF_STD_CALL GetOpenCLContext() = 0;
    virtual void* LK_AMF_STD_CALL GetOpenCLCommandQueue() = 0;
    virtual void* LK_AMF_STD_CALL GetOpenCLDeviceID() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetOpenCLComputeFactory(
        AMFComputeFactory** ppFactory) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitOpenCLEx(AMFComputeDevice* pDevice) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockOpenCL() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockOpenCL() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitOpenGL(amf_handle hOpenGLContext,
                                                  amf_handle hWindow,
                                                  amf_handle hDC) = 0;
    virtual amf_handle LK_AMF_STD_CALL GetOpenGLContext() = 0;
    virtual amf_handle LK_AMF_STD_CALL GetOpenGLDrawable() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockOpenGL() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockOpenGL() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitXV(void* pXVDevice) = 0;
    virtual void* LK_AMF_STD_CALL GetXVDevice() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockXV() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockXV() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL InitGralloc(void* pGrallocDevice) = 0;
    virtual void* LK_AMF_STD_CALL GetGrallocDevice() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL LockGralloc() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL UnlockGralloc() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL AllocBuffer(AMF_MEMORY_TYPE type,
                                                   amf_size size,
                                                   AMFBuffer** ppBuffer) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL AllocSurface(AMF_MEMORY_TYPE type,
                                                    AMF_SURFACE_FORMAT format,
                                                    amf_int32 width,
                                                    amf_int32 height,
                                                    AMFSurface** ppSurface) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL AllocAudioBuffer(
        AMF_MEMORY_TYPE type,
        AMF_AUDIO_FORMAT format,
        amf_int32 samples,
        amf_int32 sampleRate,
        amf_int32 channels,
        AMFAudioBuffer** ppAudioBuffer) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateBufferFromHostNative(
        void* pHostBuffer,
        amf_size size,
        AMFBuffer** ppBuffer,
        AMFBufferObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromHostNative(
        AMF_SURFACE_FORMAT format,
        amf_int32 width,
        amf_int32 height,
        amf_int32 hPitch,
        amf_int32 vPitch,
        void* pData,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromDX9Native(
        void* pDX9Surface,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromDX11Native(
        void* pDX11Surface,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromOpenGLNative(
        AMF_SURFACE_FORMAT format,
        amf_handle hGLTextureID,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromGrallocNative(
        amf_handle hGrallocSurface,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateSurfaceFromOpenCLNative(
        AMF_SURFACE_FORMAT format,
        amf_int32 width,
        amf_int32 height,
        void** pClPlanes,
        AMFSurface** ppSurface,
        AMFSurfaceObserver* pObserver) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateBufferFromOpenCLNative(
        void* pCLBuffer,
        amf_size size,
        AMFBuffer** ppBuffer) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetCompute(AMF_MEMORY_TYPE eMemType,
                                                  AMFCompute** ppCompute) = 0;
};
using AMFContextPtr = AMFInterfacePtr_T<AMFContext>;

enum AMF_ACCELERATION_TYPE {
    AMF_ACCEL_NOT_SUPPORTED = -1,
    AMF_ACCEL_HARDWARE,
    AMF_ACCEL_GPU,
    AMF_ACCEL_SOFTWARE,
};

class LK_AMF_NO_VTABLE AMFIOCaps : public AMFInterface {
public:
    virtual void LK_AMF_STD_CALL GetWidthRange(amf_int32* minWidth,
                                               amf_int32* maxWidth) const = 0;
    virtual void LK_AMF_STD_CALL GetHeightRange(amf_int32* minHeight,
                                                amf_int32* maxHeight) const = 0;
    virtual amf_int32 LK_AMF_STD_CALL GetVertAlign() const = 0;
    virtual amf_int32 LK_AMF_STD_CALL GetNumOfFormats() const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetFormatAt(amf_int32 index,
                                                   AMF_SURFACE_FORMAT* format,
                                                   amf_bool* native) const = 0;
    virtual amf_int32 LK_AMF_STD_CALL GetNumOfMemoryTypes() const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetMemoryTypeAt(amf_int32 index,
                                                       AMF_MEMORY_TYPE* memType,
                                                       amf_bool* native) const = 0;
    virtual amf_bool LK_AMF_STD_CALL IsInterlacedSupported() const = 0;
};
using AMFIOCapsPtr = AMFInterfacePtr_T<AMFIOCaps>;

class LK_AMF_NO_VTABLE AMFCaps : public AMFPropertyStorage {
public:
    virtual AMF_ACCELERATION_TYPE LK_AMF_STD_CALL GetAccelerationType() const = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetInputCaps(AMFIOCaps** input) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetOutputCaps(AMFIOCaps** output) = 0;
};
using AMFCapsPtr = AMFInterfacePtr_T<AMFCaps>;

class AMFDataAllocatorCB;
class AMFComponentOptimizationCallback;

class LK_AMF_NO_VTABLE AMFComponent : public AMFPropertyStorageEx {
public:
    virtual AMF_RESULT LK_AMF_STD_CALL Init(AMF_SURFACE_FORMAT format,
                                            amf_int32 width,
                                            amf_int32 height) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL ReInit(amf_int32 width,
                                              amf_int32 height) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Terminate() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Drain() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Flush() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL SubmitInput(AMFData* pData) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL QueryOutput(AMFData** ppData) = 0;
    virtual AMFContext* LK_AMF_STD_CALL GetContext() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL SetOutputDataAllocatorCB(
        AMFDataAllocatorCB* callback) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetCaps(AMFCaps** ppCaps) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL Optimize(
        AMFComponentOptimizationCallback* pCallback) = 0;
};
using AMFComponentPtr = AMFInterfacePtr_T<AMFComponent>;

class AMFDebug;
class AMFTrace;
class AMFPrograms;

class LK_AMF_NO_VTABLE AMFFactory {
public:
    virtual AMF_RESULT LK_AMF_STD_CALL CreateContext(AMFContext** ppContext) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL CreateComponent(
        AMFContext* pContext,
        const wchar_t* id,
        AMFComponent** ppComponent) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL SetCacheFolder(const wchar_t* path) = 0;
    virtual const wchar_t* LK_AMF_STD_CALL GetCacheFolder() = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetDebug(AMFDebug** ppDebug) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetTrace(AMFTrace** ppTrace) = 0;
    virtual AMF_RESULT LK_AMF_STD_CALL GetPrograms(AMFPrograms** ppPrograms) = 0;
};

using AMFInit_Fn = AMF_RESULT(LK_AMF_CDECL_CALL*)(amf_uint64 version,
                                                  AMFFactory** ppFactory);
using AMFQueryVersion_Fn = AMF_RESULT(LK_AMF_CDECL_CALL*)(amf_uint64* pVersion);

// AVC / HEVC constants matching VideoEncoderVCE.h & VideoEncoderHEVC.h
constexpr wchar_t kAmfVideoEncoderAvc[] = L"AMFVideoEncoderVCE_AVC";
constexpr wchar_t kAmfVideoEncoderHevc[] = L"AMFVideoEncoderHW_HEVC";

constexpr amf_int64 kAmfColorBitDepth8 = 8;

// AVC enums
constexpr amf_int64 kAmfAvcUsageUltraLowLatency = 1;
constexpr amf_int64 kAmfAvcProfileBaseline = 66;
constexpr amf_int64 kAmfAvcScanTypeProgressive = 0;
constexpr amf_int64 kAmfAvcRateControlCbr = 1;
constexpr amf_int64 kAmfAvcQualityPresetSpeed = 1;
constexpr amf_int64 kAmfAvcPictureTypeIdr = 2;
constexpr amf_int64 kAmfAvcPictureTypeP = 4;
constexpr amf_int64 kAmfAvcOutputDataTypeIdr = 0;
constexpr amf_int64 kAmfAvcOutputDataTypeI = 1;
constexpr amf_int64 kAmfAvcOutputDataTypeP = 2;
constexpr amf_int64 kAmfAvcPreencodeDisabled = 0;
constexpr amf_int64 kAmfAvcCodingCavlc = 2;
constexpr amf_int64 kAmfAvcOutputModeFrame = 0;

// AVC properties
constexpr wchar_t kPropAvcUsage[] = L"Usage";
constexpr wchar_t kPropAvcQualityPreset[] = L"QualityPreset";
constexpr wchar_t kPropAvcFrameSize[] = L"FrameSize";
constexpr wchar_t kPropAvcProfile[] = L"Profile";
constexpr wchar_t kPropAvcProfileLevel[] = L"ProfileLevel";
constexpr wchar_t kPropAvcScanType[] = L"ScanType";
constexpr wchar_t kPropAvcLowLatencyMode[] = L"LowLatencyInternal";
constexpr wchar_t kPropAvcPreAnalysisEnable[] = L"EnablePreAnalysis";
constexpr wchar_t kPropAvcPreencodeEnable[] = L"RateControlPreanalysisEnable";
constexpr wchar_t kPropAvcRateControlMethod[] = L"RateControlMethod";
constexpr wchar_t kPropAvcFrameRate[] = L"FrameRate";
constexpr wchar_t kPropAvcTargetBitrate[] = L"TargetBitrate";
constexpr wchar_t kPropAvcPeakBitrate[] = L"PeakBitrate";
constexpr wchar_t kPropAvcVbvBufferSize[] = L"VBVBufferSize";
constexpr wchar_t kPropAvcSkipFrameEnable[] = L"RateControlSkipFrameEnable";
constexpr wchar_t kPropAvcBPicPattern[] = L"BPicturesPattern";
constexpr wchar_t kPropAvcMaxConsecutiveBPictures[] = L"MaxConsecutiveBPictures";
constexpr wchar_t kPropAvcMaxNumRefFrames[] = L"MaxNumRefFrames";
constexpr wchar_t kPropAvcMaxNumTemporalLayers[] = L"MaxNumOfTemporalLayers";
constexpr wchar_t kPropAvcSlicesPerFrame[] = L"SlicesPerFrame";
constexpr wchar_t kPropAvcOutputMode[] = L"OutputMode";
constexpr wchar_t kPropAvcCabacEnable[] = L"CABACEnable";
constexpr wchar_t kPropAvcIdrPeriod[] = L"IDRPeriod";
constexpr wchar_t kPropAvcExtraData[] = L"ExtraData";
constexpr wchar_t kPropAvcForcePictureType[] = L"ForcePictureType";
constexpr wchar_t kPropAvcInsertSps[] = L"InsertSPS";
constexpr wchar_t kPropAvcInsertPps[] = L"InsertPPS";
constexpr wchar_t kPropAvcOutputDataType[] = L"OutputDataType";
constexpr wchar_t kPropAvcCapMaxBitrate[] = L"MaxBitrate";
constexpr wchar_t kPropAvcCapMaxLevel[] = L"MaxLevel";

// HEVC enums
constexpr amf_int64 kAmfHevcUsageUltraLowLatency = 1;
constexpr amf_int64 kAmfHevcProfileMain = 1;
constexpr amf_int64 kAmfHevcTierMain = 0;
constexpr amf_int64 kAmfHevcRateControlCbr = 3;
constexpr amf_int64 kAmfHevcQualityPresetSpeed = 10;
constexpr amf_int64 kAmfHevcPictureTypeIdr = 2;
constexpr amf_int64 kAmfHevcPictureTypeP = 4;
constexpr amf_int64 kAmfHevcOutputDataTypeIdr = 0;
constexpr amf_int64 kAmfHevcOutputDataTypeI = 1;
constexpr amf_int64 kAmfHevcOutputDataTypeP = 2;
constexpr amf_int64 kAmfHevcHeaderInsertionIdrAligned = 2;
constexpr amf_int64 kAmfHevcOutputModeFrame = 0;

// HEVC properties
constexpr wchar_t kPropHevcUsage[] = L"HevcUsage";
constexpr wchar_t kPropHevcQualityPreset[] = L"HevcQualityPreset";
constexpr wchar_t kPropHevcFrameSize[] = L"HevcFrameSize";
constexpr wchar_t kPropHevcProfile[] = L"HevcProfile";
constexpr wchar_t kPropHevcTier[] = L"HevcTier";
constexpr wchar_t kPropHevcProfileLevel[] = L"HevcProfileLevel";
constexpr wchar_t kPropHevcColorBitDepth[] = L"HevcColorBitDepth";
constexpr wchar_t kPropHevcRateControlMethod[] = L"HevcRateControlMethod";
constexpr wchar_t kPropHevcFrameRate[] = L"HevcFrameRate";
constexpr wchar_t kPropHevcTargetBitrate[] = L"HevcTargetBitrate";
constexpr wchar_t kPropHevcPeakBitrate[] = L"HevcPeakBitrate";
constexpr wchar_t kPropHevcVbvBufferSize[] = L"HevcVBVBufferSize";
constexpr wchar_t kPropHevcMaxNumRefFrames[] = L"HevcMaxNumRefFrames";
constexpr wchar_t kPropHevcMaxLtrFrames[] = L"HevcMaxOfLTRFrames";
constexpr wchar_t kPropHevcMaxNumTemporalLayers[] = L"HevcMaxNumOfTemporalLayers";
constexpr wchar_t kPropHevcSlicesPerFrame[] = L"HevcSlicesPerFrame";
constexpr wchar_t kPropHevcOutputMode[] = L"HevcOutputMode";
constexpr wchar_t kPropHevcNumGopsPerIdr[] = L"HevcGOPSPerIDR";
constexpr wchar_t kPropHevcGopSize[] = L"HevcGOPSize";
constexpr wchar_t kPropHevcHeaderInsertionMode[] = L"HevcHeaderInsertionMode";
constexpr wchar_t kPropHevcLowLatencyMode[] = L"LowLatencyInternal";
constexpr wchar_t kPropHevcPreAnalysisEnable[] = L"HevcEnablePreAnalysis";
constexpr wchar_t kPropHevcPreencodeEnable[] = L"HevcRateControlPreAnalysisEnable";
constexpr wchar_t kPropHevcSkipFrameEnable[] = L"HevcRateControlSkipFrameEnable";
constexpr wchar_t kPropHevcExtraData[] = L"HevcExtraData";
constexpr wchar_t kPropHevcForcePictureType[] = L"HevcForcePictureType";
constexpr wchar_t kPropHevcInsertHeader[] = L"HevcInsertHeader";
constexpr wchar_t kPropHevcOutputDataType[] = L"HevcOutputDataType";
constexpr wchar_t kPropHevcCapMaxBitrate[] = L"HevcMaxBitrate";
constexpr wchar_t kPropHevcCapMaxLevel[] = L"HevcMaxLevel";
constexpr wchar_t kPropHevcCapMaxProfile[] = L"HevcMaxProfile";
constexpr wchar_t kPropHevcCapMaxTier[] = L"HevcMaxTier";

}  // namespace amf_abi

using namespace amf_abi;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using webrtc::ArrayView;
using webrtc::BitstreamReader;
using webrtc::CodecSpecificInfo;
using webrtc::EncodedImage;
using webrtc::EncodedImageBuffer;
using webrtc::EncodedImageCallback;
using webrtc::H264BitstreamParser;
using webrtc::NV12Buffer;
using webrtc::NV12BufferInterface;
using webrtc::PpsParser;
using webrtc::ScalabilityMode;
using webrtc::SequenceChecker;
using webrtc::SpsParser;
using webrtc::VideoCodec;
using webrtc::VideoCodecMode;
using webrtc::VideoCodecType;
using webrtc::VideoContentType;
using webrtc::VideoEncoder;
using webrtc::VideoFrameBuffer;
using webrtc::VideoFrameType;
using webrtc::external_encoder::AmfCapability;
using webrtc::external_encoder::AmfHevcLevelViolation;
using webrtc::external_encoder::Codec;
using webrtc::external_encoder::Config;
using webrtc::external_encoder::FindAmfHevcLevelLimits;
using webrtc::external_encoder::Frame;
using webrtc::external_encoder::HardwareEncoderProbes;
using webrtc::external_encoder::HardwareEncoderVendor;
using webrtc::external_encoder::Info;
using webrtc::external_encoder::Packet;
using webrtc::external_encoder::Rates;
using webrtc::external_encoder::Status;

namespace H264 = webrtc::H264;
#if defined(RTC_ENABLE_H265)
namespace H265 = webrtc::H265;
using webrtc::H265BitstreamParser;
using webrtc::H265PpsParser;
using webrtc::H265SpsParser;
using webrtc::StringToH265Level;
#endif

constexpr std::uint32_t kAmdVendor = 0x1002;
constexpr std::size_t kInputSlots = 4;
constexpr std::size_t kMaximumPacketBytes = 64 * 1024 * 1024;
constexpr auto kFirstFrameBudget = std::chrono::milliseconds(500);
constexpr auto kFrameBudget = std::chrono::milliseconds(100);
constexpr auto kWorkerReplySlack = std::chrono::milliseconds(20);
constexpr auto kControlBudget = std::chrono::milliseconds(1000);
constexpr auto kCloseBudget = std::chrono::milliseconds(100);
constexpr wchar_t kFrameIdProperty[] = L"PTS";

std::atomic<std::uint64_t> g_dx11_native_frames_submitted{0};
std::atomic<std::uint64_t> g_host_frames_submitted{0};
std::atomic<std::uint64_t> g_simulcast_gpu_scales{0};
std::atomic<std::uint64_t> g_cross_device_shared_opens{0};

std::string Utf8(const wchar_t* value) {
    if (!value) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string AmfFailure(const char* operation, AMF_RESULT result) {
    return std::string(operation) +
           " failed, AMF_RESULT=" + std::to_string(static_cast<int>(result));
}

bool ValidLevel(std::uint8_t level) {
    constexpr std::array<std::uint8_t, 19> levels = {
        10, 11, 12, 13, 20, 21, 22, 30, 31, 32,
        40, 41, 42, 50, 51, 52, 60, 61, 62};
    for (const auto supported : levels) {
        if (level == supported) return true;
    }
    return false;
}

bool ValidRates(const Rates& rates) {
    return rates.bitrate_bps > 0 && rates.bitrate_bps <= 65535000 &&
           rates.framerate_fps > 0 && rates.framerate_fps <= 240;
}

bool ValidH265Level(std::uint8_t level) {
    return FindAmfHevcLevelLimits(level) != nullptr;
}

bool ValidConfig(const Config& config, Codec codec) {
    return config.codec == codec && config.width >= 16 && config.height >= 16 &&
           config.width <= 8192 && config.height <= 8192 &&
           config.width % 2 == 0 && config.height % 2 == 0 &&
           (codec == Codec::kH264 ? ValidLevel(config.h264_level)
                                  : ValidH265Level(config.h265_level)) &&
           ValidRates({config.bitrate_bps, config.framerate_fps});
}

bool HasNal(ArrayView<const std::uint8_t> bytes, H264::NaluType type) {
    for (const auto& index : H264::FindNaluIndices(bytes)) {
        if (index.payload_size &&
            H264::ParseNaluType(bytes[index.payload_start_offset]) == type) {
            return true;
        }
    }
    return false;
}

bool AnnexB(ArrayView<const std::uint8_t> bytes) {
    const auto indices = H264::FindNaluIndices(bytes);
    if (indices.empty() || indices.front().start_offset != 0 ||
        indices.back().payload_start_offset + indices.back().payload_size !=
            bytes.size()) {
        return false;
    }
    for (const auto& index : indices) {
        if (!index.payload_size ||
            (bytes[index.payload_start_offset] & 0x80) != 0) {
            return false;
        }
        const auto type = H264::ParseNaluType(bytes[index.payload_start_offset]);
        if (type != H264::kSlice && type != H264::kIdr && type != H264::kSei &&
            type != H264::kSps && type != H264::kPps && type != H264::kAud &&
            type != H264::kFiller) {
            return false;
        }
    }
    return true;
}

bool ValidateHeaders(ArrayView<const std::uint8_t> bytes,
                     const Config& config,
                     std::uint32_t* pps_id) {
    std::optional<SpsParser::SpsState> sps;
    std::optional<PpsParser::PpsState> pps;
    for (const auto& index : H264::FindNaluIndices(bytes)) {
        const auto nal =
            bytes.subview(index.payload_start_offset, index.payload_size);
        if (nal.empty()) return false;
        const auto type = H264::ParseNaluType(nal[0]);
        if (type == H264::kSps) {
            if (sps || nal.size() < 4 || nal[1] != 66 || !ValidLevel(nal[3]) ||
                nal[3] > config.h264_level) {
                return false;
            }
            sps = SpsParser::ParseSps(nal.subview(1));
            if (!sps || sps->width != static_cast<std::uint32_t>(config.width) ||
                sps->height != static_cast<std::uint32_t>(config.height) ||
                sps->chroma_format_idc != 1 || !sps->frame_mbs_only_flag ||
                sps->max_num_ref_frames > 1) {
                return false;
            }
        } else if (type == H264::kPps) {
            if (pps) return false;
            pps = PpsParser::ParsePps(nal.subview(1));
            if (!pps || pps->entropy_coding_mode_flag ||
                pps->num_ref_idx_l0_default_active_minus1 != 0) {
                return false;
            }
        }
    }
    if (!sps || !pps || pps->sps_id != sps->id) return false;
    *pps_id = pps->id;
    return true;
}

bool ValidatePicture(ArrayView<const std::uint8_t> bytes,
                     bool idr,
                     std::uint32_t expected_pps_id) {
    int slices = 0;
    for (const auto& index : H264::FindNaluIndices(bytes)) {
        const auto nal =
            bytes.subview(index.payload_start_offset, index.payload_size);
        const auto type = H264::ParseNaluType(nal[0]);
        if (type != H264::kSlice && type != H264::kIdr) continue;
        if (++slices != 1 || nal.size() <= 1 || (type == H264::kIdr) != idr) {
            return false;
        }
        const auto rbsp = H264::ParseRbsp(nal.subview(1));
        BitstreamReader reader(rbsp);
        const auto first_mb = reader.ReadExponentialGolomb();
        const auto slice_type = reader.ReadExponentialGolomb();
        const auto pps_id = reader.ReadExponentialGolomb();
        if (!reader.Ok() || first_mb != 0 || slice_type > 9 ||
            pps_id != expected_pps_id ||
            (slice_type % 5 != H264::kP && slice_type % 5 != H264::kI) ||
            (idr && slice_type % 5 != H264::kI)) {
            return false;
        }
    }
    return slices == 1;
}

#if defined(RTC_ENABLE_H265)
bool HasH265Nal(ArrayView<const std::uint8_t> bytes, H265::NaluType type) {
    for (const auto& index : H265::FindNaluIndices(bytes)) {
        if (index.payload_size >= 2 &&
            H265::ParseNaluType(bytes[index.payload_start_offset]) == type) {
            return true;
        }
    }
    return false;
}

bool RejectH265(std::string* reason, const std::string& message) {
    RTC_LOG(LS_WARNING) << "AMF_HEVC_REJECT " << message;
    if (reason) *reason = message;
    return false;
}

bool H265AnnexB(ArrayView<const std::uint8_t> bytes,
                std::string* reason = nullptr) {
    const auto indices = H265::FindNaluIndices(bytes);
    if (indices.empty() || indices.front().start_offset != 0 ||
        indices.back().payload_start_offset + indices.back().payload_size !=
            bytes.size()) {
        std::ostringstream details;
        details << "AnnexB framing nals=" << indices.size()
                << " bytes=" << bytes.size();
        return RejectH265(reason, details.str());
    }
    for (const auto& index : indices) {
        const auto nal =
            bytes.subview(index.payload_start_offset, index.payload_size);
        if (nal.size() < 2 || (nal[0] & 0x81) != 0 || nal[1] != 1) {
            return RejectH265(reason, "invalid HEVC NAL header");
        }
        const auto type = H265::ParseNaluType(nal[0]);
        if (type != H265::kTrailN && type != H265::kTrailR &&
            type != H265::kIdrWRadl && type != H265::kIdrNLp &&
            type != H265::kVps && type != H265::kSps && type != H265::kPps &&
            type != H265::kAud && type != H265::kPrefixSei &&
            type != H265::kSuffixSei && type != 38) {
            return RejectH265(
                reason,
                "unexpected NAL type=" + std::to_string(static_cast<int>(type)));
        }
    }
    return true;
}

bool ValidateH265Ptl(BitstreamReader& reader,
                     const Config& config,
                     std::string_view parameter_set,
                     std::string* reason,
                     std::uint8_t* emitted_level) {
    const auto space = reader.ReadBits(2);
    const bool high_tier = reader.Read<bool>();
    const auto profile = reader.ReadBits(5);
    reader.ConsumeBits(32);
    reader.ConsumeBits(1);
    const bool interlaced = reader.Read<bool>();
    reader.ConsumeBits(46);
    const auto level = reader.ReadBits(8);
    if (emitted_level && reader.Ok()) {
        *emitted_level = static_cast<std::uint8_t>(level);
    }
    const bool valid =
        reader.Ok() && space == 0 && !high_tier && profile == 1 && !interlaced &&
        ValidH265Level(static_cast<std::uint8_t>(level)) &&
        level <= config.h265_level;
    if (!valid) {
        return RejectH265(reason, std::string(parameter_set) + " PTL invalid");
    }
    return true;
}

bool ValidateH265Vps(ArrayView<const std::uint8_t> data,
                     const Config& config,
                     std::uint32_t* vps_id,
                     std::uint8_t* level_idc,
                     std::string* reason) {
    const auto rbsp = H265::ParseRbsp(data);
    BitstreamReader reader(rbsp);
    *vps_id = reader.ReadBits(4);
    const bool internal = reader.Read<bool>();
    const bool available = reader.Read<bool>();
    const auto max_layers = reader.ReadBits(6);
    const auto max_sublayers = reader.ReadBits(3);
    const bool nesting = reader.Read<bool>();
    const auto reserved = reader.ReadBits(16);
    if (!reader.Ok() || !internal || !available || max_layers != 0 ||
        max_sublayers != 0 || !nesting || reserved != 0xffff) {
        return RejectH265(reason, "VPS header invalid");
    }
    if (!ValidateH265Ptl(reader, config, "VPS", reason, level_idc)) {
        return false;
    }
    reader.ConsumeBits(1);
    const auto buffering = reader.ReadExponentialGolomb();
    const auto reorder = reader.ReadExponentialGolomb();
    (void)reader.ReadExponentialGolomb();
    const auto max_layer_id = reader.ReadBits(6);
    const auto layer_sets = reader.ReadExponentialGolomb();
    if (!reader.Ok() || buffering > 1 || reorder != 0 ||
        max_layer_id != 0 || layer_sets != 0) {
        return RejectH265(reason, "VPS order invalid");
    }
    return true;
}

bool ValidateH265Sps(ArrayView<const std::uint8_t> data,
                     const Config& config,
                     std::string* reason,
                     std::string* description) {
    const auto rbsp = H265::ParseRbsp(data);
    BitstreamReader reader(rbsp);
    const auto vps_id = reader.ReadBits(4);
    const auto sublayers = reader.ReadBits(3);
    const bool nesting = reader.Read<bool>();
    std::uint8_t level_idc = 0;
    if (!reader.Ok() || sublayers != 0 || !nesting) {
        return RejectH265(reason, "SPS header invalid");
    }
    if (!ValidateH265Ptl(reader, config, "SPS", reason, &level_idc)) {
        return false;
    }
    const auto sps_id = reader.ReadExponentialGolomb();
    const auto chroma = reader.ReadExponentialGolomb();
    if (!reader.Ok() || chroma != 1) {
        return RejectH265(reason, "SPS chroma invalid");
    }
    const auto coded_width = reader.ReadExponentialGolomb();
    const auto coded_height = reader.ReadExponentialGolomb();
    std::array<std::uint32_t, 4> crop{};
    const bool conformance_window = reader.Read<bool>();
    if (conformance_window) {
        for (auto& offset : crop) {
            offset = reader.ReadExponentialGolomb();
        }
    }
    const auto luma_depth = reader.ReadExponentialGolomb();
    const auto chroma_depth = reader.ReadExponentialGolomb();
    reader.ReadExponentialGolomb();
    reader.ConsumeBits(1);
    const auto buffering = reader.ReadExponentialGolomb();
    const auto reorder = reader.ReadExponentialGolomb();
    (void)reader.ReadExponentialGolomb();
    std::ostringstream details;
    details << "SPS vpsId=" << vps_id << " spsId=" << sps_id
            << " coded=" << coded_width << "x" << coded_height;
    *description = details.str();
    if (!reader.Ok() || luma_depth != 0 || chroma_depth != 0 ||
        buffering > 1 || reorder != 0) {
        return RejectH265(reason, *description);
    }
    const char* violation =
        AmfHevcLevelViolation(level_idc, coded_width, coded_height,
                              config.framerate_fps, config.bitrate_bps);
    if (violation) {
        return RejectH265(reason, std::string(violation));
    }
    return true;
}

bool ValidateH265Headers(ArrayView<const std::uint8_t> bytes,
                         const Config& config,
                         H265PpsParser::PpsState* state,
                         std::string* reason) {
    std::optional<std::uint32_t> vps;
    std::uint8_t vps_level = 0;
    std::optional<H265SpsParser::SpsState> sps;
    std::optional<H265PpsParser::PpsState> pps;
    std::string sps_description;
    ArrayView<const std::uint8_t> pps_data;
    for (const auto& index : H265::FindNaluIndices(bytes)) {
        const auto nal =
            bytes.subview(index.payload_start_offset, index.payload_size);
        if (nal.size() <= 2) {
            return RejectH265(reason, "parameter-set NAL too short");
        }
        const auto type = H265::ParseNaluType(nal[0]);
        if (type == H265::kVps) {
            std::uint32_t id = 0;
            if (vps ||
                !ValidateH265Vps(nal.subview(2), config, &id, &vps_level, reason)) {
                return false;
            }
            vps = id;
        } else if (type == H265::kSps) {
            if (sps ||
                !ValidateH265Sps(nal.subview(2), config, reason, &sps_description)) {
                return false;
            }
            sps = H265SpsParser::ParseSps(nal.subview(2));
            if (!sps || sps->chroma_format_idc != 1 ||
                sps->sps_max_sub_layers_minus1 != 0 ||
                sps->bit_depth_luma_minus8 != 0 ||
                sps->width != static_cast<std::uint32_t>(config.width) ||
                sps->height != static_cast<std::uint32_t>(config.height) ||
                sps->width > sps->pic_width_in_luma_samples ||
                sps->height > sps->pic_height_in_luma_samples) {
                return RejectH265(reason, "SPS parsed contract failed");
            }
        } else if (type == H265::kPps) {
            if (!pps_data.empty()) return RejectH265(reason, "duplicate PPS");
            pps_data = nal.subview(2);
        }
    }
    if (!vps || !sps || pps_data.empty() || sps->vps_id != *vps) {
        return RejectH265(reason, "parameter-set links invalid");
    }
    const char* vps_violation =
        AmfHevcLevelViolation(vps_level, sps->pic_width_in_luma_samples,
                              sps->pic_height_in_luma_samples,
                              config.framerate_fps, config.bitrate_bps);
    if (vps_violation) {
        return RejectH265(reason, vps_violation);
    }
    pps = H265PpsParser::ParsePps(pps_data, &*sps);
    if (!pps || pps->sps_id != sps->sps_id ||
        pps->num_ref_idx_l0_default_active_minus1 != 0 ||
        pps->num_ref_idx_l1_default_active_minus1 != 0) {
        return RejectH265(reason, "PPS parsed contract failed");
    }
    *state = *pps;
    return true;
}

bool ValidateH265Picture(ArrayView<const std::uint8_t> bytes,
                         bool idr,
                         const H265PpsParser::PpsState& pps) {
    int slices = 0;
    for (const auto& index : H265::FindNaluIndices(bytes)) {
        const auto nal =
            bytes.subview(index.payload_start_offset, index.payload_size);
        const auto type = H265::ParseNaluType(nal[0]);
        if (type > 31) continue;
        const bool actual_idr = type == H265::kIdrWRadl || type == H265::kIdrNLp;
        if (++slices != 1 || nal.size() <= 2 || actual_idr != idr ||
            (!actual_idr && type != H265::kTrailN && type != H265::kTrailR)) {
            return false;
        }
        const auto rbsp = H265::ParseRbsp(nal.subview(2));
        BitstreamReader reader(rbsp);
        const bool first_slice = reader.Read<bool>();
        if (actual_idr) reader.ConsumeBits(1);
        const auto pps_id = reader.ReadExponentialGolomb();
        reader.ConsumeBits(pps.num_extra_slice_header_bits);
        const auto slice_type = reader.ReadExponentialGolomb();
        if (!reader.Ok() || !first_slice || pps_id != pps.pps_id ||
            (slice_type != H265::kP && slice_type != H265::kI) ||
            (idr && slice_type != H265::kI)) {
            return false;
        }
    }
    return slices == 1;
}
#endif

AmfCapability DescribeAdapter(std::uint32_t index,
                              const DXGI_ADAPTER_DESC& desc,
                              Codec codec) {
    AmfCapability capability;
    capability.codec = codec;
    capability.sdk_version = kAmfFullVersion;
    capability.adapter_index = index;
    capability.vendor_id = desc.VendorId;
    capability.device_id = desc.DeviceId;
    capability.dedicated_video_memory_bytes = desc.DedicatedVideoMemory;
    capability.adapter_name = Utf8(desc.Description);
    return capability;
}

class AmfSession {
public:
    ~AmfSession() { Close(); }
    AMFComponent* encoder() { return encoder_; }
    AMFContext* context() { return context_; }
    ID3D11Device* device() { return device_.Get(); }
    ID3D11DeviceContext* device_context() { return device_context_.Get(); }
    LUID adapter_luid() const noexcept { return adapter_luid_; }

    AMF_RESULT TerminateEncoder() {
        if (encoder_ && !encoder_terminated_) {
            encoder_termination_result_ = encoder_->Terminate();
            encoder_terminated_ = true;
        }
        return encoder_termination_result_;
    }

    Status Open(IDXGIAdapter1* adapter,
                const DXGI_ADAPTER_DESC& adapter_desc,
                AmfCapability* capability) {
        Close();
        adapter_luid_ = adapter_desc.AdapterLuid;
        module_ = LoadLibraryExW(
            L"amfrt64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) {
            return Fail(capability, Status::kUnavailable,
                        "System32 amfrt64.dll unavailable, Win32=" +
                            std::to_string(GetLastError()));
        }
        wchar_t module_path[32768] = {};
        if (GetModuleFileNameW(module_, module_path,
                               static_cast<DWORD>(std::size(module_path)))) {
            capability->runtime_path = Utf8(module_path);
        }
        const auto init = reinterpret_cast<AMFInit_Fn>(
            GetProcAddress(module_, "AMFInit"));
        const auto version = reinterpret_cast<AMFQueryVersion_Fn>(
            GetProcAddress(module_, "AMFQueryVersion"));
        if (!init || !version) {
            return Fail(capability, Status::kUnavailable,
                        "AMFInit/AMFQueryVersion export unavailable");
        }
        amf_uint64 runtime_version = 0;
        AMF_RESULT result = version(&runtime_version);
        capability->runtime_version = runtime_version;
        if (result != AMF_OK) {
            return Fail(capability, Status::kUnavailable,
                        AmfFailure("AMFQueryVersion", result));
        }
        result = init(kAmfFullVersion, &factory_);
        if (result != AMF_OK || !factory_) {
            return Fail(capability, Status::kUnavailable,
                        AmfFailure("AMFInit", result));
        }
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0};
        const HRESULT created = D3D11CreateDevice(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested, static_cast<UINT>(std::size(requested)),
            D3D11_SDK_VERSION, &device_, &level, &device_context_);
        if (FAILED(created)) {
            return Fail(capability, Status::kUnavailable,
                        "AMD D3D11 hardware device creation failed, HRESULT=" +
                            std::to_string(static_cast<std::uint32_t>(created)));
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }
        result = factory_->CreateContext(&context_);
        if (result != AMF_OK || !context_) {
            return Fail(capability, Status::kUnavailable,
                        AmfFailure("CreateContext", result));
        }
        result = context_->InitDX11(device_.Get(), AMF_DX11_1);
        if (result != AMF_OK) {
            return Fail(capability, Status::kUnavailable,
                        AmfFailure("InitDX11", result));
        }
        const wchar_t* component = kAmfVideoEncoderAvc;
#if defined(RTC_ENABLE_H265)
        if (capability->codec == Codec::kH265) {
            component = kAmfVideoEncoderHevc;
        }
#endif
        result = factory_->CreateComponent(context_, component, &encoder_);
        encoder_terminated_ = false;
        if (result != AMF_OK || !encoder_) {
            return Fail(capability, Status::kUnavailable,
                        AmfFailure(capability->codec == Codec::kH264
                                       ? "CreateComponent AVC"
                                       : "CreateComponent HEVC",
                                   result));
        }
        return ReadCapabilities(capability);
    }

    Status Close(std::string* reason = nullptr, bool unload_runtime = true) {
        Status status = Status::kOk;
        std::string failure;
        if (encoder_) {
            const AMF_RESULT result = TerminateEncoder();
            if (result != AMF_OK) {
                status = Status::kError;
                failure = AmfFailure("encoder Terminate", result);
            }
            encoder_.Release();
        }
        if (context_) {
            const AMF_RESULT result = context_->Terminate();
            if (result != AMF_OK) {
                status = Status::kError;
                if (!failure.empty()) failure += "; ";
                failure += AmfFailure("context Terminate", result);
            }
            context_.Release();
        }
        device_context_.Reset();
        device_.Reset();
        factory_ = nullptr;
        encoder_terminated_ = false;
        encoder_termination_result_ = AMF_OK;
        if (unload_runtime) {
            ReleaseRuntime(status == Status::kOk);
        }
        if (reason) *reason = std::move(failure);
        return status;
    }

    void ReleaseRuntime(bool safe_to_unload) {
        if (!module_) return;
        if (safe_to_unload) {
            FreeLibrary(module_);
        } else {
            RTC_LOG(LS_ERROR) << "AMF_CLOSE runtime loader reference retained "
                                 "until process exit after failed SDK retirement";
        }
        module_ = nullptr;
    }

private:
    Status ReadCapabilities(AmfCapability* capability) {
        AMFCapsPtr caps;
        AMF_RESULT result = encoder_->GetCaps(&caps);
        if (result != AMF_OK || !caps) {
            return Fail(capability, Status::kUnsupported,
                        AmfFailure("GetCaps hardware encoder", result));
        }
        capability->acceleration_type = caps->GetAccelerationType();
        capability->hardware_accelerated =
            caps->GetAccelerationType() == AMF_ACCEL_HARDWARE;
        if (!capability->hardware_accelerated) {
            return Fail(capability, Status::kUnsupported,
                        "AMF component does not report hardware acceleration");
        }
        AMFIOCapsPtr input;
        result = caps->GetInputCaps(&input);
        if (result != AMF_OK || !input) {
            return Fail(capability, Status::kUnsupported,
                        AmfFailure("GetInputCaps hardware encoder", result));
        }
        input->GetWidthRange(&capability->min_width, &capability->max_width);
        input->GetHeightRange(&capability->min_height, &capability->max_height);
        capability->vertical_alignment = input->GetVertAlign();
        for (amf_int32 index = 0; index < input->GetNumOfFormats(); ++index) {
            AMF_SURFACE_FORMAT format = AMF_SURFACE_UNKNOWN;
            amf_bool native = false;
            if (input->GetFormatAt(index, &format, &native) == AMF_OK &&
                format == AMF_SURFACE_NV12) {
                capability->nv12_input = true;
            }
        }
        for (amf_int32 index = 0; index < input->GetNumOfMemoryTypes(); ++index) {
            AMF_MEMORY_TYPE memory = AMF_MEMORY_UNKNOWN;
            amf_bool native = false;
            if (input->GetMemoryTypeAt(index, &memory, &native) != AMF_OK) {
                continue;
            }
            capability->host_input |= memory == AMF_MEMORY_HOST;
            capability->dx11_input |= memory == AMF_MEMORY_DX11;
        }
        amf_int64 bitrate = 0;
        amf_int64 level = 0;
        const wchar_t* bitrate_property = kPropAvcCapMaxBitrate;
        const wchar_t* level_property = kPropAvcCapMaxLevel;
#if defined(RTC_ENABLE_H265)
        if (capability->codec == Codec::kH265) {
            bitrate_property = kPropHevcCapMaxBitrate;
            level_property = kPropHevcCapMaxLevel;
        }
#endif
        const AMF_RESULT bitrate_result =
            caps->GetProperty(bitrate_property, &bitrate);
        const AMF_RESULT level_result =
            caps->GetProperty(level_property, &level);
        capability->max_bitrate_query_result = static_cast<int>(bitrate_result);
        capability->max_level_query_result = static_cast<int>(level_result);
        if (bitrate_result == AMF_OK) capability->max_bitrate_bps = bitrate;
        if (level_result == AMF_OK) {
            if (capability->codec == Codec::kH264) {
                capability->max_h264_level = level;
            } else {
                capability->max_h265_level = level;
            }
        }
        if (bitrate_result != AMF_OK || level_result != AMF_OK) {
            return Fail(capability, Status::kUnsupported,
                        "AMF max bitrate/level capabilities unavailable");
        }
#if defined(RTC_ENABLE_H265)
        if (capability->codec == Codec::kH265) {
            amf_int64 profile = -1;
            amf_int64 tier = -1;
            const AMF_RESULT profile_result =
                caps->GetProperty(kPropHevcCapMaxProfile, &profile);
            const AMF_RESULT tier_result =
                caps->GetProperty(kPropHevcCapMaxTier, &tier);
            if (profile_result == AMF_OK) capability->max_profile = profile;
            if (tier_result == AMF_OK) capability->max_tier = tier;
            AMFVariant validated_profile;
            AMFVariant validated_tier;
            const AMF_RESULT profile_validation = encoder_->ValidateProperty(
                kPropHevcProfile, AMFVariant(kAmfHevcProfileMain),
                &validated_profile);
            const AMF_RESULT tier_validation = encoder_->ValidateProperty(
                kPropHevcTier, AMFVariant(kAmfHevcTierMain), &validated_tier);
            if (validated_profile.type == AMF_VARIANT_INT64) {
                capability->main_profile_validated_value =
                    validated_profile.int64Value;
            }
            if (validated_tier.type == AMF_VARIANT_INT64) {
                capability->main_tier_validated_value =
                    validated_tier.int64Value;
            }
            capability->main_profile_configurable =
                profile_validation == AMF_OK &&
                capability->main_profile_validated_value == kAmfHevcProfileMain;
            capability->main_tier_configurable =
                tier_validation == AMF_OK &&
                capability->main_tier_validated_value == kAmfHevcTierMain;
            if (!capability->main_profile_configurable ||
                !capability->main_tier_configurable ||
                (profile_result == AMF_OK && profile < kAmfHevcProfileMain) ||
                (tier_result == AMF_OK && tier < kAmfHevcTierMain)) {
                return Fail(capability, Status::kUnsupported,
                            "AMF HEVC Main/Main-tier configuration rejected");
            }
        }
#endif
        if (!capability->nv12_input || !capability->host_input ||
            !capability->dx11_input) {
            return Fail(capability, Status::kUnsupported,
                        "AMF requires NV12, HOST upload and DX11 input support");
        }
        capability->reason.clear();
        capability->status = Status::kOk;
        return Status::kOk;
    }

    static Status Fail(AmfCapability* capability,
                       Status status,
                       std::string reason) {
        capability->status = status;
        capability->reason = std::move(reason);
        return status;
    }

    HMODULE module_ = nullptr;
    AMFFactory* factory_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> device_context_;
    LUID adapter_luid_{};
    AMFContextPtr context_;
    AMFComponentPtr encoder_;
    bool encoder_terminated_ = false;
    AMF_RESULT encoder_termination_result_ = AMF_OK;
};

// Bounded 4-slot input pool observed by AMFSurfaceObserver.
// Each slot owns both a pre-allocated HOST NV12 buffer (for CPU camera/fallback
// frames) and an encoder-owned D3D11 NV12 texture (for Phase 2 GPU native frames
// submitted via CreateSurfaceFromDX11Native, matching OBS texture-amf.cpp).
struct InputSlot final : AMFSurfaceObserver {
    void LK_AMF_STD_CALL OnSurfaceDataRelease(AMFSurface*) override {
        available.store(true, std::memory_order_release);
    }
    std::atomic<bool> available{true};
    std::vector<std::uint8_t> pixels;
    ComPtr<ID3D11Texture2D> dx11_texture;
};

struct SharedTextureCacheEntry {
    HANDLE shared_handle = nullptr;
    ID3D11Texture2D* source_texture_identity = nullptr;
    ComPtr<ID3D11Texture2D> opened_texture;
};

class D3d11AmfBackend final {
public:
    explicit D3d11AmfBackend(Codec codec) : codec_(codec) {}
    ~D3d11AmfBackend() {
        Close();
        if (slots_ && !InputsRetired()) {
            (void)slots_.release();
            RTC_LOG(LS_ERROR) << "AMF_CLOSE four owned input slots retained until "
                                 "process exit because observers did not retire";
        }
    }

    Info GetInfo() const {
        return {codec_ == Codec::kH264 ? "AMF H264 / D3D11" : "AMF H265 / D3D11",
                true};
    }
    std::string_view LastError() const { return error_; }
    Status LastCloseStatus() const { return last_close_status_; }

    Status Open(const Config& config) {
        Close();
        std::string retirement_reason;
        if (HardwareEncoderProbes().Check(HardwareEncoderVendor::kAmf,
                                          &retirement_reason) ==
            Status::kRetirementFailed) {
            return Fail(Status::kRetirementFailed, retirement_reason);
        }
        if (retirement_failed_) {
            return Fail(Status::kError,
                        "AMF cannot reopen after failed resource retirement: " +
                            close_error_);
        }
        error_.clear();
        if (!ValidConfig(config, codec_)) {
            return Fail(Status::kInvalidArgument,
                        "invalid AMF codec/level/NV12/rate configuration");
        }
        if (codec_ == Codec::kH265) {
            const char* violation = AmfHevcLevelViolation(
                config.h265_level, config.width, config.height,
                config.framerate_fps, config.bitrate_bps);
            if (violation) {
                return Fail(Status::kUnsupported, violation);
            }
        }
        config_ = config;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return Fail(Status::kUnavailable, "DXGI factory unavailable");
        }
        Status last_status = Status::kUnavailable;
        bool found_amd = false;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) {
                return Abort(Status::kError, "DXGI adapter enumeration failed");
            }
            DXGI_ADAPTER_DESC desc = {};
            if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != kAmdVendor) {
                continue;
            }
            found_amd = true;
            capability_ = DescribeAdapter(index, desc, codec_);
            last_status = session_.Open(adapter.Get(), desc, &capability_);
            if (last_status == Status::kOk) {
                last_status = InitializeEncoder();
            } else {
                error_ = capability_.reason;
            }
            if (last_status == Status::kOk) {
                initialized_ = true;
                need_idr_ = true;
                error_.clear();
                return Status::kOk;
            }
            const std::string operation_error = error_;
            Close();
            if (retirement_failed_) {
                return Fail(Status::kError, operation_error + "; " + close_error_);
            }
            error_ = operation_error;
        }
        if (!found_amd) {
            return Fail(Status::kUnavailable, "no AMD DXGI graphics adapter");
        }
        return last_status;
    }

    Status SetRates(const Rates& rates) {
        if (!initialized_) {
            return Fail(Status::kError, "AMF encoder not initialized");
        }
        if (!ValidRates(rates)) {
            return Fail(Status::kInvalidArgument, "invalid AMF bitrate/framerate");
        }
        if (rates.bitrate_bps == config_.bitrate_bps &&
            rates.framerate_fps == config_.framerate_fps) {
            return Status::kOk;
        }
        Config config = config_;
        config.bitrate_bps = rates.bitrate_bps;
        config.framerate_fps = rates.framerate_fps;
        if (rates.framerate_fps == config_.framerate_fps) {
            return UpdateBitrate(config);
        }
        RTC_LOG(LS_INFO) << "AMF_RECONFIG codec="
                         << (codec_ == Codec::kH264 ? "H264" : "H265")
                         << " action=close_reinitialize bitrate="
                         << rates.bitrate_bps << " fps=" << rates.framerate_fps;
        return Open(config);
    }

    // Phase 2 GPU Native DX11 path: copies the GPU NV12 texture into a bounded
    // encoder-owned DX11 NV12 texture slot and submits via
    // CreateSurfaceFromDX11Native without CPU readback or host upload.
    Status EncodeDx11Native(const D3d11Nv12FrameBuffer& gpu_frame,
                            std::uint64_t frame_id,
                            bool force_key_frame,
                            Clock::time_point deadline,
                            Packet* packet) {
        if (packet) *packet = {};
        if (!initialized_) {
            return Fail(Status::kError, "AMF encoder not initialized");
        }
        if (!packet || gpu_frame.width() != config_.width ||
            gpu_frame.height() != config_.height || !gpu_frame.texture() ||
            frame_id > static_cast< std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            next_pts_ > std::numeric_limits<std::int64_t>::max() -
                            kAmfSecond / config_.framerate_fps) {
            return Fail(Status::kInvalidArgument, "invalid AMF DX11 input frame");
        }

        // Verify adapter LUID compatibility between capture/processor device and
        // AMF encoder device.
        const LUID src_luid = gpu_frame.adapter_luid();
        const LUID enc_luid = session_.adapter_luid();
        if (src_luid.LowPart != enc_luid.LowPart ||
            src_luid.HighPart != enc_luid.HighPart) {
            return Fail(Status::kUnsupported,
                        "AMF DX11 adapter LUID mismatch with capture texture");
        }

        AMF_RESULT result = session_.encoder()->QueryOutput(&output_);
        if (output_ || (result != AMF_OK && result != AMF_REPEAT)) {
            return Abort(Status::kError,
                         "AMF returned an unsubmitted/stale output");
        }

        InputSlot* slot = AcquireSlot(deadline);
        if (!slot) {
            return Abort(Status::kTimeout, "AMF input surfaces not retired");
        }
        if (!slot->dx11_texture) {
            return Fail(Status::kUnsupported,
                        "AMF DX11 input slot texture unavailable");
        }

        D3D11_BOX src_box{};
        src_box.left = 0;
        src_box.top = 0;
        src_box.front = 0;
        src_box.right = static_cast<UINT>(config_.width);
        src_box.bottom = static_cast<UINT>(config_.height);
        src_box.back = 1;

        if (gpu_frame.device() == session_.device()) {
            session_.device_context()->CopySubresourceRegion(
                slot->dx11_texture.Get(), 0, 0, 0, 0,
                gpu_frame.texture(), 0, &src_box);
        } else {
            const HANDLE shared_handle = gpu_frame.shared_handle();
            if (!shared_handle) {
                return Fail(Status::kUnsupported,
                            "cross-device D3D11 NV12 frame lacks shared handle");
            }
            ComPtr<ID3D11Texture2D> opened_tex = OpenOrGetCachedSharedTexture(
                shared_handle, gpu_frame.texture());
            if (!opened_tex) {
                return Fail(Status::kUnsupported,
                            "OpenSharedResource failed on AMF D3D11 device");
            }
            session_.device_context()->CopySubresourceRegion(
                slot->dx11_texture.Get(), 0, 0, 0, 0,
                opened_tex.Get(), 0, &src_box);
            session_.device_context()->Flush();
        }

        slot->available.store(false, std::memory_order_release);
        result = session_.context()->CreateSurfaceFromDX11Native(
            slot->dx11_texture.Get(), &input_, slot);
        if (result != AMF_OK || !input_) {
            if (!input_) {
                slot->available.store(true, std::memory_order_release);
            }
            return Abort(Status::kError,
                         AmfFailure("CreateSurfaceFromDX11Native", result));
        }

        g_dx11_native_frames_submitted.fetch_add(1, std::memory_order_relaxed);
        return SubmitAndCollectOutput(frame_id, force_key_frame, deadline, packet);
    }

    Status EncodeHost(const Frame& frame,
                      bool force_key_frame,
                      Clock::time_point deadline,
                      Packet* packet) {
        if (packet) *packet = {};
        if (!initialized_) {
            return Fail(Status::kError, "AMF encoder not initialized");
        }
        if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
            frame.height != config_.height || frame.stride_y < frame.width ||
            frame.stride_uv < frame.width ||
            frame.id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            next_pts_ > std::numeric_limits<std::int64_t>::max() -
                            kAmfSecond / config_.framerate_fps) {
            return Fail(Status::kInvalidArgument, "invalid AMF input frame");
        }
        AMF_RESULT result = session_.encoder()->QueryOutput(&output_);
        if (output_ || (result != AMF_OK && result != AMF_REPEAT)) {
            return Abort(Status::kError,
                         "AMF returned an unsubmitted/stale output");
        }

        InputSlot* slot = AcquireSlot(deadline);
        if (!slot) {
            return Abort(Status::kTimeout, "AMF input surfaces not retired");
        }
        const std::size_t luma_bytes =
            static_cast<std::size_t>(stride_) * frame.height;
        for (int row = 0; row < frame.height; ++row) {
            std::memcpy(
                slot->pixels.data() + static_cast<std::size_t>(row) * stride_,
                frame.y + static_cast<std::size_t>(row) * frame.stride_y,
                frame.width);
        }
        for (int row = 0; row < frame.height / 2; ++row) {
            std::memcpy(
                slot->pixels.data() + luma_bytes +
                    static_cast<std::size_t>(row) * stride_,
                frame.uv + static_cast<std::size_t>(row) * frame.stride_uv,
                frame.width);
        }
        slot->available.store(false, std::memory_order_release);
        result = session_.context()->CreateSurfaceFromHostNative(
            AMF_SURFACE_NV12, frame.width, frame.height, stride_, 0,
            slot->pixels.data(), &input_, slot);
        if (result != AMF_OK || !input_) {
            if (!input_) {
                slot->available.store(true, std::memory_order_release);
            }
            return Abort(Status::kError,
                         AmfFailure("CreateSurfaceFromHostNative", result));
        }

        g_host_frames_submitted.fetch_add(1, std::memory_order_relaxed);
        return SubmitAndCollectOutput(frame.id, force_key_frame, deadline, packet);
    }

    void Close() {
        initialized_ = false;
        session_.TerminateEncoder();
        buffer_.Release();
        output_.Release();
        input_.Release();
        for (auto& entry : shared_cache_) {
            entry = {};
        }
        std::string failure;
        const Status result = session_.Close(&failure, false);
        const bool inputs_retired = InputsRetired();
        if (!inputs_retired) {
            if (!failure.empty()) failure += "; ";
            failure += "AMF input surface observers did not retire after SDK release";
        }
        if (result != Status::kOk || !inputs_retired) {
            retirement_failed_ = true;
            if (close_error_.empty()) {
                close_error_ = std::move(failure);
            }
        }
        last_close_status_ = retirement_failed_ ? Status::kRetirementFailed : Status::kOk;
        session_.ReleaseRuntime(!retirement_failed_);
        if (inputs_retired && slots_) {
            for (auto& slot : *slots_) {
                slot.pixels.clear();
                slot.dx11_texture.Reset();
            }
        }
        if (retirement_failed_) {
            error_ = close_error_;
            HardwareEncoderProbes().RecordRetirementFailure(
                HardwareEncoderVendor::kAmf, close_error_);
            RTC_LOG(LS_ERROR) << "AMF_CLOSE failed: " << close_error_;
        }
        headers_.clear();
        pps_id_ = 0;
#if defined(RTC_ENABLE_H265)
        h265_pps_ = {};
#endif
        next_pts_ = 0;
        initial_vbv_bits_ = 0;
        need_idr_ = true;
    }

private:
    InputSlot* AcquireSlot(Clock::time_point deadline) {
        InputSlot* slot = nullptr;
        while (!slot) {
            for (auto& candidate : *slots_) {
                if (candidate.available.load(std::memory_order_acquire)) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot) break;
            if (Clock::now() >= deadline) return nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return slot;
    }

    ComPtr<ID3D11Texture2D> OpenOrGetCachedSharedTexture(
        HANDLE shared_handle,
        ID3D11Texture2D* source_identity) {
        for (const auto& entry : shared_cache_) {
            if (entry.shared_handle == shared_handle &&
                entry.source_texture_identity == source_identity &&
                entry.opened_texture) {
                return entry.opened_texture;
            }
        }
        ComPtr<ID3D11Texture2D> opened;
        if (FAILED(session_.device()->OpenSharedResource(
                shared_handle, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(opened.GetAddressOf()))) ||
            !opened) {
            return nullptr;
        }
        g_cross_device_shared_opens.fetch_add(1, std::memory_order_relaxed);
        shared_cache_[shared_cache_write_index_ % shared_cache_.size()] = {
            shared_handle, source_identity, opened};
        ++shared_cache_write_index_;
        return opened;
    }

    Status SubmitAndCollectOutput(std::uint64_t frame_id,
                                  bool force_key_frame,
                                  Clock::time_point deadline,
                                  Packet* packet) {
        const bool request_idr = force_key_frame || need_idr_;
        input_->SetPts(next_pts_);
        input_->SetDuration(kAmfSecond / config_.framerate_fps);
        if (input_->SetProperty(kFrameIdProperty,
                                static_cast<amf_int64>(frame_id)) != AMF_OK ||
            !SetInputPicture(request_idr)) {
            return Abort(Status::kError,
                         "AMF per-picture IDR/identity setup failed");
        }
        AMF_RESULT result = AMF_OK;
        do {
            result = session_.encoder()->SubmitInput(input_);
            if (result == AMF_OK || result == AMF_NEED_MORE_INPUT) {
                break;
            }
            if (result != AMF_INPUT_FULL) {
                return Abort(Status::kError, AmfFailure("SubmitInput", result));
            }
            if (Clock::now() >= deadline) {
                return Abort(Status::kTimeout, "AMF SubmitInput timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (true);

        while (!output_) {
            result = session_.encoder()->QueryOutput(&output_);
            if (result != AMF_OK && result != AMF_REPEAT) {
                return Abort(Status::kError, AmfFailure("QueryOutput", result));
            }
            if (output_) break;
            if (Clock::now() >= deadline) {
                return Abort(
                    Status::kTimeout,
                    "AMF did not complete this frame within polling window");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        amf_int64 output_id = -1;
        amf_int64 output_type = -1;
        const wchar_t* output_type_property = kPropAvcOutputDataType;
        amf_int64 type_idr = kAmfAvcOutputDataTypeIdr;
        amf_int64 type_i = kAmfAvcOutputDataTypeI;
        amf_int64 type_p = kAmfAvcOutputDataTypeP;
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            output_type_property = kPropHevcOutputDataType;
            type_idr = kAmfHevcOutputDataTypeIdr;
            type_i = kAmfHevcOutputDataTypeI;
            type_p = kAmfHevcOutputDataTypeP;
        }
#endif
        if (output_->GetPts() != next_pts_ ||
            output_->GetProperty(kFrameIdProperty, &output_id) != AMF_OK ||
            output_id != static_cast<amf_int64>(frame_id) ||
            output_->GetProperty(output_type_property, &output_type) != AMF_OK ||
            (output_type != type_idr && output_type != type_i &&
             output_type != type_p)) {
            return Abort(Status::kError,
                         "AMF output PTS/frame ID/type mismatch");
        }
        buffer_ = AMFBufferPtr(output_);
        if (!buffer_ || buffer_->GetMemoryType() != AMF_MEMORY_HOST ||
            buffer_->GetSize() == 0 ||
            buffer_->GetSize() > kMaximumPacketBytes ||
            !buffer_->GetNative()) {
            return Abort(Status::kError, "AMF expected one owned output buffer");
        }
        const ArrayView<const std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(buffer_->GetNative()),
            buffer_->GetSize());
        const bool idr = output_type == type_idr;
        std::string picture_error;
        bool picture_valid = codec_ == Codec::kH264 && AnnexB(bytes) &&
                             ValidatePicture(bytes, idr, pps_id_);
        bool has_headers = false;
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            picture_valid = H265AnnexB(bytes, &picture_error) &&
                            ValidateH265Picture(bytes, idr, h265_pps_);
            const bool has_vps = HasH265Nal(bytes, H265::kVps);
            const bool has_sps = HasH265Nal(bytes, H265::kSps);
            const bool has_pps = HasH265Nal(bytes, H265::kPps);
            has_headers = has_vps && has_sps && has_pps;
            if (has_vps || has_sps || has_pps) {
                H265PpsParser::PpsState emitted;
                std::string header_error;
                if (!ValidateH265Headers(bytes, config_, &emitted, &header_error) ||
                    emitted.pps_id != h265_pps_.pps_id ||
                    emitted.sps_id != h265_pps_.sps_id ||
                    emitted.num_extra_slice_header_bits !=
                        h265_pps_.num_extra_slice_header_bits) {
                    return Abort(Status::kError,
                                 "AMF emitted incompatible HEVC parameter sets: " +
                                     header_error);
                }
            }
        }
#endif
        if (!picture_valid || (request_idr && !idr)) {
            return Abort(Status::kError,
                         "AMF emitted invalid picture/requested IDR" +
                             (picture_error.empty() ? "" : ": " + picture_error));
        }
        if (codec_ == Codec::kH264) {
            const bool has_sps = HasNal(bytes, H264::kSps);
            const bool has_pps = HasNal(bytes, H264::kPps);
            has_headers = has_sps && has_pps;
            if (has_sps || has_pps) {
                std::uint32_t emitted_pps = 0;
                if (!ValidateHeaders(bytes, config_, &emitted_pps) ||
                    emitted_pps != pps_id_) {
                    return Abort(Status::kError,
                                 "AMF emitted incompatible parameter sets");
                }
            }
        }
        std::vector<std::uint8_t> owned;
        if (idr && !has_headers) {
            if (headers_.size() > kMaximumPacketBytes - bytes.size()) {
                return Abort(Status::kError, "AMF access unit exceeds byte limit");
            }
            owned = headers_;
        }
        owned.insert(owned.end(), bytes.begin(), bytes.end());
        buffer_.Release();
        output_.Release();
        input_.Release();
        result = session_.encoder()->QueryOutput(&output_);
        if (output_ || (result != AMF_OK && result != AMF_REPEAT)) {
            return Abort(Status::kError,
                         "AMF returned extra output for one input");
        }
        packet->bytes = std::move(owned);
        packet->frame_id = frame_id;
        packet->key_frame = idr;
        next_pts_ += kAmfSecond / config_.framerate_fps;
        need_idr_ = false;
        error_.clear();
        return Status::kOk;
    }

    bool InputsRetired() const {
        if (!slots_) return true;
        for (const auto& slot : *slots_) {
            if (!slot.available.load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    }

    bool SetInputPicture(bool idr) {
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            return input_->SetProperty(
                       kPropHevcForcePictureType,
                       idr ? kAmfHevcPictureTypeIdr : kAmfHevcPictureTypeP) ==
                       AMF_OK &&
                   input_->SetProperty(kPropHevcInsertHeader, idr) == AMF_OK;
        }
#endif
        return input_->SetProperty(
                   kPropAvcForcePictureType,
                   idr ? kAmfAvcPictureTypeIdr : kAmfAvcPictureTypeP) == AMF_OK &&
               input_->SetProperty(kPropAvcInsertSps, idr) == AMF_OK &&
               input_->SetProperty(kPropAvcInsertPps, idr) == AMF_OK;
    }

    template <typename T>
    bool Set(const wchar_t* property, const T& value) {
        const auto result = session_.encoder()->SetProperty(property, value);
        if (result == AMF_OK) return true;
        error_ = "AMF SetProperty " + Utf8(property) + ": " +
                 std::to_string(static_cast<int>(result));
        return false;
    }

    template <typename T>
    bool Check(const wchar_t* property, const T& expected) {
        T actual{};
        const auto result = session_.encoder()->GetProperty(property, &actual);
        if (result == AMF_OK && actual == expected) return true;
        error_ = "AMF property readback mismatch: " + Utf8(property);
        return false;
    }

    bool ReadIntegerExactly(const wchar_t* property,
                            amf_int64 expected,
                            amf_int64* value = nullptr) {
        AMFVariant actual;
        const AMF_RESULT result = session_.encoder()->GetProperty(
            property, static_cast<AMFVariantStruct*>(&actual));
        if (result == AMF_OK && actual.type == AMF_VARIANT_INT64 &&
            actual.int64Value == expected) {
            if (value) *value = actual.int64Value;
            return true;
        }
        error_ = "AMF exact integer readback mismatch: " + Utf8(property);
        return false;
    }

    Status ValidateDynamicBitrateProperty(const wchar_t* property,
                                          amf_int64 bitrate,
                                          std::string* reason) {
        const AMFPropertyInfo* info = nullptr;
        const AMF_RESULT info_result =
            session_.encoder()->GetPropertyInfo(property, &info);
        if (info_result != AMF_OK || !info) {
            *reason = "AMF dynamic property metadata " + Utf8(property);
            return info_result == AMF_NOT_FOUND ||
                           info_result == AMF_NOT_SUPPORTED ||
                           info_result == AMF_NOT_IMPLEMENTED
                       ? Status::kUnsupported
                       : Status::kError;
        }
        if (info->type != AMF_VARIANT_INT64 || !info->AllowedRead() ||
            !info->AllowedWrite() || !info->AllowedChangeInRuntime()) {
            *reason = "AMF dynamic property access " + Utf8(property);
            return Status::kUnsupported;
        }
        AMFVariant validated;
        const AMF_RESULT result = session_.encoder()->ValidateProperty(
            property, AMFVariant(bitrate), &validated);
        if (result == AMF_OK && validated.type == AMF_VARIANT_INT64 &&
            validated.int64Value == bitrate) {
            return Status::kOk;
        }
        *reason = "AMF dynamic property validation " + Utf8(property);
        return result == AMF_NOT_FOUND || result == AMF_NOT_SUPPORTED ||
                       result == AMF_NOT_IMPLEMENTED
                   ? Status::kUnsupported
                   : Status::kError;
    }

    Status UpdateBitrate(const Config& config) {
        if (config.bitrate_bps > capability_.max_bitrate_bps) {
            return Fail(Status::kUnsupported,
                        "AMF bitrate exceeds reported hardware capability");
        }
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            H265PpsParser::PpsState pps;
            std::string reason;
            if (!ValidateH265Headers(headers_, config, &pps, &reason)) {
                return Fail(Status::kUnsupported,
                            "AMF HEVC bitrate violates current parameter sets: " +
                                reason);
            }
        }
#endif
        if (initial_vbv_bits_ <= 0) {
            return Abort(Status::kError, "AMF initial VBV capacity unavailable");
        }
        const wchar_t* target_property = kPropAvcTargetBitrate;
        const wchar_t* peak_property = kPropAvcPeakBitrate;
        const wchar_t* vbv_property = kPropAvcVbvBufferSize;
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            target_property = kPropHevcTargetBitrate;
            peak_property = kPropHevcPeakBitrate;
            vbv_property = kPropHevcVbvBufferSize;
        }
#endif
        const amf_int64 bitrate = config.bitrate_bps;
        std::string target_reason;
        std::string peak_reason;
        const Status target_status = ValidateDynamicBitrateProperty(
            target_property, bitrate, &target_reason);
        const Status peak_status = ValidateDynamicBitrateProperty(
            peak_property, bitrate, &peak_reason);
        if (target_status != Status::kOk || peak_status != Status::kOk) {
            const std::string reason = target_reason + "; " + peak_reason;
            if (target_status == Status::kError ||
                peak_status == Status::kError) {
                return Abort(Status::kError, reason);
            }
            return Open(config);
        }
        const std::uint32_t old_bitrate = config_.bitrate_bps;
        const wchar_t* first =
            bitrate > old_bitrate ? peak_property : target_property;
        const wchar_t* second =
            bitrate > old_bitrate ? target_property : peak_property;
        if (!Set(first, bitrate) || !Set(second, bitrate)) {
            return Abort(Status::kError, error_);
        }
        if (!ReadIntegerExactly(target_property, bitrate) ||
            !ReadIntegerExactly(peak_property, bitrate) ||
            !ReadIntegerExactly(vbv_property, initial_vbv_bits_)) {
            return Abort(Status::kError, error_);
        }
        config_.bitrate_bps = config.bitrate_bps;
        error_.clear();
        return Status::kOk;
    }

    Status InitializeEncoder() {
#if defined(RTC_ENABLE_H265)
        if (codec_ == Codec::kH265) {
            return InitializeH265Encoder();
        }
#endif
        if (config_.width < capability_.min_width ||
            config_.width > capability_.max_width ||
            config_.height < capability_.min_height ||
            config_.height > capability_.max_height ||
            config_.h264_level > capability_.max_h264_level ||
            config_.bitrate_bps > capability_.max_bitrate_bps) {
            return Fail(Status::kUnsupported,
                        "AMF request exceeds reported hardware capabilities");
        }
        if (!Set(kPropAvcUsage, kAmfAvcUsageUltraLowLatency) ||
            !Set(kPropAvcQualityPreset, kAmfAvcQualityPresetSpeed) ||
            !Set(kPropAvcFrameSize, AMFConstructSize(config_.width, config_.height)) ||
            !Set(kPropAvcProfile, kAmfAvcProfileBaseline) ||
            !Set(kPropAvcProfileLevel, static_cast<amf_int64>(config_.h264_level)) ||
            !Set(kPropAvcScanType, kAmfAvcScanTypeProgressive) ||
            !Set(kPropAvcLowLatencyMode, true) ||
            !Set(kPropAvcPreAnalysisEnable, false) ||
            !Set(kPropAvcPreencodeEnable, kAmfAvcPreencodeDisabled) ||
            !Set(kPropAvcRateControlMethod, kAmfAvcRateControlCbr) ||
            !Set(kPropAvcFrameRate, AMFConstructRate(config_.framerate_fps, 1)) ||
            !Set(kPropAvcTargetBitrate, static_cast<amf_int64>(config_.bitrate_bps)) ||
            !Set(kPropAvcPeakBitrate, static_cast<amf_int64>(config_.bitrate_bps)) ||
            !Set(kPropAvcVbvBufferSize, static_cast<amf_int64>(config_.bitrate_bps)) ||
            !Set(kPropAvcSkipFrameEnable, false) ||
            !Set(kPropAvcBPicPattern, static_cast<amf_int64>(0)) ||
            !Set(kPropAvcMaxConsecutiveBPictures, static_cast<amf_int64>(0)) ||
            !Set(kPropAvcMaxNumRefFrames, static_cast<amf_int64>(1)) ||
            !Set(kPropAvcMaxNumTemporalLayers, static_cast<amf_int64>(1)) ||
            !Set(kPropAvcSlicesPerFrame, static_cast<amf_int64>(1)) ||
            !Set(kPropAvcOutputMode, kAmfAvcOutputModeFrame) ||
            !Set(kPropAvcCabacEnable, kAmfAvcCodingCavlc) ||
            !Set(kPropAvcIdrPeriod, static_cast<amf_int64>(0))) {
            return Status::kUnsupported;
        }
        const AMF_RESULT initialized = session_.encoder()->Init(
            AMF_SURFACE_NV12, config_.width, config_.height);
        if (initialized != AMF_OK) {
            return Fail(Status::kUnsupported, AmfFailure("Init AVC", initialized));
        }
        AMFSize size{};
        AMFRate rate{};
        if (session_.encoder()->GetProperty(kPropAvcFrameSize, &size) != AMF_OK ||
            size.width != config_.width || size.height != config_.height ||
            session_.encoder()->GetProperty(kPropAvcFrameRate, &rate) != AMF_OK ||
            rate.num != config_.framerate_fps || rate.den != 1) {
            return Fail(Status::kUnsupported,
                        "AMF changed requested frame dimensions/framerate");
        }
        if (!Check(kPropAvcUsage, kAmfAvcUsageUltraLowLatency) ||
            !Check(kPropAvcQualityPreset, kAmfAvcQualityPresetSpeed) ||
            !Check(kPropAvcProfile, kAmfAvcProfileBaseline) ||
            !Check(kPropAvcProfileLevel, static_cast<amf_int64>(config_.h264_level)) ||
            !Check(kPropAvcScanType, kAmfAvcScanTypeProgressive) ||
            !Check(kPropAvcLowLatencyMode, true) ||
            !Check(kPropAvcPreAnalysisEnable, false) ||
            !Check(kPropAvcPreencodeEnable, kAmfAvcPreencodeDisabled) ||
            !Check(kPropAvcRateControlMethod, kAmfAvcRateControlCbr) ||
            !Check(kPropAvcTargetBitrate, static_cast<amf_int64>(config_.bitrate_bps)) ||
            !Check(kPropAvcPeakBitrate, static_cast<amf_int64>(config_.bitrate_bps)) ||
            !ReadIntegerExactly(kPropAvcVbvBufferSize,
                                static_cast<amf_int64>(config_.bitrate_bps),
                                &initial_vbv_bits_) ||
            !Check(kPropAvcSkipFrameEnable, false) ||
            !Check(kPropAvcBPicPattern, static_cast<amf_int64>(0)) ||
            !Check(kPropAvcMaxConsecutiveBPictures, static_cast<amf_int64>(0)) ||
            !Check(kPropAvcMaxNumRefFrames, static_cast<amf_int64>(1)) ||
            !Check(kPropAvcMaxNumTemporalLayers, static_cast<amf_int64>(1)) ||
            !Check(kPropAvcSlicesPerFrame, static_cast<amf_int64>(1)) ||
            !Check(kPropAvcOutputMode, kAmfAvcOutputModeFrame) ||
            !Check(kPropAvcCabacEnable, kAmfAvcCodingCavlc) ||
            !Check(kPropAvcIdrPeriod, static_cast<amf_int64>(0))) {
            return Status::kUnsupported;
        }
        AMFVariant extra;
        if (session_.encoder()->GetProperty(kPropAvcExtraData, &extra) != AMF_OK ||
            extra.type != AMF_VARIANT_INTERFACE || !extra.pInterface) {
            return Fail(Status::kUnsupported,
                        "AMF AVC Annex B extradata unavailable");
        }
        AMFBufferPtr headers(extra.pInterface);
        if (!headers || headers->GetMemoryType() != AMF_MEMORY_HOST ||
            !headers->GetNative() || headers->GetSize() == 0 ||
            headers->GetSize() > kMaximumPacketBytes) {
            return Fail(Status::kUnsupported,
                        "AMF AVC extradata is not a buffer");
        }
        const ArrayView<const std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(headers->GetNative()),
            headers->GetSize());
        if (!AnnexB(bytes) || !ValidateHeaders(bytes, config_, &pps_id_)) {
            return Fail(
                Status::kUnsupported,
                "AMF AVC SPS/PPS violates Baseline/level/display contract");
        }
        headers_.assign(bytes.begin(), bytes.end());
        AllocateInputAndLog();
        return Status::kOk;
    }

#if defined(RTC_ENABLE_H265)
    Status InitializeH265Encoder() {
        if (config_.width < capability_.min_width ||
            config_.width > capability_.max_width ||
            config_.height < capability_.min_height ||
            config_.height > capability_.max_height ||
            config_.h265_level > capability_.max_h265_level ||
            config_.bitrate_bps > capability_.max_bitrate_bps) {
            return Fail(Status::kUnsupported,
                        "AMF HEVC request exceeds reported hardware capabilities");
        }
        struct IntegerSetting {
            const wchar_t* name;
            amf_int64 value;
        };
        const IntegerSetting integers[] = {
            {kPropHevcUsage, kAmfHevcUsageUltraLowLatency},
            {kPropHevcQualityPreset, kAmfHevcQualityPresetSpeed},
            {kPropHevcProfile, kAmfHevcProfileMain},
            {kPropHevcTier, kAmfHevcTierMain},
            {kPropHevcProfileLevel, config_.h265_level},
            {kPropHevcColorBitDepth, kAmfColorBitDepth8},
            {kPropHevcRateControlMethod, kAmfHevcRateControlCbr},
            {kPropHevcTargetBitrate, config_.bitrate_bps},
            {kPropHevcPeakBitrate, config_.bitrate_bps},
            {kPropHevcVbvBufferSize, config_.bitrate_bps},
            {kPropHevcMaxNumRefFrames, 1},
            {kPropHevcMaxLtrFrames, 0},
            {kPropHevcMaxNumTemporalLayers, 1},
            {kPropHevcSlicesPerFrame, 1},
            {kPropHevcOutputMode, kAmfHevcOutputModeFrame},
            {kPropHevcNumGopsPerIdr, 1},
            {kPropHevcGopSize, 240},
            {kPropHevcHeaderInsertionMode, kAmfHevcHeaderInsertionIdrAligned}};
        struct BooleanSetting {
            const wchar_t* name;
            bool value;
        };
        const BooleanSetting booleans[] = {
            {kPropHevcLowLatencyMode, true},
            {kPropHevcPreAnalysisEnable, false},
            {kPropHevcPreencodeEnable, false},
            {kPropHevcSkipFrameEnable, false}};
        for (const auto& setting : integers) {
            if (!Set(setting.name, setting.value)) return Status::kUnsupported;
        }
        for (const auto& setting : booleans) {
            if (!Set(setting.name, setting.value)) return Status::kUnsupported;
        }
        if (!Set(kPropHevcFrameSize,
                 AMFConstructSize(config_.width, config_.height)) ||
            !Set(kPropHevcFrameRate,
                 AMFConstructRate(config_.framerate_fps, 1))) {
            return Status::kUnsupported;
        }
        const AMF_RESULT initialized = session_.encoder()->Init(
            AMF_SURFACE_NV12, config_.width, config_.height);
        if (initialized != AMF_OK) {
            return Fail(Status::kUnsupported, AmfFailure("Init HEVC", initialized));
        }
        for (const auto& setting : integers) {
            if (!Check(setting.name, setting.value)) return Status::kUnsupported;
        }
        for (const auto& setting : booleans) {
            if (!Check(setting.name, setting.value)) return Status::kUnsupported;
        }
        if (!ReadIntegerExactly(kPropHevcVbvBufferSize,
                                static_cast<amf_int64>(config_.bitrate_bps),
                                &initial_vbv_bits_)) {
            return Status::kUnsupported;
        }
        AMFSize size{};
        AMFRate rate{};
        if (session_.encoder()->GetProperty(kPropHevcFrameSize, &size) != AMF_OK ||
            size.width != config_.width || size.height != config_.height ||
            session_.encoder()->GetProperty(kPropHevcFrameRate, &rate) != AMF_OK ||
            rate.num != config_.framerate_fps || rate.den != 1) {
            return Fail(Status::kUnsupported,
                        "AMF HEVC changed requested frame dimensions/framerate");
        }
        AMFVariant extra;
        if (session_.encoder()->GetProperty(kPropHevcExtraData, &extra) != AMF_OK ||
            extra.type != AMF_VARIANT_INTERFACE || !extra.pInterface) {
            return Fail(Status::kUnsupported,
                        "AMF HEVC Annex B extradata unavailable");
        }
        AMFBufferPtr headers(extra.pInterface);
        if (!headers || headers->GetMemoryType() != AMF_MEMORY_HOST ||
            !headers->GetNative() || headers->GetSize() == 0 ||
            headers->GetSize() > kMaximumPacketBytes) {
            return Fail(Status::kUnsupported,
                        "AMF HEVC extradata is not a host buffer");
        }
        const ArrayView<const std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(headers->GetNative()),
            headers->GetSize());
        std::string header_error;
        if (!H265AnnexB(bytes, &header_error) ||
            !ValidateH265Headers(bytes, config_, &h265_pps_, &header_error)) {
            return Fail(Status::kUnsupported,
                        "AMF HEVC VPS/SPS/PPS violates contract: " +
                            header_error);
        }
        headers_.assign(bytes.begin(), bytes.end());
        AllocateInputAndLog();
        return Status::kOk;
    }
#endif

    void AllocateInputAndLog() {
        stride_ = (config_.width + 15) & ~15;
        const std::size_t luma_bytes =
            static_cast<std::size_t>(stride_) * config_.height;

        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = static_cast<UINT>(config_.width);
        tex_desc.Height = static_cast<UINT>(config_.height);
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_NV12;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        tex_desc.CPUAccessFlags = 0;
        tex_desc.MiscFlags = 0;

        for (auto& slot : *slots_) {
            slot.pixels.assign(luma_bytes * 3 / 2, 128);
            std::memset(slot.pixels.data(), 16, luma_bytes);
            slot.dx11_texture.Reset();
            if (session_.device()) {
                session_.device()->CreateTexture2D(
                    &tex_desc, nullptr, slot.dx11_texture.GetAddressOf());
            }
            slot.available.store(true, std::memory_order_relaxed);
        }
        for (auto& entry : shared_cache_) {
            entry = {};
        }
    }

    Status Fail(Status status, std::string reason) {
        error_ = std::move(reason);
        return status;
    }
    Status Abort(Status status, std::string reason) {
        Close();
        if (last_close_status_ != Status::kOk && !close_error_.empty()) {
            reason += "; cleanup: " + close_error_;
        }
        return Fail(last_close_status_ == Status::kOk ? status : Status::kRetirementFailed,
                    std::move(reason));
    }

    const Codec codec_;
    Config config_;
    AmfCapability capability_;
    std::unique_ptr<std::array<InputSlot, kInputSlots>> slots_ =
        std::make_unique<std::array<InputSlot, kInputSlots>>();
    std::array<SharedTextureCacheEntry, 8> shared_cache_{};
    std::size_t shared_cache_write_index_ = 0;
    AmfSession session_;
    AMFSurfacePtr input_;
    AMFDataPtr output_;
    AMFBufferPtr buffer_;
    std::vector<std::uint8_t> headers_;
    std::string error_;
    std::string close_error_;
    Status last_close_status_ = Status::kOk;
    std::uint32_t pps_id_ = 0;
#if defined(RTC_ENABLE_H265)
    H265PpsParser::PpsState h265_pps_;
#endif
    amf_pts next_pts_ = 0;
    amf_int64 initial_vbv_bits_ = 0;
    int stride_ = 0;
    bool initialized_ = false;
    bool need_idr_ = true;
    bool retirement_failed_ = false;
};

// All vendor operations, including resource retirement and backend destruction,
// stay on one private sequence. A timed-out request owns its frame and result;
// it can never complete into an encoder stack or invoke an encoded callback.
struct AmfWorkerResult {
    Status status = Status::kError;
    std::string error;
    Packet packet;
};

class D3d11AmfBackendWorker final {
public:
    explicit D3d11AmfBackendWorker(Codec codec) : codec_(codec) {}
    ~D3d11AmfBackendWorker() { Close(); }

    Info GetInfo() const {
        return {codec_ == Codec::kH264 ? "AMF H264 / D3D11" : "AMF H265 / D3D11",
                true};
    }
    std::string_view LastError() const { return error_; }
    Status LastCloseStatus() const { return last_close_status_; }

    Status Open(const Config& config) {
        Close();
        std::string reason;
        if (retirement_failed_ ||
            HardwareEncoderProbes().Check(HardwareEncoderVendor::kAmf, &reason) ==
                Status::kRetirementFailed) {
            error_ = reason.empty() ? "AMF resource retirement is unconfirmed" : reason;
            return Status::kRetirementFailed;
        }
        try {
            worker_ = std::make_unique<Worker>(
                std::make_unique<D3d11AmfBackend>(codec_),
                [](D3d11AmfBackend& backend) {
                    backend.Close();
                    return Snapshot(backend, backend.LastCloseStatus());
                });
        } catch (const std::exception& exception) {
            error_ = std::string("AMF worker creation failed: ") + exception.what();
            return Status::kUnavailable;
        }
        first_frame_ = true;
        const Status status = Consume(worker_->Invoke(
            [config](D3d11AmfBackend& backend) {
                return Snapshot(backend, backend.Open(config));
            }, kControlBudget), "Open");
        if (status == Status::kOk) framerate_ = config.framerate_fps;
        return status;
    }

    Status SetRates(const Rates& rates) {
        if (!worker_ || retirement_failed_) return Status::kRetirementFailed;
        const Status status = Consume(worker_->Invoke(
            [rates](D3d11AmfBackend& backend) {
                return Snapshot(backend, backend.SetRates(rates));
            }, kControlBudget), "SetRates");
        if (status == Status::kOk) {
            // A framerate change recreates the AMF session, so its next frame
            // needs the same initialization allowance as the original first frame.
            first_frame_ |= framerate_ != rates.framerate_fps;
            framerate_ = rates.framerate_fps;
        }
        return status;
    }

    Status Encode(webrtc::scoped_refptr<VideoFrameBuffer> source,
                  int width, int height, std::uint64_t frame_id,
                  bool force_key_frame, Packet* packet) {
        if (!worker_ || retirement_failed_) return Status::kRetirementFailed;
        const auto budget = first_frame_ ? kFirstFrameBudget : kFrameBudget;
        // Native and host fallback share this deadline, including GPU mapping.
        const auto deadline = Clock::now() + budget;
        auto reply = worker_->Invoke(
            [source = std::move(source), width, height, frame_id,
             force_key_frame, deadline](D3d11AmfBackend& backend) mutable {
                if (Clock::now() >= deadline) {
                    return AmfWorkerResult{Status::kTimeout, "AMF frame expired before dispatch", {}};
                }
                if (source->width() != width || source->height() != height) {
                    if (source->type() != VideoFrameBuffer::Type::kNative) {
                        return AmfWorkerResult{Status::kInvalidArgument, "AMF frame dimensions mismatch", {}};
                    }
                    auto scaled = source->Scale(width, height);
                    if (!scaled || scaled->width() != width || scaled->height() != height) {
                        return AmfWorkerResult{Status::kError, "AMF GPU frame scaling failed", {}};
                    }
                    g_simulcast_gpu_scales.fetch_add(1, std::memory_order_relaxed);
                    source = std::move(scaled);
                }

                if (Clock::now() >= deadline) {
                    return AmfWorkerResult{Status::kTimeout, "AMF frame expired during GPU scaling", {}};
                }
                Packet owned_packet;
                Status status = Status::kUnsupported;
                if (const auto* native =
                        D3d11Nv12FrameBuffer::FromVideoFrameBuffer(source.get())) {
                    status = backend.EncodeDx11Native(
                        *native, frame_id, force_key_frame, deadline, &owned_packet);
                }
                if (status == Status::kUnsupported) {
                    // Unsupported native input must not receive a fresh wait budget.
                    if (Clock::now() >= deadline) {
                        return AmfWorkerResult{Status::kTimeout, "AMF frame expired before host fallback", {}};
                    }
                    webrtc::scoped_refptr<const NV12BufferInterface> nv12;
                    if (source->type() == VideoFrameBuffer::Type::kNV12) {
                        nv12 = source->GetNV12();
                    } else if (const auto* native =
                                   D3d11Nv12FrameBuffer::FromVideoFrameBuffer(source.get())) {
                        nv12 = native->ToNV12();
                    } else {
                        auto i420 = source->ToI420();
                        if (i420) nv12 = NV12Buffer::Copy(*i420);
                    }
                    if (!nv12) {
                        return AmfWorkerResult{Status::kError, "AMF CPU frame mapping failed", {}};
                    }
                    if (Clock::now() >= deadline) {
                        return AmfWorkerResult{Status::kTimeout, "AMF frame expired during CPU mapping", {}};
                    }
                    Frame frame;
                    frame.y = nv12->DataY();
                    frame.uv = nv12->DataUV();
                    frame.stride_y = nv12->StrideY();
                    frame.stride_uv = nv12->StrideUV();
                    frame.width = width;
                    frame.height = height;
                    frame.id = frame_id;
                    status = backend.EncodeHost(frame, force_key_frame, deadline, &owned_packet);
                }
                auto result = Snapshot(backend, status);
                result.packet = std::move(owned_packet);
                return result;
            }, budget + kWorkerReplySlack);
        const Status status = Consume(std::move(reply), "Encode", packet);
        if (status == Status::kOk) first_frame_ = false;
        return status;
    }

    void Close() {
        if (!worker_) return;
        // Close is ordered after the active request. Never release its resources
        // from the calling encoder sequence or join a blocked vendor thread.
        if (!retirement_failed_) {
            auto reply = worker_->Close(kCloseBudget);
            last_close_status_ = Consume(std::move(reply), "Close");
        }
        worker_.reset();
    }

private:
    using Worker = livekit::detail::BoundedEncoderWorker<D3d11AmfBackend, AmfWorkerResult>;

    static AmfWorkerResult Snapshot(D3d11AmfBackend& backend, Status status) {
        if (backend.LastCloseStatus() != Status::kOk) {
            status = Status::kRetirementFailed;
        }
        return {status, std::string(backend.LastError()), {}};
    }

    Status Consume(Worker::Reply reply, const char* operation,
                   Packet* packet = nullptr) {
        if (reply.wait == Worker::Wait::kCompleted && reply.value) {
            auto result = std::move(*reply.value);
            error_ = std::move(result.error);
            if (packet) *packet = std::move(result.packet);
            if (result.status == Status::kRetirementFailed) {
                retirement_failed_ = true;
                last_close_status_ = Status::kRetirementFailed;
            }
            return result.status;
        }
        // An expired or exceptional operation leaves retirement unconfirmed.
        // Seal this instance and the process-wide vendor gate before fallback.
        retirement_failed_ = true;
        last_close_status_ = Status::kRetirementFailed;
        error_ = std::string("AMF ") + operation +
                 " exceeded its worker wait or failed; retirement remains unconfirmed";
        if (!reply.error.empty()) error_ += ": " + reply.error;
        HardwareEncoderProbes().RecordRetirementFailure(
            HardwareEncoderVendor::kAmf, error_);
        RTC_LOG(LS_ERROR) << error_;
        return Status::kRetirementFailed;
    }

    const Codec codec_;
    std::unique_ptr<Worker> worker_;
    std::uint32_t framerate_ = 0;
    std::string error_;
    Status last_close_status_ = Status::kOk;
    bool retirement_failed_ = false;
    bool first_frame_ = true;
};

std::uint8_t NegotiatedLevel(const webrtc::SdpVideoFormat& format) {
    if (webrtc::PayloadStringToCodecType(format.name) ==
        webrtc::kVideoCodecH264) {
        return static_cast<std::uint8_t>(
            webrtc::ParseSdpForH264ProfileLevelId(format.parameters)->level);
    }
#if defined(RTC_ENABLE_H265)
    return static_cast<std::uint8_t>(
        webrtc::ParseSdpForH265ProfileTierLevel(format.parameters)->level);
#else
    return 0;
#endif
}

class D3d11AmfVideoEncoder final : public VideoEncoder {
public:
    D3d11AmfVideoEncoder(const webrtc::SdpVideoFormat& format,
                         std::unique_ptr<D3d11AmfBackendWorker> backend,
                         webrtc::BackendFailurePolicy failure_policy)
        : backend_(std::move(backend)),
          info_(backend_->GetInfo()),
          codec_type_(webrtc::PayloadStringToCodecType(format.name)),
          level_(NegotiatedLevel(format)),
          failure_policy_(failure_policy) {
        sequence_checker_.Detach();
    }

    ~D3d11AmfVideoEncoder() override { backend_->Close(); }

    int InitEncode(const VideoCodec* codec, const Settings& settings) override {
        RTC_DCHECK_RUN_ON(&sequence_checker_);
        backend_->Close();
        initialized_ = false;
        failed_ = false;
        level_constraints_failed_ = false;
        if (retirement_failed_ || backend_->LastCloseStatus() != Status::kOk) {
            retirement_failed_ = true;
            failed_ = true;
            ReportFailure(diagnostic::EncoderFailureStage::Retirement,
                Status::kRetirementFailed, WEBRTC_VIDEO_CODEC_ERROR, codec);
            RTC_LOG(LS_WARNING) << info_.implementation_name
                                << ": InitEncode cleanup: "
                                << backend_->LastError();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!codec || codec->codecType != codec_type_ || codec->width == 0 ||
            codec->height == 0 || codec->width % 2 || codec->height % 2 ||
            codec->maxFramerate == 0 || settings.number_of_cores <= 0) {
            ReportFailure(diagnostic::EncoderFailureStage::InitParameters,
                Status::kError, WEBRTC_VIDEO_CODEC_ERR_PARAMETER, codec);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec->numberOfSimulcastStreams > 1 ||
            (codec_type_ == webrtc::kVideoCodecH264 &&
             codec->H264().numberOfTemporalLayers > 1) ||
            codec->simulcastStream[0].numberOfTemporalLayers > 1 ||
            (codec->GetScalabilityMode() &&
             codec->GetScalabilityMode() != ScalabilityMode::kL1T1)) {
            ReportFailure(diagnostic::EncoderFailureStage::InitTopology,
                Status::kError, FailureStatus(), codec);
            return FailureStatus();
        }
        if (codec->startBitrate > std::numeric_limits<std::uint32_t>::max() / 1000) {
            ReportFailure(diagnostic::EncoderFailureStage::InitParameters,
                Status::kError, WEBRTC_VIDEO_CODEC_ERR_PARAMETER, codec);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_type_ == webrtc::kVideoCodecH264 &&
            detail::CheckH264LevelConstraints(static_cast<webrtc::H264Level>(level_),
                codec->width, codec->height, codec->maxFramerate) !=
                detail::H264LevelConstraintStatus::Supported) {
            ReportFailure(diagnostic::EncoderFailureStage::InitLevelConstraints,
                Status::kUnsupported, WEBRTC_VIDEO_CODEC_ERR_PARAMETER, codec);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        codec_ = *codec;
        paused_ = codec->startBitrate == 0;
        need_key_frame_ = true;
        Config config;
        config.codec = codec_type_ == webrtc::kVideoCodecH264
                           ? Codec::kH264
                           : Codec::kH265;
        config.width = codec->width;
        config.height = codec->height;
        const std::uint32_t initial_bitrate =
            codec_type_ == webrtc::kVideoCodecH265 ? 64000 : 300000;
        config.bitrate_bps =
            paused_ ? initial_bitrate : codec->startBitrate * 1000;
        config.framerate_fps = codec->maxFramerate;
        if (codec_type_ == webrtc::kVideoCodecH264) {
            config.h264_level = level_;
        } else {
            config.h265_level = level_;
#if defined(RTC_ENABLE_H265)
            h265_bitstream_parser_ = std::make_unique<H265BitstreamParser>();
#endif
        }
        const Status open_status = backend_->Open(config);
        if (open_status != Status::kOk) {
            return FailBackend(open_status, diagnostic::EncoderFailureStage::Initialization);
        }
        initialized_ = true;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(
        EncodedImageCallback* callback) override {
        RTC_DCHECK_RUN_ON(&sequence_checker_);
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        RTC_DCHECK_RUN_ON(&sequence_checker_);
        initialized_ = false;
        failed_ = false;
        level_constraints_failed_ = false;
        backend_->Close();
        retirement_failed_ |= backend_->LastCloseStatus() != Status::kOk;
        const bool close_ok = !retirement_failed_;
        if (!close_ok) {
            ReportFailure(diagnostic::EncoderFailureStage::Retirement,
                Status::kRetirementFailed, WEBRTC_VIDEO_CODEC_ERROR);
            RTC_LOG(LS_WARNING) << info_.implementation_name
                                << ": Release: " << backend_->LastError();
        }
        sequence_checker_.Detach();
        return close_ok ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    void SetRates(const RateControlParameters& parameters) override {
        RTC_DCHECK_RUN_ON(&sequence_checker_);
        if (!initialized_ || failed_) return;
        const std::uint32_t bitrate = parameters.bitrate.get_sum_bps();
        if (bitrate == 0) {
            paused_ = true;
            need_key_frame_ = true;
            return;
        }
        if (!std::isfinite(parameters.framerate_fps) ||
            parameters.framerate_fps >
                std::numeric_limits<std::uint32_t>::max()) {
            failed_ = true;
            return;
        }
        const std::uint32_t framerate =
            parameters.framerate_fps <= 0
                ? codec_.maxFramerate
                : static_cast<std::uint32_t>(
                      std::max(1.0, parameters.framerate_fps));
        if (codec_type_ == webrtc::kVideoCodecH264 &&
            detail::CheckH264LevelConstraints(static_cast<webrtc::H264Level>(level_),
                codec_.width, codec_.height, framerate) !=
                detail::H264LevelConstraintStatus::Supported) {
            failed_ = true;
            level_constraints_failed_ = true;
            VideoCodec attempted = codec_;
            attempted.maxFramerate = framerate;
            attempted.startBitrate = bitrate / 1000;
            ReportFailure(diagnostic::EncoderFailureStage::RateLevelConstraints,
                Status::kUnsupported, WEBRTC_VIDEO_CODEC_ERR_PARAMETER, &attempted);
            return;
        }
        const Rates rates{bitrate, framerate};
        const Status rate_status = backend_->SetRates(rates);
        if (rate_status != Status::kOk) {
            FailBackend(rate_status, diagnostic::EncoderFailureStage::RateUpdate);
            return;
        }
        need_key_frame_ |= paused_;
        paused_ = false;
    }

    int32_t Encode(const webrtc::VideoFrame& input,
                   const std::vector<VideoFrameType>* frame_types) override {
        RTC_DCHECK_RUN_ON(&sequence_checker_);
        if (level_constraints_failed_) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        if (failed_) return FailureStatus();
        if (!initialized_ || !callback_) {
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        }
        if (paused_ || (frame_types && !frame_types->empty() &&
                        frame_types->front() == VideoFrameType::kEmptyFrame)) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        auto source = input.video_frame_buffer();
        if (!source) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        const bool request_key_frame =
            need_key_frame_ ||
            (frame_types && !frame_types->empty() &&
             frame_types->front() == VideoFrameType::kVideoFrameKey);

        Packet packet;
        const Status status = backend_->Encode(
            std::move(source), codec_.width, codec_.height,
            input.rtp_timestamp(), request_key_frame, &packet);

        if (status == Status::kNoOutput) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        if (status != Status::kOk || packet.bytes.empty() ||
            packet.frame_id != input.rtp_timestamp()) {
            return FailBackend(status, status != Status::kOk
                ? diagnostic::EncoderFailureStage::Encode
                : diagnostic::EncoderFailureStage::OutputValidation);
        }

        bool idr = false;
        if (codec_type_ == webrtc::kVideoCodecH265) {
#if defined(RTC_ENABLE_H265)
            if (!ValidateH265Packet(packet, request_key_frame, &idr)) {
                return FailBackend();
            }
#endif
        } else {
            bool sps = false;
            bool pps = false;
            const auto nalus = H264::FindNaluIndices(packet.bytes);
            for (const auto& nalu : nalus) {
                if (nalu.payload_size == 0) return FailBackend();
                const auto type = H264::ParseNaluType(
                    packet.bytes[nalu.payload_start_offset]);
                idr |= type == H264::kIdr;
                pps |= type == H264::kPps;
                if (type == H264::kSps) {
                    if (nalu.payload_size < 4 ||
                        packet.bytes[nalu.payload_start_offset + 1] != 66 ||
                        packet.bytes[nalu.payload_start_offset + 3] >
                            static_cast<std::uint8_t>(level_)) {
                        return FailBackend();
                    }
                    sps = true;
                }
            }
            if (nalus.empty() || packet.key_frame != idr ||
                (request_key_frame && (!idr || !sps || !pps))) {
                return FailBackend();
            }
        }
        need_key_frame_ = false;

        EncodedImage encoded;
        encoded.SetEncodedData(EncodedImageBuffer::Create(
            packet.bytes.data(), packet.bytes.size()));
        encoded._encodedWidth = codec_.width;
        encoded._encodedHeight = codec_.height;
        encoded.SetRtpTimestamp(input.rtp_timestamp());
        encoded.ntp_time_ms_ = input.ntp_time_ms();
        encoded.capture_time_ms_ = input.render_time_ms();
        encoded.rotation_ = input.rotation();
        encoded.SetColorSpace(input.color_space());
        encoded._frameType = idr ? VideoFrameType::kVideoFrameKey
                                 : VideoFrameType::kVideoFrameDelta;
        encoded.content_type_ = codec_.mode == VideoCodecMode::kScreensharing
                                    ? VideoContentType::SCREENSHARE
                                    : VideoContentType::UNSPECIFIED;
        if (codec_type_ == webrtc::kVideoCodecH264) {
            bitstream_parser_.ParseBitstream(
                ArrayView<const std::uint8_t>(encoded.data(), encoded.size()));
            encoded.qp_ = bitstream_parser_.GetLastSliceQp().value_or(packet.qp);
        } else {
#if defined(RTC_ENABLE_H265)
            h265_bitstream_parser_->ParseBitstream(
                ArrayView<const std::uint8_t>(encoded.data(), encoded.size()));
            encoded.qp_ =
                h265_bitstream_parser_->GetLastSliceQp().value_or(packet.qp);
#endif
        }

        CodecSpecificInfo codec_specific;
        codec_specific.codecType = codec_type_;
        if (codec_type_ == webrtc::kVideoCodecH264) {
            codec_specific.codecSpecific.H264.packetization_mode =
                webrtc::H264PacketizationMode::NonInterleaved;
            codec_specific.codecSpecific.H264.temporal_idx =
                webrtc::kNoTemporalIdx;
            codec_specific.codecSpecific.H264.idr_frame = idr;
            codec_specific.codecSpecific.H264.base_layer_sync = false;
        }
        return callback_->OnEncodedImage(encoded, &codec_specific).error ==
                       EncodedImageCallback::Result::OK
                   ? WEBRTC_VIDEO_CODEC_OK
                   : WEBRTC_VIDEO_CODEC_ERROR;
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = info_.implementation_name;
        info.is_hardware_accelerated = info_.is_hardware_accelerated;
        info.supports_native_handle = true;
        info.supports_simulcast = false;
        info.requested_resolution_alignment = 2;
        info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNative,
                                        VideoFrameBuffer::Type::kNV12,
                                        VideoFrameBuffer::Type::kI420};
        info.has_trusted_rate_controller = false;
        return info;
    }

private:
    int FailureStatus() const {
        if (retirement_failed_) return WEBRTC_VIDEO_CODEC_ERROR;
        return codec_type_ == webrtc::kVideoCodecH265 &&
                       failure_policy_ == webrtc::BackendFailurePolicy::kDefault
                   ? WEBRTC_VIDEO_CODEC_ENCODER_FAILURE
                   : WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

#if defined(RTC_ENABLE_H265)
    bool ValidateH265Packet(const Packet& packet,
                            bool request_key_frame,
                            bool* idr) const {
        bool vps = false, sps = false, pps = false, vcl = false;
        const auto nalus = H265::FindNaluIndices(packet.bytes);
        for (const auto& nalu : nalus) {
            const auto payload = ArrayView<const std::uint8_t>(packet.bytes)
                                     .subview(nalu.payload_start_offset,
                                              nalu.payload_size);
            if (payload.size() < 2 || (payload[0] & 0x81) != 0 ||
                payload[1] != 1) {
                return false;
            }
            const auto type = H265::ParseNaluType(payload[0]);
            const bool picture_slice = type <= H265::kRaslR ||
                                       type == H265::kIdrWRadl ||
                                       type == H265::kIdrNLp;
            if ((!picture_slice &&
                 (type < H265::kVps || type > H265::kSuffixSei)) ||
                (picture_slice && payload.size() <= 2)) {
                return false;
            }
            *idr |= type == H265::kIdrWRadl || type == H265::kIdrNLp;
            vcl |= picture_slice;
            vps |= type == H265::kVps;
            pps |= type == H265::kPps;
            if (type == H265::kSps) {
                const auto data = payload.subview(2);
                const auto parsed = H265SpsParser::ParseSps(data);
                const auto rbsp = H265::ParseRbsp(data);
                BitstreamReader reader(rbsp);
                reader.ConsumeBits(4);
                const std::uint32_t max_sub_layers = reader.ReadBits(3);
                reader.ConsumeBits(1);
                if (!reader.Ok() || max_sub_layers != 0) return false;
                BitstreamReader tier_reader(rbsp);
                tier_reader.ConsumeBits(8);
                const std::uint32_t profile_space = tier_reader.ReadBits(2);
                const bool high_tier = tier_reader.Read<bool>();
                const auto ptl =
                    H265SpsParser::ParseProfileTierLevel(true, 0, reader);
                if (!tier_reader.Ok() || profile_space != 0 || high_tier ||
                    !ptl || ptl->general_profile_idc != 1 ||
                    !StringToH265Level(
                        std::to_string(ptl->general_level_idc)) ||
                    ptl->general_level_idc > level_ || !parsed ||
                    parsed->sps_max_sub_layers_minus1 != 0 ||
                    parsed->bit_depth_luma_minus8 != 0 ||
                    parsed->chroma_format_idc != 1 ||
                    parsed->width != codec_.width ||
                    parsed->height != codec_.height) {
                    return false;
                }
                reader.ReadExponentialGolomb();
                reader.ReadExponentialGolomb();
                reader.ReadExponentialGolomb();
                reader.ReadExponentialGolomb();
                if (reader.Read<bool>()) {
                    for (int i = 0; i < 4; ++i) {
                        reader.ReadExponentialGolomb();
                    }
                }
                const std::uint32_t luma_depth = reader.ReadExponentialGolomb();
                const std::uint32_t chroma_depth =
                    reader.ReadExponentialGolomb();
                if (!reader.Ok() || luma_depth != 0 || chroma_depth != 0) {
                    return false;
                }
                sps = true;
            }
        }
        return !nalus.empty() && vcl && packet.key_frame == *idr &&
               (!request_key_frame || (*idr && vps && sps && pps));
    }
#endif

    void ReportFailure(diagnostic::EncoderFailureStage stage, Status status,
            int codec_status, const VideoCodec* attempted = nullptr) const noexcept {
        const auto& parameters = attempted ? *attempted : codec_;
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::HardwareEncoderFailure;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.outcome = diagnostic::Outcome::Failure;
        event.hardware.vendor_id = 0x1002;
        event.hardware.encoder_codecs = codec_type_ == webrtc::kVideoCodecH264
            ? diagnostic::HardwareEncoderH264 : diagnostic::HardwareEncoderH265;
        event.encoder.stage = stage;
        event.encoder_level = level_;
        event.encoder.width = parameters.width;
        event.encoder.height = parameters.height;
        event.encoder.framerate = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            parameters.maxFramerate, std::numeric_limits<std::uint16_t>::max()));
        event.encoder.bitrate_bps = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::uint64_t(parameters.startBitrate) * 1000,
            std::numeric_limits<std::uint32_t>::max()));
        event.encoder.status = static_cast<std::int32_t>(status);
        event.rtc_error_type = codec_status;
        diagnostic::EmitBusinessEvent(event);
    }

    int FailBackend(Status status = Status::kError,
            diagnostic::EncoderFailureStage stage = diagnostic::EncoderFailureStage::OutputValidation) {
        failed_ = true;
        retirement_failed_ |= status == Status::kRetirementFailed;
        if (stage == diagnostic::EncoderFailureStage::Initialization &&
            backend_->LastError() == "AMF property readback mismatch: ProfileLevel") {
            stage = diagnostic::EncoderFailureStage::InitProfileLevel;
        }
        ReportFailure(stage, status, FailureStatus());
        RTC_LOG(LS_WARNING) << info_.implementation_name << ": "
                            << backend_->LastError();
        return FailureStatus();
    }

    SequenceChecker sequence_checker_;
    std::unique_ptr<D3d11AmfBackendWorker> backend_;
    const Info info_;
    const VideoCodecType codec_type_;
    const std::uint8_t level_;
    const webrtc::BackendFailurePolicy failure_policy_;
    VideoCodec codec_;
    EncodedImageCallback* callback_ = nullptr;
    H264BitstreamParser bitstream_parser_;
#if defined(RTC_ENABLE_H265)
    std::unique_ptr<H265BitstreamParser> h265_bitstream_parser_;
#endif
    bool initialized_ = false;
    bool failed_ = false;
    bool level_constraints_failed_ = false;
    bool retirement_failed_ = false;
    bool paused_ = false;
    bool need_key_frame_ = true;
};

}  // namespace

D3d11AmfEncoderDiagnostics GetD3d11AmfEncoderDiagnostics() noexcept {
    D3d11AmfEncoderDiagnostics d;
    d.dx11_native_frames_submitted =
        g_dx11_native_frames_submitted.load(std::memory_order_relaxed);
    d.host_frames_submitted =
        g_host_frames_submitted.load(std::memory_order_relaxed);
    d.simulcast_gpu_scales =
        g_simulcast_gpu_scales.load(std::memory_order_relaxed);
    d.cross_device_shared_opens =
        g_cross_device_shared_opens.load(std::memory_order_relaxed);
    return d;
}

void ResetD3d11AmfEncoderDiagnostics() noexcept {
    g_dx11_native_frames_submitted.store(0, std::memory_order_relaxed);
    g_host_frames_submitted.store(0, std::memory_order_relaxed);
    g_simulcast_gpu_scales.store(0, std::memory_order_relaxed);
    g_cross_device_shared_opens.store(0, std::memory_order_relaxed);
}

std::unique_ptr<webrtc::VideoEncoder> CreateD3d11AmfHardwareEncoder(
    const webrtc::SdpVideoFormat& format,
    webrtc::BackendFailurePolicy failure_policy) {
    if (!webrtc::IsBackendH264FormatSupported(format) &&
        !webrtc::IsBackendH265FormatSupported(format)) {
        return nullptr;
    }
    const Codec codec =
        webrtc::PayloadStringToCodecType(format.name) == webrtc::kVideoCodecH264
            ? Codec::kH264
            : Codec::kH265;
    return std::make_unique<D3d11AmfVideoEncoder>(
        format, std::make_unique<D3d11AmfBackendWorker>(codec), failure_policy);
}

std::unique_ptr<webrtc::VideoEncoder> CreateD3d11AmfVideoEncoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format,
    webrtc::external_encoder::Status* probe_status) {
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
    std::string reason;
    const auto status =
        webrtc::ProbeAmfVideoEncoderAvailability(format, &reason);
    if (probe_status) *probe_status = status;
    if (status != Status::kOk) {
        RTC_LOG(LS_WARNING)
            << format.name
            << " encoder requested=AMF effective=unavailable "
               "fallbackReason=format or actual AMD hardware initialization probe "
            << reason;
        return nullptr;
    }
    if (webrtc::IsBackendH265FormatSupported(format)) {
#if defined(RTC_ENABLE_H265)
        auto hardware = CreateD3d11AmfHardwareEncoder(
            format,
            webrtc::BackendFailurePolicy::kSameCodecSoftwareFallback);
        auto software = webrtc::CreateX265VideoEncoder(env, format);
        if (!hardware || !software) {
            RTC_LOG(LS_WARNING)
                << "H265 encoder requested=AMF effective=unavailable "
                   "fallbackReason=adapter or x265 construction failed";
            return nullptr;
        }
        RTC_LOG(LS_INFO) << "H265 encoder requested=AMF effective="
                         << hardware->GetEncoderInfo().implementation_name
                         << " runtimeFallback="
                         << software->GetEncoderInfo().implementation_name;
        return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
            env, std::move(software), std::move(hardware),
            /*prefer_temporal_support=*/false);
#else
        return nullptr;
#endif
    }
    auto hardware = CreateD3d11AmfHardwareEncoder(
        format, webrtc::BackendFailurePolicy::kDefault);
    auto software =
        webrtc::CreateH264Encoder(env, webrtc::H264EncoderSettings::Parse(format));
    if (!hardware || !software) {
        RTC_LOG(LS_WARNING)
            << "H264 encoder requested=AMF effective=unavailable "
               "fallbackReason=adapter or OpenH264 construction failed";
        return nullptr;
    }
    RTC_LOG(LS_INFO) << "H264 encoder requested=AMF effective="
                     << hardware->GetEncoderInfo().implementation_name
                     << " runtimeFallback="
                     << software->GetEncoderInfo().implementation_name;
    return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software), std::move(hardware),
        /*prefer_temporal_support=*/false);
#else
    (void)env;
    (void)format;
    if (probe_status) *probe_status = Status::kUnavailable;
    return nullptr;
#endif
}

}  // namespace livekit
