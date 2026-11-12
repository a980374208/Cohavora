#include "remote_input.h"
#ifdef _WIN32
#include "win/remote_input.h"
#endif

namespace livekit::remote_control {
Capabilities PlatformRemoteControlCapabilities(std::string_view qpa) {
    Capabilities result;
#ifdef _WIN32
    if (qpa == "windows") {
        result.controller.status = result.host.status = CapabilityStatus::Available;
        result.controllerKeys = NativeKeyEncoding::WindowsSet1;
    } else {
        result.controller.status = result.host.status = CapabilityStatus::UnsupportedWindowSystem;
    }
#elif defined(__linux__)
    // QtWayland 5.15 passes the protocol evdev code plus eight. XCB passes
    // server keycodes; enabling it needs an independently checked XKB mapping.
    if (qpa == "wayland" || qpa == "wayland-egl") {
        result.controller.status = CapabilityStatus::Available;
        result.controllerKeys = NativeKeyEncoding::LinuxEvdevOffset8;
    } else {
        result.controller.status = qpa == "xcb" ? CapabilityStatus::UnsupportedKeyboardMapping
                                                : CapabilityStatus::UnsupportedWindowSystem;
    }
    // Portal/EIS availability alone does not establish physical-input priority,
    // release-and-idle recovery, verified screen binding or background emergency
    // stop. Keep the existing Windows safety contract and fail closed.
    result.host.status = CapabilityStatus::HostSafetyUnavailable;
#endif
    return result;
}
#ifndef _WIN32
namespace {
class UnavailableInputBackend final : public InputBackend {
public:
    bool start(const ScreenBinding&, std::shared_ptr<Lease>) override { return false; }
    bool submit(const Input&, uint64_t) override { return false; }
    void pause(bool) override {}
    InputStatus status() override { return {}; }
    void stop() override {}
};
}
#endif
std::shared_ptr<InputBackend> CreatePlatformInputBackend() {
#ifdef _WIN32
    return CreateWindowsInputBackend();
#else
    return std::make_shared<UnavailableInputBackend>();
#endif
}
}
