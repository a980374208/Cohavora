#pragma once

#include "video_frame.h"
#include "screen_binding.h"
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
    virtual ~IDesktopCapture() = default;
    virtual void Start(DesktopSource source, FrameCallback frame, EndCallback ended) = 0;
    // Called by the session owner, never from a capture callback. Joins all
    // delivery before returning; no Qt/UI calls are made by the worker.
    virtual void Stop() = 0;
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
    std::function<void(DesktopCaptureProbeEvent)> on_event;
};

std::vector<DesktopSource> EnumerateDesktopSources();
std::unique_ptr<IDesktopCapture> CreateDesktopCapture();
std::unique_ptr<IDesktopCapture> CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions probe);
std::optional<ScreenBinding> ResolveScreenBinding(
    const DesktopSource &source, std::uint64_t sourceEpoch,
    std::string shareSessionId);
bool ValidateScreenBinding(const ScreenBinding &binding);

} // namespace livekit
