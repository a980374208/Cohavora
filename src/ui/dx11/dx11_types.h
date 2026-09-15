#pragma once

#include <d3d11.h>
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

// 单个参会人画框在画布内的物理像素坐标
struct TileRect {
    std::string identity;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool isSpeaking = false;
    float audioLevel = 0.0f;
    bool hasVideo = false;
};

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
