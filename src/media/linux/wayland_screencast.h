#pragma once

#include "../desktop_capture.h"
#include <mutex>
#include <thread>

namespace livekit::linux_capture {

// All portal/PipeWire control and delivery belongs to one private GLib owner.
// The session owns this facade; no QObject owns native capture resources.
class WaylandScreencast final : public IDesktopCapture {
public:
    WaylandScreencast();
    ~WaylandScreencast() override;
    void Start(DesktopSource, FrameCallback, EndCallback) override;
    void StartWithStatus(DesktopSource, FrameCallback, EndCallback, StatusCallback) override;
    void Stop() override;
    bool SetQuality(ScreenShareQuality, std::uint64_t, QualityCallback) override;
private:
    struct Run;
    std::mutex mutex_;
    std::shared_ptr<Run> run_;
    std::thread worker_;
    ScreenShareQuality quality_;
    std::uint64_t revision_ = 0;
    QualityCallback quality_callback_;
};

bool WaylandCaptureSupported();
DesktopCaptureObservation ObserveWaylandCapture();

} // namespace livekit::linux_capture
