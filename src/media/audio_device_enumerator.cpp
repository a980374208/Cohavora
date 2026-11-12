#include "audio_device_enumerator.h"
#ifdef _WIN32
#include "wasapi_enumerator.h"
namespace livekit {
std::vector<AudioCaptureDeviceInfo> AudioDeviceEnumerator::EnumerateInputDevices() {
    return WasapiEnumerator::EnumerateInputDevices();
}
std::vector<AudioCaptureDeviceInfo> AudioDeviceEnumerator::EnumerateOutputDevices() {
    return WasapiEnumerator::EnumerateOutputDevices();
}
}
#else
#include "../rtc/webrtc_manager.h"
namespace livekit {
namespace {
std::vector<AudioCaptureDeviceInfo> Convert(const std::vector<AudioDeviceInfo>& devices, AudioCaptureType flow) {
    std::vector<AudioCaptureDeviceInfo> result;
    for (const auto& device : devices) {
        AudioCaptureDeviceInfo item;
        item.id = device.id;
        item.name = device.name;
        item.is_default = device.id.empty();
        item.flow = flow;
        // ADM enumeration does not expose the negotiated device format.
        item.default_sample_rate = 0;
        item.default_channels = 0;
        result.push_back(std::move(item));
    }
    return result;
}
}
std::vector<AudioCaptureDeviceInfo> AudioDeviceEnumerator::EnumerateInputDevices() {
    auto& manager = WebRTCManager::Instance();
    if (!manager.Initialize()) return {};
    return Convert(manager.ListRecordingDevices(), AudioCaptureType::Microphone);
}
std::vector<AudioCaptureDeviceInfo> AudioDeviceEnumerator::EnumerateOutputDevices() {
    auto& manager = WebRTCManager::Instance();
    if (!manager.Initialize()) return {};
    return Convert(manager.ListPlayoutDevices(), AudioCaptureType::DesktopLoopback);
}
}
#endif
