#pragma once

#include "video_frame.h"
#include "screen_binding.h"
#include "screen_share_quality.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace livekit {

// Value-only status: never put portal paths, restore tokens or raw D-Bus errors
// in this callback. Ready means the authorized capture pipeline can deliver
// frames, so the session can start its first-frame budget.
enum class DesktopCapturePhase { AwaitingPermission, Ready, Cancelled, Denied, Failed };

class IDesktopCapture {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;
    using EndCallback = std::function<void()>;
    using QualityCallback = std::function<void(ScreenShareFrameProfile)>;
    using StatusCallback = std::function<void(DesktopCapturePhase)>;
    virtual ~IDesktopCapture() = default;
    virtual void Start(DesktopSource source, FrameCallback frame, EndCallback ended) = 0;
    // Asynchronous backends report AwaitingPermission first on their owner,
    // then Ready exactly once or a terminal cancellation/failure. They must bound
    // backend setup independently of user selection time. A terminal status
    // precedes ended; ended without a terminal status means capture failure.
    // The adapter preserves existing backends' first-frame timer origin.
    virtual void StartWithStatus(DesktopSource source, FrameCallback frame,
                                 EndCallback ended, StatusCallback status) {
        Start(std::move(source), std::move(frame), std::move(ended));
        if (status) status(DesktopCapturePhase::Ready);
    }
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
    // Cumulative process counters and fixed reason codes only, retained after
    // Stop(). These are attribution evidence, not a per-share verdict.
    std::uint64_t failures = 0;
    const char* failure_reason = "none";
    std::uint64_t binding_failures = 0;
    const char* binding_failure_reason = "none";
};
DesktopCaptureObservation ObserveDesktopCapture();

// Fast, non-blocking prerequisite for attempting capture. Portal availability
// and per-source permissions are checked asynchronously by the backend.
bool DesktopCaptureSupported();
std::vector<DesktopSource> EnumerateDesktopSources();
std::unique_ptr<IDesktopCapture> CreateDesktopCapture();
std::unique_ptr<IDesktopCapture> CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions probe);
std::optional<ScreenBinding> ResolveScreenBinding(
    const DesktopSource &source, std::uint64_t sourceEpoch,
    std::string shareSessionId);
bool ValidateScreenBinding(const ScreenBinding &binding);
ScreenBindingStatus CheckScreenBinding(const ScreenBinding &binding);

} // namespace livekit
