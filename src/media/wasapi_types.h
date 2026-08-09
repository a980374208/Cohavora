#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace livekit {

enum class WasapiCaptureType {
    Microphone = 0,    // 麦克风 / 输入音频捕获 (eCapture)
    DesktopLoopback = 1 // 扬声器回环 / 系统桌面音频捕获 (eRender with AUDCLNT_STREAMFLAGS_LOOPBACK)
};

struct WasapiDeviceInfo {
    std::string id;            // 设备的 Endpoint ID 字符串
    std::string name;          // 友好显示名称 (如 "Realtek High Definition Audio")
    bool is_default = false;   // 是否为当前系统默认设备
    WasapiCaptureType flow = WasapiCaptureType::Microphone; // 输入还是输出回环
    uint32_t default_sample_rate = 48000;
    uint16_t default_channels = 2;
};

struct WasapiCaptureConfig {
    std::string device_id;     // 为空时自动使用系统默认设备
    WasapiCaptureType type = WasapiCaptureType::Microphone;
    int target_sample_rate = 48000; // 目标重采样输出采样率 (如 48000Hz)
    int target_channels = 2;       // 目标声道数 (1: 单声道, 2: 双声道)
    int buffer_duration_ms = 20;   // 内部音频缓冲周期 (默认 20ms)
    bool auto_reconnect = true;    // 当默认设备切换或设备拔插时自动恢复
};

} // namespace livekit
