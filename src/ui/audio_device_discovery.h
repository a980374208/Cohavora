#pragma once

#include "src/media/audio_capture_types.h"
#include "src/ui/cached_device_discovery.h"

namespace MeetingUI {

using AudioDeviceDiscovery = CachedDeviceDiscovery<std::vector<livekit::AudioCaptureDeviceInfo>>;
AudioDeviceDiscovery &MicrophoneDeviceDiscovery();
AudioDeviceDiscovery &SpeakerDeviceDiscovery();

} // namespace MeetingUI
