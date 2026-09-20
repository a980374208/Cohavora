#include "gl_video_canvas.h"
#include "render/api/render_backend_module_info.h"
#include <QtGui/QWindow>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QMouseEvent>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtCore/QDebug>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>

namespace livekit::render {
namespace {
using Clock = std::chrono::steady_clock;
int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
RenderGpuFailure FailureFromCode(const QString& value, lk_render_result result) {
    if (result == LK_RENDER_DEVICE_LOST) return RenderGpuFailure::DeviceLost;
    if (result == LK_RENDER_SURFACE_LOST) return RenderGpuFailure::SurfaceLost;
    if (result == LK_RENDER_OUT_OF_MEMORY) return RenderGpuFailure::OutOfMemory;
    if (value == QStringLiteral("context-create-failed")) return RenderGpuFailure::ContextCreateFailed;
    if (value == QStringLiteral("graphics-reset")) return RenderGpuFailure::GraphicsReset;
    if (value == QStringLiteral("make-current-failed")) return RenderGpuFailure::MakeCurrentFailed;
    if (value == QStringLiteral("device-create-failed")) return RenderGpuFailure::DeviceCreateFailed;
    if (value == QStringLiteral("resize-failed")) return RenderGpuFailure::SurfaceLost;
    if (value == QStringLiteral("render-owner-exception")) return RenderGpuFailure::RenderOwnerException;
    return value.isEmpty() ? RenderGpuFailure::None : RenderGpuFailure::ModuleDeviceFailed;
}
std::atomic<unsigned> activeWorkers{0};
std::atomic<bool> presentationQuarantined{false};
struct OwnerActivity {
    std::atomic<int64_t> busySince{0};
    std::atomic<bool> retired{false};
};
std::mutex workerActivityMutex;
std::vector<std::weak_ptr<OwnerActivity>> workerActivities;
constexpr int64_t kPresentTimeoutMs = 2000, kStartupTimeoutMs = 5000;
constexpr size_t kMaxResources = 128;
void TrackWorkerActivity(const std::shared_ptr<OwnerActivity>& activity) {
    std::lock_guard lock(workerActivityMutex);
    workerActivities.emplace_back(activity);
}
bool RetiredOwnerTimedOut(int64_t now) {
    std::lock_guard lock(workerActivityMutex);
    bool timedOut = false;
    for (auto it = workerActivities.begin(); it != workerActivities.end();) {
        if (const auto activity = it->lock()) {
            const auto busy = activity->busySince.load(std::memory_order_acquire);
            timedOut = timedOut || (activity->retired.load(std::memory_order_acquire) &&
                busy && now - busy > kPresentTimeoutMs);
            ++it;
        } else {
            it = workerActivities.erase(it);
        }
    }
    return timedOut;
}
}
struct GlVideoCanvas::Scene {
    uint64_t sequence = 0;
    lk_render_frame_target target{};
    std::vector<Resource> resources;
    std::vector<lk_render_draw_command> commands;
};
struct GlVideoCanvas::Worker {
    std::mutex mutex;
    std::condition_variable wake;
    // Complete snapshots, not deltas: replacement cannot lose removals/uploads.
    // At most one pending and one executing scene; no lock encloses driver work.
    std::shared_ptr<const Scene> pending;
    std::function<void()> beforePresent;
    QString driver, failure;
    uint32_t moduleVersion = 0;
    lk_render_result failureResult = LK_RENDER_OK;
    std::atomic<bool> stop{false}, exposed{false};
    bool finished = false; // GUI thread only
    // Kept separately so a successor waiting for the serialized ANGLE slot can
    // still distinguish a slow retirement from a driver owner that timed out.
    std::shared_ptr<OwnerActivity> activity = std::make_shared<OwnerActivity>();
    std::atomic<uint64_t> presented{0};
    std::atomic<uint64_t> presentedScene{0};
    int64_t started = Now();
    void failed(const char* reason, lk_render_result result = LK_RENDER_OK) {
        std::lock_guard<std::mutex> lock(mutex);
        failure = QString::fromLatin1(reason);
        failureResult = result;
    }
    static lk_render_gl_proc LK_RENDER_CALL Proc(void* user, const char* name) {
        return reinterpret_cast<lk_render_gl_proc>(static_cast<QOpenGLContext*>(user)->getProcAddress(name));
    }
    void run(QWindow* surface, QSurfaceFormat format, std::shared_ptr<BackendModule> module) {
        // The native surface exists before this thread starts. Only GL surface
        // operations run here; all QWidget/QWindow mutations stay on the UI.
        QOpenGLContext context;
        context.setFormat(format);
        activity->busySince.store(Now());
        if (!context.create() || !context.makeCurrent(surface)) { failed("context-create-failed"); return; }
        static std::atomic<uint64_t> generation{1};
        const auto contextGeneration = generation.fetch_add(1);
        auto* gl = context.extraFunctions();
        gl->initializeOpenGLFunctions();
        {
            const auto version = reinterpret_cast<const char*>(gl->glGetString(GL_VERSION));
            const auto renderer = reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER));
            std::lock_guard<std::mutex> lock(mutex);
            driver = QString::fromLatin1(version ? version : "unknown") + " / " + QString::fromLatin1(renderer ? renderer : "unknown");
        }
        using ResetStatus = unsigned (LK_RENDER_GL_CALL*)();
        ResetStatus resetStatus = nullptr;
        if (context.hasExtension("GL_KHR_robustness")) {
            resetStatus = reinterpret_cast<ResetStatus>(context.getProcAddress(context.isOpenGLES() ? "glGetGraphicsResetStatusKHR" : "glGetGraphicsResetStatus"));
        } else if (context.hasExtension("GL_ARB_robustness")) {
            resetStatus = reinterpret_cast<ResetStatus>(context.getProcAddress("glGetGraphicsResetStatusARB"));
        } else if (context.hasExtension("GL_EXT_robustness")) {
            resetStatus = reinterpret_cast<ResetStatus>(context.getProcAddress("glGetGraphicsResetStatusEXT"));
        }
        std::unique_ptr<BackendDevice> device;
        std::map<uint64_t, Resource> uploaded;
        std::shared_ptr<const Scene> scene;
        bool lost = false;
        bool moduleInfoQueried = false;
        const auto healthy = [&] {
            if (!context.isValid() || (resetStatus && resetStatus() != GL_NO_ERROR)) {
                lost = true; failed("graphics-reset"); return false;
            }
            return true;
        };
        activity->busySince.store(0);
        while (!stop.load()) {
            std::function<void()> hook;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return stop.load() || bool(pending); });
                if (stop.load()) break;
                if (pending) scene = std::exchange(pending, {});
                hook = beforePresent;
            }
            activity->busySince.store(Now());
            // Poll even with a static video; never present an unexposed window.
            if (!healthy()) break;
            if (!scene || !exposed.load()) { activity->busySince.store(0); continue; }
            if (!context.makeCurrent(surface)) { lost = true; failed("make-current-failed"); break; }
            if (!healthy()) break;
            auto target = scene->target;
            target.context_generation = contextGeneration;
            target.gl_draw_fbo = context.defaultFramebufferObject();
            if (!device) {
                const lk_render_gl_binding binding{sizeof(binding), 0, contextGeneration, &context, Proc};
                const lk_render_create_info info{sizeof(info), LK_RENDER_SURFACE_HOST_GL, &binding, sizeof(binding), uint32_t(kMaxResources), target.surface_generation};
                lk_render_result result;
                device = module->Create(info, result);
                if (!device) { lost = true; failed("device-create-failed"); break; }
            }
            auto result = device->Resize(target);
            if (result != LK_RENDER_OK) {
                lost = true;
                failed("resize-failed", result);
                break;
            }
            std::set<uint64_t> live;
            for (const auto& resource : scene->resources) live.insert(resource.id.value);
            for (auto it = uploaded.begin(); it != uploaded.end();) {
                if (!live.count(it->first)) { device->Remove(it->second.id); it = uploaded.erase(it); }
                else ++it;
            }
            for (const auto& resource : scene->resources) {
                const auto it = uploaded.find(resource.id.value);
                if (it != uploaded.end() && it->second.frame == resource.frame && it->second.image.cacheKey() == resource.image.cacheKey()) continue;
                if (resource.frame) result = device->Upload(resource.id, resource.frame->view());
                else {
                    const auto& image = resource.image;
                    lk_render_frame_view view{};
                    view.struct_size = sizeof(view); view.format = LK_RENDER_RGBA8;
                    view.width = image.width(); view.height = image.height(); view.plane_count = 1;
                    view.alpha_mode = LK_RENDER_ALPHA_PREMULTIPLIED;
                    view.planes[0] = {image.constBits(), uint64_t(image.sizeInBytes()), uint32_t(image.bytesPerLine()), view.width, view.height, 0};
                    result = device->Upload(resource.id, view, true);
                }
                if (result != LK_RENDER_OK) break;
                uploaded[resource.id.value] = resource;
            }
            if (result == LK_RENDER_OK) {
                const lk_render_scene_view view{sizeof(view), uint32_t(scene->commands.size()), scene->commands.data(), {0.0706f, 0.0784f, 0.1020f, 1}};
                result = device->Render(target, view);
            }
            if (result != LK_RENDER_OK) {
                lost = true;
                failed("module-device-failed", result);
                break;
            }
            if (!healthy() || stop.load()) break;
            if (hook) hook(); // Deterministic blocking injection at the present boundary.
            if (stop.load()) break;
            context.swapBuffers(surface);
            if (!healthy() || stop.load()) break;
            if (!moduleInfoQueried) {
                moduleInfoQueried = true;
                lk_render_module_info_v1 moduleInfo{};
                if (device->QueryExtension(LK_RENDER_EXT_MODULE_INFO, LK_RENDER_MODULE_INFO_V1,
                        sizeof(moduleInfo), &moduleInfo) == LK_RENDER_OK &&
                        moduleInfo.struct_size == sizeof(moduleInfo) &&
                        moduleInfo.abi_version == LK_RENDER_ABI_V1 && !moduleInfo.reserved) {
                    std::lock_guard<std::mutex> lock(mutex);
                    moduleVersion = moduleInfo.module_version;
                }
            }
            presented.fetch_add(1);
            presentedScene.store(scene->sequence);
            activity->busySince.store(0);
        }
        // Cleanup can also block in a driver. Its owner stays alive, and UI
        // retirement never waits for this code or terminates its thread.
        activity->busySince.store(Now());
        if (device) device->Reset(lost ? LK_RENDER_DESTROY_ABANDON_GL : LK_RENDER_DESTROY_RELEASE);
        device.reset();
        context.doneCurrent();
        activity->busySince.store(0);
    }
};

class GlVideoCanvas::Surface final : public QWindow {
public:
    explicit Surface(GlVideoCanvas* host) : host_(host) {
        setSurfaceType(QSurface::OpenGLSurface);
        QSurfaceFormat format;
        const bool es = QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
        format.setRenderableType(es ? QSurfaceFormat::OpenGLES : QSurfaceFormat::OpenGL);
#if defined(Q_OS_MACOS)
        format.setVersion(4, 1);
#else
        format.setVersion(3, es ? 0 : 3);
#endif
        if (!es) format.setProfile(QSurfaceFormat::CoreProfile);
        format.setOption(QSurfaceFormat::ResetNotification);
        format.setRedBufferSize(8); format.setGreenBufferSize(8); format.setBlueBufferSize(8); format.setAlphaBufferSize(8);
        format.setDepthBufferSize(0); format.setStencilBufferSize(0); format.setSamples(0);
        setFormat(format);
    }
    void retire() { host_ = nullptr; hide(); setParent(nullptr); QObject::setParent(nullptr); }
protected:
    void exposeEvent(QExposeEvent*) override { if (host_) host_->requestRender(); }
    void resizeEvent(QResizeEvent*) override { if (host_) QTimer::singleShot(0, host_, [host = host_] { host->requestRender(); }); }
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
    GlVideoCanvas* host_;
};

GlVideoCanvas::GlVideoCanvas(std::shared_ptr<BackendModule> module, QWidget* parent)
    : ModuleVideoCanvas(std::move(module), parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    surface_ = new Surface(this);
    watchdog_ = new QTimer(this);
    watchdog_->setInterval(25);
    connect(watchdog_, &QTimer::timeout, this, &GlVideoCanvas::pollWorker);
    connect(this, &VideoCanvas::rendererUnavailable, this, [this] { failed_ = true; releaseRenderer(); });
}
QWindow* GlVideoCanvas::windowSurface() const { return surface_; }
GlVideoCanvas::~GlVideoCanvas() { stopRendering(); releaseRenderer(); delete surface_; }
void GlVideoCanvas::startWorker() {
    if (worker_ || failed_ || !surface_->isExposed()) return;
    // Qt 5's Windows EGL plugin inherits the base capability's constant false
    // (qwindowsopenglcontext.h), unlike WGL's override. Our bundled ANGLE path
    // uses one unshared context created/used/destroyed on ONE render owner; it
    // never migrates a GUI context or invokes Qt's threaded widget compositor.
    // Serialize that path process-wide rather than enabling concurrent EGL owners.
    bool isolatedEgl = false;
#if defined(Q_OS_WIN) && defined(QT_OPENGL_ES_2)
    isolatedEgl = QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
#endif
    if (presentationQuarantined.load(std::memory_order_acquire)) {
        fail(RenderGpuFailure::PresentationTimeout); return;
    }
    if (!isolatedEgl && !QOpenGLContext::supportsThreadedOpenGL()) {
        fail(RenderGpuFailure::ThreadedGlUnavailable); return;
    }
    if (activeWorkers.load(std::memory_order_acquire) >= (isolatedEgl ? 1u : 4u)) {
        waiting_for_worker_slot_ = true;
        watchdog_->start();
        if (RetiredOwnerTimedOut(Now())) {
            presentationQuarantined.store(true, std::memory_order_release);
            fail(RenderGpuFailure::PresentationTimeout);
        }
        return;
    }
    waiting_for_worker_slot_ = false;
    surface_->create();
    worker_ = std::make_shared<Worker>();
    auto state = worker_;
    TrackWorkerActivity(state->activity);
    auto* surface = surface_;
    auto* thread = QThread::create([state, surface, format = surface->format(), module = module_] {
        try { state->run(surface, format, module); }
        catch (...) {
            state->activity->busySince.store(0);
            state->failed("render-owner-exception");
        }
    });
    activeWorkers.fetch_add(1);
    // No completion callback references a canvas. GUI cleanup runs only after
    // both retirement and owner exit; a stuck owner pins its surface/module.
    connect(thread, &QThread::finished, qApp, [thread, surface, state] {
        state->finished = true;
        activeWorkers.fetch_sub(1);
        if (state->stop.load()) surface->deleteLater();
        thread->deleteLater();
    });
    thread->start();
    watchdog_->start();
}
void GlVideoCanvas::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (failed_ || !surface_) return;
    winId(); // Create the raster host handle on UI before parenting the GL child.
    surface_->setParent(windowHandle());
    surface_->setGeometry(rect()); surface_->show(); requestRender();
}
void GlVideoCanvas::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (surface_) surface_->hide();
    if (worker_) worker_->exposed.store(false);
}
void GlVideoCanvas::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (!failed_ && surface_) surface_->setGeometry(rect());
    requestRender();
}
void GlVideoCanvas::requestRender() {
    if (failed_ || !surface_) return;
    startWorker();
    if (worker_) { worker_->exposed.store(isVisible() && surface_->isExposed()); render(); }
}
void GlVideoCanvas::pollWorker() {
    if (failed_) return;
    if (!worker_) {
        if (waiting_for_worker_slot_) requestRender();
        return;
    }
    QString failure;
    lk_render_result failureResult = LK_RENDER_OK;
    uint32_t moduleVersion = 0;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        failure = worker_->failure;
        failureResult = worker_->failureResult;
        driver_description_ = worker_->driver;
        moduleVersion = worker_->moduleVersion;
    }
    const auto busy = worker_->activity->busySince.load(std::memory_order_acquire);
    if (!failure.isEmpty()) { fail(FailureFromCode(failure, failureResult)); return; }
    if (busy && Now() - busy > (initialized_ ? kPresentTimeoutMs : kStartupTimeoutMs)) {
        presentationQuarantined.store(true);
        fail(RenderGpuFailure::PresentationTimeout); return;
    }
    if (!initialized_ && Now() - worker_->started > kStartupTimeoutMs) { fail(RenderGpuFailure::RendererStartupTimeout); return; }
    const auto frames = worker_->presented.load();
    if (frames > presented_) {
        presented_ = frames;
        if (!initialized_) {
            initialized_ = true;
            markRendererActive(driver_description_, moduleVersion);
            emit rendererInitialized();
        }
        if (!failed_) emit framePresented();
    }
}
void GlVideoCanvas::fail(RenderGpuFailure failure) {
    if (failed_) return;
    failure_reason_ = RenderGpuFailureName(failure);
    const bool initialization = !initialized_;
    const bool lost = failure == RenderGpuFailure::GraphicsReset ||
        failure == RenderGpuFailure::DeviceLost || failure == RenderGpuFailure::MakeCurrentFailed;
    markRendererFailure(failure, lost ? RenderFallbackReason::GpuDeviceLost :
        failure == RenderGpuFailure::PresentationTimeout ? RenderFallbackReason::GpuRuntimeFailed :
        initialization ? RenderFallbackReason::GpuInitializationFailed :
                         RenderFallbackReason::GpuRuntimeFailed);
    qWarning().noquote() << "OpenGL fallback:" << RenderDiagnosticsSafeSummary(renderDiagnostics());
    failed_ = true;
    releaseRenderer();
    notifyRendererUnavailable();
}
void GlVideoCanvas::releaseRenderer() {
    failed_ = true; initialized_ = false; waiting_for_worker_slot_ = false;
    watchdog_->stop();
    if (worker_) {
        worker_->activity->retired.store(true, std::memory_order_release);
        worker_->stop.store(true);
        { std::lock_guard<std::mutex> lock(worker_->mutex); worker_->pending.reset(); }
        worker_->wake.notify_one();
        // Detach from the widget's native parent before it can be destroyed.
        // No Qt window container retains a second owning pointer to this child.
        surface_->retire();
        if (worker_->finished) surface_->deleteLater();
        surface_ = nullptr;
        worker_.reset();
    }
    clearBackend();
    module_.reset();
}
void GlVideoCanvas::submitFrame(const std::string& key, VideoRenderFrame::Ptr frame) {
    if (!videos_.count(key) && videos_.size() + overlays_.size() >= kMaxResources) { fail(RenderGpuFailure::ResourceLimit); return; }
    auto& resource = videos_[key];
    if (!resource.id.value) resource.id = {next_resource_++, target_.surface_generation};
    resource.frame = std::move(frame);
}
void GlVideoCanvas::removeFrame(const std::string& key) { videos_.erase(key); }
void GlVideoCanvas::removeDecorationTexture(const std::string& key) { overlays_.erase(key); }
void GlVideoCanvas::clearBackend() { videos_.clear(); overlays_.clear(); building_.reset(); }
VideoFrameGeometry GlVideoCanvas::frameGeometry(const std::string& key) const {
    const auto it = videos_.find(key);
    if (it == videos_.end() || !it->second.frame) return {};
    const auto& view = it->second.frame->view();
    return {int(view.width), int(view.height), view.rotation_degrees, true};
}
bool GlVideoCanvas::beginFrame(QSize& pixels) {
    if (failed_ || !worker_) return false;
    const auto dpr = surface_->devicePixelRatio();
    pixels = QSize(qRound(surface_->width()*dpr), qRound(surface_->height()*dpr));
    building_ = std::make_shared<Scene>(); building_->target = target_;
    building_->sequence = ++submitted_;
    building_->target.pixel_width = pixels.width(); building_->target.pixel_height = pixels.height();
    for (const auto& [_, resource] : videos_) building_->resources.push_back(resource);
    return true;
}
void GlVideoCanvas::addCommand(const VideoTileRect& tile, lk_render_resource_id id, float r, float g, float b) {
    building_->commands.push_back({id, {tile.x, tile.y, uint32_t(tile.width), uint32_t(tile.height)},
        {0, 0, building_->target.pixel_width, building_->target.pixel_height}, {r, g, b, 1}});
}
void GlVideoCanvas::drawSolid(const VideoTileRect& tile, float r, float g, float b) { addCommand(tile, {}, r, g, b); }
void GlVideoCanvas::drawVideo(const VideoTileRect& tile) { addCommand(tile, videos_.at(tile.identity).id); }
bool GlVideoCanvas::drawDecoration(const VideoTileRect& tile, const QImage& input) {
    if (!overlays_.count(tile.identity) && videos_.size() + overlays_.size() >= kMaxResources) return false;
    auto& resource = overlays_[tile.identity];
    if (!resource.id.value) resource.id = {next_resource_++, target_.surface_generation};
    resource.image = input.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    building_->resources.push_back(resource);
    addCommand(tile, resource.id); return true;
}
bool GlVideoCanvas::endFrame() {
    if (failed_ || !worker_ || building_->commands.size() > 65536) return false;
    { std::lock_guard<std::mutex> lock(worker_->mutex); worker_->pending = std::exchange(building_, {}); }
    worker_->wake.notify_one(); return true;
}
void GlVideoCanvas::setBeforePresentForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(worker_->mutex);
    worker_->beforePresent = std::move(hook);
}
bool GlVideoCanvas::workersIdleForTest() { return activeWorkers.load() == 0; }
bool GlVideoCanvas::scenePresentedForTest() const { return worker_ && worker_->presentedScene.load() >= submitted_; }
}
