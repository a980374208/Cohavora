#pragma once

#include "desktop_capture.h"
#include "modules/desktop_capture/desktop_capturer.h"

namespace livekit {
// Project-owned WGC session used by production capture.
// Construction, capture, Close and destruction run on the capture worker.
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcWindowCapturer(
    std::function<void(DesktopCaptureProbePhase)> report);
bool IsOwnedWgcSupported(); // Requires an initialized WinRT apartment.
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcScreenCapturer(
    std::function<void(DesktopCaptureProbePhase)> report);
}
