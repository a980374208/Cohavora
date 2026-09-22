#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "api/audio/audio_device.h"

namespace livekit::detail {

// Called only on the ADM worker thread. Keeping the backend as a template
// permits device-independent coverage of the same production switch logic.
template <typename Device, typename ResetApm>
bool ApplyPlayoutDevice(Device& device, std::optional<uint16_t> index, ResetApm reset_apm) {
    const bool was_playing = device.Playing();
    const bool was_initialized = device.PlayoutIsInitialized();
    if ((was_playing || was_initialized) && device.StopPlayout() != 0) {
        return false;
    }
    const int32_t result = index
        ? device.SetPlayoutDevice(*index)
        : device.SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
    if (result == 0) {
        reset_apm();
    }
    // Attempt to restore rendering even when selection was rejected. A
    // stopped but initialized stream must also be initialized again after
    // StopPlayout.
    if (was_playing || was_initialized) {
        if (device.InitSpeaker() != 0 || device.InitPlayout() != 0) {
            return false;
        }
        if (was_playing && device.StartPlayout() != 0) {
            return false;
        }
    }
    return result == 0;
}

template <typename Device, typename ResetApm>
bool SelectPlayoutDeviceById(Device& device, const std::string& device_id, ResetApm reset_apm) {
    if (device_id.empty()) {
        return ApplyPlayoutDevice(device, std::nullopt, reset_apm);
    }
    const int16_t count = device.PlayoutDevices();
    for (int16_t index = 0; index < count; ++index) {
        char name[webrtc::kAdmMaxDeviceNameSize] = {};
        char guid[webrtc::kAdmMaxGuidSize] = {};
        if (device.PlayoutDeviceName(static_cast<uint16_t>(index), name, guid) == 0
                && _stricmp(guid, device_id.c_str()) == 0) {
            return ApplyPlayoutDevice(device, static_cast<uint16_t>(index), reset_apm);
        }
    }
    // A removed/stale device must not switch to a different endpoint, nor
    // interrupt the currently working output.
    return false;
}

} // namespace livekit::detail
