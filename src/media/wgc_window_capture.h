#pragma once

#include "desktop_capture.h"
#include "modules/desktop_capture/desktop_capturer.h"

namespace livekit {
// Project-owned WGC session used by production capture.
// Construction, capture, Close and destruction run on the capture worker.
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcWindowCapturer(
    std::function<void(DesktopCaptureProbePhase)> report);
// Requires an initialized WinRT apartment. Support/display state is queried
// on each call; only the system runtime code image has process lifetime.
bool IsOwnedWgcSupported(DesktopSourceKind kind = DesktopSourceKind::Screen);
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcScreenCapturer(
    std::function<void(DesktopCaptureProbePhase)> report);
}
