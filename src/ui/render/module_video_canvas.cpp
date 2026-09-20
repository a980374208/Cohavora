#include "module_video_canvas.h"
#include "render/api/render_backend_dx11_native.h"
#include "render/api/render_backend_module_info.h"
#include <QtGui/QWindow>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtCore/QDebug>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace livekit::render {
namespace {
int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr size_t kMaxResources = 128, kMaxCommands = 65536;
constexpr int64_t kPresentTimeoutMs = 1000, kStartupTimeoutMs = 5000;
std::atomic<unsigned> activeWorkers{0};
std::atomic<bool> presentationQuarantined{false};
RenderGpuFailure Failure(lk_render_result result, bool creation = false) {
    switch (result) {
    case LK_RENDER_DEVICE_LOST: return RenderGpuFailure::DeviceLost;
    case LK_RENDER_SURFACE_LOST: return RenderGpuFailure::SurfaceLost;
    case LK_RENDER_OUT_OF_MEMORY: return RenderGpuFailure::OutOfMemory;
    default: return creation ? RenderGpuFailure::DeviceCreateFailed : RenderGpuFailure::ModuleDeviceFailed;
    }
}
uint32_t ModuleVersion(BackendDevice& device) {
    lk_render_module_info_v1 info{};
    return device.QueryExtension(LK_RENDER_EXT_MODULE_INFO, LK_RENDER_MODULE_INFO_V1,
        sizeof(info), &info) == LK_RENDER_OK && info.struct_size == sizeof(info) &&
        info.abi_version == LK_RENDER_ABI_V1 && !info.reserved ? info.module_version : 0;
}
QString Dx11DriverDescription(BackendDevice& device) {
#if defined(Q_OS_WIN)
    lk_render_dx11_native_v1 native{};
    if (device.QueryExtension(LK_RENDER_EXT_DX11_NATIVE, LK_RENDER_DX11_NATIVE_V1,
            sizeof(native), &native) != LK_RENDER_OK) return QStringLiteral("DX11");
    auto* d3d = reinterpret_cast<ID3D11Device*>(native.device);
    if (!d3d) return QStringLiteral("DX11");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(adapter.GetAddressOf())) || FAILED(adapter->GetDesc(&description)))
        return QStringLiteral("DX11");
    return QStringLiteral("DX11 / ") + QString::fromWCharArray(description.Description);
#else
    return QStringLiteral("DX11");
#endif
}
}

struct ModuleVideoCanvas::Scene {
    uint64_t sequence = 0;
    lk_render_frame_target target{};
    std::vector<Resource> resources;
    std::vector<lk_render_draw_command> commands;
};
struct ModuleVideoCanvas::Worker {
    std::mutex mutex;
    std::condition_variable wake;
    // Complete immutable snapshots: dropping a scene cannot drop a removal.
    // One pending and one executing scene; never hold this mutex in the driver.
    std::shared_ptr<const Scene> pending;
    std::function<void(BackendDevice&)> testTask;
    QString driver;
    uint32_t moduleVersion = 0;
    std::atomic<RenderGpuFailure> failure{RenderGpuFailure::None};
    std::atomic<bool> stop{false}, exposed{false};
    std::atomic<int64_t> busySince{0};
    std::atomic<uint64_t> presentedScene{0};
    const int64_t started = Now();
    bool finished = false; // UI only; completion never references a canvas.

    void run(uintptr_t hwnd, uint64_t generation, const std::shared_ptr<BackendModule>& module) {
        // Every native call, including device creation, queries and destruction,
        // stays on this owner. Only a borrowed, pinned HWND crosses threads.
        std::unique_ptr<BackendDevice> device;
        std::map<uint64_t, Resource> uploaded;
        try {
            busySince.store(Now());
            const lk_render_win32_binding binding{sizeof(binding), 0, hwnd};
            const lk_render_create_info info{sizeof(info), LK_RENDER_SURFACE_WIN32, &binding,
                sizeof(binding), uint32_t(kMaxResources), generation};
            lk_render_result result;
            device = module->Create(info, result);
            if (!device) {
                failure.store(Failure(result, true));
            } else {
                const auto description = Dx11DriverDescription(*device);
                const auto version = ModuleVersion(*device);
                { std::lock_guard lock(mutex); driver = description; moduleVersion = version; }
                busySince.store(0);
                while (!stop.load()) {
                    std::shared_ptr<const Scene> scene;
                    std::function<void(BackendDevice&)> task;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait(lock, [&] { return stop.load() || bool(pending); });
                        if (stop.load()) break;
                        scene = std::exchange(pending, {});
                        task = std::exchange(testTask, {});
                    }
                    if (!exposed.load()) continue;
                    if (scene->target.surface_generation != generation) continue;
                    busySince.store(Now());
                    result = device->Resize(scene->target);
                    if (result == LK_RENDER_NOT_READY) { busySince.store(0); continue; }
                    if (result != LK_RENDER_OK) { failure.store(Failure(result)); break; }
                    std::set<uint64_t> live;
                    for (const auto& resource : scene->resources) live.insert(resource.id.value);
                    for (auto it = uploaded.begin(); it != uploaded.end();) {
                        if (!live.count(it->first)) { device->Remove(it->second.id); it = uploaded.erase(it); }
                        else ++it;
                    }
                    for (const auto& resource : scene->resources) {
                        if (stop.load()) break;
                        const auto it = uploaded.find(resource.id.value);
                        if (it != uploaded.end() && it->second.frame == resource.frame &&
                            it->second.image.cacheKey() == resource.image.cacheKey()) continue;
                        if (resource.frame) result = device->Upload(resource.id, resource.frame->view());
                        else {
                            const auto& image = resource.image;
                            lk_render_frame_view view{};
                            view.struct_size = sizeof(view); view.format = LK_RENDER_RGBA8;
                            view.width = image.width(); view.height = image.height(); view.plane_count = 1;
                            view.alpha_mode = LK_RENDER_ALPHA_PREMULTIPLIED;
                            view.planes[0] = {image.constBits(), uint64_t(image.sizeInBytes()),
                                uint32_t(image.bytesPerLine()), view.width, view.height, 0};
                            result = device->Upload(resource.id, view, true);
                        }
                        if (result != LK_RENDER_OK) break;
                        uploaded[resource.id.value] = resource;
                    }
                    if (stop.load()) break;
                    if (result == LK_RENDER_OK) {
                        const lk_render_scene_view view{sizeof(view), uint32_t(scene->commands.size()),
                            scene->commands.data(), {0.0706f, 0.0784f, 0.1020f, 1}};
                        result = device->Render(scene->target, view); // Includes Present.
                    }
                    // Publish failure BEFORE destruction: driver cleanup can hang too.
                    if (result != LK_RENDER_OK) { failure.store(Failure(result)); break; }
                    if (stop.load()) break;
                    if (task) task(*device);
                    if (stop.load()) break;
                    presentedScene.store(scene->sequence);
                    busySince.store(0);
                }
            }
        } catch (...) {
            failure.store(RenderGpuFailure::RenderOwnerException);
        }
        busySince.store(Now());
        uploaded.clear();
        device.reset(); // Owner only; surface/module stay pinned if this blocks.
    }
};

class ModuleVideoCanvas::Surface final : public QWindow {
public:
    explicit Surface(ModuleVideoCanvas* host) : host_(host) { setSurfaceType(QSurface::RasterSurface); }
    void retire() {
        host_ = nullptr;
        hide();
        // The canvas HWND is about to go away. Preserve this separate HWND and
        // detach its Qt/native parent before destroying any ancestor QWidget.
        setParent(nullptr);
        QObject::setParent(nullptr);
    }
protected:
    void exposeEvent(QExposeEvent*) override { if (host_) host_->requestRender(); }
    void resizeEvent(QResizeEvent*) override {
        if (host_) QTimer::singleShot(0, host_, [host = host_] { host->requestRender(); });
    }
    bool event(QEvent* e) override {
        if (host_) switch (e->type()) {
        case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: case QEvent::Leave:
            QCoreApplication::sendEvent(host_, e); setCursor(host_->cursor()); return true;
        default: break;
        }
        return QWindow::event(e);
    }
private:
    ModuleVideoCanvas* host_;
};

ModuleVideoCanvas::ModuleVideoCanvas(std::shared_ptr<BackendModule> module, QWidget* parent)
    : VideoCanvas(parent), module_(std::move(module)) {
    initializeModuleDiagnostics(module_->info().backend_kind == LK_RENDER_BACKEND_DX11
        ? RenderBackend::Dx11 : RenderBackend::OpenGL, module_->abiVersion());
    if (module_->info().surface_kind == LK_RENDER_SURFACE_WIN32) {
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        surface_ = new Surface(this);
    }
    static std::atomic<uint64_t> generation{1};
    target_.struct_size = sizeof(target_);
    target_.surface_generation = generation.fetch_add(1);
    watchdog_ = new QTimer(this);
    watchdog_->setInterval(25);
    connect(watchdog_, &QTimer::timeout, this, &ModuleVideoCanvas::pollWorker);
    connect(this, &VideoCanvas::rendererUnavailable, this, [this] { releaseRenderer(); });
}
ModuleVideoCanvas::~ModuleVideoCanvas() { stopRendering(); releaseRenderer(); delete surface_; }
QString ModuleVideoCanvas::backendName() const { return QStringLiteral("DX11"); }
QWindow* ModuleVideoCanvas::windowSurface() const { return surface_; }
void ModuleVideoCanvas::startWorker() {
    if (worker_ || failed_ || !surface_ || !module_) return;
    // Cap owners even when teardown hangs, and do not create more devices after
    // a timeout. A stranded owner is never terminated or joined from the UI.
    if (presentationQuarantined.load() || activeWorkers.load() >= 4) {
        fail(RenderGpuFailure::PresentationTimeout); return;
    }
    surface_->create();
    const auto hwnd = uintptr_t(surface_->winId());
    worker_ = std::make_shared<Worker>();
    auto state = worker_;
    auto* surface = surface_;
    auto* owner = QThread::create([state, hwnd, generation = target_.surface_generation, module = module_] {
        state->run(hwnd, generation, module);
    });
    activeWorkers.fetch_add(1);
    connect(owner, &QThread::finished, qApp, [owner, surface, state] {
        state->finished = true;
        activeWorkers.fetch_sub(1);
        if (state->stop.load()) surface->deleteLater();
        owner->deleteLater();
    });
    owner->start();
    watchdog_->start();
}
void ModuleVideoCanvas::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (failed_ || !surface_) return;
    winId();
    surface_->setParent(windowHandle());
    surface_->setGeometry(rect()); surface_->show();
    requestRender();
}
void ModuleVideoCanvas::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (worker_) worker_->exposed.store(false);
    if (surface_) surface_->hide();
}
void ModuleVideoCanvas::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (!failed_ && surface_) surface_->setGeometry(rect());
    requestRender();
}
void ModuleVideoCanvas::paintEvent(QPaintEvent*) { requestRender(); }
void ModuleVideoCanvas::requestRender() {
    if (failed_ || !surface_ || !isVisible()) return;
    startWorker();
    if (worker_) { worker_->exposed.store(true); render(); }
}
void ModuleVideoCanvas::pollWorker() {
    if (failed_ || !worker_) return;
    const auto failure = worker_->failure.load();
    if (failure != RenderGpuFailure::None) { fail(failure); return; }
    const auto busy = worker_->busySince.load();
    if (busy && Now() - busy > (initialized_ ? kPresentTimeoutMs : kStartupTimeoutMs)) {
        presentationQuarantined.store(true);
        fail(initialized_ ? RenderGpuFailure::PresentationTimeout : RenderGpuFailure::RendererStartupTimeout);
        return;
    }
    if (!initialized_ && Now() - worker_->started > kStartupTimeoutMs) {
        presentationQuarantined.store(true);
        fail(RenderGpuFailure::RendererStartupTimeout); return;
    }
    if (!initialized_ && worker_->presentedScene.load()) {
        QString driver;
        uint32_t version;
        { std::lock_guard lock(worker_->mutex); driver = worker_->driver; version = worker_->moduleVersion; }
        initialized_ = true;
        markRendererActive(std::move(driver), version);
        emit rendererInitialized();
    }
}
void ModuleVideoCanvas::fail(RenderGpuFailure failure) {
    if (failed_) return;
    markRendererFailure(failure, !initialized_ ? RenderFallbackReason::GpuInitializationFailed :
        failure == RenderGpuFailure::DeviceLost ? RenderFallbackReason::GpuDeviceLost :
                                                 RenderFallbackReason::GpuRuntimeFailed);
    qWarning().noquote() << "DX11 fallback:" << RenderDiagnosticsSafeSummary(renderDiagnostics());
    releaseRenderer();
    notifyRendererUnavailable();
}
void ModuleVideoCanvas::releaseRenderer() {
    failed_ = true; initialized_ = false;
    watchdog_->stop();
    if (worker_) {
        worker_->stop.store(true);
        std::shared_ptr<const Scene> pending;
        { std::lock_guard lock(worker_->mutex); pending = std::exchange(worker_->pending, {}); }
        worker_->wake.notify_one();
        surface_->retire();
        if (worker_->finished) surface_->deleteLater();
        surface_ = nullptr;
        worker_.reset();
    }
    clearBackend();
    module_.reset();
}
lk_render_resource_id ModuleVideoCanvas::NewId() { return {next_resource_++, target_.surface_generation}; }
void ModuleVideoCanvas::submitFrame(const std::string& key, VideoRenderFrame::Ptr frame) {
    if (failed_) return;
    if (!videos_.count(key) && videos_.size() + overlays_.size() >= kMaxResources) {
        fail(RenderGpuFailure::ResourceLimit); return;
    }
    auto& video = videos_[key];
    if (!video.id.value) video.id = NewId();
    video.frame = std::move(frame);
}
void ModuleVideoCanvas::removeFrame(const std::string& key) { videos_.erase(key); }
void ModuleVideoCanvas::removeDecorationTexture(const std::string& key) { overlays_.erase(key); }
void ModuleVideoCanvas::clearBackend() { videos_.clear(); overlays_.clear(); building_.reset(); }
VideoFrameGeometry ModuleVideoCanvas::frameGeometry(const std::string& key) const {
    const auto it = videos_.find(key);
    if (it == videos_.end() || !it->second.frame) return {};
    const auto& frame = it->second.frame->view();
    return {int(frame.width), int(frame.height), frame.rotation_degrees, true};
}
bool ModuleVideoCanvas::beginFrame(QSize& pixels) {
    if (failed_) return false;
    if (!worker_ || !surface_) return true;
#if defined(Q_OS_WIN)
    RECT rect{};
    if (!GetClientRect(reinterpret_cast<HWND>(surface_->winId()), &rect)) {
        fail(RenderGpuFailure::SurfaceLost); return false;
    }
    pixels = QSize(rect.right - rect.left, rect.bottom - rect.top);
#endif
    if (pixels.isEmpty()) return true;
    building_ = std::make_shared<Scene>();
    building_->sequence = ++submitted_;
    building_->target = target_;
    building_->target.pixel_width = pixels.width(); building_->target.pixel_height = pixels.height();
    for (const auto& [_, resource] : videos_) building_->resources.push_back(resource);
    return true;
}
void ModuleVideoCanvas::AddCommand(const VideoTileRect& tile, lk_render_resource_id id, float r, float g, float b) {
    if (failed_ || !building_) return;
    if (building_->commands.size() >= kMaxCommands) { fail(RenderGpuFailure::ResourceLimit); return; }
    building_->commands.push_back({id, {tile.x, tile.y, uint32_t(tile.width), uint32_t(tile.height)},
        {0, 0, building_->target.pixel_width, building_->target.pixel_height}, {r, g, b, 1}});
}
void ModuleVideoCanvas::drawSolid(const VideoTileRect& tile, float r, float g, float b) { AddCommand(tile, {}, r, g, b); }
void ModuleVideoCanvas::drawVideo(const VideoTileRect& tile) {
    if (!failed_) AddCommand(tile, videos_.at(tile.identity).id);
}
bool ModuleVideoCanvas::drawDecoration(const VideoTileRect& tile, const QImage& input) {
    if (failed_ || !building_) return false;
    if (!overlays_.count(tile.identity) && videos_.size() + overlays_.size() >= kMaxResources) {
        fail(RenderGpuFailure::ResourceLimit); return false;
    }
    auto& overlay = overlays_[tile.identity];
    if (!overlay.id.value) overlay.id = NewId();
    overlay.image = input.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    building_->resources.push_back(overlay);
    AddCommand(tile, overlay.id);
    return !failed_;
}
bool ModuleVideoCanvas::endFrame() {
    if (failed_ || !worker_ || !building_) return false;
    std::shared_ptr<const Scene> previous;
    { std::lock_guard lock(worker_->mutex); previous = std::exchange(worker_->pending, std::exchange(building_, {})); }
    worker_->wake.notify_one();
    return true;
}
void ModuleVideoCanvas::runOnOwnerForTest(std::function<void(BackendDevice&)> task) {
    Q_ASSERT(worker_ && !failed_);
    { std::lock_guard lock(worker_->mutex); Q_ASSERT(!worker_->testTask); worker_->testTask = std::move(task); }
    requestRender();
}
bool ModuleVideoCanvas::scenePresentedForTest() const { return worker_ && worker_->presentedScene.load() >= submitted_; }
bool ModuleVideoCanvas::workersIdleForTest() { return activeWorkers.load() == 0; }
} // namespace livekit::render
