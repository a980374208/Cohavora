#include "module_video_canvas.h"
#include "gl_video_canvas.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace livekit::render {
namespace {
RenderBackend BackendName(uint32_t backend) {
    return backend == LK_RENDER_BACKEND_DX11 ? RenderBackend::Dx11 : RenderBackend::OpenGL;
}
RenderGpuFailure LoadFailure(ModuleLoadError error) {
    switch (error) {
    case ModuleLoadError::None: return RenderGpuFailure::None;
    case ModuleLoadError::InvalidPath: return RenderGpuFailure::InvalidModulePath;
    case ModuleLoadError::OpenFailed: return RenderGpuFailure::ModuleOpenFailed;
    case ModuleLoadError::EntryMissing: return RenderGpuFailure::ModuleEntryMissing;
    case ModuleLoadError::IdentityMismatch: return RenderGpuFailure::ModuleIdentityMismatch;
    case ModuleLoadError::InvalidAbi: return RenderGpuFailure::ModuleAbiMismatch;
    case ModuleLoadError::InvalidCapabilities: return RenderGpuFailure::ModuleCapabilitiesMismatch;
    case ModuleLoadError::SelectionLocked: return RenderGpuFailure::ModuleSelectionLocked;
    }
    return RenderGpuFailure::ModuleLoadException;
}
void PublishInitial(RenderDiagnostics* output, const RenderDiagnostics& diagnostics) {
    if (output) *output = diagnostics;
}
void LogFallback(const RenderDiagnostics& diagnostics) {
    qWarning().noquote() << "Render fallback:" << RenderDiagnosticsSafeSummary(diagnostics);
}
}
VideoCanvas* CreateVideoCanvasFromDirectory(QWidget* parent, const QString& directory,
        RenderDiagnostics* initialDiagnostics) {
    auto backend = BackendModule::DefaultBackend();
    const auto selection = (qEnvironmentVariableIsSet("COHAVORA_RENDER_BACKEND")
        ? qgetenv("COHAVORA_RENDER_BACKEND")
        : qgetenv("LIVEKIT_RENDER_BACKEND")).trimmed().toLower();
    RenderDiagnostics diagnostics;
    if (selection == "cpu") {
        diagnostics.requested_backend = RenderBackend::QtCpu;
        diagnostics.fallback_reason = RenderFallbackReason::UserSelectedCpu;
        PublishInitial(initialDiagnostics, diagnostics);
        return nullptr;
    }
    if (selection == "opengl") backend = LK_RENDER_BACKEND_OPENGL;
    else if (selection == "dx11") backend = LK_RENDER_BACKEND_DX11;
    else if (!selection.isEmpty()) {
        diagnostics.gpu_failure = RenderGpuFailure::InvalidBackendSelection;
        diagnostics.fallback_reason = RenderFallbackReason::InvalidConfiguration;
        PublishInitial(initialDiagnostics, diagnostics);
        LogFallback(diagnostics);
        return nullptr;
    }
    diagnostics.requested_backend = BackendName(backend);
#if defined(Q_OS_WIN)
    if (backend == LK_RENDER_BACKEND_DX11 && GetSystemMetrics(SM_REMOTESESSION)) {
        diagnostics.gpu_failure = RenderGpuFailure::RemoteSessionPolicy;
        diagnostics.fallback_reason = RenderFallbackReason::RemoteSessionPolicy;
        PublishInitial(initialDiagnostics, diagnostics);
        LogFallback(diagnostics);
        return nullptr;
    }
    const std::filesystem::path applicationDirectory(directory.toStdWString());
#else
    const auto utf8 = directory.toUtf8();
    const auto applicationDirectory = std::filesystem::u8path(utf8.constData(), utf8.constData() + utf8.size());
#endif
    try {
        ModuleLoadError error;
        auto module = BackendModule::LoadFromDirectory(applicationDirectory, backend, error);
        if (!module) {
            diagnostics.gpu_failure = LoadFailure(error);
            diagnostics.fallback_reason = RenderFallbackReason::ModuleLoadFailed;
            PublishInitial(initialDiagnostics, diagnostics);
            LogFallback(diagnostics);
            return nullptr;
        }
        diagnostics.abi_version = module->abiVersion();
        VideoCanvas* canvas = nullptr;
#if defined(Q_OS_WIN)
        if (module->info().surface_kind == LK_RENDER_SURFACE_WIN32)
            canvas = new ModuleVideoCanvas(std::move(module), parent);
#endif
        if (!canvas && module->info().surface_kind == LK_RENDER_SURFACE_HOST_GL)
            canvas = new GlVideoCanvas(std::move(module), parent);
        if (!canvas) {
            diagnostics.gpu_failure = RenderGpuFailure::ModuleCapabilitiesMismatch;
            diagnostics.fallback_reason = RenderFallbackReason::ModuleLoadFailed;
            PublishInitial(initialDiagnostics, diagnostics);
            LogFallback(diagnostics);
            return nullptr;
        }
        PublishInitial(initialDiagnostics, canvas->renderDiagnostics());
        return canvas;
    } catch (...) {
        diagnostics.gpu_failure = RenderGpuFailure::ModuleLoadException;
        diagnostics.fallback_reason = RenderFallbackReason::ModuleLoadFailed;
        PublishInitial(initialDiagnostics, diagnostics);
        LogFallback(diagnostics);
        return nullptr;
    }
}
VideoCanvas* CreateVideoCanvas(QWidget* parent, RenderDiagnostics* initialDiagnostics) {
    return CreateVideoCanvasFromDirectory(parent, QCoreApplication::applicationDirPath(), initialDiagnostics);
}
}
