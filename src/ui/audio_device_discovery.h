#pragma once

#include "src/media/wasapi_types.h"
#include "src/ui/cached_device_discovery.h"

namespace MeetingUI {

using AudioDeviceDiscovery = CachedDeviceDiscovery<std::vector<livekit::WasapiDeviceInfo>>;
AudioDeviceDiscovery &MicrophoneDeviceDiscovery();
AudioDeviceDiscovery &SpeakerDeviceDiscovery();

} // namespace MeetingUI
