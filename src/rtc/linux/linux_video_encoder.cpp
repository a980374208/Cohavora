#include "linux_video_encoder.h"
#include "linux_encoder_probe_worker.h"
#include "linux_software_fallback_guard.h"
#include "linux_hardware_runtime_guard.h"

#include "api/environment/environment_factory.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device.h"
#if defined(COHAVORA_HAS_LINUX_NVENC)
#include "modules/video_coding/codecs/external/linux_nvenc_video_encoder.h"
#endif
#if defined(COHAVORA_HAS_LINUX_QSV)
#include "modules/video_coding/codecs/external/linux_qsv_video_encoder.h"
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
#include "modules/video_coding/codecs/external/linux_vaapi_video_encoder.h"
#endif

namespace livekit::detail {
namespace {

using Status = VideoEncoderPreflightStatus;
using Executor = LinuxEncoderProbeExecutor<VideoEncoderPreflightResult>;
using Backend = LinuxHardwareEncoderBackend;
using Vendor = webrtc::external_encoder::HardwareEncoderVendor;
constexpr auto kTotalProbeBudget = std::chrono::seconds(4);

const char* BackendName(Backend backend) {
    switch (backend) {
    case Backend::Nvenc: return "nvenc";
    case Backend::Qsv: return "qsv";
    case Backend::Vaapi: return "vaapi";
    default: return "none";
    }
}

void RecordRetirementFailure(Backend backend) {
    constexpr auto reason = "linux_encoder_probe_deadline_exceeded";
#if defined(WEBRTC_LINUX_BACKEND_RETIREMENT_REGISTRY)
    webrtc::external_encoder::RecordLinuxBackendRetirementFailure(reason);
#endif
    switch (backend) {
    case Backend::Nvenc:
        webrtc::external_encoder::HardwareEncoderProbes().RecordRetirementFailure(Vendor::kNvenc, reason);
        break;
    case Backend::Qsv:
        webrtc::external_encoder::HardwareEncoderProbes().RecordRetirementFailure(Vendor::kQsv, reason);
        break;
    case Backend::Vaapi:
#if defined(COHAVORA_HAS_LINUX_VAAPI)
        webrtc::external_encoder::RecordLinuxVaapiRetirement(reason);
#endif
        break;
    default: break;
    }
}

VideoEncoderPreflightResult FinishProbe(Executor::Reply reply, Backend backend) {
    VideoEncoderPreflightResult result;
    result.backend = BackendName(backend);
    if (reply.wait == Executor::Wait::TimedOut) {
        // Record the actual owner, including VAAPI's independent retirement
        // state; it must never be attributed to the Windows AMF vendor.
        RecordRetirementFailure(backend);
        result.status = Status::TimedOut;
        return result;
    }
    if (reply.wait == Executor::Wait::Completed && reply.value)
        result = std::move(*reply.value);
    result.backend = BackendName(backend);
    if (LinuxHardwareEncoderRetirementFailed()) {
        result.status = Status::RetirementFailed;
        result.formats.clear();
    }
    return result;
}

bool Cancelled(const std::shared_ptr<std::atomic<bool>>& cancelled) {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

LinuxEncoderCapabilityReason CapabilityReason(Status status) {
    switch (status) {
    case Status::Ready: return LinuxEncoderCapabilityReason::None;
    case Status::UnsupportedFormat: return LinuxEncoderCapabilityReason::UnsupportedConfiguration;
    case Status::TimedOut: return LinuxEncoderCapabilityReason::TimedOut;
    case Status::RetirementFailed: return LinuxEncoderCapabilityReason::RetirementFailed;
    case Status::Cancelled: return LinuxEncoderCapabilityReason::Cancelled;
    default: return LinuxEncoderCapabilityReason::UnknownUnavailable;
    }
}

template <typename Selection>
LinuxEncoderDeviceIdentity RenderIdentity(const Selection& selected) {
    LinuxEncoderDeviceIdentity device;
    device.valid = !selected.render_node.empty() && selected.vendor_id != 0;
    device.render_node = selected.render_node;
    device.vendor_id = selected.vendor_id;
    device.device_id = selected.device_id;
    device.pci_domain = selected.pci_domain;
    device.pci_bus = selected.pci_bus;
    device.pci_device = selected.pci_device;
    device.pci_function = selected.pci_function;
    return device;
}
template <typename Selection>
Selection RenderSelection(const LinuxEncoderDeviceIdentity& device) {
    Selection selected;
    selected.render_node = device.render_node;
    selected.vendor_id = device.vendor_id;
    selected.device_id = device.device_id;
    selected.pci_domain = device.pci_domain;
    selected.pci_bus = device.pci_bus;
    selected.pci_device = device.pci_device;
    selected.pci_function = device.pci_function;
    return selected;
}

std::unique_ptr<webrtc::VideoEncoder> CreateHardwareEncoder(
        const LinuxHardwareEncoderPlan& plan, const webrtc::Environment& environment,
        const webrtc::SdpVideoFormat& format) {
    if (!plan.device.valid) return nullptr;
    switch (plan.backend) {
#if defined(COHAVORA_HAS_LINUX_NVENC)
    case Backend::Nvenc: {
        webrtc::external_encoder::LinuxNvencDeviceSelection selected;
        selected.cuda_ordinal = plan.device.cuda_ordinal;
        selected.pci_bus_id = plan.device.pci_bus_id;
        selected.cuda_driver_version = plan.device.cuda_driver_version.value_or(0);
        selected.nvenc_api_version = plan.device.nvenc_api_version.value_or(0);
        return webrtc::CreateLinuxNvencVideoEncoder(environment, format, selected);
    }
#endif
#if defined(COHAVORA_HAS_LINUX_QSV)
    case Backend::Qsv: {
        auto selected = RenderSelection<webrtc::external_encoder::LinuxQsvDeviceSelection>(plan.device);
        selected.vpl_api_major = plan.device.vpl_api_major.value_or(0);
        selected.vpl_api_minor = plan.device.vpl_api_minor.value_or(0);
        selected.va_api_major = plan.device.va_api_major.value_or(0);
        selected.va_api_minor = plan.device.va_api_minor.value_or(0);
        return webrtc::CreateLinuxQsvVideoEncoder(environment, format, selected);
    }
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
    case Backend::Vaapi: return webrtc::CreateLinuxVaapiVideoEncoder(environment, format,
        RenderSelection<webrtc::external_encoder::LinuxVaapiDeviceSelection>(plan.device));
#endif
    default: return nullptr;
    }
}

VideoEncoderBackendCapability ProbeBackend(Backend backend, const VideoEncoderBackendCapability& pinned,
                                           std::shared_ptr<std::atomic<bool>> cancelled) {
    VideoEncoderPreflightResult result;
    result.backend = BackendName(backend);
    VideoEncoderBackendCapability capability;
    capability.compiled = true;
    capability.reason = LinuxEncoderCapabilityReason::UnknownUnavailable;
    if (Cancelled(cancelled)) {
        capability.reason = LinuxEncoderCapabilityReason::Cancelled;
        return capability;
    }
    if (LinuxHardwareEncoderRetirementFailed()) {
        capability.reason = LinuxEncoderCapabilityReason::RetirementFailed;
        return capability;
    }
    result = FinishProbe(Executor::Run([backend, pinned, cancelled] {
        VideoEncoderPreflightResult checked;
        checked.backend = BackendName(backend);
        const webrtc::SdpVideoFormat format("H264", {
            {"level-asymmetry-allowed", "1"}, {"packetization-mode", "1"},
            {"profile-level-id", "42001f"}});
        if (Cancelled(cancelled)) { checked.status = Status::Cancelled; return checked; }
        if (pinned.device.valid) {
            // A successful process pin is never released implicitly. Revalidate
            // that exact identity; an unplug cannot redirect Auto to another GPU.
            LinuxHardwareEncoderPlan plan;
            plan.backend = backend;
            plan.device = pinned.device;
            const auto environment = webrtc::EnvironmentFactory().Create();
            VideoEncoderPreflightRequest request;
            request.preference = BackendName(backend);
            request.layers.push_back({160, 128, 15, 64000});
            const VideoEncoderPreflightFactory factory = [environment, plan](const auto& offered) {
                return CreateHardwareEncoder(plan, environment, offered);
            };
            checked = CheckVideoEncoderPreflightFormat(request, format, BackendName(backend), factory, cancelled);
            if (checked.ok()) checked.linux_capability.device = pinned.device;
            return checked;
        }
        auto status = webrtc::external_encoder::Status::kUnavailable;
        std::string safe_reason_source;
        switch (backend) {
#if defined(COHAVORA_HAS_LINUX_NVENC)
        case Backend::Nvenc: {
            webrtc::external_encoder::LinuxNvencDeviceSelection selected;
            status = webrtc::ProbeLinuxNvencVideoEncoderAvailability(format, -1, &safe_reason_source, &selected);
            auto& device = checked.linux_capability.device;
            device.valid = selected.cuda_ordinal >= 0 && !selected.pci_bus_id.empty();
            device.cuda_ordinal = selected.cuda_ordinal;
            device.pci_bus_id = std::move(selected.pci_bus_id);
            if (selected.cuda_driver_version) device.cuda_driver_version = selected.cuda_driver_version;
            if (selected.nvenc_api_version) device.nvenc_api_version = selected.nvenc_api_version;
            break;
        }
#endif
#if defined(COHAVORA_HAS_LINUX_QSV)
        case Backend::Qsv: {
            webrtc::external_encoder::LinuxQsvDeviceSelection selected;
            status = webrtc::ProbeLinuxQsvVideoEncoderAvailability(format, -1, &safe_reason_source, &selected);
            auto device = RenderIdentity(selected);
            if (selected.vpl_api_major) {
                device.vpl_api_major = selected.vpl_api_major;
                device.vpl_api_minor = selected.vpl_api_minor;
            }
            if (selected.va_api_major) {
                device.va_api_major = selected.va_api_major;
                device.va_api_minor = selected.va_api_minor;
            }
            checked.linux_capability.device = std::move(device);
            break;
        }
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
        case Backend::Vaapi: {
            webrtc::external_encoder::LinuxVaapiDeviceSelection selected;
            status = webrtc::ProbeLinuxVaapiVideoEncoderAvailability(format, {}, &safe_reason_source, &selected);
            checked.linux_capability.device = RenderIdentity(selected);
            break;
        }
#endif
        default: break;
        }
        if (status == webrtc::external_encoder::Status::kOk) checked.status = Status::Ready;
        else if (status == webrtc::external_encoder::Status::kRetirementFailed)
            checked.status = Status::RetirementFailed;
        else if (status == webrtc::external_encoder::Status::kTimeout) checked.status = Status::TimedOut;
        else if (status == webrtc::external_encoder::Status::kUnsupported) checked.status = Status::UnsupportedFormat;
        checked.linux_capability.reason = ClassifyLinuxEncoderProbeReason(safe_reason_source);
        return checked;
    }, kTotalProbeBudget), backend);
    capability.device = std::move(result.linux_capability.device);
    capability.discovered = result.ok() && capability.device.valid;
    capability.probe_ready = capability.h264 = capability.discovered;
    capability.reason = capability.probe_ready ? LinuxEncoderCapabilityReason::None :
        (result.ok() ? LinuxEncoderCapabilityReason::UnknownUnavailable : CapabilityReason(result.status));
    if (!capability.probe_ready && capability.reason != LinuxEncoderCapabilityReason::TimedOut &&
        capability.reason != LinuxEncoderCapabilityReason::RetirementFailed &&
        capability.reason != LinuxEncoderCapabilityReason::Cancelled &&
        result.linux_capability.reason != LinuxEncoderCapabilityReason::NotCompiled &&
        result.linux_capability.reason != LinuxEncoderCapabilityReason::UnknownUnavailable)
        capability.reason = result.linux_capability.reason;
    if (!capability.probe_ready) capability.device = {};
    if (Cancelled(cancelled) && capability.reason != LinuxEncoderCapabilityReason::RetirementFailed &&
        capability.reason != LinuxEncoderCapabilityReason::TimedOut) {
        capability.h264 = capability.probe_ready = false;
        capability.reason = LinuxEncoderCapabilityReason::Cancelled;
        capability.device = {};
    }
    return capability;
}

} // namespace

LinuxHardwareEncoderPlan ResolveLinuxHardwareEncoderPlan(
        const std::string& preference, const VideoEncoderCapabilities& capabilities) {
    LinuxHardwareEncoderPlan plan;
    plan.software_only = preference == "software";
    plan.generation = capabilities.generation;
    if (plan.software_only || !capabilities.ready) return plan;
    const bool automatic = preference.empty() || preference == "auto";
#if defined(COHAVORA_HAS_LINUX_NVENC)
    if ((automatic || preference == "nvenc") && capabilities.nvenc.probe_ready && capabilities.nvenc.device.valid) {
        plan.backend = Backend::Nvenc;
        plan.device = capabilities.nvenc.device;
        return plan;
    }
#endif
#if defined(COHAVORA_HAS_LINUX_QSV)
    if ((automatic || preference == "qsv") && capabilities.qsv.probe_ready && capabilities.qsv.device.valid) {
        plan.backend = Backend::Qsv;
        plan.device = capabilities.qsv.device;
        return plan;
    }
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
    if ((automatic || preference == "vaapi") && capabilities.vaapi.probe_ready && capabilities.vaapi.device.valid) {
        plan.backend = Backend::Vaapi;
        plan.device = capabilities.vaapi.device;
        return plan;
    }
#endif
    return plan;
}

bool LinuxHardwareEncoderRetirementFailed() {
#if defined(WEBRTC_LINUX_BACKEND_RETIREMENT_REGISTRY)
    if (webrtc::external_encoder::CheckLinuxBackendRetirement() ==
        webrtc::external_encoder::Status::kRetirementFailed) return true;
#else
    if (webrtc::external_encoder::HardwareEncoderProbes().CheckAny() ==
        webrtc::external_encoder::Status::kRetirementFailed) return true;
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
    if (webrtc::external_encoder::CheckLinuxVaapiRetirement() ==
        webrtc::external_encoder::Status::kRetirementFailed) return true;
#endif
    return false;
}

bool IsLinuxHardwareH264FormatSupported(const webrtc::SdpVideoFormat& format) {
    return webrtc::IsBackendH264FormatSupported(format);
}

LinuxHardwareEncoderProbeResults ProbeLinuxHardwareVideoEncoderCapabilities(
        const VideoEncoderCapabilities& pinned, std::shared_ptr<std::atomic<bool>> cancelled) {
    LinuxHardwareEncoderProbeResults result;
#if defined(COHAVORA_HAS_LINUX_NVENC)
    result.nvenc = ProbeBackend(Backend::Nvenc, pinned.nvenc, cancelled);
#endif
#if defined(COHAVORA_HAS_LINUX_QSV)
    result.qsv = ProbeBackend(Backend::Qsv, pinned.qsv, cancelled);
#endif
#if defined(COHAVORA_HAS_LINUX_VAAPI)
    result.vaapi = ProbeBackend(Backend::Vaapi, pinned.vaapi, cancelled);
#endif
    return result;
}

VideoEncoderPreflightResult PreflightLinuxHardwareVideoEncoder(
        const VideoEncoderPreflightRequest& request,
        std::shared_ptr<std::atomic<bool>> cancelled) {
    VideoEncoderPreflightResult result;
    if (Cancelled(cancelled)) {
        result.status = Status::Cancelled;
        return result;
    }
    if (LinuxHardwareEncoderRetirementFailed()) {
        result.status = Status::RetirementFailed;
        return result;
    }
    // Resolve exactly once from the same immutable preference/capability policy
    // used by an owned runtime factory. Failure never tries another vendor.
    const auto plan = ResolveLinuxHardwareEncoderPlan(request.preference, request.linux_capabilities);
    result.backend = BackendName(plan.backend);
    if (plan.backend == Backend::None) return result;
    if (GetVideoEncoderCapabilities().generation != plan.generation) {
        result.status = Status::Cancelled;
        return result;
    }
    result = FinishProbe(Executor::Run([request, cancelled, plan] {
        VideoEncoderPreflightResult checked;
        checked.backend = BackendName(plan.backend);
        const auto environment = webrtc::EnvironmentFactory().Create();
        const VideoEncoderPreflightFactory factory = [environment, plan](const auto& format) {
            return CreateHardwareEncoder(plan, environment, format);
        };
        for (const auto& format : request.formats) {
            if (Cancelled(cancelled)) {
                checked.status = Status::Cancelled;
                break;
            }
            if (!IsLinuxHardwareH264FormatSupported(format)) continue;
            checked = CheckVideoEncoderPreflightFormat(
                request, format, BackendName(plan.backend), factory, cancelled);
            if (checked.ok() || checked.status == Status::Cancelled ||
                checked.status == Status::RetirementFailed) break;
        }
        return checked;
    }, kTotalProbeBudget), plan.backend);
    if (result.status != Status::RetirementFailed && result.status != Status::TimedOut &&
        (Cancelled(cancelled) || GetVideoEncoderCapabilities().generation != plan.generation)) {
        result.status = Status::Cancelled;
        result.formats.clear();
    }
    if (result.ok()) {
        result.linux_capability = plan.backend == Backend::Nvenc ? request.linux_capabilities.nvenc :
            (plan.backend == Backend::Qsv ? request.linux_capabilities.qsv : request.linux_capabilities.vaapi);
    }
    return result;
}

std::unique_ptr<webrtc::VideoEncoder> CreateLinuxH264VideoEncoder(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format,
        const LinuxHardwareEncoderPlan& plan) {
    if (!plan.software_only && LinuxHardwareEncoderRetirementFailed()) return nullptr;
    if (!plan.software_only && plan.generation != GetVideoEncoderCapabilities().generation) return nullptr;
    auto software = webrtc::CreateH264Encoder(env, webrtc::H264EncoderSettings::Parse(format));
    if (plan.software_only || !software) return software;
    software = std::make_unique<LinuxSoftwareFallbackGuard>(std::move(software));
    if (!IsLinuxHardwareH264FormatSupported(format) ||
        plan.backend == Backend::None) return software;
    auto hardware = CreateHardwareEncoder(plan, env, format);
    if (LinuxHardwareEncoderRetirementFailed()) return nullptr;
    if (!hardware) {
        InvalidateVideoEncoderCapabilities(LinuxEncoderCapabilityReason::UnknownUnavailable);
        return software;
    }
    hardware = std::make_unique<LinuxHardwareRuntimeGuard>(std::move(hardware), plan.generation);
    // The SDK wrapper confirms Release before making software active and
    // treats unconfirmed retirement as sticky rather than ordinary loss.
    return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software), std::move(hardware), false);
}

} // namespace livekit::detail
