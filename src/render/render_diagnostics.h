#pragma once

#include <QtCore/QString>
#include <cstdint>

namespace livekit::render {

enum class RenderBackend {
    None,
    QtCpu,
    Dx11,
    OpenGL,
};

enum class RenderGpuFailure {
    None,
    InvalidBackendSelection,
    RemoteSessionPolicy,
    InvalidModulePath,
    ModuleOpenFailed,
    ModuleEntryMissing,
    ModuleIdentityMismatch,
    ModuleAbiMismatch,
    ModuleCapabilitiesMismatch,
    ModuleSelectionLocked,
    ModuleLoadException,
    ThreadedGlUnavailable,
    ContextCreateFailed,
    DeviceCreateFailed,
    GraphicsReset,
    MakeCurrentFailed,
    SurfaceLost,
    DeviceLost,
    OutOfMemory,
    ResourceLimit,
    PresentationTimeout,
    RendererStartupTimeout,
    ModuleDeviceFailed,
    RenderOwnerException,
    UnknownRendererFailure,
};

enum class RenderFallbackReason {
    None,
    UserSelectedCpu,
    RemoteSessionPolicy,
    InvalidConfiguration,
    ModuleLoadFailed,
    GpuInitializationFailed,
    GpuDeviceLost,
    GpuRuntimeFailed,
};

struct RenderDiagnostics {
    RenderBackend requested_backend = RenderBackend::None;
    RenderBackend actual_backend = RenderBackend::QtCpu;
    uint32_t abi_version = 0;
    uint32_t module_version = 0;
    QString driver_description;
    RenderGpuFailure gpu_failure = RenderGpuFailure::None;
    RenderFallbackReason fallback_reason = RenderFallbackReason::None;

    bool operator==(const RenderDiagnostics&) const = default;
};

inline QString RenderBackendName(RenderBackend value) {
    switch (value) {
    case RenderBackend::None: return QStringLiteral("none");
    case RenderBackend::QtCpu: return QStringLiteral("qt-cpu");
    case RenderBackend::Dx11: return QStringLiteral("dx11");
    case RenderBackend::OpenGL: return QStringLiteral("opengl");
    }
    return QStringLiteral("unknown");
}

inline QString RenderGpuFailureName(RenderGpuFailure value) {
    switch (value) {
    case RenderGpuFailure::None: return QStringLiteral("none");
    case RenderGpuFailure::InvalidBackendSelection: return QStringLiteral("invalid-backend-selection");
    case RenderGpuFailure::RemoteSessionPolicy: return QStringLiteral("remote-session-policy");
    case RenderGpuFailure::InvalidModulePath: return QStringLiteral("invalid-module-path");
    case RenderGpuFailure::ModuleOpenFailed: return QStringLiteral("module-open-failed");
    case RenderGpuFailure::ModuleEntryMissing: return QStringLiteral("module-entry-missing");
    case RenderGpuFailure::ModuleIdentityMismatch: return QStringLiteral("module-identity-mismatch");
    case RenderGpuFailure::ModuleAbiMismatch: return QStringLiteral("module-abi-mismatch");
    case RenderGpuFailure::ModuleCapabilitiesMismatch: return QStringLiteral("module-capabilities-mismatch");
    case RenderGpuFailure::ModuleSelectionLocked: return QStringLiteral("module-selection-locked");
    case RenderGpuFailure::ModuleLoadException: return QStringLiteral("module-load-exception");
    case RenderGpuFailure::ThreadedGlUnavailable: return QStringLiteral("threaded-gl-unavailable");
    case RenderGpuFailure::ContextCreateFailed: return QStringLiteral("context-create-failed");
    case RenderGpuFailure::DeviceCreateFailed: return QStringLiteral("device-create-failed");
    case RenderGpuFailure::GraphicsReset: return QStringLiteral("graphics-reset");
    case RenderGpuFailure::MakeCurrentFailed: return QStringLiteral("make-current-failed");
    case RenderGpuFailure::SurfaceLost: return QStringLiteral("surface-lost");
    case RenderGpuFailure::DeviceLost: return QStringLiteral("device-lost");
    case RenderGpuFailure::OutOfMemory: return QStringLiteral("out-of-memory");
    case RenderGpuFailure::ResourceLimit: return QStringLiteral("resource-limit");
    case RenderGpuFailure::PresentationTimeout: return QStringLiteral("presentation-timeout");
    case RenderGpuFailure::RendererStartupTimeout: return QStringLiteral("renderer-startup-timeout");
    case RenderGpuFailure::ModuleDeviceFailed: return QStringLiteral("module-device-failed");
    case RenderGpuFailure::RenderOwnerException: return QStringLiteral("render-owner-exception");
    case RenderGpuFailure::UnknownRendererFailure: return QStringLiteral("unknown-renderer-failure");
    }
    return QStringLiteral("unknown-renderer-failure");
}

inline QString RenderFallbackReasonName(RenderFallbackReason value) {
    switch (value) {
    case RenderFallbackReason::None: return QStringLiteral("none");
    case RenderFallbackReason::UserSelectedCpu: return QStringLiteral("user-selected-cpu");
    case RenderFallbackReason::RemoteSessionPolicy: return QStringLiteral("remote-session-policy");
    case RenderFallbackReason::InvalidConfiguration: return QStringLiteral("invalid-configuration");
    case RenderFallbackReason::ModuleLoadFailed: return QStringLiteral("module-load-failed");
    case RenderFallbackReason::GpuInitializationFailed: return QStringLiteral("gpu-initialization-failed");
    case RenderFallbackReason::GpuDeviceLost: return QStringLiteral("gpu-device-lost");
    case RenderFallbackReason::GpuRuntimeFailed: return QStringLiteral("gpu-runtime-failed");
    }
    return QStringLiteral("gpu-runtime-failed");
}

inline QString SanitizeRenderDriverDescription(QString value) {
    value = value.left(256);
    for (auto& character : value) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) character = QLatin1Char(' ');
    }
    return value.simplified();
}

inline QString RenderDiagnosticsSafeSummary(const RenderDiagnostics& value) {
    return QStringLiteral("requested=%1 actual=%2 abi=%3 module=%4 failure=%5 fallback=%6")
        .arg(RenderBackendName(value.requested_backend), RenderBackendName(value.actual_backend))
        .arg(value.abi_version)
        .arg(value.module_version)
        .arg(RenderGpuFailureName(value.gpu_failure), RenderFallbackReasonName(value.fallback_reason));
}

} // namespace livekit::render
