#pragma once

#include "video_frame.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace livekit {

enum class DesktopSourceKind { Screen, Window };
struct DesktopSource {
    DesktopSourceKind kind = DesktopSourceKind::Screen;
    intptr_t id = 0;
    std::string title;
};

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

} // namespace livekit
