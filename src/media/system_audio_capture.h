#pragma once

#include "../rtc/audio_frame.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace livekit {

struct SystemAudioOutput {
    std::string id; // PulseAudio sink identity, never an ADM display index.
    std::string name;
    bool is_default = false;
};

struct SystemAudioCaptureConfig {
    std::string output_id; // Empty follows the server's default output.
};

enum class SystemAudioCapturePhase { Connecting, Ready, Failed };
enum class SystemAudioCaptureError {
    None, Unavailable, Connect, OutputUnavailable, Format, Stream, TooManyStreams
};
struct SystemAudioCaptureStatus {
    SystemAudioCapturePhase phase = SystemAudioCapturePhase::Connecting;
    SystemAudioCaptureError error = SystemAudioCaptureError::None;
};
struct SystemAudioCaptureStats {
    std::uint64_t delivered_frames = 0;
    std::uint64_t dropped_samples = 0;
    std::uint64_t excluded_streams = 0;
    std::uint64_t route_changes = 0;
};

class ISystemAudioCapture {
public:
    using FrameCallback = std::function<void(const AudioFrame&)>;
    using StatusCallback = std::function<void(SystemAudioCaptureStatus)>;
    virtual ~ISystemAudioCapture() = default;
    // Nonblocking startup. Callbacks run on capture workers, never Qt/strand.
    virtual bool Start(SystemAudioCaptureConfig, FrameCallback, StatusCallback) = 0;
    // Cleanup worker only: joins capture and delivery workers. No later callbacks.
    virtual void Stop() = 0;
    virtual SystemAudioCaptureStats stats() const = 0;
};

// PulseAudio sink-input monitors mixed at 48 kHz stereo / 10 ms. Each input
// MUST have a known process ID different from this process; meeting playback
// and unattributed inputs are excluded before any record stream is connected.
// This is output audio, independent of the portal's screen/window selection.
std::unique_ptr<ISystemAudioCapture> CreateSystemAudioCapture();
// Bounded blocking discovery; call on a discovery/cleanup worker, never Qt.
std::vector<SystemAudioOutput> EnumerateSystemAudioOutputs();
bool SystemAudioSharingAvailable();

} // namespace livekit
