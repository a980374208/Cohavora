#pragma once

#include "video_frame.h"
#include "screen_binding.h"
#include "screen_share_quality.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace livekit {

class IDesktopCapture {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;
    using EndCallback = std::function<void()>;
    using QualityCallback = std::function<void(ScreenShareFrameProfile)>;
    virtual ~IDesktopCapture() = default;
    virtual void Start(DesktopSource source, FrameCallback frame, EndCallback ended) = 0;
    // Called by the session owner, never from a capture callback. Joins all
    // delivery before returning; no Qt/UI calls are made by the worker.
    virtual void Stop() = 0;
    // Thread-safe mailbox. Backends and scaling are changed on the worker only.
    // A rejected request leaves the active capture untouched. The callback runs
    // after a frame with the requested profile has been delivered.
    virtual bool SetQuality(ScreenShareQuality, uint64_t, QualityCallback) { return false; }
};

enum class DesktopCaptureProbePhase {
    Created, Started, BackendFrame, Destroyed, Joined,
    SessionClosed, FramePoolClosed, D3dReleased, CloseFailed
};
struct DesktopCaptureProbeEvent {
    DesktopCaptureProbePhase phase;
    std::uint32_t capturer_id = 0;
    std::uint32_t thread_id = 0;
};
struct DesktopCaptureProbeOptions {
    bool allow_wgc_window = true;
    // Diagnostic controls only; defaults follow production capability detection.
    bool simulate_wgc_unsupported = false;
    bool simulate_dxgi_unsupported = false;
    bool use_legacy_wgc_window = false;
    unsigned simulate_dxgi_failure_after_frames = 0;
    unsigned simulate_wgc_failure_after_frames = 0;
    std::function<void(DesktopCaptureProbeEvent)> on_event;
    // Optional probe-only work/wait timings in microseconds. Production does
    // not sample these clocks or call a diagnostic sink on every frame.
    std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)> on_capture_timing;
    std::function<void(std::uint64_t)> on_conversion_timing;
};

// Process-wide diagnostics only: the backend is observed on a delivered frame,
// not inferred from capability detection. It remains available after Stop().
struct DesktopCaptureObservation {
    const char* backend = "unknown";
    std::uint64_t frames = 0;
};
DesktopCaptureObservation ObserveDesktopCapture();

std::vector<DesktopSource> EnumerateDesktopSources();
std::unique_ptr<IDesktopCapture> CreateDesktopCapture();
std::unique_ptr<IDesktopCapture> CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions probe);
std::optional<ScreenBinding> ResolveScreenBinding(
    const DesktopSource &source, std::uint64_t sourceEpoch,
    std::string shareSessionId);
bool ValidateScreenBinding(const ScreenBinding &binding);

} // namespace livekit
