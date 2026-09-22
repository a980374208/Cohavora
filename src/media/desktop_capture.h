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

std::vector<DesktopSource> EnumerateDesktopSources();
std::unique_ptr<IDesktopCapture> CreateDesktopCapture();
std::optional<ScreenBinding> ResolveScreenBinding(
    const DesktopSource &source, std::uint64_t sourceEpoch,
    std::string shareSessionId);
bool ValidateScreenBinding(const ScreenBinding &binding);

} // namespace livekit
