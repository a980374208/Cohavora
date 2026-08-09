#pragma once

#include <vector>
#include <string>
#include <memory>
#include "wasapi_types.h"

namespace livekit {

class WasapiEnumerator {
public:
    // 枚举所有当前启用的麦克风/音频输入设备
    static std::vector<WasapiDeviceInfo> EnumerateInputDevices();

    // 枚举所有当前启用的扬声器/耳机输出设备 (用于桌面音频回环录制)
    static std::vector<WasapiDeviceInfo> EnumerateOutputDevices();

    // 获取系统当前默认的音频输入设备
    static WasapiDeviceInfo GetDefaultInputDevice();

    // 获取系统当前默认的音频输出设备
    static WasapiDeviceInfo GetDefaultOutputDevice();

    // 根据设备 ID 查找设备信息
    static bool GetDeviceInfo(const std::string& device_id, WasapiCaptureType type, WasapiDeviceInfo& out_info);
};

} // namespace livekit
