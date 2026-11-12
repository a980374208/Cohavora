#include "desktop_capture.h"
#include "linux/wayland_screencast.h"

namespace livekit {
bool DesktopCaptureSupported() { return linux_capture::WaylandCaptureSupported(); }
std::vector<DesktopSource> EnumerateDesktopSources() {
    if (!DesktopCaptureSupported()) return {};
    // These are capture intents, not a synthetic list of desktop identities.
    // The system picker supplies the actual authorized screen/window.
    return {
        {DesktopSourceKind::Screen, 0, "Choose a screen in the system dialog"},
        {DesktopSourceKind::Window, 0, "Choose a window in the system dialog"}
    };
}
std::unique_ptr<IDesktopCapture> CreateDesktopCapture() {
    return std::make_unique<linux_capture::WaylandScreencast>();
}
std::unique_ptr<IDesktopCapture> CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions) {
    return CreateDesktopCapture();
}
DesktopCaptureObservation ObserveDesktopCapture() {
    return linux_capture::ObserveWaylandCapture();
}
std::optional<ScreenBinding> ResolveScreenBinding(const DesktopSource&, std::uint64_t, std::string) { return {}; }
bool ValidateScreenBinding(const ScreenBinding&) { return false; }
ScreenBindingStatus CheckScreenBinding(const ScreenBinding&) { return ScreenBindingStatus::Unavailable; }
}
