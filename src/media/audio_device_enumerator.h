#pragma once
#include "audio_capture_types.h"

namespace livekit {
class AudioDeviceEnumerator {
public:
    static std::vector<AudioCaptureDeviceInfo> EnumerateInputDevices();
    static std::vector<AudioCaptureDeviceInfo> EnumerateOutputDevices();
};
}
