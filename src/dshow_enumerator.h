#pragma once

#include <vector>
#include <string>
#include <memory>
#include "dshow_types.h"

namespace livekit {

class DShowEnumerator {
public:
    // 枚举系统中所有 DirectShow 视频输入设备 (摄像头、USB采集卡、OBS虚拟摄像头等)
    static std::vector<DShowDeviceInfo> EnumerateVideoDevices();

    // 枚举指定视频设备支持的所有分辨率、帧率与格式能力
    static std::vector<DShowCapability> GetDeviceCapabilities(const std::string& device_path);

    // 获取系统默认/第一个可用的视频捕获设备
    static DShowDeviceInfo GetDefaultVideoDevice();

    // 格式化输出设备信息与能力表
    static std::string DumpDeviceInfo(const DShowDeviceInfo& info);
};

} // namespace livekit
