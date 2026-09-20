#pragma once

#include <d3d11.h>
#include "render/video_layout.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "src/rtc/video_frame.h"

namespace livekit {
namespace dx11 {

using Microsoft::WRL::ComPtr;

// 顶点结构 (归一化 Quad)
struct Vertex {
    float x, y, z;
    float u, v;
};

using TileRect = render::VideoTileRect;

enum class PixelFormatType {
    Unknown,
    I420,
    NV12,
    RGBA
};

inline PixelFormatType MapBufferTypeToPixelFormat(VideoBufferType type) {
    switch (type) {
    case VideoBufferType::I420:
    case VideoBufferType::I420A:
        return PixelFormatType::I420;
    case VideoBufferType::NV12:
        return PixelFormatType::NV12;
    case VideoBufferType::RGBA:
    case VideoBufferType::BGRA:
    case VideoBufferType::ARGB:
    case VideoBufferType::ABGR:
        return PixelFormatType::RGBA;
    default:
        return PixelFormatType::Unknown;
    }
}

} // namespace dx11
} // namespace livekit
