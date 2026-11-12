#pragma once
#include "camera_types.h"

namespace livekit {
class CameraEnumerator {
public:
    static std::vector<CameraDeviceInfo> EnumerateVideoDevices();
    static std::vector<CameraCapability> GetDeviceCapabilities(const std::string& device_path);
};
} // namespace livekit
