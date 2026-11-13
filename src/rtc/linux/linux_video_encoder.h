#pragma once

#include "rtc/webrtc_manager.h"

namespace livekit::detail {

enum class LinuxHardwareEncoderBackend { None, Nvenc, Qsv, Vaapi };
struct LinuxHardwareEncoderPlan {
    LinuxHardwareEncoderBackend backend = LinuxHardwareEncoderBackend::None;
    bool software_only = false;
    std::uint64_t generation = 0;
    LinuxEncoderDeviceIdentity device;
};
struct LinuxHardwareEncoderProbeResults {
    VideoEncoderBackendCapability nvenc;
    VideoEncoderBackendCapability qsv;
    VideoEncoderBackendCapability vaapi;
};

// These functions are linked only by the reviewed Linux hardware build.
// Capability reads and encoder construction do not discover a driver.
LinuxHardwareEncoderPlan ResolveLinuxHardwareEncoderPlan(
    const std::string& preference, const VideoEncoderCapabilities& capabilities);
bool LinuxHardwareEncoderRetirementFailed();
bool IsLinuxHardwareH264FormatSupported(const webrtc::SdpVideoFormat& format);
LinuxHardwareEncoderProbeResults ProbeLinuxHardwareVideoEncoderCapabilities(
    const VideoEncoderCapabilities& pinned,
    std::shared_ptr<std::atomic<bool>> cancelled = {});
VideoEncoderPreflightResult PreflightLinuxHardwareVideoEncoder(
    const VideoEncoderPreflightRequest& request,
    std::shared_ptr<std::atomic<bool>> cancelled);
std::unique_ptr<webrtc::VideoEncoder> CreateLinuxH264VideoEncoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format,
    const LinuxHardwareEncoderPlan& plan);

} // namespace livekit::detail
