#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace livekit {

enum class LinuxEncoderCapabilityReason {
    None, NotCompiled, NotProbed, UnknownUnavailable, MissingLibrary, NoDevice,
    PermissionDenied, UnsupportedProfile, TimedOut, RetirementFailed,
    DeviceChanged, Cancelled, UnsupportedConfiguration, RuntimeIncompatible
};

inline std::string_view LinuxEncoderCapabilityReasonText(LinuxEncoderCapabilityReason reason) noexcept {
    switch (reason) {
    case LinuxEncoderCapabilityReason::None: return "none";
    case LinuxEncoderCapabilityReason::NotCompiled: return "not_compiled";
    case LinuxEncoderCapabilityReason::NotProbed: return "not_probed";
    case LinuxEncoderCapabilityReason::MissingLibrary: return "missing_library";
    case LinuxEncoderCapabilityReason::NoDevice: return "no_device";
    case LinuxEncoderCapabilityReason::PermissionDenied: return "permission_denied";
    case LinuxEncoderCapabilityReason::UnsupportedProfile: return "unsupported_profile";
    case LinuxEncoderCapabilityReason::TimedOut: return "timed_out";
    case LinuxEncoderCapabilityReason::RetirementFailed: return "retirement_failed";
    case LinuxEncoderCapabilityReason::DeviceChanged: return "device_changed";
    case LinuxEncoderCapabilityReason::Cancelled: return "cancelled";
    case LinuxEncoderCapabilityReason::UnsupportedConfiguration: return "unsupported_configuration";
    case LinuxEncoderCapabilityReason::RuntimeIncompatible: return "runtime_incompatible";
    default: return "unknown_unavailable";
    }
}

// Only fixed SDK reason codes are classified. Unknown/driver-provided text is
// deliberately collapsed and is never copied into a diagnostic or UI string.
inline LinuxEncoderCapabilityReason ClassifyLinuxEncoderProbeReason(std::string_view reason) noexcept {
    using Reason = LinuxEncoderCapabilityReason;
    if (reason == "cuda_library_unavailable" || reason == "cuda_entry_point_missing" ||
        reason == "nvenc_library_unavailable" || reason == "nvenc_entry_point_missing" ||
        reason == "qsv_vpl_library_unavailable" || reason == "qsv_va_library_unavailable" ||
        reason == "qsv_va_drm_library_unavailable" || reason == "qsv_drm_library_unavailable" ||
        reason == "qsv_driver_entry_point_missing" || reason == "vaapi_runtime_unavailable" ||
        reason == "vaapi_runtime_function_missing") return Reason::MissingLibrary;
    if (reason == "cuda_device_unavailable" ||
        reason == "qsv_intel_render_device_unavailable" || reason == "qsv_drm_enumeration_unavailable" ||
        reason == "vaapi_device_unavailable" || reason == "vaapi_render_node_unavailable") return Reason::NoDevice;
    if (reason == "qsv_render_node_permission_denied" || reason == "vaapi_render_node_permission_denied")
        return Reason::PermissionDenied;
    if (reason == "vaapi_runtime_abi_mismatch" || reason == "nvenc_api_version_unsupported")
        return Reason::RuntimeIncompatible;
    if (reason == "nvenc_device_identity_changed" || reason == "qsv_opened_drm_identity_mismatch" ||
        reason == "qsv_selected_runtime_or_display_mismatch" || reason == "vaapi_device_identity_changed")
        return Reason::DeviceChanged;
    return Reason::UnknownUnavailable;
}

// Owned machine-local identity, used only to bind the SDK's typed factory.
// Device paths/PCI strings and raw driver detail never enter ordinary logs/UI.
struct LinuxEncoderDeviceIdentity {
    bool valid = false;
    int cuda_ordinal = -1;
    std::string pci_bus_id;
    std::string render_node;
    std::uint16_t vendor_id = 0;
    std::uint16_t device_id = 0;
    std::uint32_t pci_domain = 0;
    std::uint8_t pci_bus = 0;
    std::uint8_t pci_device = 0;
    std::uint8_t pci_function = 0;
    std::optional<std::uint32_t> cuda_driver_version;
    std::optional<std::uint32_t> nvenc_api_version;
    std::optional<std::uint16_t> vpl_api_major;
    std::optional<std::uint16_t> vpl_api_minor;
    std::optional<std::uint16_t> va_api_major;
    std::optional<std::uint16_t> va_api_minor;
    bool operator==(const LinuxEncoderDeviceIdentity&) const = default;
};

struct VideoEncoderBackendCapability {
    bool h264 = false;
    bool h265 = false;
    bool compiled = false;
    bool discovered = false;
    bool probe_ready = false;
    LinuxEncoderCapabilityReason reason = LinuxEncoderCapabilityReason::NotCompiled;
    LinuxEncoderDeviceIdentity device;
};
struct VideoEncoderCapabilities {
    bool ready = false;
    VideoEncoderBackendCapability amf;
    VideoEncoderBackendCapability nvenc;
    VideoEncoderBackendCapability qsv;
    VideoEncoderBackendCapability vaapi;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool probing = false;
};

inline bool SameLinuxEncoderDevice(const LinuxEncoderDeviceIdentity& left,
                                  const LinuxEncoderDeviceIdentity& right) noexcept {
    return left.valid && right.valid && left.cuda_ordinal == right.cuda_ordinal &&
        left.pci_bus_id == right.pci_bus_id && left.render_node == right.render_node &&
        left.vendor_id == right.vendor_id && left.device_id == right.device_id &&
        left.pci_domain == right.pci_domain && left.pci_bus == right.pci_bus &&
        left.pci_device == right.pci_device && left.pci_function == right.pci_function;
}

// Only a completed, successful probe can establish a process pin. Invalidation
// retains it; a refresh can verify it or report unavailable, never replace it.
inline void PreserveLinuxEncoderDevicePins(VideoEncoderCapabilities& next,
                                          const VideoEncoderCapabilities& previous) {
    const auto preserve = [](auto& candidate, const auto& old) {
        if (!candidate.probe_ready) {
            candidate.device = old.device;
            return;
        }
        if (old.device.valid && !SameLinuxEncoderDevice(old.device, candidate.device)) {
            candidate.h264 = candidate.h265 = candidate.probe_ready = candidate.discovered = false;
            candidate.reason = LinuxEncoderCapabilityReason::DeviceChanged;
            candidate.device = old.device;
        }
    };
    preserve(next.nvenc, previous.nvenc);
    preserve(next.qsv, previous.qsv);
    preserve(next.vaapi, previous.vaapi);
}

inline LinuxEncoderCapabilityReason LinuxEncoderRequestReason(
        std::string_view preference, const VideoEncoderCapabilities& capabilities) noexcept {
    if (preference == "software") return LinuxEncoderCapabilityReason::None;
    if (preference == "nvenc") return capabilities.nvenc.reason;
    if (preference == "qsv") return capabilities.qsv.reason;
    if (preference == "vaapi") return capabilities.vaapi.reason;
    if (capabilities.nvenc.probe_ready || capabilities.qsv.probe_ready || capabilities.vaapi.probe_ready)
        return LinuxEncoderCapabilityReason::None;
    if (capabilities.nvenc.reason == LinuxEncoderCapabilityReason::RetirementFailed ||
        capabilities.qsv.reason == LinuxEncoderCapabilityReason::RetirementFailed ||
        capabilities.vaapi.reason == LinuxEncoderCapabilityReason::RetirementFailed)
        return LinuxEncoderCapabilityReason::RetirementFailed;
    return capabilities.nvenc.compiled || capabilities.qsv.compiled || capabilities.vaapi.compiled
        ? LinuxEncoderCapabilityReason::UnknownUnavailable : LinuxEncoderCapabilityReason::NotCompiled;
}

} // namespace livekit
