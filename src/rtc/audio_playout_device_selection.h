#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "api/audio/audio_device.h"

namespace livekit {

enum class PlayoutSelection {
    Requested,
    Previous,
    Unknown,
};

enum class PlayoutFailureStage {
    None,
    Resolve,
    Stop,
    Select,
    Init,
    Start,
    Verify,
    Rollback,
};

struct PlayoutSwitchResult {
    bool applied = false;
    PlayoutSelection selection = PlayoutSelection::Unknown;
    bool playing = false;
    bool initialized = false;
    PlayoutFailureStage failure_stage = PlayoutFailureStage::None;
};

namespace detail {

// Linux's numeric ADM devices must be stopped before selection. If the new
// stream cannot start, select and restart the previous endpoint while its
// identity is still known. Report the actual stream state on every path.
template <typename Device, typename ResetApm, typename ValidatePrevious>
PlayoutSwitchResult ApplyPlayoutDeviceWithRollback(
        Device& device, uint16_t requested, std::optional<uint16_t> previous,
        ResetApm reset_apm, ValidatePrevious validate_previous) {
    const bool was_playing = device.Playing();
    const bool was_initialized = device.PlayoutIsInitialized();
    const auto previous_current = [&]() {
        return previous && validate_previous(*previous);
    };
    const auto state = [&](bool applied, PlayoutSelection selection,
                           PlayoutFailureStage failure = PlayoutFailureStage::None) {
        return PlayoutSwitchResult{applied, selection, device.Playing(),
                                   device.PlayoutIsInitialized(), failure};
    };
    if (previous_current() && *previous == requested) {
        if (!was_playing) {
            if (!was_initialized && device.InitPlayout() != 0) {
                return state(false, PlayoutSelection::Requested,
                             PlayoutFailureStage::Init);
            }
            if (!device.PlayoutIsInitialized()) {
                return state(false, PlayoutSelection::Requested,
                             PlayoutFailureStage::Verify);
            }
            if (device.StartPlayout() != 0) {
                return state(false, PlayoutSelection::Requested,
                             PlayoutFailureStage::Start);
            }
            if (!device.Playing()) {
                return state(false, PlayoutSelection::Requested,
                             PlayoutFailureStage::Verify);
            }
        }
        return state(true, PlayoutSelection::Requested);
    }
    if ((was_playing || was_initialized) && device.StopPlayout() != 0) {
        return state(false, previous_current() ? PlayoutSelection::Previous
                                                : PlayoutSelection::Unknown,
                     PlayoutFailureStage::Stop);
    }

    const auto restart = [&]() {
        if (!was_playing && !was_initialized) return PlayoutFailureStage::None;
        if (device.InitPlayout() != 0) return PlayoutFailureStage::Init;
        if (!device.PlayoutIsInitialized()) return PlayoutFailureStage::Verify;
        if (was_playing) {
            if (device.StartPlayout() != 0) return PlayoutFailureStage::Start;
            if (!device.Playing()) return PlayoutFailureStage::Verify;
        }
        return PlayoutFailureStage::None;
    };
    if (device.SetPlayoutDevice(requested) != 0) {
        if (previous_current() && device.SetPlayoutDevice(*previous) == 0) {
            const auto rollback = restart();
            return state(false, PlayoutSelection::Previous,
                         rollback == PlayoutFailureStage::None
                             ? PlayoutFailureStage::Select
                             : PlayoutFailureStage::Rollback);
        }
        return state(false, PlayoutSelection::Unknown,
                     previous ? PlayoutFailureStage::Rollback
                              : PlayoutFailureStage::Select);
    }
    reset_apm();
    const auto failure = restart();
    if (failure == PlayoutFailureStage::None) {
        return state(true, PlayoutSelection::Requested);
    }

    if (device.StopPlayout() != 0) {
        return state(false, PlayoutSelection::Requested,
                     PlayoutFailureStage::Rollback);
    }
    if (previous_current() && device.SetPlayoutDevice(*previous) == 0) {
        reset_apm();
        const auto rollback = restart();
        return state(false, PlayoutSelection::Previous,
                     rollback == PlayoutFailureStage::None
                         ? failure : PlayoutFailureStage::Rollback);
    }
    return state(false, previous ? PlayoutSelection::Unknown
                                 : PlayoutSelection::Requested,
                 previous ? PlayoutFailureStage::Rollback : failure);
}

template <typename Device, typename ResetApm>
PlayoutSwitchResult ApplyPlayoutDeviceWithRollback(
        Device& device, uint16_t requested, std::optional<uint16_t> previous,
        ResetApm reset_apm) {
    return ApplyPlayoutDeviceWithRollback(
        device, requested, previous, reset_apm, [](uint16_t) { return true; });
}

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
#if defined(_WIN32)
                && _stricmp(guid, device_id.c_str()) == 0) {
#else
                && device_id == guid) {
#endif
            return ApplyPlayoutDevice(device, static_cast<uint16_t>(index), reset_apm);
        }
    }
    // A removed/stale device must not switch to a different endpoint, nor
    // interrupt the currently working output.
    return false;
}

} // namespace detail
} // namespace livekit
