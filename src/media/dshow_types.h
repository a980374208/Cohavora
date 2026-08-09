#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "video_frame.h"

namespace livekit {

enum class DShowPixelFormat {
    Unknown = 0,
    YUY2,
    NV12,
    RGB24,
    ARGB32,
    MJPEG,
    UYVY,
    I420
};

struct DShowCapability {
    int width = 0;
    int height = 0;
    int min_fps = 0;
    int max_fps = 0;
    DShowPixelFormat format = DShowPixelFormat::Unknown;
    std::int64_t min_frame_interval = 0; // 100-nanoseconds unit
    std::int64_t max_frame_interval = 0;
};

struct DShowDeviceInfo {
    std::string name;          // 友好显示名称 (如 "Integrated Camera")
    std::string path;          // 硬件设备路径 DevicePath / Moniker 字符串
    bool is_default = false;
    std::vector<DShowCapability> capabilities;
};

struct DShowCaptureConfig {
    std::string device_path;   // 设备路径 (为空时默认打开第一个可用摄像头)
    int width = 1280;          // 请求分辨率宽度
    int height = 720;          // 请求分辨率高度
    int fps = 30;              // 请求帧率 (如 30, 60)
    DShowPixelFormat preferred_format = DShowPixelFormat::NV12; // 期望优先采集格式
    VideoBufferType output_format = VideoBufferType::RGBA;      // 转换后输出给 LiveKit 的格式
    bool flip_vertically = false;                               // 垂直翻转纠正
    bool auto_reconnect = true;                                 // 设备拔插自动重连
};

} // namespace livekit
