#if defined(IDA2_WINDOW_ACCEPTANCE)

#include "base/basic_types.h"
#include "crl/crl.h"
#include "rpl/rpl.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/remote_track_publication.h"
#include "src/net/service_endpoint_policy.h"
#include "src/rtc/webrtc_manager.h"
#include "src/render/owned_i420_frame.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/render/module_video_canvas.h"
#include "src/ui/render/gl_video_canvas.h"
#include <QtGui/QWindow>
#include <QtGui/QOpenGLContext>
#include <QtGui/QScreen>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSaveFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDateTime>
void RunOpenGlContract();
#include "src/render/api/render_backend_dx11_native.h"
#include "src/render/api/render_backend_module_info.h"
#include "tests/render_p2/dx11_test_hooks.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "src/ui/meeting_ui_integration.h"
#include "tests/support/test_check.h"
#include "ui/integration.h"
#include "ui/style/style_core.h"
#include "api/video/i420_buffer.h"
#include "media/base/adapted_video_track_source.h"
#include "pc/video_track.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <vector>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace crl {
rpl::producer<> on_main_update_requests() { return rpl::never<>(); }
}

namespace OpenMeeting {
class SessionManagerTestAccess final {
public:
    using ScopedSession = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;
    static ScopedSession create(std::unique_ptr<QSettings> settings, OpenMeetingHttpClient *client = nullptr) {
        return ScopedSession(new SessionManager(std::move(settings), nullptr, client, nullptr), [](SessionManager *value) { delete value; });
    }
};
class MeetingCoordinatorTestAccess final {
public:
    static std::shared_ptr<MeetingCoordinator> create(SessionManager &session) {
        return std::shared_ptr<MeetingCoordinator>(new MeetingCoordinator(session, {}, nullptr));
    }
    static std::shared_ptr<livekit::RoomListener> bind(MeetingCoordinator &owner,
        const std::shared_ptr<livekit::Room> &room, const std::shared_ptr<MeetingSessionRuntime> &runtime) {
        owner._room = room;
        owner._sessionRuntime = runtime;
        owner._nextSessionGeneration = runtime->generation();
        owner._sessionRunning.store(true, std::memory_order_release);
        owner._state = MeetingState::InMeeting;
        return owner.participantEventListenerForTesting(runtime);
    }
    static void prepareInMeetingEntry(MeetingCoordinator &owner) { owner._state = MeetingState::ConnectingRoom; }
    static void enterInMeeting(MeetingCoordinator &owner) { owner.setState(MeetingState::InMeeting); }
    static void screenSnapshot(MeetingCoordinator &owner, uint64_t generation, livekit::ScreenShareSnapshot snapshot) {
        owner.applyScreenShareSnapshotOnUiThread(generation, snapshot);
    }
    static void commitLocalStartupPrecondition(MeetingCoordinator &owner) {
        owner._startupCommitted = true;
        owner.setState(MeetingState::InMeeting);
    }
    static void setInvitationState(
            MeetingCoordinator &owner,
            MeetingState state,
            const QString &meetingId) {
        owner._state = state;
        owner._currentMeetingId = meetingId;
    }
};
} // namespace OpenMeeting

namespace livekit {
class ParticipantSnapshotRoomTestAccess final {
public:
    using BeforeSubscriptionSend =
        std::function<asio::awaitable<void>(bool, uint64_t)>;
    using BeforeSubscriptionSyncSend =
        std::function<asio::awaitable<void>(const proto::SyncState&)>;

    static void establishLocalConnectedPrecondition(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        room.connection_state_ = ConnectionState::Connected;
    }
    static void attach(Room &room, const std::shared_ptr<RemoteParticipant> &participant,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::string &sid) {
        room.AttachRemoteTrackToParticipant(participant, std::move(track), nullptr, sid);
    }
    static std::size_t bindingCount(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        return room.remote_track_sinks_.size();
    }
    static void setSubscriptionHooks(
            Room &room,
            BeforeSubscriptionSend beforeSend,
            BeforeSubscriptionSyncSend beforeSyncSend) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) {
            room.connect_attempt_test_hooks_ =
                std::make_shared<Room::ConnectAttemptTestHooks>();
        }
        room.connect_attempt_test_hooks_->before_subscription_send =
            std::move(beforeSend);
        room.connect_attempt_test_hooks_->before_subscription_sync_send =
            std::move(beforeSyncSend);
    }
    static void clearSubscriptionHooks(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.connect_attempt_test_hooks_) return;
        room.connect_attempt_test_hooks_->before_subscription_send = {};
        room.connect_attempt_test_hooks_->before_subscription_sync_send = {};
    }
};
} // namespace livekit

class ParticipantWindowTestAccess final {
public:
    static livekit::ScreenShareState shareState(const MeetingUI::MeetingRoomWindow &window) {
        return window._bottomBar->_screenShareState;
    }
    static void deliverDefaultScreenSources(
            MeetingUI::MeetingRoomWindow &window,
            const std::vector<livekit::DesktopSource> &sources) {
        window._defaultScreenSharePending = true;
        window.handleScreenShareSources(sources);
    }
    static void clickShareDuringRecovery(MeetingUI::MeetingRoomWindow &window) {
        auto *bar = window._bottomBar;
        bar->resize(1120, 80);
        QResizeEvent resize(bar->size(), bar->size());
        QApplication::sendEvent(bar, &resize);
        bar->setInRecovery(true);
        bool clicked = false;
        for (const auto &item : bar->_toolItems) {
            if (item.id != 3) continue;
            clicked = true;
            QMouseEvent press(QEvent::MouseButtonPress, item.rect.center(), Qt::LeftButton,
                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(bar, &press);
        }
        TEST_CHECK(clicked);
    }
    static std::unique_ptr<MeetingUI::MeetingRoomWindow> create(
            const std::shared_ptr<OpenMeeting::MeetingCoordinator> &coordinator) {
        MeetingUI::MeetingRoomWindow::Config config;
        config.audioMuted = true; config.videoEnabled = false;
        config.displayName = QStringLiteral("IDA2 Window Acceptance");
        return create(coordinator, std::move(config));
    }
    static std::unique_ptr<MeetingUI::MeetingRoomWindow> create(
            const std::shared_ptr<OpenMeeting::MeetingCoordinator> &coordinator,
            MeetingUI::MeetingRoomWindow::Config config) {
        config.audioMuted = true; config.videoEnabled = false;
        return std::unique_ptr<MeetingUI::MeetingRoomWindow>(new MeetingUI::MeetingRoomWindow(
            MeetingUI::MeetingRoomWindow::ParticipantWindowTestTag{}, config, coordinator));
    }
    static std::size_t tileCount(const MeetingUI::MeetingRoomWindow &window) { return window._remoteTiles.size(); }
    static std::size_t screenCount(const MeetingUI::MeetingRoomWindow &window) { return window._remoteScreenTiles.size(); }
    static MeetingUI::VideoTileWidget *screen(MeetingUI::MeetingRoomWindow &window, const QString &sid) {
        const auto it = window._remoteScreenTiles.find(sid);
        return it == window._remoteScreenTiles.end() ? nullptr : it->second.get();
    }
    static MeetingUI::VideoTileWidget *localScreen(MeetingUI::MeetingRoomWindow &window) { return window._localScreenTile.get(); }
    static bool sharingBanner(const MeetingUI::MeetingRoomWindow &window) {
        return window._screenShareBanner && !window._screenShareBanner->isHidden() &&
            window._screenShareBanner->text().startsWith(QString::fromUtf8("正在共享："));
    }
    static QImage tileFrame(MeetingUI::VideoTileWidget *tile) {
        TEST_CHECK(tile);
        std::lock_guard lock(tile->_frameMutex);
        return tile->_currentFrame.copy();
    }
    static void checkAspect(MeetingUI::VideoTileWidget &tile) {
        for (const auto source : {QSize(400, 300), QSize(160, 90), QSize(90, 160), QSize(234, 66)}) {
            QImage frame(source, QImage::Format_RGB32);
            frame.fill(Qt::white);
            tile.setFrame(frame);
            QImage painted(500, 300, QImage::Format_RGB32);
            painted.fill(Qt::black);
            { QPainter painter(&painted); tile.drawVideoFrame(painter, painted.rect()); }
            int left = 500, top = 300, right = -1, bottom = -1;
            for (int y = 0; y < painted.height(); ++y) for (int x = 0; x < painted.width(); ++x) {
                if (painted.pixelColor(x, y).red() < 240) continue;
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
            TEST_CHECK(right >= left && bottom >= top);
            TEST_CHECK(std::abs(double(right - left + 1) / (bottom - top + 1) -
                double(source.width()) / source.height()) < 0.025);
        }
    }
    struct NativeRenderer {
        ID3D11Device* device_;
        ID3D11DeviceContext* context_;
        ID3D11Device* device() const { return device_; }
        ID3D11DeviceContext* context() const { return context_; }
    };
    template <typename Task>
    static auto onDxOwner(livekit::render::ModuleVideoCanvas& canvas, Task task) {
        using Result = decltype(task(std::declval<livekit::render::BackendDevice&>()));
        auto completion = std::make_shared<std::promise<Result>>();
        auto result = completion->get_future();
        canvas.runOnOwnerForTest([completion, task = std::move(task)](livekit::render::BackendDevice& device) mutable {
            TEST_CHECK(QThread::currentThread() != qApp->thread());
            if constexpr (std::is_void_v<Result>) { task(device); completion->set_value(); }
            else completion->set_value(task(device));
        });
        QElapsedTimer wait; wait.start();
        while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && wait.elapsed() < 5000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        return result.get();
    }
    // Borrowed native handles never leave their BackendDevice owner invocation.
    static NativeRenderer nativeRenderer(livekit::render::BackendDevice& device) {
        lk_render_dx11_native_v1 native{};
        TEST_CHECK(device.QueryExtension(LK_RENDER_EXT_DX11_NATIVE,
            LK_RENDER_DX11_NATIVE_V1, sizeof(native), &native) == LK_RENDER_OK);
        return {reinterpret_cast<ID3D11Device*>(native.device), reinterpret_cast<ID3D11DeviceContext*>(native.context)};
    }
    static QImage dxImage(livekit::render::BackendDevice& device) {
        using Microsoft::WRL::ComPtr;
        const auto renderer = nativeRenderer(device);
        ComPtr<ID3D11RenderTargetView> view;
        renderer.context()->OMGetRenderTargets(1, view.GetAddressOf(), nullptr);
        TEST_CHECK(view);
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        TEST_CHECK(SUCCEEDED(resource.As(&texture)));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        TEST_CHECK(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> readback;
        TEST_CHECK(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, readback.GetAddressOf())));
        renderer.context()->CopyResource(readback.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        TEST_CHECK(SUCCEEDED(renderer.context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto image = QImage(static_cast<const uchar*>(mapped.pData), desc.Width, desc.Height,
            mapped.RowPitch, QImage::Format_RGBA8888).copy();
        renderer.context()->Unmap(readback.Get(), 0);
        return image;
    }
    static void checkGlStageAndLoss(MeetingUI::MeetingRoomWindow& window) {
        auto* canvas = dynamic_cast<livekit::render::GlVideoCanvas*>(window._videoCanvas);
        TEST_CHECK(canvas && canvas->rendererReady());
        const auto activeDiagnostics = window.renderDiagnostics();
        TEST_CHECK(activeDiagnostics.requested_backend == livekit::render::RenderBackend::OpenGL &&
            activeDiagnostics.actual_backend == livekit::render::RenderBackend::OpenGL &&
            activeDiagnostics.abi_version == LK_RENDER_ABI_V1 &&
            activeDiagnostics.module_version == LK_RENDER_MODULE_VERSION &&
            !activeDiagnostics.driver_description.isEmpty() &&
            activeDiagnostics.gpu_failure == livekit::render::RenderGpuFailure::None &&
            activeDiagnostics.fallback_reason == livekit::render::RenderFallbackReason::None);
        const auto native = window.winId();
        const auto capture = [&](const QString& name, bool gpu) {
            int swapped = 0;
            const auto connection = QObject::connect(canvas, &livekit::render::GlVideoCanvas::framePresented,
                &window, [&] { ++swapped; });
            if (gpu) canvas->requestRender();
            QElapsedTimer wait; wait.start();
            while ((wait.elapsed() < 150 || (gpu && !swapped)) && wait.elapsed() < 1500)
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            QObject::disconnect(connection);
            TEST_CHECK(!gpu || swapped > 0);
            auto image = window.screen()->grabWindow(window.winId()).toImage();
            TEST_CHECK(!image.isNull());
            const auto origin = canvas->mapTo(&window, QPoint());
            const qreal sx = qreal(image.width()) / window.width(), sy = qreal(image.height()) / window.height();
            image = image.copy(qRound(origin.x()*sx), qRound(origin.y()*sy),
                qRound(canvas->width()*sx), qRound(canvas->height()*sy));
            const auto directory = qEnvironmentVariable("LIVEKIT_PRESENTATION_EVIDENCE_DIR");
            if (!directory.isEmpty()) TEST_CHECK(image.save(QDir(directory).filePath(name + ".png")));
            return image;
        };
        auto* banner = window._recoveryBanner;
        banner->setStyleSheet("QLabel { background: rgb(20, 220, 60); color: black; }");
        banner->setText("GL recovery overlay");
        banner->setGeometry(20, 20, 260, 38); banner->show();
        auto image = gpuImage(window, "recovery-overlay");
        auto sample = [&](const QImage& input) {
            return input.pixelColor(25 * input.width() / canvas->width(), 25 * input.height() / canvas->height());
        };
        TEST_CHECK(sample(image).green() > 200 && sample(image).red() < 35);
        TEST_CHECK(sample(capture("native-gl-presented", true)).green() > 200);
        window.resize(window.width() + 40, window.height() + 30);
        banner->setGeometry(20, 20, 260, 38);
        image = capture("native-gl-resized", true);
        TEST_CHECK(sample(image).green() > 200 && native == window.winId());
        banner->setStyleSheet("QLabel { background: rgb(30, 60, 220); color: white; }");
        image = gpuImage(window, "recovery-updated");
        TEST_CHECK(sample(image).blue() > 200 && sample(image).green() < 80);
        banner->hide();
        image = gpuImage(window, "recovery-hidden");
        TEST_CHECK(sample(image).blue() < 200);
        // Retire on UI without touching a render-owner context. Late completion
        // must not reactivate the GPU session before/after queued CPU fallback.
        canvas->fail(livekit::render::RenderGpuFailure::DeviceLost);
        TEST_CHECK(!canvas->rendererReady() && window._usingGpuBackend.load());
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        TEST_CHECK(!window._usingGpuBackend.load() && !canvas->isVisible() && native == window.winId());
        TEST_CHECK(window._remoteRenderSession->backend() == livekit::render::VideoRenderSession::Backend::QtCpu);
        const auto fallbackDiagnostics = window.renderDiagnostics();
        TEST_CHECK(fallbackDiagnostics.actual_backend == livekit::render::RenderBackend::QtCpu &&
            fallbackDiagnostics.gpu_failure == livekit::render::RenderGpuFailure::DeviceLost &&
            fallbackDiagnostics.fallback_reason == livekit::render::RenderFallbackReason::GpuDeviceLost);
        banner->setStyleSheet("QLabel { background: rgb(20, 220, 60); color: black; }");
        banner->setGeometry(20, 20, 260, 38); banner->show(); banner->raise();
        TEST_CHECK(sample(capture("native-cpu-fallback", false)).green() > 200);
        std::cout << "P3_GL_SURFACE PASS: native presented pixels/swap/resize, recovery overlay/update/hide, context-retirement injection, queued CPU pixels, stable top-level window\n";
    }
    static void checkGlDiagnostics(MeetingUI::MeetingRoomWindow& window) {
        auto* canvas = dynamic_cast<livekit::render::GlVideoCanvas*>(window._videoCanvas);
        TEST_CHECK(canvas && canvas->rendererReady() && window._usingGpuBackend.load());
        auto diagnostics = window.renderDiagnostics();
        TEST_CHECK(diagnostics.requested_backend == livekit::render::RenderBackend::OpenGL &&
            diagnostics.actual_backend == livekit::render::RenderBackend::OpenGL &&
            diagnostics.abi_version == LK_RENDER_ABI_V1 &&
            diagnostics.module_version == LK_RENDER_MODULE_VERSION &&
            diagnostics.driver_description == livekit::render::SanitizeRenderDriverDescription(canvas->driverDescription()) &&
            !diagnostics.driver_description.isEmpty() &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::None &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::None);
        TEST_CHECK(!livekit::render::RenderDiagnosticsSafeSummary(diagnostics).contains(
            diagnostics.driver_description, Qt::CaseSensitive));

        canvas->fail(livekit::render::RenderGpuFailure::DeviceLost);
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        diagnostics = window.renderDiagnostics();
        TEST_CHECK(!window._usingGpuBackend.load() &&
            diagnostics.actual_backend == livekit::render::RenderBackend::QtCpu &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::DeviceLost &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::GpuDeviceLost);
        std::cout << "RENDER_DIAGNOSTICS_GL PASS: active backend, ABI/module version, sanitized driver, typed device-loss fallback\n";
    }
    static void checkDriverLoss(MeetingUI::MeetingRoomWindow& window, const QString& directory,
            bool smoke, bool angle, bool liveMeeting = false,
            const std::shared_ptr<OpenMeeting::MeetingCoordinator>& coordinator = {}) {
        using Microsoft::WRL::ComPtr;
        TEST_CHECK(QDir().mkpath(directory));
        const QString api = angle ? "angle" : "dx11";
        if (angle) TEST_CHECK(QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES);
        else {
            TEST_CHECK(!qEnvironmentVariableIsSet("LIVEKIT_RENDER_BACKEND"));
            TEST_CHECK(livekit::render::BackendModule::DefaultBackend() == LK_RENDER_BACKEND_DX11);
        }
        std::shared_ptr<livekit::VideoSource> source;
        if (!liveMeeting) {
            source = std::make_shared<livekit::VideoSource>(16, 9);
            bindLocal(window, source);
        } else {
            TEST_CHECK(!angle && coordinator);
        }
        TEST_CHECK(enableGpu(window, true));
        layout(window, MeetingUI::VideoViewMode::Grid);
        auto* canvas = window._videoCanvas;
        TEST_CHECK(canvas && canvas->rendererReady());
        auto* gl = dynamic_cast<livekit::render::GlVideoCanvas*>(canvas);
        DXGI_ADAPTER_DESC rendererDescription{};
        QString driver;
        if (angle) {
            TEST_CHECK(gl);
            driver = gl->driverDescription();
            TEST_CHECK(driver.contains("OpenGL ES") && driver.contains("ANGLE") && driver.contains("Direct3D11"));
            TEST_CHECK(!GetModuleHandleW(L"livekit-render-dx11.dll"));
        } else {
            auto* dx = dynamic_cast<livekit::render::ModuleVideoCanvas*>(canvas);
            TEST_CHECK(dx && !gl && canvas->backendName() == "DX11");
            TEST_CHECK(GetModuleHandleW(L"livekit-render-dx11.dll") && !GetModuleHandleW(L"livekit-render-opengl.dll"));
            rendererDescription = onDxOwner(*dx, [](livekit::render::BackendDevice& device) {
                const auto renderer = nativeRenderer(device);
                TEST_CHECK(renderer.device()->GetDeviceRemovedReason() == S_OK);
                ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
                DXGI_ADAPTER_DESC description{};
                TEST_CHECK(SUCCEEDED(renderer.device()->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) &&
                    SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())) && SUCCEEDED(adapter->GetDesc(&description)));
                return description;
            });
            driver = "DX11 / " + QString::fromWCharArray(rendererDescription.Description);
        }
        std::cout << api.toUpper().toStdString() << "_TDR_DRIVER " << driver.toStdString() << std::endl;
        const auto native = window.winId();
        QElapsedTimer clock; clock.start();
        const auto wait = [&](const std::function<bool()>& done, int timeout = 5000) {
            QElapsedTimer deadline; deadline.start();
            while (!done() && deadline.elapsed() < timeout) QApplication::processEvents(QEventLoop::AllEvents, 20);
            return done();
        };
        const auto cpuActive = [&] {
            return !window._usingGpuBackend.load() &&
                window._remoteRenderSession->backend() == livekit::render::VideoRenderSession::Backend::QtCpu;
        };
        const auto videoTile = [&]() -> MeetingUI::VideoTileWidget* {
            if (!liveMeeting) return window._localTile;
            for (const auto& [trackSid, binding] : window._remoteVideoBindings) {
                auto* tile = window.remoteVideoTile(trackSid);
                if (tile && tile->isVideoActive()) return tile;
            }
            return nullptr;
        };
        const auto renderKey = [&] {
            auto* tile = videoTile();
            return tile ? tile->renderKey().toStdString() : std::string();
        };
        const auto sendFrame = [&] {
            if (source) {
                auto frame = livekit::VideoFrame::create(16, 9, livekit::VideoBufferType::RGBA);
                const bool cpu = cpuActive();
                for (size_t i = 0; i < frame.dataSize(); i += 4) {
                    frame.data()[i] = cpu ? 20 : 240; frame.data()[i + 1] = cpu ? 80 : 20;
                    frame.data()[i + 2] = cpu ? 220 : 20; frame.data()[i + 3] = 255;
                }
                source->captureFrame(frame, {});
            }
            render(window); // Production latest-frame/session/CPU-or-GPU entry.
        };
        const auto capture = [&](const QString& name, const std::function<bool(QColor)>& accept) {
            const auto start = clock.elapsed();
            QImage image;
            QColor color;
            TEST_CHECK(wait([&] {
                if (clock.elapsed() - start <= 180) return false;
                if (liveMeeting) render(window);
                auto* tile = videoTile();
                if (!tile) return false;
                image = window.screen()->grabWindow(native).toImage();
                if (image.isNull()) return false;
                const auto point = tile->mapTo(&window,
                    QPoint(tile->width() / 2, tile->height() / 2));
                color = image.pixelColor(point.x() * image.width() / window.width(),
                    point.y() * image.height() / window.height());
                return accept(color);
            }, liveMeeting ? 6000 : 1500));
            TEST_CHECK(!image.isNull());
            TEST_CHECK(image.save(QDir(directory).filePath(name + ".png")));
            return color;
        };
        sendFrame();
        TEST_CHECK(wait([&] {
            if (liveMeeting) render(window);
            const auto key = renderKey();
            const auto stats = statistics(window);
            return (!liveMeeting || coordinator->state() == OpenMeeting::MeetingState::InMeeting) &&
                !key.empty() && canvas->hasVideo(key) && stats.delivered_to_gpu >= (liveMeeting ? 10 : 1) &&
                stats.attached_track_count >= (liveMeeting ? 1u : 0u) && (!gl || gl->scenePresentedForTest());
        }, liveMeeting ? 45000 : 5000));
        const auto colorful = [](QColor color) {
            const auto high = std::max({color.red(), color.green(), color.blue()});
            const auto low = std::min({color.red(), color.green(), color.blue()});
            return high > 150 && high - low > 80;
        };
        const auto red = [](QColor color) {
            return color.red() > 220 && color.green() < 40 && color.blue() < 40;
        };
        capture("native-" + api + (liveMeeting ? "-live-remote-before-reset" : "-before-reset"),
            liveMeeting ? std::function<bool(QColor)>(colorful) : std::function<bool(QColor)>(red));
        ComPtr<ID3D11Device> witness;
        TEST_CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, witness.GetAddressOf(), nullptr, nullptr)));
        TEST_CHECK(witness->GetDeviceRemovedReason() == S_OK);
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC description{};
        TEST_CHECK(SUCCEEDED(witness.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())) &&
            SUCCEEDED(adapter->GetDesc(&description)));
        const auto luid = QString("%1:%2").arg(quint32(description.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
            .arg(description.AdapterLuid.LowPart, 8, 16, QLatin1Char('0'));
        if (!angle) TEST_CHECK(rendererDescription.AdapterLuid.HighPart == description.AdapterLuid.HighPart &&
            rendererDescription.AdapterLuid.LowPart == description.AdapterLuid.LowPart);
        int failures = 0, lateReady = 0, uiTicks = 0;
        qint64 lastTick = clock.elapsed(), maxUiGap = 0, resetObservedMs = -1, fallbackMs = -1;
        QObject scope; // Disconnect captures before returning, including failure paths.
        QObject::connect(canvas, &livekit::render::VideoCanvas::rendererUnavailable, &scope, [&] { ++failures; });
        QObject::connect(canvas, &livekit::render::VideoCanvas::rendererInitialized, &scope, [&] { ++lateReady; });
        const auto statePath = QDir(directory).filePath("driver-observer.json");
        const auto writeState = [&](const QString& phase) {
            const auto stats = statistics(window);
            const auto diagnostics = window.renderDiagnostics();
            QSaveFile file(statePath); TEST_CHECK(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{
                {"pid", double(QCoreApplication::applicationPid())}, {"timestampMs", double(QDateTime::currentMSecsSinceEpoch())},
                {"phase", phase}, {"api", api}, {"scenario", liveMeeting ? "live-meeting" : "fixture"},
                {"driver", driver}, {"adapterLuid", luid}, {"smoke", smoke},
                {"reason", QString("0x%1").arg(quint32(witness->GetDeviceRemovedReason()), 0, 16)},
                // The renderer device is owner-only and unavailable after retirement.
                {"rendererDeviceReason", QJsonValue()},
                {"defaultBackend", !angle && !qEnvironmentVariableIsSet("LIVEKIT_RENDER_BACKEND")},
                {"nativeGpuPixelsVerified", true},
                {"meetingConnected", liveMeeting && coordinator && coordinator->state() == OpenMeeting::MeetingState::InMeeting},
                {"remoteTrackCount", liveMeeting ? double(stats.attached_track_count) : 0.0},
                {"remoteMediaFlowing", liveMeeting && stats.attached_track_count > 0 &&
                    (stats.delivered_to_gpu > 0 || stats.delivered_to_qt_cpu > 0)},
                {"gpuReady", canvas->rendererReady()}, {"cpuActive", cpuActive()},
                // DX11 has no public present counter; do not mislabel submitted frames as swaps.
                {"swaps", gl ? QJsonValue(double(gl->presentedFrames())) : QJsonValue()},
                {"failures", failures}, {"lateReady", lateReady},
                {"renderBackend", livekit::render::RenderBackendName(diagnostics.actual_backend)},
                {"renderAbiVersion", int(diagnostics.abi_version)},
                {"renderModuleVersion", int(diagnostics.module_version)},
                {"renderDriver", diagnostics.driver_description},
                {"hostFailure", livekit::render::RenderGpuFailureName(diagnostics.gpu_failure)},
                {"fallbackReason", livekit::render::RenderFallbackReasonName(diagnostics.fallback_reason)},
                {"uiTicks", uiTicks}, {"maxUiGapMs", double(maxUiGap)},
                {"resetObservedMs", double(resetObservedMs)}, {"fallbackMs", double(fallbackMs)},
                {"deliveredToCpu", double(stats.delivered_to_qt_cpu)}, {"deliveredToGpu", double(stats.delivered_to_gpu)},
                {"topLevelStable", window.winId() == native}
            }).toJson()); TEST_CHECK(file.commit());
        };
        QString phase = smoke ? "smoke" : "waiting";
        QTimer heartbeat, producer;
        QObject::connect(&heartbeat, &QTimer::timeout, &scope, [&] {
            const auto now = clock.elapsed(); maxUiGap = std::max(maxUiGap, now - lastTick); lastTick = now;
            ++uiTicks; writeState(phase);
        });
        QObject::connect(&producer, &QTimer::timeout, &scope, sendFrame);
        heartbeat.start(250); producer.start(33); writeState(phase);
        std::cout << api.toUpper().toStdString()
            << (liveMeeting ? "_LIVE_MEETING_WAITING_FOR_REAL_DRIVER_LOSS" :
                (smoke ? "_OBSERVER_SMOKE" : "_WAITING_FOR_REAL_DRIVER_LOSS"))
            << " pid=" << QCoreApplication::applicationPid() << " adapter=" << luid.toStdString() << std::endl;
        if (smoke) {
            if (gl) gl->fail(livekit::render::RenderGpuFailure::DeviceLost);
            else canvas->notifyRendererUnavailable();
        }
        const bool recovered = wait([&] {
            if (FAILED(witness->GetDeviceRemovedReason()) && resetObservedMs < 0) resetObservedMs = clock.elapsed();
            if (cpuActive() && fallbackMs < 0) fallbackMs = clock.elapsed();
            return (smoke || resetObservedMs >= 0) && cpuActive();
        }, smoke ? 5000 : 180000);
        phase = recovered ? "observed" : "timeout"; writeState(phase);
        TEST_CHECK(recovered && failures == 1 && !lateReady && window.winId() == native);
        TEST_CHECK(smoke ? witness->GetDeviceRemovedReason() == S_OK : FAILED(witness->GetDeviceRemovedReason()));
        TEST_CHECK(!canvas->rendererReady() && !canvas->isVisible());
        const auto gpuCount = statistics(window).delivered_to_gpu;
        const auto cpuCount = statistics(window).delivered_to_qt_cpu;
        const auto ticksBefore = uiTicks;
        heartbeat.start(25);
        TEST_CHECK(wait([&] {
            return uiTicks >= ticksBefore + 10 && statistics(window).delivered_to_qt_cpu > cpuCount &&
                (!liveMeeting || coordinator->state() == OpenMeeting::MeetingState::InMeeting);
        }, liveMeeting ? 15000 : 5000));
        const auto blue = [](QColor color) { return color.blue() > 200 && color.green() > 60 && color.green() < 100 && color.red() < 40; };
        capture("native-" + api + (liveMeeting ? "-live-remote-cpu-after-reset" : "-cpu-after-reset"),
            liveMeeting ? std::function<bool(QColor)>(colorful) : std::function<bool(QColor)>(blue));
        window.resize(window.width() + 40, window.height() + 30);
        window.hide(); QApplication::processEvents(); window.show();
        capture("native-" + api + (liveMeeting ? "-live-remote-cpu-responsive" : "-cpu-responsive"),
            liveMeeting ? std::function<bool(QColor)>(colorful) : std::function<bool(QColor)>(blue));
        TEST_CHECK(statistics(window).delivered_to_gpu == gpuCount && !lateReady && failures == 1 && window.winId() == native);
        TEST_CHECK(!liveMeeting || (coordinator->state() == OpenMeeting::MeetingState::InMeeting &&
            statistics(window).attached_track_count > 0 && statistics(window).delivered_to_qt_cpu > cpuCount));
        producer.stop(); heartbeat.stop(); phase = "verified"; writeState(phase);
        std::cout << api.toUpper().toStdString()
            << (liveMeeting ? "_LIVE_MEETING_REAL_TDR PASS" : (smoke ? "_OBSERVER_SMOKE PASS" : "_REAL_TDR PASS"))
            << " reason=0x" << std::hex << quint32(witness->GetDeviceRemovedReason()) << std::dec
            << " maxUiGapMs=" << maxUiGap
            << " uiTicks=" << uiTicks << " CPU frames=" << statistics(window).delivered_to_qt_cpu << std::endl;
    }
    static void waitGlOwners() {
        QElapsedTimer wait; wait.start();
        while (!livekit::render::GlVideoCanvas::workersIdleForTest() && wait.elapsed() < 5000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(livekit::render::GlVideoCanvas::workersIdleForTest());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    static bool glWorkersIdle() {
        return livekit::render::GlVideoCanvas::workersIdleForTest();
    }
    static void waitDxOwners() {
        QElapsedTimer wait; wait.start();
        while (!livekit::render::ModuleVideoCanvas::workersIdleForTest() && wait.elapsed() < 5000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(livekit::render::ModuleVideoCanvas::workersIdleForTest());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    static livekit::render::ModuleVideoCanvas& dxCanvas(MeetingUI::MeetingRoomWindow& window) {
        auto* canvas = dynamic_cast<livekit::render::ModuleVideoCanvas*>(window._videoCanvas);
        TEST_CHECK(canvas && !dynamic_cast<livekit::render::GlVideoCanvas*>(canvas));
        return *canvas;
    }
    static QWindow* dxSurface(MeetingUI::MeetingRoomWindow& window) {
        return dxCanvas(window).windowSurface();
    }
    static void requestDxScene(MeetingUI::MeetingRoomWindow& window) {
        // A scheduled scene may already have failed after hook installation.
        // Rendering a retired canvas is deliberately a no-op in production.
        dxCanvas(window).render();
    }
    static void installDxHooks(MeetingUI::MeetingRoomWindow& window, lk_render_dx11_test_hooks_v1 hooks) {
        onDxOwner(dxCanvas(window), [hooks](livekit::render::BackendDevice& device) {
            lk_render_dx11_test_control_v1 control{};
            TEST_CHECK(device.QueryExtension(LK_RENDER_EXT_DX11_TEST_HOOKS, LK_RENDER_DX11_TEST_HOOKS_V1,
                sizeof(control), &control) == LK_RENDER_OK);
            TEST_CHECK(control.set_hooks && control.set_hooks(control.device, &hooks) == LK_RENDER_OK);
        });
    }
    static void throwOnDxOwner(MeetingUI::MeetingRoomWindow& window) {
        dxCanvas(window).runOnOwnerForTest([](livekit::render::BackendDevice&) {
            TEST_CHECK(QThread::currentThread() != qApp->thread());
            throw std::runtime_error("deterministic render owner exception");
        });
    }
    static void simulateModuleFailure(MeetingUI::MeetingRoomWindow& window) {
        window._videoCanvas->notifyRendererUnavailable();
        TEST_CHECK(window._usingGpuBackend.load()); // Delivery is queued until draw returns.
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        TEST_CHECK(!window._usingGpuBackend.load());
    }
    static void queueModuleFailure(MeetingUI::MeetingRoomWindow& window) {
        TEST_CHECK(window._videoCanvas && window._usingGpuBackend.load());
        window._videoCanvas->notifyRendererUnavailable();
        TEST_CHECK(window._usingGpuBackend.load());
    }
    static void checkGpuAspect() {
        using Microsoft::WRL::ComPtr;
        std::unique_ptr<livekit::render::VideoCanvas> owner(livekit::render::CreateVideoCanvas(nullptr));
        TEST_CHECK(owner);
        auto &canvas = *static_cast<livekit::render::ModuleVideoCanvas*>(owner.get());
        canvas.setAttribute(Qt::WA_DontShowOnScreen);
        canvas.resize(640, 360);
        canvas.show();
        QElapsedTimer initialized; initialized.start();
        while (!canvas.rendererReady() && canvas.rendererPending() && initialized.elapsed() < 6000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(canvas.rendererReady());
        const auto diagnostics = canvas.renderDiagnostics();
        TEST_CHECK(diagnostics.requested_backend == livekit::render::RenderBackend::Dx11 &&
            diagnostics.actual_backend == livekit::render::RenderBackend::Dx11 &&
            diagnostics.abi_version == LK_RENDER_ABI_V1 &&
            diagnostics.module_version == LK_RENDER_MODULE_VERSION &&
            diagnostics.driver_description.startsWith("DX11") &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::None &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::None);
        for (const auto size : {QSize(400, 300), QSize(160, 90), QSize(90, 160), QSize(234, 66)}) {
            for (const auto rotation : {livekit::VideoRotation::VIDEO_ROTATION_0,
                                       livekit::VideoRotation::VIDEO_ROTATION_90}) {
                std::vector<uint8_t> y(size.width() * size.height(), 235);
                std::vector<uint8_t> uv(((size.width() + 1) / 2) * ((size.height() + 1) / 2), 128);
                auto frame = livekit::render::OwnedI420Frame::CopyFromPlanes(size.width(), size.height(),
                    y.data(), size.width(), uv.data(), (size.width() + 1) / 2,
                    uv.data(), (size.width() + 1) / 2, 0, rotation);
                canvas.setTilesLayout({{"aspect", 0, 0, canvas.width(), canvas.height(), false, 0.0f, true}});
                canvas.updateI420Frame("aspect", frame);
                // Reproduce a stale, differently shaped backbuffer after a
                // native-window size change; the draw must resynchronize it.
                auto staleTarget = canvas.target_;
                staleTarget.pixel_width = staleTarget.pixel_height = 800;
                onDxOwner(canvas, [staleTarget](livekit::render::BackendDevice& device) {
                    TEST_CHECK(device.Resize(staleTarget) == LK_RENDER_OK);
                });
                const auto image = onDxOwner(canvas, [](livekit::render::BackendDevice& device) {
                    return dxImage(device);
                });
                RECT client{};
                TEST_CHECK(GetClientRect(reinterpret_cast<HWND>(canvas.windowSurface()->winId()), &client));
                TEST_CHECK(image.width() == client.right && image.height() == client.bottom);
                int left = image.width(), top = image.height(), right = -1, bottom = -1;
                for (int row = 0; row < image.height(); ++row) for (int col = 0; col < image.width(); ++col) {
                    const auto pixel = image.pixelColor(col, row);
                    if (pixel.red() < 240 || pixel.green() < 240 || pixel.blue() < 240) continue;
                    left = std::min(left, int(col)); right = std::max(right, int(col));
                    top = std::min(top, int(row)); bottom = std::max(bottom, int(row));
                }
                const double expected = rotation == livekit::VideoRotation::VIDEO_ROTATION_0
                    ? double(size.width()) / size.height() : double(size.height()) / size.width();
                TEST_CHECK(right >= left && bottom >= top);
                TEST_CHECK(std::abs(double(right - left + 1) / (bottom - top + 1) - expected) < 0.025);
                std::cout << "GPU_ASPECT source=" << size.width() << 'x' << size.height()
                    << " rotation=" << int(rotation) << " displayed=" << (right - left + 1) << 'x' << (bottom - top + 1) << '\n';
            }
        }
    }
    static MeetingUI::VideoTileWidget *tile(MeetingUI::MeetingRoomWindow &window, const QString &identity) {
        const auto found = window._remoteTiles.find(identity);
        return found == window._remoteTiles.end() ? nullptr : found->second.get();
    }
    static QImage frame(MeetingUI::MeetingRoomWindow &window, const QString &identity) {
        auto *value = tile(window, identity);
        if (!value) return {};
        std::lock_guard lock(value->_frameMutex);
        return value->_currentFrame.copy();
    }
    static livekit::render::VideoRenderSession::Statistics statistics(const MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(window._remoteRenderSession);
        return window._remoteRenderSession->statistics();
    }
    static uint64_t renderGeneration(const MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(window._remoteRenderSession);
        return window._remoteRenderSession->generation();
    }
    static bool renderSessionActive(const MeetingUI::MeetingRoomWindow &window) {
        return window._remoteRenderSession && window._remoteRenderSession->active();
    }
    static bool usingGpu(const MeetingUI::MeetingRoomWindow &window) {
        return window._usingGpuBackend.load(std::memory_order_acquire);
    }
    static void render(MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(QThread::currentThread() == window.thread());
        window.onRemoteRenderTick();
    }
    static void renderGpuCanvas(MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(window._videoCanvas && window._usingGpuBackend.load());
        window._videoCanvas->render();
    }
    static void bindLocal(MeetingUI::MeetingRoomWindow &window,
            const std::shared_ptr<livekit::VideoSource> &source, bool enabled = true) {
        window._localVideoSource = source;
        window._config.videoEnabled = enabled;
        window._localTile->setVideoActive(enabled);
        window.updateVideoLayout();
    }
    static void stopRenderSession(MeetingUI::MeetingRoomWindow &window) {
        window.stopLiveKitSession();
        TEST_CHECK(!window._remoteRenderSession->active());
    }
    static bool paused(const MeetingUI::VideoTileWidget *tile) {
        TEST_CHECK(tile);
        return tile->_isVideoStreamPaused;
    }
    static void savePresentation(MeetingUI::MeetingRoomWindow &window, const QString &name) {
        const auto directory = qEnvironmentVariable("LIVEKIT_PRESENTATION_EVIDENCE_DIR");
        if (directory.isEmpty()) return;
        TEST_CHECK(QDir().mkpath(directory));
        window.resize(960, 640);
        QResizeEvent resize(window.size(), window.size());
        window.resizeEvent(&resize);
        QImage image(window._stageContainer->size(), QImage::Format_ARGB32);
        image.fill(QColor("#12141a"));
        window._stageContainer->render(&image);
        TEST_CHECK(image.save(QDir(directory).filePath(name + ".png")));
    }
    static livekit::render::VideoCanvas* startGpu(MeetingUI::MeetingRoomWindow &window, bool visible = false,
            std::shared_ptr<livekit::render::BackendModule> module = {}) {
        if (!(GetSystemMetrics(SM_REMOTESESSION) == 0)) return nullptr;
        const bool gl = qgetenv("LIVEKIT_RENDER_BACKEND") == "opengl";
        window.setAttribute(Qt::WA_DontShowOnScreen, !gl && !visible);
        window._videoCanvas = module
            ? new livekit::render::ModuleVideoCanvas(std::move(module), window._stageContainer)
            : livekit::render::CreateVideoCanvas(window._stageContainer, &window._renderDiagnostics);
        if (!window._videoCanvas) return nullptr;
        QObject::connect(window._videoCanvas, &livekit::render::VideoCanvas::rendererUnavailable,
            &window, &MeetingUI::MeetingRoomWindow::fallBackToQtCpuBackend, Qt::QueuedConnection);
        window.show(); // Production showEvent selects the mutually exclusive backend.
        return window._videoCanvas;
    }
    static bool enableGpu(MeetingUI::MeetingRoomWindow &window, bool visible = false,
            std::shared_ptr<livekit::render::BackendModule> module = {}) {
        if (!startGpu(window, visible, std::move(module))) return false;
        {
            QElapsedTimer wait; wait.start();
            while (!window._usingGpuBackend.load() && window._videoCanvas->rendererPending() && wait.elapsed() < 6000)
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            QApplication::processEvents(); // queued initialized signal
            TEST_CHECK(window._usingGpuBackend.load());
        }
        return window._usingGpuBackend.load();
    }
    static livekit::render::GlVideoCanvas& glCanvas(MeetingUI::MeetingRoomWindow& window) {
        auto* canvas = dynamic_cast<livekit::render::GlVideoCanvas*>(window._videoCanvas);
        TEST_CHECK(canvas);
        return *canvas;
    }
    static void setGlBeforePresent(MeetingUI::MeetingRoomWindow& window, std::function<void()> hook) {
        glCanvas(window).setBeforePresentForTest(std::move(hook));
    }
    static void requestGlScene(MeetingUI::MeetingRoomWindow& window) {
        glCanvas(window).requestRender();
    }
    static bool gpuHasFrame(MeetingUI::MeetingRoomWindow &window, const QString &key) {
        return window._videoCanvas->hasVideo(key.toStdString());
    }
    static void prepareResizeWindow(MeetingUI::MeetingRoomWindow &window) {
        window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
            Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
        window.setMinimumSize(850, 560);
    }
    static int checkNativeResize(MeetingUI::MeetingRoomWindow &window, bool before, const char *backend,
        bool disabled = false) {
        const auto handle = reinterpret_cast<HWND>(window.winId());
        if (!disabled) TEST_CHECK(GetWindowLongPtr(handle, GWL_STYLE) & WS_THICKFRAME);
        RECT client{};
        TEST_CHECK(GetClientRect(handle, &client));
        const int w = client.right, h = client.bottom;
        const std::array<std::pair<POINT, int>, 8> edges{{
            {{2, h / 2}, HTLEFT}, {{w - 3, h / 2}, HTRIGHT},
            {{w / 2, 2}, HTTOP}, {{w / 2, h - 3}, HTBOTTOM},
            {{2, 2}, HTTOPLEFT}, {{w - 3, 2}, HTTOPRIGHT},
            {{2, h - 3}, HTBOTTOMLEFT}, {{w - 3, h - 3}, HTBOTTOMRIGHT}
        }};
        std::vector<HWND> children;
        EnumChildWindows(handle, [](HWND child, LPARAM param) -> BOOL {
            reinterpret_cast<std::vector<HWND> *>(param)->push_back(child);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&children));
        int blocked = 0, tested = 0;
        for (const auto &[clientPoint, expected] : edges) {
            auto point = clientPoint;
            TEST_CHECK(ClientToScreen(handle, &point));
            const auto position = MAKELPARAM(point.x, point.y);
            const auto rootHit = SendMessage(handle, WM_NCHITTEST, 0, position);
            if (disabled) TEST_CHECK(rootHit < HTLEFT || rootHit > HTBOTTOMRIGHT);
            else TEST_CHECK(rootHit == expected);
            for (const auto child : children) {
                RECT bounds{};
                TEST_CHECK(GetWindowRect(child, &bounds));
                if (!PtInRect(&bounds, point)) continue;
                ++tested;
                const auto result = SendMessage(child, WM_NCHITTEST, 0, position);
                if (result != HTTRANSPARENT && !disabled) ++blocked;
                if (!before) TEST_CHECK(disabled ? result != HTTRANSPARENT : result == HTTRANSPARENT);
            }
        }
        TEST_CHECK(tested > 0);
        // Card content is still delivered to the child HWND, not the window frame.
        const auto canvas = reinterpret_cast<HWND>(window._videoCanvas->winId());
        RECT canvasRect{};
        TEST_CHECK(GetClientRect(canvas, &canvasRect));
        POINT center{canvasRect.right / 2, canvasRect.bottom / 2};
        TEST_CHECK(ClientToScreen(canvas, &center));
        TEST_CHECK(SendMessage(canvas, WM_NCHITTEST, 0, MAKELPARAM(center.x, center.y)) == HTCLIENT);
        std::cout << "NATIVE_RESIZE " << backend << (before ? " BEFORE" : " AFTER")
            << " root-edges=8 child-edge-hits=" << tested << " blocked=" << blocked
            << " dpr=" << window.devicePixelRatioF() << '\n';
        return blocked;
    }
    static MeetingUI::VideoTileWidget *local(MeetingUI::MeetingRoomWindow &window) { return window._localTile; }
    static QString pinned(const MeetingUI::MeetingRoomWindow &window) { return window._pinnedRenderKey; }
    static void cpuPin(MeetingUI::VideoTileWidget *tile) { tile->_pinBtn->click(); }
    static void cpuDoubleClick(MeetingUI::VideoTileWidget *tile) {
        QMouseEvent event(QEvent::MouseButtonDblClick, tile->rect().center(), Qt::LeftButton,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tile, &event);
    }
    static void gpuPin(MeetingUI::MeetingRoomWindow &window, MeetingUI::VideoTileWidget *tile) {
        const QPoint point = tile->pos() + tile->pinButtonRect().center();
        QMouseEvent move(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(window._videoCanvas, &move);
        QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window._videoCanvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(window._videoCanvas, &release);
    }
    static void checkOrder(MeetingUI::MeetingRoomWindow &window, MeetingUI::VideoTileWidget *main) {
        TEST_CHECK(window._videoCanvas->tiles_.front().identity == main->renderKey().toStdString());
        TEST_CHECK(main->geometry() == window._stageContainer->rect());
        for (const auto &item : window._videoCanvas->tiles_) {
            MeetingUI::VideoTileWidget *tile = nullptr;
            if (window._localTile->renderKey().toStdString() == item.identity) tile = window._localTile;
            for (auto &[id, candidate] : window._remoteTiles)
                if (candidate->renderKey().toStdString() == item.identity) tile = candidate.get();
            for (auto &[id, candidate] : window._remoteScreenTiles)
                if (candidate->renderKey().toStdString() == item.identity) tile = candidate.get();
            TEST_CHECK(tile && tile->geometry() == QRect(item.x, item.y, item.width, item.height));
        }
    }
    static qint64 decorationKey(MeetingUI::MeetingRoomWindow &window, MeetingUI::VideoTileWidget *tile) {
        return window._videoCanvas->decorations_.at(tile->renderKey().toStdString()).cacheKey;
    }
    static bool decorationExists(MeetingUI::MeetingRoomWindow &window, const std::string &key) {
        return window._videoCanvas->decorations_.count(key) != 0;
    }
    static bool gpuDirty(MeetingUI::MeetingRoomWindow &window) {
        return window._videoCanvas->frame_dirty_.exchange(false);
    }
    static void resizeWithSidebar(MeetingUI::MeetingRoomWindow &window, const QSize &size) {
        if (!window._participantsSidebar) window._participantsSidebar =
            new OpenMeeting::ParticipantsSidebarWidget(window._coordinator, &window);
        window.resize(size);
        window.switchSidebar(MeetingUI::ActiveSidebar::Participants);
        QResizeEvent resize(window.size(), window.size());
        window.resizeEvent(&resize);
    }
    static void fallback(MeetingUI::MeetingRoomWindow &window) { window.fallBackToQtCpuBackend(); }
    static void layout(MeetingUI::MeetingRoomWindow &window, MeetingUI::VideoViewMode mode) {
        window._viewMode = mode;
        window.resize(960, 640);
        QResizeEvent resize(window.size(), window.size());
        window.resizeEvent(&resize); // Exercise the production layout, including stacking.
    }
    static void pin(MeetingUI::MeetingRoomWindow &window, MeetingUI::VideoTileWidget *tile) {
        TEST_CHECK(tile);
        window.setPinnedTile(tile->renderKey(), true);
    }
    static QString doubleClickGpu(MeetingUI::MeetingRoomWindow &window, const QPoint &point) {
        QString hit;
        const auto connection = QObject::connect(window._videoCanvas,
            &livekit::render::VideoCanvas::tileDoubleClicked, &window,
            [&](const QString &key) { hit = key; });
        QMouseEvent event(QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (auto* gl = dynamic_cast<livekit::render::GlVideoCanvas*>(window._videoCanvas))
            QApplication::sendEvent(gl->windowSurface(), &event);
        else QApplication::sendEvent(window._videoCanvas, &event);
        QObject::disconnect(connection);
        return hit;
    }
    static QImage gpuImage(MeetingUI::MeetingRoomWindow &window, const QString &name) {
        using Microsoft::WRL::ComPtr;
        auto &canvas = *static_cast<livekit::render::ModuleVideoCanvas*>(window._videoCanvas);
        if (auto* gl = dynamic_cast<livekit::render::GlVideoCanvas*>(&canvas)) {
            QApplication::processEvents();
            gl->requestRender();
            QElapsedTimer wait; wait.start();
            while (!gl->scenePresentedForTest() && wait.elapsed() < 3000)
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            TEST_CHECK(gl->scenePresentedForTest());
            wait.restart();
            while (wait.elapsed() < 100) QApplication::processEvents(QEventLoop::AllEvents, 20);
            auto image = window.screen()->grabWindow(window.winId()).toImage();
            const auto origin = canvas.mapTo(&window, QPoint());
            const qreal sx = qreal(image.width()) / window.width(), sy = qreal(image.height()) / window.height();
            image = image.copy(qRound(origin.x()*sx), qRound(origin.y()*sy),
                qRound(canvas.width()*sx), qRound(canvas.height()*sy));
            TEST_CHECK(canvas.rendererReady() && !image.isNull());
            const auto directory = qEnvironmentVariable("LIVEKIT_PRESENTATION_EVIDENCE_DIR");
            if (!directory.isEmpty()) {
                TEST_CHECK(QDir().mkpath(directory));
                TEST_CHECK(image.save(QDir(directory).filePath("after-gl-" + name + ".png")));
            }
            return image;
        }
        TEST_CHECK(canvas.rendererReady());
        const auto image = onDxOwner(canvas, [](livekit::render::BackendDevice& device) { return dxImage(device); });
        const auto directory = qEnvironmentVariable("LIVEKIT_PRESENTATION_EVIDENCE_DIR");
        if (!directory.isEmpty()) {
            TEST_CHECK(QDir().mkpath(directory));
            TEST_CHECK(image.save(QDir(directory).filePath("after-gpu-" + name + ".png")));
        }
        return image;
    }
    static QColor gpuTileCenter(MeetingUI::MeetingRoomWindow &window,
        const QImage &image, const MeetingUI::VideoTileWidget *tile) {
        // Sample away from the shared avatar, waiting text and status overlay.
        const auto point = tile->pos() + QPoint(tile->width() / 2, tile->height() / 4);
        return image.pixelColor(point.x() * image.width() / window._stageContainer->width(),
            point.y() * image.height() / window._stageContainer->height());
    }
    static void invite(MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(window._bottomBar);
        window._bottomBar->_inviteStream.fire({});
    }
    static void setInvitationNoticeEffect(
            MeetingUI::MeetingRoomWindow &window,
            MeetingUI::MeetingRoomWindow::InvitationNoticeEffect effect) {
        window._invitationNoticeEffect = std::move(effect);
    }
    static QString configuredToken(const MeetingUI::MeetingRoomWindow &window) {
        return window._config.token;
    }
    static void bindAccount(MeetingUI::MeetingRoomWindow &window, OpenMeeting::SessionManager &session) {
        window.setupCameraCompletionOwner(session);
    }
};

namespace {

void WindowPhase(const char *phase) {
    std::fprintf(stderr, "AK_WINDOW_PHASE %s\n", phase);
    std::fflush(stderr);
}

void WindowDrainNative(asio::io_context &io) { io.restart(); while (io.poll() != 0) {} }
void WindowDrainQt() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

livekit::proto::ParticipantUpdate WindowParticipant(
        const std::string &name,
        bool active = true,
        bool video = true,
        const std::string &participantSid = "PA_WINDOW",
        const std::string &trackSid = "TR_PA_WINDOW",
        const std::string &identity = "window-peer") {
    livekit::proto::ParticipantUpdate update;
    auto *participant = update.add_participants();
    participant->set_sid(participantSid); participant->set_identity(identity); participant->set_name(name);
    participant->set_state(active ? livekit::proto::ParticipantInfo::ACTIVE : livekit::proto::ParticipantInfo::DISCONNECTED);
    participant->mutable_permission()->set_can_subscribe(true);
    participant->mutable_permission()->set_can_publish(true);
    participant->mutable_permission()->set_can_publish_data(true);
    if (active && video) {
        auto *track = participant->add_tracks();
        track->set_sid(trackSid); track->set_name("window-video"); track->set_type(livekit::proto::TrackType::VIDEO);
    }
    return update;
}

class WindowValueObserver final : public livekit::RoomListener {
public:
    bool ConsumesParticipantEvents() const override { return true; }
    void OnParticipantEvent(const livekit::ParticipantEvent &event) override { events.push_back(event); }
    void OnConnected() override { ++connected; }
    void OnReconnecting() override { ++reconnecting; }
    void OnReconnected() override { ++reconnected; }
    livekit::ParticipantEvent latest(livekit::ParticipantEventKind kind) const {
        const auto found = std::find_if(events.rbegin(), events.rend(),
            [kind](const auto &event) { return event.kind == kind; });
        TEST_CHECK(found != events.rend());
        return *found;
    }
    std::vector<livekit::ParticipantEvent> events;
    int connected = 0;
    int reconnecting = 0;
    int reconnected = 0;
};

// A memory-only RTC source. Real VideoTrack, Room NativeVideoTrackSink,
// immutable frame conversion, Track subscription and CPU Window rendering run.
class WindowMemoryVideoSource : public webrtc::AdaptedVideoTrackSource {
public:
    SourceState state() const override { return kLive; }
    bool remote() const override { return true; }
    bool is_screencast() const override { return false; }
    std::optional<bool> needs_denoising() const override { return false; }
    void push(uint8_t luminance, int64_t timestamp) {
        TEST_CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
        auto buffer = webrtc::I420Buffer::Create(4, 4);
        std::memset(buffer->MutableDataY(), luminance, buffer->StrideY() * 4);
        std::memset(buffer->MutableDataU(), 128, buffer->StrideU() * 2);
        std::memset(buffer->MutableDataV(), 128, buffer->StrideV() * 2);
        OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(timestamp).build());
    }
};

struct WindowMedia {
    webrtc::scoped_refptr<WindowMemoryVideoSource> source;
    webrtc::scoped_refptr<webrtc::VideoTrack> rtc;
    std::shared_ptr<livekit::Track> track;
};

class WindowFixture final {
public:
    explicit WindowFixture(bool localConnectedPrecondition = true, bool authenticated = false)
        : session(OpenMeeting::SessionManagerTestAccess::create(
              std::make_unique<QSettings>(settingsDirectory.filePath("settings.ini"), QSettings::IniFormat), &httpClient)),
          room(livekit::Room::Create(io.get_executor())),
          runtime(std::make_shared<OpenMeeting::MeetingSessionRuntime>(io, 71, QStringLiteral("local-user"))),
          coordinator(OpenMeeting::MeetingCoordinatorTestAccess::create(*session)),
          observer(std::make_shared<WindowValueObserver>()) {
        TEST_CHECK(settingsDirectory.isValid());
        if (authenticated) session->loginAsGuest("Account fixture", "local-user");
        if (localConnectedPrecondition) {
            livekit::ParticipantSnapshotRoomTestAccess::establishLocalConnectedPrecondition(*room);
        }
        listener = OpenMeeting::MeetingCoordinatorTestAccess::bind(*coordinator, room, runtime);
        if (!localConnectedPrecondition) {
            OpenMeeting::MeetingCoordinatorTestAccess::prepareInMeetingEntry(*coordinator);
        }
        room->AddListener(listener); room->AddListener(observer);
        WindowPhase("fixture-constructed");
    }
    ~WindowFixture() {
        // Destruction is not a tested UI-effect boundary. Disconnect the actual
        // bindings only now, before Window's production leave emits meetingLeft.
        WindowPhase("cleanup-disconnect-window-bindings");
        if (window) QObject::disconnect(coordinator.get(), nullptr, window.get(), nullptr);
        WindowPhase("cleanup-window-reset");
        window.reset();
        WindowPhase("cleanup-remove-room-listeners");
        room->RemoveListener(listener); room->RemoveListener(observer);
        // This deterministic GUI fixture supplies an external io/runtime; it
        // does not install Coordinator's owned worker/context. Complete the
        // real queued departure cleanup while its QObject receiver is alive.
        // Owned-runtime stop/barrier/join ordering is tested separately by K.
        WindowPhase("cleanup-pending-owner-work");
        pump();
        WindowPhase("cleanup-coordinator-reset");
        coordinator.reset();
        WindowPhase("cleanup-event-values-clear");
        observer->events.clear();
        WindowPhase("cleanup-room-disconnect");
        room->Disconnect();
        WindowPhase("cleanup-native-drain");
        WindowDrainNative(io);
        WindowPhase("cleanup-qt-drain");
        WindowDrainQt();
        // These explicit releases retain the original member destruction order
        // while making the crashing lifetime boundary visible in the log.
        WindowPhase("cleanup-listener-reset");
        listener.reset();
        WindowPhase("cleanup-observer-reset");
        observer.reset();
        WindowPhase("cleanup-runtime-reset");
        runtime.reset();
        WindowPhase("cleanup-room-reset");
        room.reset();
        WindowPhase("cleanup-fixture-body-complete");
    }
    void pump() { WindowDrainNative(io); WindowDrainQt(); WindowDrainNative(io); WindowDrainQt(); }
    WindowMedia add(const std::string &name, const std::string &rtcId) {
        room->UpdateParticipantsForTesting(WindowParticipant(name));
        return attachExisting(rtcId);
    }
    WindowMedia attachExisting(const std::string &rtcId, bool drain = true,
                              const std::string &trackSid = "TR_PA_WINDOW",
                              const std::string &participantSid = "PA_WINDOW") {
        auto participant = room->remote_participants().at(participantSid);
        WindowMedia media;
        media.source = webrtc::make_ref_counted<WindowMemoryVideoSource>();
        media.rtc = webrtc::VideoTrack::Create(rtcId, media.source, webrtc::Thread::Current());
        TEST_CHECK(media.rtc);
        livekit::ParticipantSnapshotRoomTestAccess::attach(*room, participant, media.rtc, trackSid);
        media.track = participant->get_publication(trackSid)->track();
        TEST_CHECK(media.track && media.track->rtc_track().get() == media.rtc.get());
        if (drain) pump();
        return media;
    }
    void open() { window = ParticipantWindowTestAccess::create(coordinator); }
    void open(MeetingUI::MeetingRoomWindow::Config config) {
        window = ParticipantWindowTestAccess::create(coordinator, std::move(config));
    }

    QTemporaryDir settingsDirectory;
    OpenMeeting::OpenMeetingHttpClient httpClient;
    OpenMeeting::SessionManagerTestAccess::ScopedSession session;
    asio::io_context io;
    std::shared_ptr<livekit::Room> room;
    std::shared_ptr<OpenMeeting::MeetingSessionRuntime> runtime;
    std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator;
    std::shared_ptr<WindowValueObserver> observer;
    std::shared_ptr<livekit::RoomListener> listener;
    std::unique_ptr<MeetingUI::MeetingRoomWindow> window;
};

class ClipboardSnapshot final {
public:
    ClipboardSnapshot() : clipboard_(QApplication::clipboard()), snapshot_(std::make_unique<QMimeData>()) {
        TEST_CHECK(clipboard_);
        const auto *source = clipboard_->mimeData();
        if (!source) return;
        for (const auto &format : source->formats()) {
            snapshot_->setData(format, source->data(format));
        }
    }
    ~ClipboardSnapshot() {
        if (clipboard_) clipboard_->setMimeData(snapshot_.release());
    }

private:
    QClipboard *clipboard_ = nullptr;
    std::unique_ptr<QMimeData> snapshot_;
};

void PrSec005InvitationContract() {
    ClipboardSnapshot restoreClipboard;
    auto *clipboard = QApplication::clipboard();
    TEST_CHECK(clipboard);

    WindowFixture business;
    MeetingUI::MeetingRoomWindow::Config businessConfig;
    businessConfig.displayName = QStringLiteral("Business Invite");
    businessConfig.serverUrl = QStringLiteral(
        "wss://user:password-secret@example.invalid/livekit?loginToken=login-token-secret");
    businessConfig.token = QStringLiteral("participant-bearer-secret");
    businessConfig.meetingId = QStringLiteral("stale-config-meeting");
    businessConfig.invitationMode = MeetingUI::InvitationMode::BusinessMeetingId;
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *business.coordinator, OpenMeeting::MeetingState::InMeeting, QStringLiteral("business-current-001"));
    business.open(businessConfig);
    std::vector<bool> businessNotices;
    ParticipantWindowTestAccess::setInvitationNoticeEffect(*business.window,
        [&](bool success, const QString &, const QString &) { businessNotices.push_back(success); });
    const auto roomBeforeInvite = business.coordinator->room();
    const auto tokenBeforeInvite = ParticipantWindowTestAccess::configuredToken(*business.window);

    clipboard->setText(QStringLiteral("business-clipboard-sentinel"));
    ParticipantWindowTestAccess::invite(*business.window);
    const auto firstInvite = clipboard->text();
    TEST_CHECK(firstInvite.contains(QStringLiteral("business-current-001")));
    TEST_CHECK(!firstInvite.contains(QStringLiteral("stale-config-meeting")));
    TEST_CHECK(!firstInvite.contains(QStringLiteral("participant-bearer-secret")));
    TEST_CHECK(!firstInvite.contains(QStringLiteral("login-token-secret")));
    TEST_CHECK(!firstInvite.contains(QStringLiteral("password-secret")));
    TEST_CHECK(!firstInvite.contains(QStringLiteral("wss://")));
    TEST_CHECK(businessNotices == std::vector<bool>{true});
    TEST_CHECK(ParticipantWindowTestAccess::configuredToken(*business.window) == tokenBeforeInvite);
    TEST_CHECK(business.coordinator->room() == roomBeforeInvite);
    TEST_CHECK(business.coordinator->state() == OpenMeeting::MeetingState::InMeeting);

    businessNotices.clear();
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *business.coordinator, OpenMeeting::MeetingState::InMeeting, QStringLiteral("business-current-002"));
    ParticipantWindowTestAccess::invite(*business.window);
    const auto refreshedInvite = clipboard->text();
    TEST_CHECK(refreshedInvite.contains(QStringLiteral("business-current-002")));
    TEST_CHECK(!refreshedInvite.contains(QStringLiteral("business-current-001")));
    TEST_CHECK(businessNotices == std::vector<bool>{true});

    WindowFixture quick;
    MeetingUI::MeetingRoomWindow::Config quickConfig;
    quickConfig.displayName = QStringLiteral("Quick Invite");
    quickConfig.invitationMode = MeetingUI::InvitationMode::BusinessMeetingId;
    quick.open(quickConfig);
    std::vector<bool> quickNotices;
    ParticipantWindowTestAccess::setInvitationNoticeEffect(*quick.window,
        [&](bool success, const QString &, const QString &) { quickNotices.push_back(success); });
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *quick.coordinator, OpenMeeting::MeetingState::InMeeting, QString());
    clipboard->setText(QStringLiteral("quick-not-ready-sentinel"));
    ParticipantWindowTestAccess::invite(*quick.window);
    TEST_CHECK(clipboard->text() == QStringLiteral("quick-not-ready-sentinel"));
    TEST_CHECK(quickNotices == std::vector<bool>{false});

    quickNotices.clear();
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *quick.coordinator, OpenMeeting::MeetingState::InMeeting, QStringLiteral("quick-real-31415"));
    ParticipantWindowTestAccess::invite(*quick.window);
    TEST_CHECK(clipboard->text().contains(QStringLiteral("quick-real-31415")));
    TEST_CHECK(quickNotices == std::vector<bool>{true});

    quickNotices.clear();
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *quick.coordinator, OpenMeeting::MeetingState::Leaving, QStringLiteral("quick-real-31415"));
    clipboard->setText(QStringLiteral("quick-leaving-sentinel"));
    ParticipantWindowTestAccess::invite(*quick.window);
    TEST_CHECK(clipboard->text() == QStringLiteral("quick-leaving-sentinel"));
    TEST_CHECK(quickNotices == std::vector<bool>{false});

    for (const auto &invalidId : {QStringLiteral("invalid meeting"), QStringLiteral("invalid\nmeeting")}) {
        quickNotices.clear();
        OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
            *quick.coordinator, OpenMeeting::MeetingState::InMeeting, invalidId);
        clipboard->setText(QStringLiteral("invalid-id-sentinel"));
        ParticipantWindowTestAccess::invite(*quick.window);
        TEST_CHECK(clipboard->text() == QStringLiteral("invalid-id-sentinel"));
        TEST_CHECK(quickNotices == std::vector<bool>{false});
    }

    WindowFixture direct;
    MeetingUI::MeetingRoomWindow::Config directConfig;
    directConfig.displayName = QStringLiteral("Direct Invite");
    directConfig.serverUrl = QStringLiteral("ws://127.0.0.1:7880/private-path");
    directConfig.token = QStringLiteral("direct-bearer-secret");
    directConfig.meetingId = QStringLiteral("custom-direct-room");
    directConfig.invitationMode = MeetingUI::InvitationMode::Disabled;
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *direct.coordinator, OpenMeeting::MeetingState::InMeeting, QStringLiteral("livekit_room"));
    direct.open(directConfig);
    std::vector<bool> directNotices;
    ParticipantWindowTestAccess::setInvitationNoticeEffect(*direct.window,
        [&](bool success, const QString &, const QString &) { directNotices.push_back(success); });
    clipboard->setText(QStringLiteral("direct-clipboard-sentinel"));
    ParticipantWindowTestAccess::invite(*direct.window);
    TEST_CHECK(clipboard->text() == QStringLiteral("direct-clipboard-sentinel"));
    TEST_CHECK(directNotices == std::vector<bool>{false});

    directNotices.clear();
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *direct.coordinator, OpenMeeting::MeetingState::InMeeting, QStringLiteral("custom-direct-room"));
    ParticipantWindowTestAccess::invite(*direct.window);
    TEST_CHECK(clipboard->text() == QStringLiteral("direct-clipboard-sentinel"));
    TEST_CHECK(directNotices == std::vector<bool>{false});
    TEST_CHECK(ParticipantWindowTestAccess::configuredToken(*direct.window)
        == QStringLiteral("direct-bearer-secret"));

    WindowFixture clipboardReentrant;
    MeetingUI::MeetingRoomWindow::Config clipboardReentrantConfig;
    clipboardReentrantConfig.displayName = QStringLiteral("Clipboard Reentrant Invite");
    clipboardReentrantConfig.invitationMode = MeetingUI::InvitationMode::BusinessMeetingId;
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *clipboardReentrant.coordinator, OpenMeeting::MeetingState::InMeeting,
        QStringLiteral("clipboard-reentrant-001"));
    clipboardReentrant.open(clipboardReentrantConfig);
    QPointer<MeetingUI::MeetingRoomWindow> clipboardReentrantWindow = clipboardReentrant.window.get();
    bool clipboardNotice = false;
    ParticipantWindowTestAccess::setInvitationNoticeEffect(*clipboardReentrant.window,
        [&](bool success, const QString &, const QString &) { clipboardNotice = success; });
    bool clipboardDestroyedFromSignal = false;
    QMetaObject::Connection clipboardConnection;
    clipboardConnection = QObject::connect(clipboard, &QClipboard::dataChanged, [&] {
        QObject::disconnect(clipboardConnection);
        clipboardDestroyedFromSignal = clipboardDestroyedFromSignal || !!clipboardReentrant.window;
        if (auto *window = clipboardReentrant.window.release()) {
            window->deleteLater();
        }
    });
    ParticipantWindowTestAccess::invite(*clipboardReentrant.window);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QObject::disconnect(clipboardConnection);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    TEST_CHECK(clipboardDestroyedFromSignal);
    TEST_CHECK(clipboardReentrantWindow.isNull());
    TEST_CHECK(clipboard->text().contains(QStringLiteral("clipboard-reentrant-001")));
    TEST_CHECK(clipboardNotice);

    WindowFixture noticeReentrant;
    MeetingUI::MeetingRoomWindow::Config noticeReentrantConfig;
    noticeReentrantConfig.displayName = QStringLiteral("Notice Reentrant Invite");
    noticeReentrantConfig.invitationMode = MeetingUI::InvitationMode::BusinessMeetingId;
    OpenMeeting::MeetingCoordinatorTestAccess::setInvitationState(
        *noticeReentrant.coordinator, OpenMeeting::MeetingState::InMeeting,
        QStringLiteral("notice-reentrant-001"));
    noticeReentrant.open(noticeReentrantConfig);
    QPointer<MeetingUI::MeetingRoomWindow> noticeReentrantWindow = noticeReentrant.window.get();
    bool noticeDestroyedWindow = false;
    ParticipantWindowTestAccess::setInvitationNoticeEffect(*noticeReentrant.window,
        [&](bool success, const QString &, const QString &) {
            noticeDestroyedWindow = success && !!noticeReentrant.window;
            if (auto *window = noticeReentrant.window.release()) {
                window->deleteLater();
            }
        });
    ParticipantWindowTestAccess::invite(*noticeReentrant.window);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    TEST_CHECK(noticeDestroyedWindow);
    TEST_CHECK(noticeReentrantWindow.isNull());

    std::cout << "PR_SEC_005_INVITATION_EXECUTED=1 PASSED=1 FAILED=0" << std::endl;
}

void CheckWindowPeer(WindowFixture &fixture, const QString &name) {
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 1);
    auto *tile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
    TEST_CHECK(tile && tile->identity() == "window-peer" && tile->displayName() == name);
    const auto stats = ParticipantWindowTestAccess::statistics(*fixture.window);
    TEST_CHECK(stats.attached_track_count == 1 && stats.backend == livekit::render::VideoRenderSession::Backend::QtCpu);
}

// All server/client operations are driven on this test's GUI thread. This is
// real loopback WebSocket signalling, with no SFU, account, capture or device.
class WindowLoopbackServer final : public std::enable_shared_from_this<WindowLoopbackServer> {
    struct Connection {
        explicit Connection(asio::io_context &io) : socket(io) {}
        asio::ip::tcp::socket socket;
        std::deque<std::shared_ptr<std::vector<uint8_t>>> writes;
    };
public:
    explicit WindowLoopbackServer(asio::io_context &io)
        : io_(io), acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0)) {}
    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    void start() {
        auto self = shared_from_this();
        auto connection = std::make_shared<Connection>(io_);
        acceptor_.async_accept(connection->socket, [self, connection](std::error_code error) {
            if (!error) {
                self->connections_.push_back(connection);
                asio::co_spawn(self->io_, self->serve(connection),
                    [self, connection](std::exception_ptr failure) {
                        if (failure && !self->stopped_) self->protocolFailure = true;
                    });
            }
            if (self->acceptor_.is_open()) self->start();
        });
    }
    void closeActive() {
        for (const auto &connection : connections_) {
            std::error_code ignored;
            connection->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            connection->socket.close(ignored);
        }
    }
    void stop() {
        stopped_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        closeActive();
    }
    void sendParticipants(const livekit::proto::ParticipantUpdate &update) {
        livekit::proto::SignalResponse response;
        *response.mutable_update() = update;
        for (const auto &connection : connections_) {
            if (connection->socket.is_open()) send(connection, response);
        }
    }
    bool rejectResume = false;
    bool addRestartTrack = false;
    bool protocolFailure = false;
    std::string restartRoomSid = "RM_WINDOW_LOOPBACK";
    std::string restartParticipantSid = "PA_WINDOW";
    std::string restartParticipantIdentity = "window-peer";
    int joins = 0;
    int resumes = 0;
    int rejectedResumes = 0;
    int syncStates = 0;
    std::vector<livekit::proto::SyncState> syncStateMessages;
    std::vector<livekit::proto::UpdateSubscription> subscriptionMessages;
    std::vector<std::string> subscriptionWireOrder;
private:
    void send(const std::shared_ptr<Connection> &connection, const livekit::proto::SignalResponse &response) {
        std::string payload;
        TEST_CHECK(response.SerializeToString(&payload));
        sendFrame(connection, 2, payload);
    }
    void sendFrame(const std::shared_ptr<Connection> &connection, uint8_t opcode, const std::string &payload) {
        TEST_CHECK(payload.size() <= 65535);
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        bytes->push_back(static_cast<uint8_t>(0x80 | opcode));
        if (payload.size() < 126) bytes->push_back(static_cast<uint8_t>(payload.size()));
        else {
            bytes->push_back(126);
            bytes->push_back(static_cast<uint8_t>(payload.size() >> 8));
            bytes->push_back(static_cast<uint8_t>(payload.size()));
        }
        bytes->insert(bytes->end(), payload.begin(), payload.end());
        const bool idle = connection->writes.empty();
        connection->writes.push_back(std::move(bytes));
        if (idle) writeNext(connection);
    }
    void writeNext(const std::shared_ptr<Connection> &connection) {
        const auto bytes = connection->writes.front();
        auto self = shared_from_this();
        asio::async_write(connection->socket, asio::buffer(*bytes),
            [self, connection, bytes](std::error_code error, std::size_t) {
                if (error) { connection->writes.clear(); return; }
                connection->writes.pop_front();
                if (!connection->writes.empty()) self->writeNext(connection);
            });
    }
    asio::awaitable<void> serve(std::shared_ptr<Connection> connection) {
        std::error_code error;
        asio::streambuf http;
        co_await asio::async_read_until(connection->socket, http, "\r\n\r\n",
            asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return;
        std::istream input(&http);
        std::string request, header, key;
        std::getline(input, request);
        while (std::getline(input, header) && header != "\r") {
            if (header.rfind("Sec-WebSocket-Key:", 0) != 0) continue;
            key = header.substr(18);
            while (!key.empty() && key.front() == ' ') key.erase(key.begin());
            if (!key.empty() && key.back() == '\r') key.pop_back();
        }
        if (key.empty()) { protocolFailure = true; co_return; }
        const auto accept = QCryptographicHash::hash(
            QByteArray::fromStdString(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
            QCryptographicHash::Sha1).toBase64().toStdString();
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        co_await asio::async_write(connection->socket, asio::buffer(response),
            asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return;
        livekit::proto::SignalResponse initial;
        if (request.find("reconnect=1") != std::string::npos) {
            ++resumes;
            if (rejectResume) {
                ++rejectedResumes;
                connection->socket.close(error);
                co_return;
            }
            initial.mutable_reconnect()->mutable_client_configuration()->set_resume_connection(
                livekit::proto::ClientConfigSetting::ENABLED);
        } else {
            ++joins;
            auto *join = initial.mutable_join();
            join->set_ping_interval(10); join->set_ping_timeout(20);
            join->mutable_room()->set_sid(
                joins == 1 ? "RM_WINDOW_LOOPBACK" : restartRoomSid);
            join->mutable_room()->set_name("window-loopback");
            auto *local = join->mutable_participant();
            local->set_sid("PA_WINDOW_LOCAL"); local->set_identity("local-user");
            local->set_name("local-user"); local->set_state(livekit::proto::ParticipantInfo::ACTIVE);
            local->mutable_permission()->set_can_subscribe(true);
            local->mutable_permission()->set_can_publish(true);
            local->mutable_permission()->set_can_publish_data(true);
            *join->add_other_participants() = WindowParticipant(
                joins == 1 ? "join-window-peer" : "restart-window-peer",
                true,
                true,
                joins == 1 ? "PA_WINDOW" : restartParticipantSid,
                "TR_PA_WINDOW",
                joins == 1 ? "window-peer" : restartParticipantIdentity).participants(0);
            if (joins > 1 && addRestartTrack) {
                auto *track = join->mutable_other_participants(0)->add_tracks();
                track->set_sid("TR_PA_WINDOW_NEW");
                track->set_name("window-video-new");
                track->set_type(livekit::proto::TrackType::VIDEO);
            }
            join->mutable_client_configuration()->set_resume_connection(livekit::proto::ClientConfigSetting::ENABLED);
        }
        send(connection, initial);
        while (connection->socket.is_open()) {
            std::array<uint8_t, 2> frameHeader{};
            co_await asio::async_read(connection->socket, asio::buffer(frameHeader),
                asio::redirect_error(asio::use_awaitable, error));
            if (error) co_return;
            const uint8_t opcode = frameHeader[0] & 0x0f;
            const bool masked = (frameHeader[1] & 0x80) != 0;
            uint64_t length = frameHeader[1] & 0x7f;
            if (length >= 126) {
                const auto count = length == 126 ? 2u : 8u;
                std::array<uint8_t, 8> extended{};
                co_await asio::async_read(connection->socket, asio::buffer(extended.data(), count),
                    asio::redirect_error(asio::use_awaitable, error));
                if (error) co_return;
                length = 0;
                for (unsigned i = 0; i < count; ++i) length = (length << 8) | extended[i];
            }
            if (length > 65535 || !(frameHeader[0] & 0x80)) { protocolFailure = true; co_return; }
            std::vector<uint8_t> body(static_cast<std::size_t>(length) + (masked ? 4 : 0));
            if (!body.empty()) {
                co_await asio::async_read(connection->socket, asio::buffer(body),
                    asio::redirect_error(asio::use_awaitable, error));
                if (error) co_return;
            }
            std::string payload(static_cast<std::size_t>(length), '\0');
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(body[i + (masked ? 4 : 0)] ^ (masked ? body[i % 4] : 0));
            }
            if (opcode == 8) co_return;
            if (opcode == 9) { sendFrame(connection, 10, payload); continue; }
            if (opcode == 10) continue;
            livekit::proto::SignalRequest signal;
            if (opcode != 2 || !signal.ParseFromString(payload)) { protocolFailure = true; co_return; }
            if (signal.has_sync_state()) {
                ++syncStates;
                syncStateMessages.push_back(signal.sync_state());
                subscriptionWireOrder.push_back(std::string("sync:") +
                    (signal.sync_state().subscription().subscribe() ? "true" : "false"));
            }
            if (signal.has_subscription()) {
                subscriptionMessages.push_back(signal.subscription());
                subscriptionWireOrder.push_back(std::string("update:") +
                    (signal.subscription().subscribe() ? "true" : "false"));
            }
            if (signal.has_ping_req()) {
                livekit::proto::SignalResponse pong;
                pong.mutable_pong_resp()->set_last_ping_timestamp(signal.ping_req().timestamp());
                send(connection, pong);
            }
        }
    }
    asio::io_context &io_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<Connection>> connections_;
    bool stopped_ = false;
};

struct WindowServerGuard {
    std::shared_ptr<WindowLoopbackServer> server;
    ~WindowServerGuard() { server->stop(); }
};

template <typename Predicate>
void WindowPumpUntil(WindowFixture &fixture, Predicate complete, const char *boundary) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!complete() && std::chrono::steady_clock::now() < deadline) {
        fixture.io.restart();
        fixture.io.run_one_for(std::chrono::milliseconds(10));
        WindowDrainQt();
    }
    if (!complete()) std::cerr << "AK_WINDOW_TIMEOUT boundary=" << boundary << std::endl;
    TEST_CHECK(complete());
    fixture.pump();
}

void WindowConnect(WindowFixture &fixture,
                   const std::shared_ptr<WindowLoopbackServer> &server,
                   bool autoSubscribe = true,
                   int expectedJoins = 1) {
    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    const std::string token = "ida2-window-local-test-token";
    livekit::SignalOptions options;
    options.allow_insecure_transport = true;
    options.auto_subscribe = autoSubscribe;
    options.single_peer_connection = false;
    options.create_webrtc_pc = false;
    options.timeouts.reconnect_attempt = std::chrono::milliseconds(800);
    options.timeouts.reconnect_total = std::chrono::seconds(8);
    bool complete = false;
    std::exception_ptr failure;
    // URL/token/options remain in this scope until the actual awaitable ends.
    asio::co_spawn(fixture.io, fixture.room->ConnectAsync(url, token, options),
        [&](std::exception_ptr error) { failure = error; complete = true; });
    WindowPumpUntil(fixture, [&] { return complete; }, "initial-connect");
    if (failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception &error) { std::cerr << "AK_WINDOW_CONNECT " << error.what() << std::endl; }
    }
    TEST_CHECK(!failure && fixture.room->connection_state() == livekit::ConnectionState::Connected);
    TEST_CHECK(server->joins == expectedJoins && !server->protocolFailure &&
        fixture.observer->connected == expectedJoins);
    // Network Connect is real. Local capture/HTTP startup is deliberately a
    // pre-established premise; this does not pretend to validate devices.
    OpenMeeting::MeetingCoordinatorTestAccess::commitLocalStartupPrecondition(*fixture.coordinator);
    fixture.pump();
}

void AkWindowAliveLate() {
    WindowPhase("alive-late-begin");
    WindowFixture fixture;
    auto media = fixture.add("first-window-peer", "window-live");
    fixture.open(); CheckWindowPeer(fixture, "first-window-peer");
    const auto before = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    media.source->push(80, 1000);
    ParticipantWindowTestAccess::render(*fixture.window);
    const auto frame = ParticipantWindowTestAccess::frame(*fixture.window, "window-peer");
    TEST_CHECK(!frame.isNull() && frame.width() == 4 && frame.height() == 4);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == before + 1);
    std::cout << "AK_CASE_F4 alive-late/real-VideoTrack/Room/adapter/Window/CPU-frame PASS" << std::endl;
}

void AkWindowOldTrack() {
    WindowPhase("old-track-begin");
    WindowFixture fixture;
    auto oldMedia = fixture.add("old-window-peer", "window-old");
    fixture.open(); CheckWindowPeer(fixture, "old-window-peer");
    auto oldAvailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("old-window-peer", true, false));
    fixture.pump();
    auto oldUnavailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackUnavailable);
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("old-window-peer", false));
    fixture.pump();
    auto oldDeparture = fixture.observer->latest(livekit::ParticipantEventKind::Departure);
    auto successor = fixture.add("successor-window-peer", "window-successor");
    TEST_CHECK(oldMedia.track != successor.track && oldMedia.track->sid() == successor.track->sid());
    CheckWindowPeer(fixture, "successor-window-peer");
    auto *successorTile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
    successor.source->push(200, 2000);
    ParticipantWindowTestAccess::render(*fixture.window);
    const auto newFrame = ParticipantWindowTestAccess::frame(*fixture.window, "window-peer");
    TEST_CHECK(!newFrame.isNull());
    const auto delivered = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    TEST_CHECK(!livekit::IsParticipantTicketActive(oldAvailable.participant.ticket, oldAvailable.participant.key));
    // Replay the actual earlier native values through the real queued adapter;
    // no invented key, sequence, TrackAvailable or cleanup payload.
    fixture.listener->OnParticipantEvent(oldAvailable);
    fixture.listener->OnParticipantEvent(oldUnavailable);
    fixture.listener->OnParticipantEvent(oldDeparture);
    WindowDrainQt();
    CheckWindowPeer(fixture, "successor-window-peer");
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer") == successorTile);
    oldMedia.source->push(30, 3000);
    const uint8_t y[16] = {30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30};
    const uint8_t uv[4] = {128,128,128,128};
    oldMedia.track->notifyI420VideoFrame(livekit::render::OwnedI420Frame::CopyFromPlanes(4, 4, y, 4, uv, 2, uv, 2, 4000));
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == delivered);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer") == newFrame);
    successor.source->push(90, 5000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == delivered + 1);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer") != newFrame);
    CheckWindowPeer(fixture, "successor-window-peer");
    std::cout << "AK_CASE_F5 same-SID-successor/old-values+frames/no-tile-or-renderer-pollution PASS" << std::endl;
}

void AkWindowRetiredPresentation(bool inMeetingEntry) {
    WindowPhase(inMeetingEntry ? "retired-inmeeting-begin" : "retired-late-begin");
    WindowFixture fixture;
    auto media = fixture.add("retired-window-peer", "window-retired");
    WindowPhase("retired-media-ready");
    const auto old = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    const auto savedPresentations = fixture.coordinator->participantPresentations();
    TEST_CHECK(savedPresentations.size() == 1 && savedPresentations.front().videoTracks.size() == 1);
    const auto &savedPresentation = savedPresentations.front();
    TEST_CHECK(fixture.coordinator->isParticipantPresentationCurrent(savedPresentation,
        &savedPresentation.videoTracks.front()));
    if (inMeetingEntry) {
        OpenMeeting::MeetingCoordinatorTestAccess::prepareInMeetingEntry(*fixture.coordinator);
        fixture.open();
        TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 0);
    }
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("retired-window-peer", false));
    WindowDrainNative(fixture.io); // Queue the real tombstone; deliberately no Qt drain.
    WindowPhase("retired-native-tombstone-queued");
    TEST_CHECK(!livekit::IsParticipantTicketActive(old.participant.ticket, old.participant.key));
    TEST_CHECK(fixture.room->remote_participants().empty());
    TEST_CHECK(!fixture.coordinator->isParticipantPresentationCurrent(savedPresentation));
    TEST_CHECK(!fixture.coordinator->isParticipantPresentationCurrent(savedPresentation,
        &savedPresentation.videoTracks.front()));
    TEST_CHECK(fixture.coordinator->participantPresentations().empty());
    if (inMeetingEntry) OpenMeeting::MeetingCoordinatorTestAccess::enterInMeeting(*fixture.coordinator);
    else fixture.open();
    WindowPhase("retired-window-entry-returned");
    const auto tiles = ParticipantWindowTestAccess::tileCount(*fixture.window);
    const auto attached = ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count;
    std::cout << "AK_CASE_F4 retired-before-Qt/inmeeting-entry=" << inMeetingEntry
              << " stale-tiles=" << tiles << " stale-renderer-bindings=" << attached << std::endl;
    TEST_CHECK(tiles == 0 && attached == 0);
    WindowDrainQt();
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 0);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0);
    std::cout << "AK_CASE_F4 retired-before-Qt/inmeeting-entry=" << inMeetingEntry << " PASS" << std::endl;
}

void AkWindowInitialRoster() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    fixture.open();
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 0);
    bool deltaInjected = false;
    fixture.room->SetLogHandler([&](const std::string &, const std::string &tag, const std::string &) {
        if (tag != "SUB_PERM" || deltaInjected) return;
        TEST_CHECK(fixture.room->connection_state() == livekit::ConnectionState::Connecting);
        TEST_CHECK(fixture.observer->events.empty());
        livekit::proto::SignalResponse delta;
        *delta.mutable_update() = WindowParticipant("delta-window-peer");
        auto *second = delta.mutable_update()->add_participants();
        second->set_sid("PA_WINDOW_B"); second->set_identity("window-peer-b");
        second->set_name("delta-peer-b"); second->set_state(livekit::proto::ParticipantInfo::ACTIVE);
        // Deliberate deterministic injection at the real signal-handler layer:
        // Join traverses the socket; this delta is not claimed to be wire I/O.
        fixture.room->HandleSignalMessageForTesting(delta);
        TEST_CHECK(fixture.room->remote_participants().size() == 1);
        TEST_CHECK(fixture.room->remote_participants().at("PA_WINDOW")->name() == "join-window-peer");
        deltaInjected = true;
    });
    WindowConnect(fixture, server);
    fixture.room->SetLogHandler({});
    TEST_CHECK(deltaInjected && fixture.room->remote_participants().size() == 2);
    uint64_t rosterSequence = 0, deltaSequence = 0, secondSequence = 0;
    for (const auto &event : fixture.observer->events) {
        if (event.kind != livekit::ParticipantEventKind::Upsert || event.participant.is_local) continue;
        if (event.participant.state.name == "join-window-peer") rosterSequence = event.event_sequence;
        if (event.participant.state.name == "delta-window-peer") deltaSequence = event.event_sequence;
        if (event.participant.state.name == "delta-peer-b") secondSequence = event.event_sequence;
    }
    TEST_CHECK(rosterSequence != 0 && rosterSequence < deltaSequence && rosterSequence < secondSequence);
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 2);
    auto *first = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
    auto *second = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer-b");
    TEST_CHECK(first && first->displayName() == "delta-window-peer");
    TEST_CHECK(second && second->displayName() == "delta-peer-b");
    auto media = fixture.attachExisting("window-initial-wire");
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 1);
    media.source->push(70, 10000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::frame(*fixture.window, "window-peer").isNull());
    server->sendParticipants(WindowParticipant("wire-window-peer"));
    WindowPumpUntil(fixture, [&] {
        const auto *tile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
        return tile && tile->displayName() == "wire-window-peer";
    }, "initial-live-wire-delta");
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer") == first);
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer-b") == second);
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 2 && !server->protocolFailure);
    std::cout << "AK_CASE_F1 real-loopback-Join/Connecting-handler-injection/roster-before-delta/"
                 "wire-update/actual-Window PASS" << std::endl;
}

void AkWindowSoftResume() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    auto media = fixture.attachExisting("window-resume");
    const auto participant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto initial = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    fixture.open(); CheckWindowPeer(fixture, "join-window-peer");
    auto *tile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
    media.source->push(60, 20000); ParticipantWindowTestAccess::render(*fixture.window);
    const auto before = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    server->closeActive();
    WindowPumpUntil(fixture, [&] {
        return fixture.observer->reconnected == 1 && server->syncStates == 1 &&
            fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting;
    }, "soft-resume");
    TEST_CHECK(server->joins == 1 && server->resumes == 1 && server->rejectedResumes == 0);
    TEST_CHECK(fixture.observer->reconnecting == 1 && fixture.observer->connected == 1);
    TEST_CHECK(fixture.room->connection_state() == livekit::ConnectionState::Connected);
    TEST_CHECK(fixture.room->remote_participants().at("PA_WINDOW") == participant);
    TEST_CHECK(participant->get_publication("TR_PA_WINDOW")->track() == media.track);
    const auto resumed = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    TEST_CHECK(resumed.participant.key == initial.participant.key && resumed.track_key == initial.track_key);
    TEST_CHECK(livekit::IsParticipantTicketActive(initial.participant.ticket, initial.participant.key));
    TEST_CHECK(livekit::IsTrackTicketActive(initial.track_ticket, initial.track_key));
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer") == tile);
    CheckWindowPeer(fixture, "join-window-peer");
    server->sendParticipants(WindowParticipant("resumed-window-peer"));
    WindowPumpUntil(fixture, [&] {
        const auto *currentTile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
        return currentTile && currentTile->displayName() == "resumed-window-peer";
    }, "soft-resume-wire-state");
    media.source->push(140, 21000); ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == before + 1);
    CheckWindowPeer(fixture, "resumed-window-peer");
    TEST_CHECK(!server->protocolFailure);
    std::cout << "AK_CASE_F2 real-TCP-drop/resume/SyncState/same-R-P-Track/one-tile-one-binding/"
                 "new-frame PASS" << std::endl;
}

void AkWindowFullRestart() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    auto firstMedia = fixture.attachExisting("window-before-unpublish");
    fixture.open(); CheckWindowPeer(fixture, "join-window-peer");
    // Retain a real earlier unavailable value, not a synthesized tombstone.
    // Its Participant is A1; its publication predates the active full-restart track.
    server->sendParticipants(WindowParticipant("join-window-peer", true, false));
    WindowPumpUntil(fixture, [&] {
        return ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0;
    }, "pre-restart-wire-unpublish");
    const auto oldUnavailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackUnavailable);
    server->sendParticipants(WindowParticipant("join-window-peer"));
    WindowPumpUntil(fixture, [&] {
        return fixture.room->remote_participants().at("PA_WINDOW")->get_publication("TR_PA_WINDOW") != nullptr;
    }, "pre-restart-wire-republish");
    auto oldMedia = fixture.attachExisting("window-full-old");
    const auto oldParticipant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto oldAvailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    const auto retainedOldMembership = oldAvailable.participant.ticket.lock();
    TEST_CHECK(retainedOldMembership && retainedOldMembership->active.load());
    CheckWindowPeer(fixture, "join-window-peer");
    server->rejectResume = true;
    server->closeActive();
    WindowPumpUntil(fixture, [&] {
        return server->joins == 2 && fixture.observer->reconnected == 1 &&
            fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting;
    }, "full-restart-after-rejected-resume");
    TEST_CHECK(server->resumes == 1 && server->rejectedResumes == 1 && server->syncStates == 0);
    // The full restart suppresses OnConnected, but must still deliver roster.
    TEST_CHECK(fixture.observer->connected == 1 && fixture.observer->reconnecting == 1);
    TEST_CHECK(fixture.room->connection_state() == livekit::ConnectionState::Connected);
    const auto successor = fixture.room->remote_participants().at("PA_WINDOW");
    TEST_CHECK(successor != oldParticipant && successor->sid() == oldParticipant->sid());
    TEST_CHECK(successor->identity() == oldParticipant->identity());
    TEST_CHECK(!retainedOldMembership->active.load());
    TEST_CHECK(!livekit::IsParticipantTicketActive(oldAvailable.participant.ticket, oldAvailable.participant.key));
    auto newMedia = fixture.attachExisting("window-full-new");
    const auto newAvailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    TEST_CHECK(newAvailable.participant.key.native_room_generation > oldAvailable.participant.key.native_room_generation);
    TEST_CHECK(newAvailable.participant.key.incarnation != oldAvailable.participant.key.incarnation);
    TEST_CHECK(newAvailable.track_key != oldAvailable.track_key && newMedia.track != oldMedia.track);
    CheckWindowPeer(fixture, "restart-window-peer");
    auto *successorTile = ParticipantWindowTestAccess::tile(*fixture.window, "window-peer");
    newMedia.source->push(170, 30000); ParticipantWindowTestAccess::render(*fixture.window);
    const auto frame = ParticipantWindowTestAccess::frame(*fixture.window, "window-peer");
    const auto before = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    fixture.listener->OnParticipantEvent(oldAvailable);
    fixture.listener->OnParticipantEvent(oldUnavailable);
    WindowDrainQt();
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer") == successorTile);
    CheckWindowPeer(fixture, "restart-window-peer");
    firstMedia.source->push(20, 31000);
    oldMedia.source->push(30, 32000);
    const uint8_t y[16] = {30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30};
    const uint8_t uv[4] = {128,128,128,128};
    oldMedia.track->notifyI420VideoFrame(livekit::render::OwnedI420Frame::CopyFromPlanes(4, 4, y, 4, uv, 2, uv, 2, 33000));
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == before);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer") == frame);
    newMedia.source->push(90, 34000); ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == before + 1);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer") != frame);
    TEST_CHECK(!server->protocolFailure);
    std::cout << "AK_CASE_F3 real-resume-rejection/full-Join/roster/new-R-P/old-Ticket-inactive/"
                 "old-Qt-values+frames-rejected/new-frame PASS" << std::endl;
}

bool SubscriptionContains(const livekit::proto::UpdateSubscription &subscription,
                          const std::string &participantSid,
                          const std::string &trackSid) {
    const bool flat = std::find(subscription.track_sids().begin(),
        subscription.track_sids().end(), trackSid) != subscription.track_sids().end();
    const bool scoped = std::any_of(
        subscription.participant_tracks().begin(),
        subscription.participant_tracks().end(),
        [&](const auto &participant) {
            return participant.participant_sid() == participantSid &&
                std::find(participant.track_sids().begin(),
                    participant.track_sids().end(), trackSid) !=
                    participant.track_sids().end();
        });
    return flat && scoped;
}

bool HasSubscriptionSince(
        const WindowLoopbackServer &server,
        std::size_t begin,
        const std::string &participantSid,
        const std::string &trackSid,
        bool subscribed) {
    return std::any_of(
        server.subscriptionMessages.begin() +
            std::min(begin, server.subscriptionMessages.size()),
        server.subscriptionMessages.end(),
        [&](const auto &message) {
            return message.subscribe() == subscribed &&
                SubscriptionContains(message, participantSid, trackSid);
        });
}

void GapWindowSoftResumeUnsubscribe() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    auto media = fixture.attachExisting("gap-soft-resume");
    const auto participant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto publication = participant->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(publication && publication->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] {
        return !server->subscriptionMessages.empty() &&
            !server->subscriptionMessages.back().subscribe() &&
            SubscriptionContains(server->subscriptionMessages.back(),
                "PA_WINDOW", "TR_PA_WINDOW");
    }, "gap-soft-unsubscribe-wire");
    TEST_CHECK(!publication->is_subscribed());
    TEST_CHECK(!publication->track()->rtc_track());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);

    server->closeActive();
    WindowPumpUntil(fixture, [&] {
        return fixture.observer->reconnected == 1 && server->syncStates == 1;
    }, "gap-soft-resume");
    TEST_CHECK(server->syncStateMessages.size() == 1);
    const auto &subscription = server->syncStateMessages.front().subscription();
    TEST_CHECK(!subscription.subscribe());
    TEST_CHECK(SubscriptionContains(subscription, "PA_WINDOW", "TR_PA_WINDOW"));
    TEST_CHECK(publication->is_subscribed() == false);
    TEST_CHECK(!publication->track()->rtc_track());
    media.source->push(115, 41000);
    TEST_CHECK(!media.track->rtc_track());
    TEST_CHECK(!server->protocolFailure);
    std::cout << "GAP_P1_03_CASE_4 real-soft-resume/SyncState/unsubscribe/no-sink PASS" << std::endl;
}

void GapWindowFullRestartUnsubscribe() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    const auto originalParticipant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto original = originalParticipant->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(original && original->SetSubscribed(true));
    TEST_CHECK(original->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] {
        return !server->subscriptionMessages.empty() &&
            !server->subscriptionMessages.back().subscribe();
    }, "gap-full-unsubscribe-wire");

    const auto restartMessages = server->subscriptionMessages.size();
    server->addRestartTrack = true;
    server->rejectResume = true;
    server->closeActive();
    WindowPumpUntil(fixture, [&] {
        return server->joins == 2 && fixture.observer->reconnected == 1 &&
            fixture.room->connection_state() == livekit::ConnectionState::Connected;
    }, "gap-full-restart");
    const auto successorParticipant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto successor = successorParticipant->get_remote_publication("TR_PA_WINDOW");
    const auto newTrack = successorParticipant->get_remote_publication("TR_PA_WINDOW_NEW");
    TEST_CHECK(successorParticipant != originalParticipant);
    TEST_CHECK(successorParticipant->sid() == originalParticipant->sid());
    TEST_CHECK(successorParticipant->identity() == originalParticipant->identity());
    TEST_CHECK(successor && successor != original && !successor->is_subscribed());
    TEST_CHECK(newTrack && newTrack->is_subscribed());
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, restartMessages,
                   "PA_WINDOW", "TR_PA_WINDOW", false) &&
            HasSubscriptionSince(*server, restartMessages,
                   "PA_WINDOW", "TR_PA_WINDOW_NEW", true);
    }, "gap-full-restored-wire");

    auto source = webrtc::make_ref_counted<WindowMemoryVideoSource>();
    auto rtc = webrtc::VideoTrack::Create(
        "gap-full-late", source, webrtc::Thread::Current());
    TEST_CHECK(rtc);
    livekit::ParticipantSnapshotRoomTestAccess::attach(
        *fixture.room, successorParticipant, rtc, "TR_PA_WINDOW");
    fixture.pump();
    TEST_CHECK(!successor->track()->rtc_track());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);

    // The first control on the new object must be accepted even though the
    // logical intent retained the old object's last request sequence.
    const auto successorMessages = server->subscriptionMessages.size();
    TEST_CHECK(successor->SetSubscribed(true));
    TEST_CHECK(successor->is_subscribed());
    TEST_CHECK(!original->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, successorMessages,
            "PA_WINDOW", "TR_PA_WINDOW", true);
    }, "gap-full-successor-first-control");
    TEST_CHECK(!HasSubscriptionSince(*server, successorMessages,
        "PA_WINDOW", "TR_PA_WINDOW", false));
    auto restored = fixture.attachExisting("gap-full-successor-controlled");
    fixture.open();
    CheckWindowPeer(fixture, "restart-window-peer");
    restored.source->push(120, 1000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::frame(*fixture.window, "window-peer").isNull());

    const auto unsubscribeMessages = server->subscriptionMessages.size();
    TEST_CHECK(successor->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, unsubscribeMessages,
            "PA_WINDOW", "TR_PA_WINDOW", false);
    }, "gap-full-successor-second-control");
    TEST_CHECK(!successor->is_subscribed() && !successor->track()->rtc_track());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0);
    TEST_CHECK(!server->protocolFailure);
    std::cout << "GAP_P1_03_CASE_5 real-full-restart/restored-false/successor-controls/old-owner-rejected PASS" << std::endl;
}

void GapWindowOrderedSenderAndResumeSnapshot() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    const auto participant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto publication = participant->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(publication);
    fixture.pump();

    asio::steady_timer sendGate(fixture.io);
    sendGate.expires_at(std::chrono::steady_clock::time_point::max());
    bool sendEntered = false;
    bool pauseNextSend = true;
    livekit::ParticipantSnapshotRoomTestAccess::setSubscriptionHooks(
        *fixture.room,
        [&](bool, uint64_t) -> asio::awaitable<void> {
            if (!pauseNextSend) co_return;
            pauseNextSend = false;
            sendEntered = true;
            std::error_code ignored;
            co_await sendGate.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        },
        {});
    const auto orderedBegin = server->subscriptionMessages.size();
    TEST_CHECK(publication->SetSubscribed(true));
    WindowPumpUntil(fixture, [&] { return sendEntered; }, "gap-sender-true-paused");
    TEST_CHECK(publication->SetSubscribed(false));
    std::error_code ignored;
    sendGate.cancel(ignored);
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, orderedBegin,
                   "PA_WINDOW", "TR_PA_WINDOW", true) &&
            HasSubscriptionSince(*server, orderedBegin,
                   "PA_WINDOW", "TR_PA_WINDOW", false);
    }, "gap-sender-ordered-latest");
    std::vector<bool> ordered;
    for (std::size_t i = orderedBegin; i < server->subscriptionMessages.size(); ++i) {
        const auto &message = server->subscriptionMessages[i];
        if (SubscriptionContains(message, "PA_WINDOW", "TR_PA_WINDOW")) {
            ordered.push_back(message.subscribe());
        }
    }
    TEST_CHECK(ordered.size() >= 2 && ordered[ordered.size() - 2] && !ordered.back());

    livekit::ParticipantSnapshotRoomTestAccess::clearSubscriptionHooks(*fixture.room);
    asio::steady_timer syncGate(fixture.io);
    syncGate.expires_at(std::chrono::steady_clock::time_point::max());
    bool syncEntered = false;
    livekit::proto::SyncState capturedSync;
    livekit::ParticipantSnapshotRoomTestAccess::setSubscriptionHooks(
        *fixture.room,
        {},
        [&](const livekit::proto::SyncState &snapshot) -> asio::awaitable<void> {
            capturedSync = snapshot;
            syncEntered = true;
            std::error_code waitError;
            co_await syncGate.async_wait(
                asio::redirect_error(asio::use_awaitable, waitError));
        });
    const auto resumeWireBegin = server->subscriptionWireOrder.size();
    const auto resumeMessageBegin = server->subscriptionMessages.size();
    server->closeActive();
    WindowPumpUntil(fixture, [&] { return syncEntered; }, "gap-sync-snapshot-paused");
    TEST_CHECK(!capturedSync.subscription().subscribe());
    TEST_CHECK(SubscriptionContains(
        capturedSync.subscription(), "PA_WINDOW", "TR_PA_WINDOW"));
    TEST_CHECK(publication->SetSubscribed(true));
    syncGate.cancel(ignored);
    WindowPumpUntil(fixture, [&] {
        return fixture.observer->reconnected == 1 && server->syncStates == 1 &&
            HasSubscriptionSince(*server, resumeMessageBegin,
                "PA_WINDOW", "TR_PA_WINDOW", true);
    }, "gap-sync-latest-delta");
    TEST_CHECK(server->subscriptionWireOrder.size() >= resumeWireBegin + 2);
    TEST_CHECK(server->subscriptionWireOrder[resumeWireBegin] == "sync:false");
    TEST_CHECK(std::find(
        server->subscriptionWireOrder.begin() + resumeWireBegin + 1,
        server->subscriptionWireOrder.end(),
        "update:true") != server->subscriptionWireOrder.end());
    TEST_CHECK(publication->is_subscribed());
    livekit::ParticipantSnapshotRoomTestAccess::clearSubscriptionHooks(*fixture.room);
    TEST_CHECK(!server->protocolFailure);
    std::cout << "GAP_P1_03_CASE_6 sender-order/SyncState-snapshot/latest-revision PASS" << std::endl;
}

void GapWindowDefaultFalseSoftResume() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server, false);
    const auto participant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto publication = participant->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(publication && !publication->is_subscribed());
    TEST_CHECK(publication->SetSubscribed(true));
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, 0,
            "PA_WINDOW", "TR_PA_WINDOW", true);
    }, "gap-default-false-explicit-true");
    auto media = fixture.attachExisting("gap-default-false");
    fixture.open();
    CheckWindowPeer(fixture, "join-window-peer");
    const auto bindingCount =
        livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room);

    server->closeActive();
    WindowPumpUntil(fixture, [&] {
        return fixture.observer->reconnected == 1 && server->syncStates == 1;
    }, "gap-default-false-resume");
    const auto &subscription = server->syncStateMessages.back().subscription();
    TEST_CHECK(subscription.subscribe());
    TEST_CHECK(SubscriptionContains(subscription, "PA_WINDOW", "TR_PA_WINDOW"));
    TEST_CHECK(publication->is_subscribed());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) ==
        bindingCount);
    TEST_CHECK(publication->track()->rtc_track().get() == media.rtc.get());
    TEST_CHECK(!server->protocolFailure);
    std::cout << "GAP_P1_03_CASE_7 auto-subscribe-false/explicit-true/soft-resume PASS" << std::endl;
}

void GapWindowFullRestartIdentityBoundaries() {
    enum class Boundary { ParticipantSid, ParticipantIdentity, RoomSid };
    for (const auto boundary : {Boundary::ParticipantSid,
            Boundary::ParticipantIdentity, Boundary::RoomSid}) {
        WindowFixture fixture(false);
        auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
        WindowServerGuard stop{server};
        server->start();
        WindowConnect(fixture, server);
        const auto originalParticipant = fixture.room->remote_participants().at("PA_WINDOW");
        const auto original = originalParticipant->get_remote_publication("TR_PA_WINDOW");
        TEST_CHECK(original && original->SetSubscribed(false));
        WindowPumpUntil(fixture, [&] {
            return HasSubscriptionSince(*server, 0,
                "PA_WINDOW", "TR_PA_WINDOW", false);
        }, "gap-boundary-unsubscribe");
        const auto restartMessages = server->subscriptionMessages.size();
        if (boundary == Boundary::RoomSid) {
            server->restartRoomSid = "RM_WINDOW_REPLACEMENT";
        } else if (boundary == Boundary::ParticipantSid) {
            server->restartParticipantSid = "PA_WINDOW_REPLACEMENT";
        } else {
            // Keep both participant SID and track SID: only identity changes.
            server->restartParticipantIdentity = "replacement-window-peer";
        }
        server->rejectResume = true;
        server->closeActive();
        WindowPumpUntil(fixture, [&] {
            return server->joins == 2 && fixture.observer->reconnected == 1;
        }, "gap-full-restart-identity-boundary");
        const auto &participantSid = server->restartParticipantSid;
        const auto participants = fixture.room->remote_participants();
        TEST_CHECK(participants.contains(participantSid));
        TEST_CHECK(participants.at(participantSid)->identity() == server->restartParticipantIdentity);
        if (boundary == Boundary::ParticipantIdentity) {
            TEST_CHECK(server->restartRoomSid == "RM_WINDOW_LOOPBACK");
            TEST_CHECK(participantSid == originalParticipant->sid());
            TEST_CHECK(participants.at(participantSid)->identity() != originalParticipant->identity());
        }
        const auto successor =
            participants.at(participantSid)->get_remote_publication("TR_PA_WINDOW");
        TEST_CHECK(successor && successor != original && successor->is_subscribed());
        WindowPumpUntil(fixture, [&] {
            return HasSubscriptionSince(*server, restartMessages,
                participantSid, "TR_PA_WINDOW", true);
        }, "gap-full-restart-boundary-default");
        TEST_CHECK(!HasSubscriptionSince(*server, restartMessages,
            participantSid, "TR_PA_WINDOW", false));
        TEST_CHECK(!original->SetSubscribed(false));

        const auto controlMessages = server->subscriptionMessages.size();
        TEST_CHECK(successor->SetSubscribed(false));
        WindowPumpUntil(fixture, [&] {
            return HasSubscriptionSince(*server, controlMessages,
                participantSid, "TR_PA_WINDOW", false);
        }, "gap-full-restart-boundary-control");
        TEST_CHECK(!successor->is_subscribed());
        TEST_CHECK(!server->protocolFailure);
    }
    std::cout << "GAP_P1_03_CASE_8 full-restart/new-SID+identity-only+new-room/default-isolation PASS" << std::endl;
}

void GapWindowDisconnectInvalidatesOldSender() {
    WindowFixture fixture(false);
    auto server = std::make_shared<WindowLoopbackServer>(fixture.io);
    WindowServerGuard stop{server};
    server->start();
    WindowConnect(fixture, server);
    const auto original = fixture.room->remote_participants()
        .at("PA_WINDOW")->get_remote_publication("TR_PA_WINDOW");
    asio::steady_timer gate(fixture.io);
    gate.expires_at(std::chrono::steady_clock::time_point::max());
    bool entered = false;
    livekit::ParticipantSnapshotRoomTestAccess::setSubscriptionHooks(
        *fixture.room,
        [&](bool, uint64_t) -> asio::awaitable<void> {
            if (entered) co_return;
            entered = true;
            std::error_code waitError;
            co_await gate.async_wait(
                asio::redirect_error(asio::use_awaitable, waitError));
        },
        {});
    TEST_CHECK(original && original->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] { return entered; }, "gap-old-sender-paused");
    fixture.room->Disconnect();
    fixture.pump();
    server->subscriptionMessages.clear();
    server->subscriptionWireOrder.clear();
    WindowConnect(fixture, server, true, 2);
    const auto successor = fixture.room->remote_participants()
        .at("PA_WINDOW")->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(successor && successor != original && successor->is_subscribed());
    std::error_code ignored;
    gate.cancel(ignored);
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, 0,
            "PA_WINDOW", "TR_PA_WINDOW", true);
    }, "gap-replacement-sender");
    TEST_CHECK(std::none_of(
        server->subscriptionMessages.begin(),
        server->subscriptionMessages.end(),
        [](const auto &message) { return !message.subscribe(); }));
    const auto replacementUpdate = server->subscriptionMessages.size();
    TEST_CHECK(successor->SetSubscribed(false));
    WindowPumpUntil(fixture, [&] {
        return HasSubscriptionSince(*server, replacementUpdate,
            "PA_WINDOW", "TR_PA_WINDOW", false);
    }, "gap-replacement-sender-still-live");
    livekit::ParticipantSnapshotRoomTestAccess::clearSubscriptionHooks(*fixture.room);
    TEST_CHECK(!server->protocolFailure);
    std::cout << "GAP_P1_03_CASE_9 Disconnect/new-session/old-sender-invalidated PASS" << std::endl;
}

void GapWindowQueuedVideoBindingLease() {
    WindowFixture fixture;
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("gap-lease-peer"));
    fixture.pump();
    const auto participant = fixture.room->remote_participants().at("PA_WINDOW");
    const auto publication = participant->get_remote_publication("TR_PA_WINDOW");
    TEST_CHECK(publication);
    QObject observer;
    int available = 0, unavailable = 0;
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::remoteVideoTrackAvailable, &observer,
        [&](const QString &, std::shared_ptr<livekit::Track>) { ++available; });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable, &observer,
        [&](const QString &, const QString &) { ++unavailable; });

    auto queued = fixture.attachExisting("gap-lease-queued", false);
    WindowDrainNative(fixture.io); // Real Room delivery has queued the Qt adapter.
    const auto oldAvailable = fixture.observer->latest(livekit::ParticipantEventKind::TrackAvailable);
    const auto oldLease = oldAvailable.media_binding_ticket.lock();
    TEST_CHECK(oldLease && oldLease->active.load());
    TEST_CHECK(available == 0);
    TEST_CHECK(publication->SetSubscribed(false));
    TEST_CHECK(!oldLease->active.load());
    TEST_CHECK(livekit::IsParticipantTicketActive(oldAvailable.participant.ticket, oldAvailable.participant.key));
    TEST_CHECK(livekit::IsTrackTicketActive(oldAvailable.track_ticket, oldAvailable.track_key));
    TEST_CHECK(participant->get_remote_publication("TR_PA_WINDOW") == publication);
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);

    // Do not drain Room again: only the previously admitted Available reaches
    // Qt here. Its lease must reject it before any Available signal is emitted.
    WindowDrainQt();
    TEST_CHECK(available == 0 && unavailable == 0);
    auto presentations = fixture.coordinator->participantPresentations();
    TEST_CHECK(presentations.size() == 1 && presentations.front().videoTracks.empty());
    fixture.open();
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 1);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0);
    fixture.pump();

    TEST_CHECK(publication->SetSubscribed(true));
    auto live = fixture.attachExisting("gap-lease-presented");
    TEST_CHECK(live.track == queued.track && available == 1);
    CheckWindowPeer(fixture, "gap-lease-peer");
    presentations = fixture.coordinator->participantPresentations();
    TEST_CHECK(presentations.size() == 1 && presentations.front().videoTracks.size() == 1);
    const auto saved = presentations.front();
    const auto &savedTrack = saved.videoTracks.front();
    TEST_CHECK(fixture.coordinator->isParticipantPresentationCurrent(saved, &savedTrack));
    live.source->push(80, 1000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::frame(*fixture.window, "window-peer").isNull());

    TEST_CHECK(publication->SetSubscribed(false));
    WindowDrainNative(fixture.io); // Unavailable is queued, projection still holds the old lease.
    TEST_CHECK(livekit::IsParticipantTicketActive(saved.participant.participantTicket,
        saved.participant.participantKey));
    TEST_CHECK(livekit::IsTrackTicketActive(savedTrack.ticket, savedTrack.key));
    TEST_CHECK(fixture.coordinator->isParticipantPresentationCurrent(saved));
    TEST_CHECK(!fixture.coordinator->isParticipantPresentationCurrent(saved, &savedTrack));
    presentations = fixture.coordinator->participantPresentations();
    TEST_CHECK(presentations.size() == 1 && presentations.front().videoTracks.empty());
    TEST_CHECK(unavailable == 0);
    WindowDrainQt();
    TEST_CHECK(available == 1 && unavailable == 1);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0);

    TEST_CHECK(publication->SetSubscribed(true));
    auto successor = fixture.attachExisting("gap-lease-successor");
    TEST_CHECK(successor.track == live.track && available == 2);
    CheckWindowPeer(fixture, "gap-lease-peer");
    presentations = fixture.coordinator->participantPresentations();
    TEST_CHECK(presentations.size() == 1 && presentations.front().videoTracks.size() == 1);
    const auto &currentTrack = presentations.front().videoTracks.front();
    TEST_CHECK(currentTrack.key == savedTrack.key);
    TEST_CHECK(currentTrack.mediaBindingKey != savedTrack.mediaBindingKey);
    TEST_CHECK(fixture.coordinator->isParticipantPresentationCurrent(presentations.front(), &currentTrack));
    TEST_CHECK(!fixture.coordinator->isParticipantPresentationCurrent(saved, &savedTrack));
    const auto delivered = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    queued.source->push(20, 2000);
    live.source->push(30, 3000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == delivered);
    successor.source->push(160, 4000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == delivered + 1);
    std::cout << "GAP_P1_03_CASE_11 Room-drain/revoke/Qt-drain/video-lease/presentation/successor PASS" << std::endl;
}

void ScreenShareCameraCoexistence() {
    WindowFixture fixture;
    auto update = WindowParticipant("camera-and-screen");
    update.mutable_participants(0)->mutable_tracks(0)->set_source(livekit::proto::CAMERA);
    auto *screenInfo = update.mutable_participants(0)->add_tracks();
    screenInfo->set_sid("TR_WINDOW_SCREEN");
    screenInfo->set_name("screen");
    screenInfo->set_type(livekit::proto::VIDEO);
    screenInfo->set_source(livekit::proto::SCREEN_SHARE);
    fixture.room->UpdateParticipantsForTesting(update);
    auto camera = fixture.attachExisting("camera-rtc");
    auto screen = fixture.attachExisting("screen-rtc", true, "TR_WINDOW_SCREEN");
    fixture.open(); // Late-open hydration must reconstruct both views.
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 1);
    TEST_CHECK(ParticipantWindowTestAccess::screenCount(*fixture.window) == 1);
    camera.source->push(50, 1000);
    screen.source->push(200, 1000);
    ParticipantWindowTestAccess::render(*fixture.window);
    const auto cameraImage = ParticipantWindowTestAccess::frame(*fixture.window, "window-peer");
    auto *screenTile = ParticipantWindowTestAccess::screen(*fixture.window, "TR_WINDOW_SCREEN");
    TEST_CHECK(!cameraImage.isNull());
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile).pixelColor(0, 0).red() >
        cameraImage.pixelColor(0, 0).red() + 100);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == 2);
    ParticipantWindowTestAccess::checkAspect(*screenTile);

    // Screen mute/unpublish cannot blank or detach the camera.
    update.mutable_participants(0)->mutable_tracks(1)->set_muted(true);
    fixture.room->UpdateParticipantsForTesting(update);
    fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::tile(*fixture.window, "window-peer")->isVideoActive());
    update.mutable_participants(0)->mutable_tracks()->RemoveLast();
    fixture.room->UpdateParticipantsForTesting(update);
    fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::screenCount(*fixture.window) == 0);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer") == cameraImage);
    const auto delivered = ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu;
    screen.source->push(235, 2000);
    camera.source->push(80, 2000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).delivered_to_qt_cpu == delivered + 1);
    TEST_CHECK(ParticipantWindowTestAccess::frame(*fixture.window, "window-peer").pixelColor(0, 0).red() >
        cameraImage.pixelColor(0, 0).red());
    std::cout << "SCREEN_SHARE_WINDOW camera/screen independent, mute/stop isolation, late open, aspect ratios PASS\n";
}

// Drive real Room protobuf state, Coordinator projection, Track sinks and the
// production widgets. The explicit before mode records the unmodified product;
// it is never part of the regression gate and does not claim a passing verdict.
void TrackPresentationAcceptance(bool before = false) {
    WindowFixture fixture;
    auto roster = WindowParticipant("Camera and screen");
    roster.mutable_participants(0)->mutable_tracks(0)->set_source(livekit::proto::CAMERA);
    auto *share = roster.mutable_participants(0)->add_tracks();
    share->set_sid("TR_WINDOW_SCREEN");
    share->set_name("screen");
    share->set_type(livekit::proto::VIDEO);
    share->set_source(livekit::proto::SCREEN_SHARE);
    fixture.room->UpdateParticipantsForTesting(roster);
    auto camera = fixture.attachExisting("presentation-camera");
    auto screen = fixture.attachExisting("presentation-screen", true, "TR_WINDOW_SCREEN");
    fixture.open();
    const auto cameraTile = [&] { return ParticipantWindowTestAccess::tile(*fixture.window, "window-peer"); };
    const auto screenTile = [&] { return ParticipantWindowTestAccess::screen(*fixture.window, "TR_WINDOW_SCREEN"); };
    const auto snapshot = [&](const QString &step) {
        ParticipantWindowTestAccess::savePresentation(*fixture.window, (before ? "before-" : "after-") + step);
    };
    const auto pause = [&](const char *sid, bool paused) {
        livekit::proto::SignalResponse response;
        auto *state = response.mutable_stream_state_update()->add_stream_states();
        state->set_participant_sid("PA_WINDOW");
        state->set_track_sid(sid);
        state->set_state(paused ? livekit::proto::PAUSED : livekit::proto::ACTIVE);
        fixture.room->HandleSignalMessageForTesting(response);
        fixture.pump();
    };
    snapshot("first-frame");
    if (!before) {
        TEST_CHECK(cameraTile()->isVideoActive() && screenTile()->isVideoActive());
        TEST_CHECK(ParticipantWindowTestAccess::tileFrame(cameraTile()).isNull());
        TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    }
    camera.source->push(90, 1000);
    screen.source->push(210, 1000);
    ParticipantWindowTestAccess::render(*fixture.window);
    const auto cameraImage = ParticipantWindowTestAccess::tileFrame(cameraTile());
    snapshot("playing");
    pause("TR_WINDOW_SCREEN", true);
    snapshot("screen-paused");
    std::cout << "TRACK_PRESENTATION " << (before ? "BEFORE" : "AFTER")
        << " screen_pause: camera=" << ParticipantWindowTestAccess::paused(cameraTile())
        << " screen=" << ParticipantWindowTestAccess::paused(screenTile()) << std::endl;
    if (before) return;
    TEST_CHECK(!ParticipantWindowTestAccess::paused(cameraTile()));
    TEST_CHECK(ParticipantWindowTestAccess::paused(screenTile()));
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(cameraTile()) == cameraImage);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    screen.source->push(235, 2000); // A paused/in-flight frame cannot revive the view.
    pause("TR_WINDOW_SCREEN", false);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    screen.source->push(160, 3000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    pause("TR_PA_WINDOW", true);
    TEST_CHECK(ParticipantWindowTestAccess::paused(cameraTile()));
    TEST_CHECK(!ParticipantWindowTestAccess::paused(screenTile()));
    // Late hydration must recover per-track state, not the participant aggregate.
    auto lateWindow = ParticipantWindowTestAccess::create(fixture.coordinator);
    TEST_CHECK(ParticipantWindowTestAccess::paused(ParticipantWindowTestAccess::tile(*lateWindow, "window-peer")));
    TEST_CHECK(!ParticipantWindowTestAccess::paused(ParticipantWindowTestAccess::screen(*lateWindow, "TR_WINDOW_SCREEN")));
    pause("TR_PA_WINDOW", false);
    camera.source->push(90, 4000);
    screen.source->push(210, 4000);
    ParticipantWindowTestAccess::render(*fixture.window);
    roster.mutable_participants(0)->mutable_tracks(1)->set_muted(true);
    fixture.room->UpdateParticipantsForTesting(roster);
    fixture.pump();
    TEST_CHECK(cameraTile()->isVideoActive() && !screenTile()->isVideoActive());
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    screen.source->push(235, 5000);
    roster.mutable_participants(0)->mutable_tracks(1)->set_muted(false);
    fixture.room->UpdateParticipantsForTesting(roster);
    fixture.pump();
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(screenTile()->isVideoActive());
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    screen.source->push(210, 6000);
    ParticipantWindowTestAccess::render(*fixture.window);
    auto successor = fixture.attachExisting("presentation-screen-successor", true, "TR_WINDOW_SCREEN");
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    screen.source->push(235, 7000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    successor.source->push(140, 8000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::tileFrame(screenTile()).isNull());
    snapshot("track-replaced");
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("departed", false));
    fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::tileCount(*fixture.window) == 0);
    TEST_CHECK(ParticipantWindowTestAccess::screenCount(*fixture.window) == 0);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*fixture.window).attached_track_count == 0);
    successor.source->push(235, 9000);
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::screenCount(*fixture.window) == 0);
    std::cout << "TRACK_PRESENTATION PASS: independent pause/mute, first frame, late hydration, replacement, departure\n";
}

void NativeWindowResizeAcceptance(bool before = false) {
    WindowFixture fixture;
    fixture.open();
    auto &window = *fixture.window;
    ParticipantWindowTestAccess::prepareResizeWindow(window);
    if (!ParticipantWindowTestAccess::enableGpu(window)) {
        std::cout << "NATIVE_RESIZE_GPU NOT_RUN: DX11 unavailable\n";
        return;
    }
    const int blocked = ParticipantWindowTestAccess::checkNativeResize(window, before, "DX11");
    if (before) { TEST_CHECK(blocked > 0); return; }
    TEST_CHECK(ParticipantWindowTestAccess::doubleClickGpu(window,
        ParticipantWindowTestAccess::local(window)->geometry().center()) == "local");
    TEST_CHECK(ParticipantWindowTestAccess::local(window)->isPinned());
    window.showMaximized();
    ParticipantWindowTestAccess::checkNativeResize(window, false, "maximized", true);
    window.showFullScreen();
    ParticipantWindowTestAccess::checkNativeResize(window, false, "fullscreen", true);
    window.showNormal();
    ParticipantWindowTestAccess::checkNativeResize(window, false, "restored");
    ParticipantWindowTestAccess::fallback(window);
    ParticipantWindowTestAccess::checkNativeResize(window, false, "CPU-fallback");
    QWidget unrelated;
    unrelated.setAttribute(Qt::WA_DontShowOnScreen);
    unrelated.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    unrelated.resize(320, 240); unrelated.show();
    const auto other = reinterpret_cast<HWND>(unrelated.winId());
    POINT edge{2, 120}; TEST_CHECK(ClientToScreen(other, &edge));
    TEST_CHECK(SendMessage(other, WM_NCHITTEST, 0, MAKELPARAM(edge.x, edge.y)) == HTCLIENT);
    std::cout << "NATIVE_RESIZE PASS: eight edges, DX11/CPU, maximize/fullscreen/restore, content Pin, unrelated HWND\n";
}

void CardChromeAcceptance(bool before = false) {
    WindowFixture fixture;
    auto media = fixture.add("Card name / camera", "card-chrome-camera");
    fixture.open();
    auto &window = *fixture.window;
    const QString prefix = before ? "baseline-card-" : "card-";
    auto *local = ParticipantWindowTestAccess::local(window);
    auto *remote = ParticipantWindowTestAccess::tile(window, "window-peer");
    if (!before) {
        ParticipantWindowTestAccess::cpuPin(local);
        TEST_CHECK(local->isPinned() && !remote->isPinned());
        ParticipantWindowTestAccess::cpuDoubleClick(remote);
        TEST_CHECK(!local->isPinned() && remote->isPinned());
        ParticipantWindowTestAccess::cpuPin(remote);
        TEST_CHECK(!remote->isPinned() && ParticipantWindowTestAccess::pinned(window).isEmpty());
        // A participant identity must not alias a local-camera render key.
        fixture.room->UpdateParticipantsForTesting(WindowParticipant("Remote named local", true, false,
            "PA_ALIAS", "TR_ALIAS", "local")); fixture.pump();
        auto *alias = ParticipantWindowTestAccess::tile(window, "local");
        ParticipantWindowTestAccess::cpuPin(alias);
        TEST_CHECK(alias->isPinned() && !local->isPinned() && !remote->isPinned());
        fixture.room->UpdateParticipantsForTesting(WindowParticipant("Remote named local", false, false,
            "PA_ALIAS", "TR_ALIAS", "local")); fixture.pump();
        TEST_CHECK(ParticipantWindowTestAccess::pinned(window).isEmpty());
    }
    if (!qEnvironmentVariableIsSet("LIVEKIT_PRESENTATION_TEST_GPU")) {
        std::cout << "CARD_CHROME_CPU PASS; GPU NOT_RUN: explicit GPU gate not requested\n";
        return;
    }
    if (!ParticipantWindowTestAccess::enableGpu(window)) {
        std::cout << "CARD_CHROME_CPU PASS; GPU NOT_RUN: DX11 unavailable\n";
        return;
    }
    media.source->push(190, 1000);
    ParticipantWindowTestAccess::render(window);
    remote->setAudioMuted(true);
    remote->setConnectionQuality(livekit::ConnectionQuality::Poor);
    ParticipantWindowTestAccess::layout(window, MeetingUI::VideoViewMode::Grid);
    const auto grid = ParticipantWindowTestAccess::gpuImage(window, prefix + "grid");
    std::cout << "CARD_CHROME_SCALE logical-window=" << window.width()
        << " physical-canvas=" << grid.width() << " dpr=" << window.devicePixelRatioF() << "\n";
    ParticipantWindowTestAccess::layout(window, MeetingUI::VideoViewMode::Pip);
    const auto pipImage = ParticipantWindowTestAccess::gpuImage(window, prefix + "pip");
    if (!before) {
        ParticipantWindowTestAccess::checkOrder(window, remote);
        // The upper card's opaque corner is visible, not the main video's black letterbox.
        const auto point = local->pos() + QPoint(8, local->height() / 2);
        const auto pixel = pipImage.pixelColor(point.x() * pipImage.width() / window.width(),
            point.y() * pipImage.height() / remote->height());
        TEST_CHECK(pixel.red() >= 20 && pixel.red() < 40);
    }
    const auto hit = ParticipantWindowTestAccess::doubleClickGpu(window, local->geometry().center());
    std::cout << "CARD_CHROME " << (before ? "BEFORE" : "AFTER")
        << " local-thumbnail-hit=" << hit.toStdString() << "\n";
    if (before) return;
    TEST_CHECK(hit == local->renderKey());
    TEST_CHECK(local->isPinned() && !remote->isPinned());
    ParticipantWindowTestAccess::checkOrder(window, local);
    ParticipantWindowTestAccess::gpuImage(window, "card-pip-local-pinned");
    TEST_CHECK(ParticipantWindowTestAccess::doubleClickGpu(window, remote->geometry().center()) == remote->renderKey());
    TEST_CHECK(!local->isPinned() && remote->isPinned());
    ParticipantWindowTestAccess::gpuPin(window, remote);
    TEST_CHECK(ParticipantWindowTestAccess::pinned(window).isEmpty() && !remote->isPinned());
    ParticipantWindowTestAccess::layout(window, MeetingUI::VideoViewMode::Grid);
    auto plain = ParticipantWindowTestAccess::gpuImage(window, "card-before-state");
    const auto cachedKey = ParticipantWindowTestAccess::decorationKey(window, remote);
    media.source->push(190, 2000);
    ParticipantWindowTestAccess::render(window);
    ParticipantWindowTestAccess::gpuImage(window, "card-next-frame");
    TEST_CHECK(ParticipantWindowTestAccess::decorationKey(window, remote) == cachedKey);
    // Accepted native events must update card metadata without a resize or a new video frame.
    ParticipantWindowTestAccess::gpuDirty(window);
    livekit::proto::SignalResponse quality;
    auto *q = quality.mutable_connection_quality()->add_updates();
    q->set_participant_sid("PA_WINDOW"); q->set_quality(livekit::proto::ConnectionQuality::EXCELLENT);
    fixture.room->HandleSignalMessageForTesting(quality);
    fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::gpuDirty(window));
    auto qualityImage = ParticipantWindowTestAccess::gpuImage(window, "card-quality");
    TEST_CHECK(qualityImage != plain);
    auto roster = WindowParticipant("Card name / camera");
    auto *mic = roster.mutable_participants(0)->add_tracks();
    mic->set_sid("TR_CARD_MIC"); mic->set_type(livekit::proto::AUDIO); mic->set_muted(true);
    fixture.room->UpdateParticipantsForTesting(roster); fixture.pump();
    TEST_CHECK(remote->isAudioMuted());
    mic->set_muted(false);
    fixture.room->UpdateParticipantsForTesting(roster); fixture.pump();
    TEST_CHECK(!remote->isAudioMuted());
    ParticipantWindowTestAccess::gpuDirty(window);
    livekit::proto::SignalResponse speakerUpdate;
    auto *speaker = speakerUpdate.mutable_speakers_changed()->add_speakers();
    speaker->set_sid("PA_WINDOW"); speaker->set_level(0.6f); speaker->set_active(true);
    fixture.room->HandleSignalMessageForTesting(speakerUpdate);
    fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::gpuDirty(window));
    auto speaking = ParticipantWindowTestAccess::gpuImage(window, "card-speaking");
    TEST_CHECK(speaking != qualityImage);
    roster.mutable_participants(0)->set_name("Renamed camera / long participant name to elide");
    roster.mutable_participants(0)->mutable_tracks(0)->set_source(livekit::proto::CAMERA);
    auto *share = roster.mutable_participants(0)->add_tracks();
    share->set_sid("TR_CARD_SCREEN"); share->set_type(livekit::proto::VIDEO);
    share->set_source(livekit::proto::SCREEN_SHARE);
    fixture.room->UpdateParticipantsForTesting(roster); fixture.pump();
    auto screenMedia = fixture.attachExisting("card-chrome-screen", true, "TR_CARD_SCREEN");
    auto *screen = ParticipantWindowTestAccess::screen(window, "TR_CARD_SCREEN");
    TEST_CHECK(remote->displayName().startsWith("Renamed") && screen);
    ParticipantWindowTestAccess::gpuImage(window, "card-screen-waiting");
    screenMedia.source->push(235, 3000);
    ParticipantWindowTestAccess::render(window);
    ParticipantWindowTestAccess::layout(window, MeetingUI::VideoViewMode::Pip);
    ParticipantWindowTestAccess::gpuPin(window, screen);
    TEST_CHECK(screen->isPinned() && !remote->isPinned() && !local->isPinned());
    ParticipantWindowTestAccess::checkOrder(window, screen);
    ParticipantWindowTestAccess::gpuImage(window, "card-screen-pinned");
    ParticipantWindowTestAccess::resizeWithSidebar(window, QSize(1120, 720));
    ParticipantWindowTestAccess::checkOrder(window, screen);
    TEST_CHECK(ParticipantWindowTestAccess::doubleClickGpu(window, remote->geometry().center()) == remote->renderKey());
    ParticipantWindowTestAccess::gpuImage(window, "card-sidebar");
    ParticipantWindowTestAccess::gpuPin(window, screen);
    roster.mutable_participants(0)->mutable_tracks()->RemoveLast();
    fixture.room->UpdateParticipantsForTesting(roster); fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::pinned(window).isEmpty());
    TEST_CHECK(!ParticipantWindowTestAccess::decorationExists(window, "remote-screen/TR_CARD_SCREEN"));
    ParticipantWindowTestAccess::gpuPin(window, remote);
    ParticipantWindowTestAccess::fallback(window);
    TEST_CHECK(remote->isPinned() && !local->isPinned());
    ParticipantWindowTestAccess::savePresentation(window, "after-card-cpu-fallback");
    ParticipantWindowTestAccess::cpuPin(local);
    TEST_CHECK(local->isPinned() && !remote->isPinned());
    ParticipantWindowTestAccess::cpuPin(remote);
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("departed", false)); fixture.pump();
    TEST_CHECK(ParticipantWindowTestAccess::pinned(window).isEmpty() && !local->isPinned());
    TEST_CHECK(!ParticipantWindowTestAccess::decorationExists(window, "remote-screen/TR_CARD_SCREEN"));
    std::cout << "CARD_CHROME PASS: native metadata, cached overlays, single Pin, PiP draw/hit order, sidebar, fallback, departure\n";
}

void TrackPresentationGpuAcceptance() {
    if (!qEnvironmentVariableIsSet("LIVEKIT_PRESENTATION_TEST_GPU")) {
        std::cout << "TRACK_PRESENTATION_GPU NOT_RUN: explicit GPU gate not requested\n";
        return;
    }
    WindowFixture fixture;
    auto roster = WindowParticipant("GPU camera and screen");
    roster.mutable_participants(0)->mutable_tracks(0)->set_source(livekit::proto::CAMERA);
    auto *share = roster.mutable_participants(0)->add_tracks();
    share->set_sid("TR_WINDOW_SCREEN"); share->set_type(livekit::proto::VIDEO);
    share->set_source(livekit::proto::SCREEN_SHARE);
    fixture.room->UpdateParticipantsForTesting(roster);
    auto camera = fixture.attachExisting("gpu-presentation-camera");
    auto screen = fixture.attachExisting("gpu-presentation-screen", true, "TR_WINDOW_SCREEN");
    fixture.open();
    if (!ParticipantWindowTestAccess::enableGpu(*fixture.window)) {
        std::cout << "TRACK_PRESENTATION_GPU NOT_RUN: DX11 unavailable\n";
        return;
    }
    auto &window = *fixture.window;
    const auto cameraTile = [&] { return ParticipantWindowTestAccess::tile(window, "window-peer"); };
    const auto screenTile = [&] { return ParticipantWindowTestAccess::screen(window, "TR_WINDOW_SCREEN"); };
    const auto pause = [&](bool paused) {
        livekit::proto::SignalResponse response;
        auto *state = response.mutable_stream_state_update()->add_stream_states();
        state->set_participant_sid("PA_WINDOW"); state->set_track_sid("TR_WINDOW_SCREEN");
        state->set_state(paused ? livekit::proto::PAUSED : livekit::proto::ACTIVE);
        fixture.room->HandleSignalMessageForTesting(response);
        fixture.pump();
    };
    camera.source->push(90, 1000); screen.source->push(210, 1000);
    ParticipantWindowTestAccess::render(window);
    auto playing = ParticipantWindowTestAccess::gpuImage(window, "playing");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, playing, screenTile()).red() > 200);
    const auto cameraColor = ParticipantWindowTestAccess::gpuTileCenter(window, playing, cameraTile());
    pause(true);
    auto paused = ParticipantWindowTestAccess::gpuImage(window, "screen-paused");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, paused, cameraTile()) == cameraColor);
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, paused, screenTile()).red() < 80);
    TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(window, screenTile()->renderKey()));
    screen.source->push(235, 2000);
    pause(false);
    ParticipantWindowTestAccess::render(window);
    auto waiting = ParticipantWindowTestAccess::gpuImage(window, "resume-waiting");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, waiting, screenTile()).red() < 80);
    screen.source->push(210, 3000);
    ParticipantWindowTestAccess::render(window);
    ParticipantWindowTestAccess::gpuImage(window, "resumed");
    // Include both an already uploaded frame and a pending frame at replacement.
    screen.source->push(235, 4000);
    auto successor = fixture.attachExisting("gpu-presentation-successor", true, "TR_WINDOW_SCREEN");
    auto replaced = ParticipantWindowTestAccess::gpuImage(window, "replacement-waiting");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, replaced, screenTile()).red() < 80);
    screen.source->push(235, 5000);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(window, screenTile()->renderKey()));
    successor.source->push(140, 6000);
    ParticipantWindowTestAccess::render(window);
    auto current = ParticipantWindowTestAccess::gpuImage(window, "replacement-frame");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, current, screenTile()).red() > 120);
    roster.mutable_participants(0)->mutable_tracks(0)->set_muted(true);
    fixture.room->UpdateParticipantsForTesting(roster);
    fixture.pump();
    auto muted = ParticipantWindowTestAccess::gpuImage(window, "camera-muted");
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, muted, cameraTile()).red() < 80);
    TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, muted, screenTile()) ==
        ParticipantWindowTestAccess::gpuTileCenter(window, current, screenTile()));
    fixture.room->UpdateParticipantsForTesting(WindowParticipant("departed", false));
    fixture.pump();
    TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(window, "remote-camera/window-peer"));
    TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(window, "remote-screen/TR_WINDOW_SCREEN"));
    TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu == 0);
    std::cout << "TRACK_PRESENTATION_GPU PASS: actual draw/readback, pause/mute isolation, replacement, cleanup\n";
}

void ScreenShareWindowControls() {
    WindowFixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::commitLocalStartupPrecondition(*fixture.coordinator);
    int starts = 0, stops = 0;
    livekit::DesktopSource selectedSource;
    livekit::IDesktopCapture::FrameCallback captureFrame;
    class PendingCapture final : public livekit::IDesktopCapture {
    public:
        PendingCapture(
                int &starts,
                int &stops,
                livekit::DesktopSource &selectedSource,
                FrameCallback &frame)
            : starts(starts), stops(stops), selectedSource(selectedSource), frame(frame) {}
        void Start(livekit::DesktopSource source, FrameCallback callback, EndCallback) override {
            ++starts;
            selectedSource = std::move(source);
            frame = std::move(callback);
        }
        void Stop() override { ++stops; }
        int &starts, &stops;
        livekit::DesktopSource &selectedSource;
        FrameCallback &frame;
    };
    auto backend = livekit::ScreenShareSession::ForRoom(fixture.room);
    backend.capture = [&] {
        return std::make_unique<PendingCapture>(starts, stops, selectedSource, captureFrame);
    };
    backend.publish = [](auto) -> asio::awaitable<void> { co_return; };
    backend.unpublish = [](auto) -> asio::awaitable<void> { co_return; };
    auto share = std::make_shared<livekit::ScreenShareSession>(fixture.runtime->strand(), std::move(backend),
        [&](livekit::ScreenShareSnapshot snapshot) {
            const auto generation = fixture.runtime->generation();
            QMetaObject::invokeMethod(fixture.coordinator.get(), [&, generation, snapshot] {
                OpenMeeting::MeetingCoordinatorTestAccess::screenSnapshot(*fixture.coordinator, generation, snapshot);
            }, Qt::QueuedConnection);
        });
    asio::post(fixture.runtime->strand(), [&] { fixture.runtime->screenShareOnStrand() = share; });
    fixture.pump();
    fixture.open();

    const std::vector<livekit::DesktopSource> defaultSources{
        {livekit::DesktopSourceKind::Window, 123, "test window"},
        {livekit::DesktopSourceKind::Screen, 456, "primary screen"},
    };
    ParticipantWindowTestAccess::deliverDefaultScreenSources(*fixture.window, defaultSources);
    fixture.pump();
    TEST_CHECK(fixture.window->findChild<QInputDialog *>(QStringLiteral("screen-share-picker")) == nullptr);
    TEST_CHECK(starts == 1 && selectedSource.kind == livekit::DesktopSourceKind::Screen &&
        selectedSource.id == 456);
    fixture.coordinator->stopScreenShare();
    fixture.pump();
    TEST_CHECK(stops == 1);
    starts = 0;
    stops = 0;
    captureFrame = {};

    const std::vector<livekit::DesktopSource> sources{{livekit::DesktopSourceKind::Window, 123, "test window"}};
    emit fixture.coordinator->screenShareSourcesReady(sources);
    auto *picker = fixture.window->findChild<QInputDialog *>(QStringLiteral("screen-share-picker"));
    TEST_CHECK(picker != nullptr);
    picker->reject();
    fixture.pump();
    TEST_CHECK(starts == 0 && stops == 0);
    emit fixture.coordinator->screenShareSourcesReady(sources);
    picker = fixture.window->findChild<QInputDialog *>(QStringLiteral("screen-share-picker"));
    TEST_CHECK(picker != nullptr);
    picker->accept();
    fixture.pump();
    TEST_CHECK(starts == 1);
    TEST_CHECK(ParticipantWindowTestAccess::shareState(*fixture.window) == livekit::ScreenShareState::Starting);
    ParticipantWindowTestAccess::clickShareDuringRecovery(*fixture.window);
    fixture.pump();
    TEST_CHECK(stops == 1);
    TEST_CHECK(ParticipantWindowTestAccess::shareState(*fixture.window) == livekit::ScreenShareState::Idle);
    fixture.coordinator->startScreenShare(sources.front());
    fixture.pump();
    auto localFrame = livekit::VideoFrame::create(640, 480, livekit::VideoBufferType::I420);
    std::fill(localFrame.data(), localFrame.data() + 640 * 480, uint8_t(200));
    std::fill(localFrame.data() + 640 * 480, localFrame.data() + 640 * 480 * 3 / 2, uint8_t(128));
    captureFrame(localFrame);
    WindowPumpUntil(fixture, [&] {
        return ParticipantWindowTestAccess::shareState(*fixture.window) == livekit::ScreenShareState::Active;
    }, "local-screen-preview");
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(ParticipantWindowTestAccess::sharingBanner(*fixture.window));
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(ParticipantWindowTestAccess::localScreen(*fixture.window)).size() == QSize(640, 480));
    ParticipantWindowTestAccess::cpuPin(ParticipantWindowTestAccess::localScreen(*fixture.window));
    TEST_CHECK(ParticipantWindowTestAccess::pinned(*fixture.window) == "local-screen");
    TEST_CHECK(!ParticipantWindowTestAccess::local(*fixture.window)->isPinned());
    fixture.coordinator->stopScreenShare();
    fixture.pump();
    captureFrame(localFrame); // Old capture callback cannot recreate the preview.
    ParticipantWindowTestAccess::render(*fixture.window);
    TEST_CHECK(!ParticipantWindowTestAccess::localScreen(*fixture.window));
    TEST_CHECK(ParticipantWindowTestAccess::pinned(*fixture.window).isEmpty());
    TEST_CHECK(!ParticipantWindowTestAccess::sharingBanner(*fixture.window));
    OpenMeeting::MeetingCoordinatorTestAccess::screenSnapshot(*fixture.coordinator, fixture.runtime->generation() - 1,
        {livekit::ScreenShareState::Active, livekit::ScreenShareError::None});
    TEST_CHECK(ParticipantWindowTestAccess::shareState(*fixture.window) == livekit::ScreenShareState::Idle);
    asio::post(fixture.runtime->strand(), [&] { share->Shutdown(); fixture.runtime->screenShareOnStrand().reset(); });
    fixture.pump();
    share.reset();
    std::cout << "SCREEN_SHARE_WINDOW default screen, picker cancel/select, native start/cancel, recovery stop, stale generation PASS\n";
}

void AccountLogoutAndDuplicateLogin() {
    const auto sendKick = [](WindowFixture &fixture, bool fromParticipant, const std::string &target, int reason) {
        openmeeting::meeting::NotifyMeetingData notify;
        auto *kick = notify.mutable_kickoffmeetingdata();
        kick->set_userid(target);
        kick->set_reasoncode(static_cast<openmeeting::meeting::KickOffReason>(reason));
        livekit::proto::DataPacket packet;
        if (fromParticipant) {
            packet.set_participant_sid("PA_WINDOW");
            packet.set_participant_identity("window-peer");
        }
        packet.mutable_user()->set_payload(notify.SerializeAsString());
        const auto bytes = packet.SerializeAsString();
        fixture.room->OnIncomingDataPacket({bytes.begin(), bytes.end()}, "", "");
    };

    // Real Room data -> Coordinator -> isolated SessionManager -> real Qt window.
    // Match the server: send DuplicatedLogin then immediately remove the participant.
    {
        WindowFixture fixture(true, true);
        fixture.open();
        auto *window = fixture.window.release();
        ParticipantWindowTestAccess::bindAccount(*window, *fixture.session);
        window->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<MeetingUI::MeetingRoomWindow> guard(window);
        int invalidations = 0;
        QObject::connect(fixture.session.get(), &OpenMeeting::SessionManager::sessionInvalidated,
            fixture.session.get(), [&](OpenMeeting::SessionInvalidationReason reason) {
                TEST_CHECK(reason == OpenMeeting::SessionInvalidationReason::DuplicatedLogin);
                ++invalidations;
            });
        sendKick(fixture, false, "local-user", 0);
        sendKick(fixture, false, "local-user", 0); // repeated server delivery is idempotent
        fixture.listener->OnDisconnected(livekit::RoomDisconnectReason::ParticipantRemoved, "");
        fixture.pump();
        TEST_CHECK(invalidations == 1 && !fixture.session->isLoggedIn());
        TEST_CHECK(fixture.httpClient.token().isEmpty() && !guard);
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
    }
    // Neither a participant's forged packet, a different target, nor a delayed
    // packet bound to an earlier login of the same user may clear current auth.
    for (int scenario = 0; scenario != 3; ++scenario) {
        WindowFixture fixture(true, true);
        fixture.room->UpdateParticipantsForTesting(WindowParticipant("peer", true, false));
        fixture.pump();
        sendKick(fixture, scenario == 0, scenario == 1 ? "other-user" : "local-user", 0);
        if (scenario == 2) fixture.session->loginAsGuest("Replacement login", "local-user");
        fixture.pump();
        TEST_CHECK(fixture.session->isLoggedIn() && !fixture.session->isSessionInvalidating());
    }
    for (bool noticeAlreadyOpen : {false, true}) {
        WindowFixture fixture(true, true);
        fixture.open();
        auto *window = fixture.window.release();
        ParticipantWindowTestAccess::bindAccount(*window, *fixture.session);
        window->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<MeetingUI::MeetingRoomWindow> guard(window);
        if (noticeAlreadyOpen) fixture.coordinator->kickedOff("old notification", 2);
        QPointer<QMessageBox> notice(window->findChild<QMessageBox*>("meetingDepartureNotice"));
        sendKick(fixture, false, "local-user", 2); // queued old server logout reply
        fixture.session->logout(false);
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
        TEST_CHECK(!fixture.coordinator->room()); // no waiting for server reply
        fixture.pump();
        TEST_CHECK(!guard && !notice);
    }
    std::cout << "ACCOUNT_LIFECYCLE PASS: duplicate-login/removal race, auth generation, trusted origin, logout, late reply\n";
}

void LocalRenderInputAcceptance() {
    WindowFixture fixture;
    fixture.open();
    auto &window = *fixture.window;
    auto source = std::make_shared<livekit::VideoSource>(8, 4);
    ParticipantWindowTestAccess::bindLocal(window, source);
    auto frame = livekit::VideoFrame::create(8, 4, livekit::VideoBufferType::RGBA);
    for (size_t i = 0; i < frame.dataSize(); i += 4) {
        frame.data()[i] = 240; frame.data()[i + 1] = 20;
        frame.data()[i + 2] = 10; frame.data()[i + 3] = 255;
    }
    livekit::VideoCaptureOptions options;
    options.rotation = livekit::VideoRotation::VIDEO_ROTATION_90;
    source->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    auto *tile = ParticipantWindowTestAccess::local(window);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(tile).size() == QSize(4, 8));
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(tile).pixelColor(1, 1) == QColor(240, 20, 10));
    const auto cpuBeforeGpu = ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu;
    if (ParticipantWindowTestAccess::enableGpu(window)) {
        ParticipantWindowTestAccess::layout(window, MeetingUI::VideoViewMode::Grid);
        source->captureFrame(frame, options);
        ParticipantWindowTestAccess::render(window);
        auto image = ParticipantWindowTestAccess::gpuImage(window, "p1-local-gpu");
        TEST_CHECK(ParticipantWindowTestAccess::gpuHasFrame(window, "local"));
        TEST_CHECK(ParticipantWindowTestAccess::gpuTileCenter(window, image, tile).red() > 220);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu == cpuBeforeGpu);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_gpu == 1);
        for (auto format : {livekit::VideoBufferType::I420, livekit::VideoBufferType::NV12}) {
            auto yuv = livekit::VideoFrame::create(3, 5, format);
            std::fill(yuv.data(), yuv.data() + 15, uint8_t(81));
            for (size_t i = 15; i < yuv.dataSize(); ++i) {
                yuv.data()[i] = format == livekit::VideoBufferType::NV12
                    ? ((i - 15) % 2 == 0 ? 90 : 240) : (i < 21 ? 90 : 240);
            }
            source->captureFrame(yuv, options);
            ParticipantWindowTestAccess::render(window);
            image = ParticipantWindowTestAccess::gpuImage(window,
                format == livekit::VideoBufferType::NV12 ? "p1-local-nv12" : "p1-local-i420");
            const auto color = ParticipantWindowTestAccess::gpuTileCenter(window, image, tile);
            TEST_CHECK(color.red() > 220 && color.green() < 35 && color.blue() < 35);
        }
        // An already queued local frame must be consumed by the selected CPU backend.
        source->captureFrame(frame, options);
        ParticipantWindowTestAccess::simulateModuleFailure(window);
        ParticipantWindowTestAccess::render(window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu == cpuBeforeGpu + 1);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_gpu == 3);
        TEST_CHECK(ParticipantWindowTestAccess::tileFrame(tile).size() == QSize(4, 8));
        TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(window, "local"));
        std::cout << "P1_LOCAL_GPU PASS: common canvas, exclusive dispatch, queued-frame CPU fallback\n";
    } else {
        std::cout << "P1_LOCAL_GPU NOT_RUN: DX11 unavailable\n";
    }
    source->captureFrame(frame, options);
    ParticipantWindowTestAccess::bindLocal(window, source, false);
    source->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(tile).isNull());
    ParticipantWindowTestAccess::bindLocal(window, source);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(ParticipantWindowTestAccess::tileFrame(tile).isNull());
    source->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(!ParticipantWindowTestAccess::tileFrame(tile).isNull());
    auto replacement = std::make_shared<livekit::VideoSource>(8, 4);
    ParticipantWindowTestAccess::bindLocal(window, replacement);
    const auto delivered = ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu;
    source->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu == delivered);
    replacement->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(window).delivered_to_qt_cpu == delivered + 1);
    ParticipantWindowTestAccess::stopRenderSession(window);
    replacement->captureFrame(frame, options);
    ParticipantWindowTestAccess::render(window);
    fixture.window.reset();
    replacement->captureFrame(frame, options); // Producer can outlive the window.
    std::cout << "P1_LOCAL_INPUT PASS: rotation, disable/reopen, replacement, stop, window destruction\n";
}

void RenderMultiSessionLifecycleAcceptance() {
    qunsetenv("LIVEKIT_RENDER_BACKEND"); // Exercise the Windows production default.
    const auto modulePath = livekit::render::BackendModule::DefaultPath(
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()));
    webrtc::scoped_refptr<WindowMemoryVideoSource> retiredSource;
    std::shared_ptr<livekit::Track> retiredTrack;
    uint64_t firstGeneration = 0;
    QPointer<MeetingUI::MeetingRoomWindow> retiredWindow;

    {
        WindowFixture first;
        auto roster = WindowParticipant("first-generation");
        roster.mutable_participants(0)->mutable_tracks(0)->set_source(livekit::proto::CAMERA);
        auto *screenInfo = roster.mutable_participants(0)->add_tracks();
        screenInfo->set_sid("TR_WINDOW_SCREEN");
        screenInfo->set_name("screen");
        screenInfo->set_type(livekit::proto::VIDEO);
        screenInfo->set_source(livekit::proto::SCREEN_SHARE);
        const auto secondParticipant = WindowParticipant("second-participant", true, true,
            "PA_SECOND", "TR_SECOND", "second-peer");
        *roster.add_participants() = secondParticipant.participants(0);
        first.room->UpdateParticipantsForTesting(roster);
        auto camera = first.attachExisting("lifecycle-camera-a");
        auto screen = first.attachExisting("lifecycle-screen-a", true, "TR_WINDOW_SCREEN");
        auto second = first.attachExisting("lifecycle-camera-second", true, "TR_SECOND", "PA_SECOND");
        first.open();
        TEST_CHECK(ParticipantWindowTestAccess::tileCount(*first.window) == 2);
        TEST_CHECK(ParticipantWindowTestAccess::screenCount(*first.window) == 1);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).attached_track_count == 3);
        TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*first.window));
        firstGeneration = ParticipantWindowTestAccess::renderGeneration(*first.window);
        TEST_CHECK(firstGeneration != 0);

        camera.source->push(60, 1000);
        screen.source->push(180, 1000);
        second.source->push(220, 1000);
        ParticipantWindowTestAccess::render(*first.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu == 3);
        auto *cameraTile = ParticipantWindowTestAccess::tile(*first.window, "window-peer");
        TEST_CHECK(cameraTile);
        ParticipantWindowTestAccess::layout(*first.window, MeetingUI::VideoViewMode::Pip);
        ParticipantWindowTestAccess::pin(*first.window, cameraTile);
        ParticipantWindowTestAccess::checkOrder(*first.window, cameraTile);
        ParticipantWindowTestAccess::layout(*first.window, MeetingUI::VideoViewMode::Grid);

        roster.mutable_participants(0)->mutable_tracks(1)->set_muted(true);
        first.room->UpdateParticipantsForTesting(roster);
        first.pump();
        TEST_CHECK(!ParticipantWindowTestAccess::screen(*first.window, "TR_WINDOW_SCREEN")->isVideoActive());
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).attached_track_count == 2);
        const auto hiddenDelivered = ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu;
        screen.source->push(235, 2000);
        ParticipantWindowTestAccess::render(*first.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu == hiddenDelivered);

        roster.mutable_participants(0)->mutable_tracks(1)->set_muted(false);
        first.room->UpdateParticipantsForTesting(roster);
        first.pump();
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).attached_track_count == 3);
        TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(*first.window, "remote-screen/TR_WINDOW_SCREEN"));
        screen.source->push(170, 3000);
        ParticipantWindowTestAccess::render(*first.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu == hiddenDelivered + 1);

        first.room->UpdateParticipantsForTesting(WindowParticipant("departed", false, false,
            "PA_SECOND", "TR_SECOND", "second-peer"));
        first.pump();
        TEST_CHECK(ParticipantWindowTestAccess::tileCount(*first.window) == 1);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).attached_track_count == 2);
        const auto beforeLateSecond = ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu;
        second.source->push(240, 4000);
        ParticipantWindowTestAccess::render(*first.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*first.window).delivered_to_gpu == beforeLateSecond);

        retiredSource = camera.source;
        retiredTrack = camera.track;
        retiredWindow = first.window.get();
        ParticipantWindowTestAccess::queueModuleFailure(*first.window);
        TEST_CHECK(first.window->close());
        TEST_CHECK(!ParticipantWindowTestAccess::renderSessionActive(*first.window));
        TEST_CHECK(!ParticipantWindowTestAccess::usingGpu(*first.window));
        first.window.reset();
        first.pump();
        TEST_CHECK(retiredWindow.isNull());
    }
    ParticipantWindowTestAccess::waitDxOwners();
    TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));

    {
        WindowFixture second;
        auto successor = second.add("second-generation", "lifecycle-camera-b");
        second.open();
        TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*second.window));
        const auto secondGeneration = ParticipantWindowTestAccess::renderGeneration(*second.window);
        TEST_CHECK(secondGeneration != 0 && secondGeneration != firstGeneration);
        WindowDrainQt(); // The retired window cannot receive its queued failure.
        TEST_CHECK(ParticipantWindowTestAccess::usingGpu(*second.window));
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*second.window).delivered_to_gpu == 0);

        const uint8_t y[16] = {35,35,35,35,35,35,35,35,35,35,35,35,35,35,35,35};
        const uint8_t uv[4] = {128,128,128,128};
        retiredSource->push(35, 5000);
        retiredTrack->notifyI420VideoFrame(livekit::render::OwnedI420Frame::CopyFromPlanes(
            4, 4, y, 4, uv, 2, uv, 2, 5000));
        ParticipantWindowTestAccess::render(*second.window);
        ParticipantWindowTestAccess::renderGpuCanvas(*second.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*second.window).delivered_to_gpu == 0);
        TEST_CHECK(!ParticipantWindowTestAccess::gpuHasFrame(*second.window, "remote-camera/window-peer"));

        successor.source->push(190, 6000);
        ParticipantWindowTestAccess::render(*second.window);
        ParticipantWindowTestAccess::renderGpuCanvas(*second.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*second.window).delivered_to_gpu == 1);
        TEST_CHECK(ParticipantWindowTestAccess::gpuHasFrame(*second.window, "remote-camera/window-peer"));

        ParticipantWindowTestAccess::simulateModuleFailure(*second.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*second.window).backend ==
            livekit::render::VideoRenderSession::Backend::QtCpu);
        const auto cpuBefore = ParticipantWindowTestAccess::statistics(*second.window).delivered_to_qt_cpu;
        successor.source->push(120, 7000);
        ParticipantWindowTestAccess::render(*second.window);
        TEST_CHECK(ParticipantWindowTestAccess::statistics(*second.window).delivered_to_qt_cpu == cpuBefore + 1);
        TEST_CHECK(!ParticipantWindowTestAccess::tileFrame(
            ParticipantWindowTestAccess::tile(*second.window, "window-peer")).isNull());
        TEST_CHECK(second.window->close());
        TEST_CHECK(!ParticipantWindowTestAccess::renderSessionActive(*second.window));
        second.window.reset();
        second.pump();
    }
    retiredTrack.reset();
    retiredSource = nullptr;
    WindowDrainQt();
    ParticipantWindowTestAccess::waitDxOwners();
    TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
    std::cout << "RENDER_MULTISESSION_LIFECYCLE PASS: leave/rejoin, window rebuild, track churn, hide/restore, layout, stale generation/frame/GPU callback isolation, CPU fallback, unload\n";
}

struct GlOwnerExitLatch final {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> entered{false};
    bool released = false;
    void block() {
        entered.store(true, std::memory_order_release);
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return released; });
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }
};

void AngleRapidRebuildAcceptance() {
    using namespace livekit::render;
    qputenv("LIVEKIT_RENDER_BACKEND", "opengl");
    TEST_CHECK(QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES);
    const auto modulePath = BackendModule::PathForBackend(
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()),
        LK_RENDER_BACKEND_OPENGL);
    const auto wait = [](const std::function<bool()>& done, int timeout = 6000) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < timeout)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(done());
    };

    auto latch = std::make_shared<GlOwnerExitLatch>();
    auto retiredSource = std::make_shared<livekit::VideoSource>(16, 9);
    uint64_t retiredGeneration = 0;
    {
        WindowFixture retired;
        retired.open();
        ParticipantWindowTestAccess::bindLocal(*retired.window, retiredSource);
        TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*retired.window, true));
        retiredGeneration = ParticipantWindowTestAccess::renderGeneration(*retired.window);
        ParticipantWindowTestAccess::setGlBeforePresent(*retired.window,
            [latch] { latch->block(); });
        ParticipantWindowTestAccess::requestGlScene(*retired.window);
        wait([&] { return latch->entered.load(std::memory_order_acquire); });
        QPointer<MeetingUI::MeetingRoomWindow> retiredWindow(retired.window.get());
        QElapsedTimer close; close.start();
        TEST_CHECK(retired.window->close());
        retired.window.reset();
        TEST_CHECK(close.elapsed() < 500 && !retiredWindow);
        TEST_CHECK(!ParticipantWindowTestAccess::glWorkersIdle() && GetModuleHandleW(modulePath.c_str()));
    }

    WindowFixture successor;
    successor.open();
    auto successorSource = std::make_shared<livekit::VideoSource>(16, 9);
    ParticipantWindowTestAccess::bindLocal(*successor.window, successorSource);
    auto* pending = ParticipantWindowTestAccess::startGpu(*successor.window, true);
    TEST_CHECK(pending && pending->rendererPending() && !pending->rendererReady());
    const auto successorGeneration = ParticipantWindowTestAccess::renderGeneration(*successor.window);
    TEST_CHECK(successorGeneration && successorGeneration != retiredGeneration);
    int ready = 0, failures = 0;
    QObject scope;
    QObject::connect(pending, &VideoCanvas::rendererInitialized, &scope, [&] { ++ready; });
    QObject::connect(pending, &VideoCanvas::rendererUnavailable, &scope, [&] { ++failures; });

    QElapsedTimer clock; clock.start();
    qint64 previous = 0, maximum = 0;
    int ticks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &scope, [&] {
        const auto now = clock.elapsed();
        maximum = std::max(maximum, now - previous);
        previous = now;
        ++ticks;
    });
    heartbeat.start(25);
    wait([&] { return clock.elapsed() >= 300; }, 1000);
    TEST_CHECK(ticks >= 8 && maximum < 500 && clock.elapsed() - previous < 500);
    TEST_CHECK(pending->rendererPending() && !pending->rendererReady() &&
        !ParticipantWindowTestAccess::usingGpu(*successor.window) && failures == 0);
    const auto waitingDiagnostics = pending->renderDiagnostics();
    TEST_CHECK(waitingDiagnostics.requested_backend == RenderBackend::OpenGL &&
        waitingDiagnostics.actual_backend == RenderBackend::QtCpu &&
        waitingDiagnostics.gpu_failure == RenderGpuFailure::None &&
        waitingDiagnostics.fallback_reason == RenderFallbackReason::None);

    latch->release();
    wait([&] { return ParticipantWindowTestAccess::usingGpu(*successor.window); });
    TEST_CHECK(ready == 1 && failures == 0 && pending->rendererReady());
    const auto activeDiagnostics = successor.window->renderDiagnostics();
    TEST_CHECK(activeDiagnostics.actual_backend == RenderBackend::OpenGL &&
        activeDiagnostics.gpu_failure == RenderGpuFailure::None &&
        activeDiagnostics.fallback_reason == RenderFallbackReason::None);

    const auto beforeLate = ParticipantWindowTestAccess::statistics(*successor.window).delivered_to_gpu;
    auto late = livekit::VideoFrame::create(16, 9, livekit::VideoBufferType::RGBA);
    std::memset(late.data(), 0, late.dataSize());
    retiredSource->captureFrame(late, {});
    ParticipantWindowTestAccess::render(*successor.window);
    TEST_CHECK(ParticipantWindowTestAccess::statistics(*successor.window).delivered_to_gpu == beforeLate);
    successorSource->captureFrame(late, {});
    ParticipantWindowTestAccess::render(*successor.window);
    wait([&] { return ParticipantWindowTestAccess::statistics(*successor.window).delivered_to_gpu == beforeLate + 1; });
    TEST_CHECK(ready == 1 && failures == 0 && maximum < 500 && clock.elapsed() - previous < 500);

    TEST_CHECK(successor.window->close());
    successor.window.reset();
    ParticipantWindowTestAccess::waitGlOwners();
    TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
    std::cout << "ANGLE_RAPID_REBUILD PASS: temporary owner capacity stayed pending, UI progressed, successor activated once, stale generation/frame rejected, module unloaded; heartbeat ticks="
        << ticks << " maxUiGapMs=" << maximum << " (asserted <500ms)\n";
}

// The callback runs inside the real DX11 EndFrame/Destroy paths. The test only
// controls a deterministic latch; it never resets the adapter or triggers TDR.
struct DxOwnerFault final {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> presentEntered{false}, destroyEntered{false};
    bool release = false;
    bool blockPresent = false, blockDestroy = false;
    HRESULT result = S_OK;
    const std::thread::id ui = std::this_thread::get_id();
    static int32_t LK_RENDER_CALL beforePresent(void* context) {
        auto& fault = *static_cast<DxOwnerFault*>(context);
        TEST_CHECK(std::this_thread::get_id() != fault.ui);
        fault.presentEntered.store(true, std::memory_order_release);
        if (fault.blockPresent) {
            std::unique_lock lock(fault.mutex);
            fault.changed.wait(lock, [&] { return fault.release; });
        }
        return int32_t(fault.result);
    }
    static void LK_RENDER_CALL beforeDestroy(void* context) {
        auto& fault = *static_cast<DxOwnerFault*>(context);
        TEST_CHECK(std::this_thread::get_id() != fault.ui);
        fault.destroyEntered.store(true, std::memory_order_release);
        if (fault.blockDestroy) {
            std::unique_lock lock(fault.mutex);
            fault.changed.wait(lock, [&] { return fault.release; });
        }
    }
    lk_render_dx11_test_hooks_v1 hooks() {
        return {sizeof(lk_render_dx11_test_hooks_v1), 0, this, beforePresent, beforeDestroy};
    }
    void unblock() {
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all();
    }
};

void Dx11RenderOwnerAcceptance(const QString& fixturePath) {
    using namespace livekit::render;
    qunsetenv("LIVEKIT_RENDER_BACKEND");
    TEST_CHECK(BackendModule::DefaultBackend() == LK_RENDER_BACKEND_DX11);
    const auto modulePath = std::filesystem::path(fixturePath.toStdWString());
    const auto load = [&] {
        ModuleLoadError error{};
        auto module = BackendModule::Load(modulePath, LK_RENDER_BACKEND_DX11, error);
        TEST_CHECK(module && error == ModuleLoadError::None);
        return module;
    };
    const auto wait = [](const std::function<bool()>& done, int timeout = 4000) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < timeout) QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(done());
    };
    const auto cpuFrame = [](MeetingUI::MeetingRoomWindow& window,
            const std::shared_ptr<livekit::VideoSource>& source, QColor color) {
        auto frame = livekit::VideoFrame::create(16, 9, livekit::VideoBufferType::RGBA);
        for (size_t i = 0; i < frame.dataSize(); i += 4) {
            frame.data()[i] = color.red(); frame.data()[i + 1] = color.green();
            frame.data()[i + 2] = color.blue(); frame.data()[i + 3] = 255;
        }
        source->captureFrame(frame, {});
        ParticipantWindowTestAccess::render(window);
        const auto image = ParticipantWindowTestAccess::tileFrame(ParticipantWindowTestAccess::local(window));
        TEST_CHECK(!image.isNull() && image.pixelColor(0, 0) == color);
    };
    // Failure results cross the owner/UI boundary as typed diagnostics, once.
    for (int scenario = 0; scenario != 3; ++scenario) {
        DxOwnerFault fault;
        fault.result = scenario == 1 ? DXGI_ERROR_DEVICE_REMOVED : E_FAIL;
        WindowFixture fixture; fixture.open();
        TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*fixture.window, true, load()));
        auto& canvas = ParticipantWindowTestAccess::dxCanvas(*fixture.window);
        QObject scope;
        int failures = 0;
        QObject::connect(&canvas, &VideoCanvas::rendererUnavailable, &scope, [&] { ++failures; });
        if (scenario == 2) ParticipantWindowTestAccess::throwOnDxOwner(*fixture.window);
        else {
            ParticipantWindowTestAccess::installDxHooks(*fixture.window, fault.hooks());
            ParticipantWindowTestAccess::requestDxScene(*fixture.window);
        }
        wait([&] { return !ParticipantWindowTestAccess::usingGpu(*fixture.window); });
        TEST_CHECK(scenario == 2 || fault.presentEntered.load(std::memory_order_acquire));
        const auto diagnostics = fixture.window->renderDiagnostics();
        const auto expected = scenario == 0 ? RenderGpuFailure::ModuleDeviceFailed :
            scenario == 1 ? RenderGpuFailure::DeviceLost : RenderGpuFailure::RenderOwnerException;
        TEST_CHECK(failures == 1 && diagnostics.actual_backend == RenderBackend::QtCpu &&
            diagnostics.gpu_failure == expected && diagnostics.fallback_reason ==
                (scenario == 1 ? RenderFallbackReason::GpuDeviceLost : RenderFallbackReason::GpuRuntimeFailed));
        auto source = std::make_shared<livekit::VideoSource>(16, 9);
        ParticipantWindowTestAccess::bindLocal(*fixture.window, source);
        cpuFrame(*fixture.window, source, QColor(20, 80, 220));
        fixture.window.reset();
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
    }
    // Driver resource cleanup may block too: close and surface detachment must
    // return while the independent owner retains all of the driver lifetimes.
    {
        DxOwnerFault fault; fault.blockDestroy = true;
        WindowFixture fixture; fixture.open();
        TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*fixture.window, true, load()));
        QPointer<QWindow> surface(ParticipantWindowTestAccess::dxSurface(*fixture.window));
        const auto hwnd = reinterpret_cast<HWND>(surface->winId());
        ParticipantWindowTestAccess::installDxHooks(*fixture.window, fault.hooks());
        QElapsedTimer close; close.start();
        TEST_CHECK(fixture.window->close()); fixture.window.reset();
        TEST_CHECK(close.elapsed() < 500);
        wait([&] { return fault.destroyEntered.load(std::memory_order_acquire); });
        TEST_CHECK(surface && IsWindow(hwnd) && GetModuleHandleW(modulePath.c_str()));
        QElapsedTimer clock; clock.start();
        QTimer heartbeat; int ticks = 0; qint64 previous = 0, maximum = 0;
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
            maximum = std::max(maximum, clock.elapsed() - previous); previous = clock.elapsed(); ++ticks;
        });
        heartbeat.start(25);
        wait([&] { return clock.elapsed() >= 300; }, 1000);
        TEST_CHECK(ticks >= 8 && maximum < 500 && clock.elapsed() - previous < 500);
        fault.unblock();
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!surface && !IsWindow(hwnd) && !GetModuleHandleW(modulePath.c_str()));
    }
    // A hung Present is the final case: timeout deliberately quarantines DX11
    // for this process, bounding the number of abandoned graphics owners.
    DxOwnerFault fault; fault.blockPresent = true; fault.result = DXGI_ERROR_DEVICE_REMOVED;
    WindowFixture old; old.open();
    auto oldSource = std::make_shared<livekit::VideoSource>(16, 9);
    ParticipantWindowTestAccess::bindLocal(*old.window, oldSource);
    TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*old.window, true, load()));
    const auto oldGeneration = ParticipantWindowTestAccess::renderGeneration(*old.window);
    QPointer<QWindow> surface(ParticipantWindowTestAccess::dxSurface(*old.window));
    const auto hwnd = reinterpret_cast<HWND>(surface->winId());
    QElapsedTimer clock; clock.start();
    QTimer heartbeat; int ticks = 0; qint64 previous = 0, maximum = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        const auto now = clock.elapsed(); maximum = std::max(maximum, now - previous); previous = now; ++ticks;
    });
    heartbeat.start(25);
    ParticipantWindowTestAccess::installDxHooks(*old.window, fault.hooks());
    ParticipantWindowTestAccess::requestDxScene(*old.window);
    wait([&] { return fault.presentEntered.load(std::memory_order_acquire); });
    wait([&] { return !ParticipantWindowTestAccess::usingGpu(*old.window); });
    TEST_CHECK(ticks >= 20 && maximum < 500 && clock.elapsed() - previous < 500);
    const auto diagnostics = old.window->renderDiagnostics();
    TEST_CHECK(diagnostics.actual_backend == RenderBackend::QtCpu &&
        diagnostics.gpu_failure == RenderGpuFailure::PresentationTimeout &&
        diagnostics.fallback_reason == RenderFallbackReason::GpuRuntimeFailed);
    cpuFrame(*old.window, oldSource, QColor(20, 80, 220));
    QPointer<MeetingUI::MeetingRoomWindow> oldWindow(old.window.get());
    QElapsedTimer close; close.start();
    TEST_CHECK(old.window->close()); old.window.reset();
    TEST_CHECK(close.elapsed() < 500 && !oldWindow);
    TEST_CHECK(surface && IsWindow(hwnd) && GetModuleHandleW(modulePath.c_str()));
    WindowFixture successor; successor.open();
    const auto newGeneration = ParticipantWindowTestAccess::renderGeneration(*successor.window);
    TEST_CHECK(newGeneration != oldGeneration);
    auto newSource = std::make_shared<livekit::VideoSource>(16, 9);
    ParticipantWindowTestAccess::bindLocal(*successor.window, newSource);
    cpuFrame(*successor.window, newSource, QColor(220, 80, 20));
    const auto successorDiagnostics = successor.window->renderDiagnostics();
    const auto delivered = ParticipantWindowTestAccess::statistics(*successor.window).delivered_to_qt_cpu;
    auto late = livekit::VideoFrame::create(16, 9, livekit::VideoBufferType::RGBA);
    std::memset(late.data(), 0, late.dataSize()); oldSource->captureFrame(late, {});
    fault.unblock();
    ParticipantWindowTestAccess::waitDxOwners();
    ParticipantWindowTestAccess::render(*successor.window);
    TEST_CHECK(!surface && !IsWindow(hwnd) && !GetModuleHandleW(modulePath.c_str()));
    TEST_CHECK(successor.window->renderDiagnostics() == successorDiagnostics &&
        ParticipantWindowTestAccess::statistics(*successor.window).delivered_to_qt_cpu == delivered &&
        ParticipantWindowTestAccess::tileFrame(ParticipantWindowTestAccess::local(*successor.window)).pixelColor(0, 0) == QColor(220, 80, 20));
    TEST_CHECK(maximum < 500 && clock.elapsed() - previous < 500);
    std::cout << "DX11_OWNER PASS: typed Present/device-loss/owner-exception, blocked cleanup/Present, CPU frames, close<500ms, retained surface/module, old generation/frame/callback isolation; heartbeat ticks="
        << ticks << " maxUiGapMs=" << maximum << " (asserted <500ms)\n";
}

void DepartureNoticeLifetime() {
    for (const bool duplicateIdentity : {false, true}) {
        for (const bool destroyWhileOpen : {false, true}) {
            WindowFixture fixture;
            fixture.open();
            auto *window = fixture.window.release();
            window->setAttribute(Qt::WA_DeleteOnClose);
            QPointer<MeetingUI::MeetingRoomWindow> guard(window);
            bool queuedCallbackRan = false;
            QTimer::singleShot(0, window, [&] { queuedCallbackRan = true; });
            const auto notify = [&] {
                if (duplicateIdentity)
                    fixture.coordinator->meetingKickOff(livekit::RoomDisconnectReason::DuplicateIdentity);
                else
                    fixture.coordinator->kickedOff(QStringLiteral("host removed participant"), 2);
            };
            notify();
            TEST_CHECK(guard && !queuedCallbackRan); // no nested event loop
            QPointer<QMessageBox> notice(window->findChild<QMessageBox*>("meetingDepartureNotice"));
            TEST_CHECK(notice && notice->isVisible() && notice->testAttribute(Qt::WA_DeleteOnClose));
            notify();
            TEST_CHECK(window->findChildren<QMessageBox*>("meetingDepartureNotice").size() == 1);
            fixture.coordinator->meetingLeft();
            fixture.pump();
            TEST_CHECK(guard && notice && queuedCallbackRan);
            if (destroyWhileOpen) {
                // Reproduce the attachment's queued parent destruction while
                // the notice is still visible (session invalidation/teardown).
                window->deleteLater();
            } else {
                notice->accept();
            }
            fixture.pump();
            TEST_CHECK(!guard && !notice);
        }
    }
    std::cout << "DEPARTURE_NOTICE PASS: kick/duplicate identity, leave-before-ack, parent deletion, deduplication\n";
}

int WindowAcceptanceMain(int argc, char **argv) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QStandardPaths::setTestModeEnabled(true);
    crl::details::init();
    // Match Qt's main-scope application lifetime: Qt post routines run before
    // process-static caches are destroyed, not from an atexit application.
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("IDA2ParticipantWindowAcceptance"));
    MeetingUI::MeetingUiIntegration integration;
    Ui::Integration::Set(&integration);
    style::StartManager(100);
    const bool wrappedThread = webrtc::Thread::Current() == nullptr;
    if (wrappedThread) webrtc::ThreadManager::Instance()->WrapCurrentThread();
    TEST_CHECK(webrtc::Thread::Current());
    QTemporaryDir settingsDirectory;
    TEST_CHECK(settingsDirectory.isValid());
    QSettings probe(settingsDirectory.filePath("explicit.ini"), QSettings::IniFormat);
    TEST_CHECK(probe.format() == QSettings::IniFormat &&
        QDir::cleanPath(probe.fileName()).startsWith(QDir::cleanPath(settingsDirectory.path()) + "/"));
    // No Notify or account callback is emitted by this target. All Coordinator
    // instances above use explicitly injected temporary SessionManager objects.
    if (application.arguments().contains("--window-resize-before")) {
        NativeWindowResizeAcceptance(true);
    } else if (application.arguments().contains("--window-resize")) {
        NativeWindowResizeAcceptance();
    } else if (application.arguments().contains("--card-chrome-before")) {
        CardChromeAcceptance(true);
    } else if (application.arguments().contains("--card-chrome")) {
        CardChromeAcceptance();
    } else if (application.arguments().contains("--track-presentation-before")) {
        TrackPresentationAcceptance(true);
    } else if (application.arguments().contains("--track-presentation")) {
        TrackPresentationAcceptance();
        TrackPresentationGpuAcceptance();
    } else if (application.arguments().contains("--account-lifecycle")) {
        AccountLogoutAndDuplicateLogin();
    } else if (application.arguments().contains("--departure-notice")) {
        DepartureNoticeLifetime();
    } else if (application.arguments().contains("--screen-share-gpu")) {
        ParticipantWindowTestAccess::checkGpuAspect();
    } else if (application.arguments().contains("--opengl-contract")) {
        RunOpenGlContract();
    } else if (application.arguments().contains("--live-meeting-dx11-driver-loss") ||
               application.arguments().contains("--live-meeting-dx11-driver-loss-smoke")) {
        const bool liveSmoke = application.arguments().contains("--live-meeting-dx11-driver-loss-smoke");
        qunsetenv("LIVEKIT_RENDER_BACKEND");
        const auto output = application.arguments().indexOf("--output");
        TEST_CHECK(output >= 0 && output + 1 < application.arguments().size());
        const auto url = qEnvironmentVariable("LIVEKIT_URL");
        const auto token = qEnvironmentVariable("LIVEKIT_RENDER_OBSERVER_TOKEN");
        TEST_CHECK(!url.isEmpty() && !token.isEmpty());
        OpenMeeting::initializeServiceEndpointPolicy(true);
        auto coordinator = OpenMeeting::MeetingCoordinator::create();
        OpenMeeting::MediaPreferences prefs;
        prefs.enableMicrophone = false;
        prefs.enableVideo = false;
        coordinator->connectDirectlyAsync(url, token, QStringLiteral("render-recovery"),
            QStringLiteral("render-observer"), prefs);
        MeetingUI::MeetingRoomWindow::Config config;
        config.audioMuted = true;
        config.videoEnabled = false;
        config.displayName = QStringLiteral("Live meeting render observer");
        auto window = ParticipantWindowTestAccess::create(coordinator, std::move(config));
        ParticipantWindowTestAccess::checkDriverLoss(*window, application.arguments()[output + 1],
            liveSmoke, false, true, coordinator);
        WindowPhase("live-meeting-window-reset");
        QElapsedTimer close; close.start(); window.reset();
        TEST_CHECK(close.elapsed() < 5000);
        WindowPhase("live-meeting-window-reset-complete");
        coordinator.reset();
        WindowPhase("live-meeting-coordinator-reset-complete");
        livekit::WebRTCManager::Instance().Deinitialize();
        WindowPhase("live-meeting-webrtc-deinitialize-complete");
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(L"livekit-render-opengl.dll"));
        TEST_CHECK(!GetModuleHandleW(L"livekit-render-dx11.dll"));
        std::cout << "DX11_LIVE_MEETING_TDR_LIFETIME PASS: room leave, window close, module unload\n";
    } else if (application.arguments().contains("--opengl-driver-loss") ||
               application.arguments().contains("--opengl-driver-loss-smoke") ||
               application.arguments().contains("--dx11-driver-loss") ||
               application.arguments().contains("--dx11-driver-loss-smoke")) {
        const bool angle = application.arguments().contains("--opengl-driver-loss") ||
            application.arguments().contains("--opengl-driver-loss-smoke");
        if (angle) qputenv("LIVEKIT_RENDER_BACKEND", "opengl");
        else qunsetenv("LIVEKIT_RENDER_BACKEND"); // Exercise the Windows default selection.
        const auto output = application.arguments().indexOf("--output");
        TEST_CHECK(output >= 0 && output + 1 < application.arguments().size());
        {
            WindowFixture fixture; fixture.open();
            ParticipantWindowTestAccess::checkDriverLoss(*fixture.window, application.arguments()[output + 1],
                application.arguments().contains(angle ? "--opengl-driver-loss-smoke" : "--dx11-driver-loss-smoke"), angle);
            QElapsedTimer close; close.start(); fixture.window.reset();
            TEST_CHECK(close.elapsed() < 1000);
        }
        if (angle) ParticipantWindowTestAccess::waitGlOwners();
        else ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(L"livekit-render-opengl.dll"));
        TEST_CHECK(!GetModuleHandleW(L"livekit-render-dx11.dll"));
        std::cout << (angle ? "ANGLE" : "DX11") << "_TDR_LIFETIME PASS: window close, render owner completion, module unload\n";
    } else if (application.arguments().contains("--opengl-window")) {
        qputenv("LIVEKIT_RENDER_BACKEND", "opengl");
        qputenv("LIVEKIT_PRESENTATION_TEST_GPU", "1");
        LocalRenderInputAcceptance();
        ParticipantWindowTestAccess::waitGlOwners();
        CardChromeAcceptance();
        ParticipantWindowTestAccess::waitGlOwners();
        {
            WindowFixture fixture; fixture.open();
            TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*fixture.window));
            ParticipantWindowTestAccess::checkGlStageAndLoss(*fixture.window);
        }
        ParticipantWindowTestAccess::waitGlOwners();
        const auto path = livekit::render::BackendModule::PathForBackend(
            std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()), LK_RENDER_BACKEND_OPENGL);
        TEST_CHECK(!GetModuleHandleW(path.c_str()));
        std::cout << "P3_GL_WINDOW PASS: asynchronous activation, I420/NV12/RGBA, chrome, Pin/PiP, CPU fallback, reload/unload\n";
    } else if (application.arguments().contains("--opengl-rapid-rebuild")) {
        AngleRapidRebuildAcceptance();
    } else if (application.arguments().contains("--opengl-diagnostics")) {
        qputenv("LIVEKIT_RENDER_BACKEND", "opengl");
        {
            WindowFixture fixture; fixture.open();
            TEST_CHECK(ParticipantWindowTestAccess::enableGpu(*fixture.window));
            ParticipantWindowTestAccess::checkGlDiagnostics(*fixture.window);
        }
        ParticipantWindowTestAccess::waitGlOwners();
        TEST_CHECK(!GetModuleHandleW(L"livekit-render-opengl.dll"));
    } else if (application.arguments().contains("--module-lifecycle")) {
        QTemporaryDir emptyDirectory;
        TEST_CHECK(emptyDirectory.isValid());
        TEST_CHECK(!livekit::render::CreateVideoCanvasFromDirectory(nullptr, emptyDirectory.path()));
        const auto modulePath = livekit::render::BackendModule::DefaultPath(
            std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()));
        LocalRenderInputAcceptance();
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
        qputenv("LIVEKIT_PRESENTATION_TEST_GPU", "1");
        CardChromeAcceptance();
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
        ParticipantWindowTestAccess::checkGpuAspect();
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
        std::cout << "P2_MODULE_WINDOW PASS: missing module, loaded display, queued fallback/unload, same-module reload, destroy\n";
    } else if (application.arguments().contains("--dx11-owner")) {
        const auto mode = application.arguments().indexOf("--dx11-owner");
        TEST_CHECK(mode + 1 < application.arguments().size());
        Dx11RenderOwnerAcceptance(application.arguments()[mode + 1]);
    } else if (application.arguments().contains("--render-multisession-lifecycle")) {
        RenderMultiSessionLifecycleAcceptance();
    } else if (application.arguments().contains("--module-fallback")) {
        const auto mode = application.arguments().indexOf("--module-fallback");
        TEST_CHECK(mode >= 0 && mode + 2 < application.arguments().size());
        QTemporaryDir directory;
        TEST_CHECK(directory.isValid());
        const auto root = std::filesystem::path(directory.path().toStdWString());
        const auto modulePath = livekit::render::BackendModule::DefaultPath(root);
        TEST_CHECK(QDir().mkpath(QString::fromStdWString(modulePath.parent_path().wstring())));

        livekit::render::VideoRenderSession session([](const std::string&, const QImage&) {});
        livekit::render::RenderDiagnostics diagnostics;
        TEST_CHECK(session.backend() == livekit::render::VideoRenderSession::Backend::QtCpu);
        qputenv("LIVEKIT_RENDER_BACKEND", "cpu");
        TEST_CHECK(!livekit::render::CreateVideoCanvasFromDirectory(nullptr, directory.path(), &diagnostics));
        TEST_CHECK(diagnostics.requested_backend == livekit::render::RenderBackend::QtCpu &&
            diagnostics.actual_backend == livekit::render::RenderBackend::QtCpu &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::UserSelectedCpu &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::None);
        qunsetenv("LIVEKIT_RENDER_BACKEND");
        TEST_CHECK(!livekit::render::CreateVideoCanvasFromDirectory(nullptr, directory.path(), &diagnostics));
        TEST_CHECK(diagnostics.gpu_failure == livekit::render::RenderGpuFailure::ModuleOpenFailed &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::ModuleLoadFailed);

        const auto installFixture = [&](const QString& source) {
            TEST_CHECK(QFile::remove(QString::fromStdWString(modulePath.wstring())) || !QFile::exists(QString::fromStdWString(modulePath.wstring())));
            TEST_CHECK(QFile::copy(source, QString::fromStdWString(modulePath.wstring())));
        };
        installFixture(application.arguments()[mode + 1]);
        TEST_CHECK(!livekit::render::CreateVideoCanvasFromDirectory(nullptr, directory.path(), &diagnostics));
        TEST_CHECK(diagnostics.gpu_failure == livekit::render::RenderGpuFailure::ModuleAbiMismatch &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::ModuleLoadFailed &&
            diagnostics.actual_backend == livekit::render::RenderBackend::QtCpu);
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));

        installFixture(application.arguments()[mode + 2]);
        std::unique_ptr<livekit::render::VideoCanvas> canvas(
            livekit::render::CreateVideoCanvasFromDirectory(nullptr, directory.path(), &diagnostics));
        TEST_CHECK(canvas && GetModuleHandleW(modulePath.c_str()));
        TEST_CHECK(diagnostics.abi_version == LK_RENDER_ABI_V1 &&
            diagnostics.actual_backend == livekit::render::RenderBackend::QtCpu &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::None);
        int unavailable = 0;
        session.UseGpuBackend([](const std::string&, livekit::render::VideoRenderFrame::Ptr) {});
        QObject::connect(canvas.get(), &livekit::render::VideoCanvas::rendererUnavailable,
            canvas.get(), [&] {
                ++unavailable;
                session.UseQtCpuBackend();
                canvas->shutdownRenderer();
                canvas->hide();
            }, Qt::QueuedConnection);
        canvas->resize(64, 64);
        canvas->show();
        QElapsedTimer wait;
        wait.start();
        while (!unavailable && wait.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        TEST_CHECK(unavailable == 1);
        TEST_CHECK(session.backend() == livekit::render::VideoRenderSession::Backend::QtCpu);
        ParticipantWindowTestAccess::waitDxOwners();
        TEST_CHECK(!canvas->rendererReady() && !GetModuleHandleW(modulePath.c_str()));
        diagnostics = canvas->renderDiagnostics();
        TEST_CHECK(diagnostics.actual_backend == livekit::render::RenderBackend::QtCpu &&
            diagnostics.abi_version == LK_RENDER_ABI_V1 && diagnostics.module_version == 0 &&
            diagnostics.gpu_failure == livekit::render::RenderGpuFailure::DeviceCreateFailed &&
            diagnostics.fallback_reason == livekit::render::RenderFallbackReason::GpuInitializationFailed);
        TEST_CHECK(!livekit::render::RenderDiagnosticsSafeSummary(diagnostics).contains("token", Qt::CaseInsensitive));
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        TEST_CHECK(unavailable == 1);
        canvas.reset();
        TEST_CHECK(!GetModuleHandleW(modulePath.c_str()));
        std::cout << "PRODUCTION_MODULE_FALLBACK PASS: explicit CPU, missing module, ABI rejection, initialization failure, CPU fallback, unload\n";
    } else if (application.arguments().contains("--local-render-input")) {
        LocalRenderInputAcceptance();
    } else if (application.arguments().contains("--screen-share")) {
        ScreenShareWindowControls();
        ScreenShareCameraCoexistence();
    } else if (application.arguments().contains("--pr-sec-005")) {
        PrSec005InvitationContract();
    } else if (application.arguments().contains("--gap-full-restart")) {
        GapWindowFullRestartUnsubscribe();
    } else if (application.arguments().contains("--gap-identity-boundaries")) {
        GapWindowFullRestartIdentityBoundaries();
    } else if (application.arguments().contains("--gap-video-lease")) {
        GapWindowQueuedVideoBindingLease();
    } else if (application.arguments().contains("--ak-window-late")) {
        AkWindowRetiredPresentation(false);
        std::cout << "AK_WINDOW_LATE_EXECUTED=1 PASSED=1 FAILED=0" << std::endl;
    } else if (application.arguments().contains("--ak-window-inmeeting")) {
        AkWindowRetiredPresentation(true);
        std::cout << "AK_WINDOW_INMEETING_EXECUTED=1 PASSED=1 FAILED=0" << std::endl;
    } else if (application.arguments().contains("--ak-window-network")) {
        AkWindowInitialRoster();
        AkWindowSoftResume();
        AkWindowFullRestart();
        GapWindowSoftResumeUnsubscribe();
        GapWindowFullRestartUnsubscribe();
        GapWindowOrderedSenderAndResumeSnapshot();
        GapWindowDefaultFalseSoftResume();
        GapWindowFullRestartIdentityBoundaries();
        GapWindowDisconnectInvalidatesOldSender();
        std::cout << "AK_WINDOW_NETWORK_EXECUTED=9 PASSED=9 FAILED=0" << std::endl;
    } else {
        std::cout << "AK_WINDOW_PLANNED=15 (PR-SEC-005 invitation; F4/F5 and GAP video lease local precondition; F1/F2/F3 and GAP recovery real loopback)" << std::endl;
        PrSec005InvitationContract();
        AkWindowAliveLate();
        AkWindowOldTrack();
        AkWindowRetiredPresentation(false);
        AkWindowRetiredPresentation(true);
        AkWindowInitialRoster();
        AkWindowSoftResume();
        AkWindowFullRestart();
        GapWindowSoftResumeUnsubscribe();
        GapWindowFullRestartUnsubscribe();
        GapWindowOrderedSenderAndResumeSnapshot();
        GapWindowDefaultFalseSoftResume();
        GapWindowFullRestartIdentityBoundaries();
        GapWindowDisconnectInvalidatesOldSender();
        GapWindowQueuedVideoBindingLease();
        std::cout << "AK_WINDOW_EXECUTED=15 PASSED=15 FAILED=0" << std::endl;
        ScreenShareWindowControls();
        ScreenShareCameraCoexistence();
        DepartureNoticeLifetime();
        AccountLogoutAndDuplicateLogin();
        TrackPresentationAcceptance();
        TrackPresentationGpuAcceptance();
        CardChromeAcceptance();
        NativeWindowResizeAcceptance();
        if ((GetSystemMetrics(SM_REMOTESESSION) == 0))
            ParticipantWindowTestAccess::checkGpuAspect();
        LocalRenderInputAcceptance();
    }
    if (wrappedThread) webrtc::ThreadManager::Instance()->UnwrapCurrentThread();
    style::StopManager();
    return 0;
}

} // namespace

int main(int argc, char **argv) { return WindowAcceptanceMain(argc, argv); }

#else // Original core target: QCoreApplication and synchronous log hook.

#include "src/core/meeting_coordinator.h"
#include "src/core/remote_track_publication.h"
#include "src/ui/meeting_log_console.h"
#include "tests/support/test_check.h"
#include "api/notifier.h"
#include "pc/audio_track.h"
#include "rtc_base/ref_counted_object.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <functional>
#include <future>
#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace livekit {

// Only supplies the resolved-attach fixture's initial state and calls the
// existing production operations. It never replaces their validity checks.
class ParticipantSnapshotRoomTestAccess final {
public:
    static void establishConnectedAttachPrecondition(Room &room,
                                                     bool autoSubscribe = true) {
        std::lock_guard lock(room.room_mutex_);
        room.ResetSubscriptionSessionLocked(autoSubscribe);
        room.connection_state_ = ConnectionState::Connected;
    }
    static void attach(Room &room, const std::shared_ptr<RemoteParticipant> &participant,
                       webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
                       const std::string &sid) {
        room.AttachRemoteTrackToParticipant(participant, std::move(track), nullptr, sid);
    }
    static uint64_t bindingSerial(Room &room, const RemoteTrackPublication *publication) {
        std::lock_guard lock(room.room_mutex_);
        const auto binding = room.current_remote_binding_serials_.find(publication);
        return binding == room.current_remote_binding_serials_.end() ? 0 : binding->second;
    }
    static std::size_t bindingCount(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        return room.remote_track_sinks_.size();
    }
    static void detach(Room &room, RemoteTrackPublication *publication, uint64_t serial) {
        room.DetachRemotePublicationMedia(publication, false, serial);
    }
    static proto::SyncState syncState(Room &room) { return room.BuildSyncState(); }
    static void pauseParticipantDrain(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        room.participant_event_drain_paused_for_testing_ = true;
    }
    static std::size_t pendingParticipantEvents(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        return room.participant_events_.size();
    }
    static std::size_t pausedDrainAttempts(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        return room.participant_event_paused_attempts_for_testing_;
    }
    static void resumeParticipantDrain(Room &room) {
        {
            std::lock_guard lock(room.room_mutex_);
            room.participant_event_drain_paused_for_testing_ = false;
        }
        room.DrainParticipantEvents();
    }
    static ParticipantTicket firstPendingTicket(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        TEST_CHECK(!room.participant_events_.empty());
        return room.participant_events_.front().participant.ticket;
    }
    static bool containsListener(Room &room, const RoomListener *listener) {
        std::lock_guard lock(room.room_mutex_);
        return std::any_of(room.listeners_.begin(), room.listeners_.end(),
            [listener](const auto &current) { return current.get() == listener; });
    }
};

} // namespace livekit

namespace OpenMeeting {

class SessionManagerTestAccess final {
public:
    using ScopedSession = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;

    static ScopedSession create(std::unique_ptr<QSettings> settings) {
        return ScopedSession(new SessionManager(std::move(settings)), &destroy);
    }

private:
    static void destroy(SessionManager *session) { delete session; }
};

class MeetingCoordinatorTestAccess final {
public:
    static std::unique_ptr<MeetingCoordinator> create(SessionManager &session) {
        MeetingCoordinator::AdmissionBackend backend;
        return std::unique_ptr<MeetingCoordinator>(
            new MeetingCoordinator(session, std::move(backend), nullptr));
    }

    static void bindSession(MeetingCoordinator &coordinator,
                            const std::shared_ptr<livekit::Room> &room,
                            const std::shared_ptr<MeetingSessionRuntime> &runtime) {
        coordinator._room = room;
        coordinator._sessionRuntime = runtime;
        coordinator._nextSessionGeneration = runtime->generation();
        coordinator._sessionRunning.store(true, std::memory_order_release);
    }

    static void apply(MeetingCoordinator &coordinator,
                      uint64_t generation,
                      const livekit::ParticipantEvent &event) {
        coordinator.applyParticipantEventOnUiThread(generation, event);
    }

    static std::vector<ParticipantInfo> participants(const MeetingCoordinator &coordinator) {
        return coordinator.participants();
    }

    static std::size_t inboundLedgerSize(const MeetingCoordinator &coordinator) {
        return coordinator._inboundTransferLedger.size();
    }

    static void markMeetingActive(MeetingCoordinator &coordinator) {
        coordinator._state = MeetingState::InMeeting;
    }

    static void markCommittedReconnect(MeetingCoordinator &coordinator) {
        coordinator._state = MeetingState::Reconnecting;
        coordinator._startupCommitted = true;
    }

    static void prepareStartup(MeetingCoordinator &coordinator,
                               bool audioMuted = false,
                               bool videoEnabled = true) {
        coordinator._state = MeetingState::InMeeting;
        coordinator._startupCommitted = false;
        coordinator._startupReconnectPending = false;
        coordinator._startupListenOnly = false;
        coordinator._localAudioTrack.reset();
        coordinator._localVideoTrack.reset();
        coordinator._audioMuted = audioMuted;
        coordinator._videoEnabled = videoEnabled;
        coordinator._participants.clear();
        coordinator.ensureLocalParticipant();
    }

    static uint64_t sessionGeneration(const MeetingCoordinator &coordinator) {
        return coordinator._nextSessionGeneration;
    }

    static void queueSuccessfulStartup(MeetingCoordinator &coordinator,
                                       uint64_t sessionGeneration) {
        auto *target = &coordinator;
        QMetaObject::invokeMethod(target, [target, sessionGeneration]() {
            target->completeRoomStartupOnUiThread(
                sessionGeneration, {}, {}, target->_audioMuted, target->_videoEnabled,
                target->_audioMuted, target->_videoEnabled);
        }, Qt::QueuedConnection);
    }

    static void parseMetadata(MeetingCoordinator &coordinator, const std::string &metadata) {
        coordinator.parseRoomMetadata(metadata);
    }

    static void queueDegradedStartup(MeetingCoordinator &coordinator,
                                     uint64_t sessionGeneration) {
        auto *target = &coordinator;
        QMetaObject::invokeMethod(target, [target, sessionGeneration]() {
            target->completeRoomStartupDegradedOnUiThread(
                sessionGeneration, QStringLiteral("local-media"), QStringLiteral("test-only"));
        }, Qt::QueuedConnection);
    }

    static bool startupCommitted(const MeetingCoordinator &coordinator) {
        return coordinator._startupCommitted;
    }

    static bool reconnectPending(const MeetingCoordinator &coordinator) {
        return coordinator._startupReconnectPending;
    }

    static bool startupListenOnly(const MeetingCoordinator &coordinator) {
        return coordinator._startupListenOnly;
    }

    static bool hasNoLocalTracks(const MeetingCoordinator &coordinator) {
        return !coordinator._localAudioTrack && !coordinator._localVideoTrack;
    }

    static bool localProjection(const MeetingCoordinator &coordinator,
                                bool audioMuted,
                                bool videoEnabled) {
        const auto local = std::find_if(coordinator._participants.begin(), coordinator._participants.end(),
            [](const auto &entry) { return entry.second.isLocal; });
        return local != coordinator._participants.end() &&
            local->second.isAudioMuted == audioMuted &&
            local->second.isVideoEnabled == videoEnabled;
    }

    static void invalidateAdmissionOnly(MeetingCoordinator &coordinator) {
        coordinator.invalidateAdmission();
    }

    static std::shared_ptr<livekit::RoomListener> listener(
        MeetingCoordinator &coordinator,
        const std::shared_ptr<MeetingSessionRuntime> &runtime) {
        return coordinator.participantEventListenerForTesting(runtime);
    }

    static bool sessionReleased(const MeetingCoordinator &coordinator) {
        return !coordinator._sessionRunning.load() && !coordinator._room &&
            !coordinator._sessionRuntime && coordinator._inboundTransferLedger.empty();
    }
    static std::shared_ptr<livekit::RoomListener> createOwnedSession(MeetingCoordinator &coordinator) {
        TEST_CHECK(!coordinator._ioContext && !coordinator._ioThread.joinable());
        coordinator._ioContext = std::make_unique<asio::io_context>();
        coordinator._workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            coordinator._ioContext->get_executor());
        coordinator._sessionRuntime = std::make_shared<MeetingSessionRuntime>(
            *coordinator._ioContext, 61, QStringLiteral("local-user"));
        coordinator._room = livekit::Room::Create(coordinator._ioContext->get_executor());
        coordinator._nextSessionGeneration = coordinator._sessionRuntime->generation();
        coordinator._sessionRunning.store(true, std::memory_order_release);
        coordinator._state = MeetingState::InMeeting;
        auto listener = coordinator.participantEventListenerForTesting(coordinator._sessionRuntime, true);
        coordinator._room->AddListener(listener);
        return listener;
    }
    static asio::io_context &ownedContext(MeetingCoordinator &coordinator) { return *coordinator._ioContext; }
    static std::weak_ptr<MeetingSessionRuntime> ownedRuntime(MeetingCoordinator &coordinator) {
        return coordinator._sessionRuntime;
    }
    static std::weak_ptr<livekit::Room> ownedRoom(MeetingCoordinator &coordinator) { return coordinator._room; }
    static void startOwnedWorker(MeetingCoordinator &coordinator, std::shared_ptr<std::atomic<bool>> exited) {
        auto *context = coordinator._ioContext.get();
        coordinator._ioThread = std::thread([context, room = coordinator._room,
            runtime = coordinator._sessionRuntime, exited = std::move(exited)] {
            context->run();
            exited->store(true, std::memory_order_release);
        });
    }
    static bool ownedSessionReleased(const MeetingCoordinator &coordinator) {
        return sessionReleased(coordinator) && !coordinator._ioContext && !coordinator._workGuard &&
            !coordinator._roomListener && !coordinator._ioThread.joinable();
    }
    static void stopAgain(MeetingCoordinator &coordinator) { coordinator.stopRoomSession(); }
    static void cancel(MeetingCoordinator &coordinator, const livekit::ParticipantKey &key) {
        coordinator.cancelInboundTransfersForParticipant(key);
    }
};

} // namespace OpenMeeting

namespace MeetingUI {
std::function<void(const QString &)> participantSnapshotLogHook;
std::function<void(const QString &, const QString &)> participantSnapshotSecurityLogHook;

// This target's own sink can synchronously reenter exactly where production
// LogToConsole runs; the other regression targets retain their original sink.
void LogToConsole(LogCategory, const QString &tag, const QString &message) {
    const auto hook = participantSnapshotLogHook;
    if (hook) hook(tag);
    const auto securityHook = participantSnapshotSecurityLogHook;
    if (securityHook) securityHook(tag, message);
}
} // namespace MeetingUI

namespace {

constexpr uint64_t kCoordinatorGeneration = 41;

void DrainQt() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void DrainNative(asio::io_context &context) {
    context.restart();
    while (context.poll() != 0) {
    }
}

livekit::proto::ParticipantUpdate MakeParticipantUpdate(
    const std::string &sid,
    const std::string &identity,
    const std::string &name,
    livekit::proto::ParticipantInfo::State state,
    bool withVideo = true) {
    livekit::proto::ParticipantUpdate update;
    auto *participant = update.add_participants();
    participant->set_sid(sid);
    participant->set_identity(identity);
    participant->set_name(name);
    participant->set_state(state);
    auto *permission = participant->mutable_permission();
    permission->set_can_subscribe(true);
    permission->set_can_publish(true);
    permission->set_can_publish_data(true);
    permission->set_can_update_metadata(true);
    if (withVideo && state != livekit::proto::ParticipantInfo::DISCONNECTED) {
        auto *track = participant->add_tracks();
        track->set_sid("TR_" + sid);
        track->set_name("camera");
        track->set_type(livekit::proto::TrackType::VIDEO);
        track->set_muted(false);
    }
    return update;
}

const OpenMeeting::ParticipantInfo *FindParticipant(
    const std::vector<OpenMeeting::ParticipantInfo> &participants,
    const QString &identity) {
    const auto found = std::find_if(
        participants.begin(), participants.end(), [&](const auto &participant) {
            return participant.identity == identity;
        });
    return found == participants.end() ? nullptr : &*found;
}

class ValueBridge final : public livekit::RoomListener {
public:
    bool ConsumesParticipantEvents() const override { return true; }

    void OnParticipantEvent(const livekit::ParticipantEvent &event) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(event);
        }
    }

    std::vector<livekit::ParticipantEvent> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<livekit::ParticipantEvent> events_;
};

class Fixture {
public:
    Fixture()
        : session(OpenMeeting::SessionManagerTestAccess::create(
              std::make_unique<QSettings>(settingsDirectory.filePath("settings.ini"),
                                          QSettings::IniFormat))),
          room(livekit::Room::Create(io.get_executor())),
          runtime(std::make_shared<OpenMeeting::MeetingSessionRuntime>(
              io, kCoordinatorGeneration, QStringLiteral("local-user"))),
          coordinator(OpenMeeting::MeetingCoordinatorTestAccess::create(*session)),
          bridge(std::make_shared<ValueBridge>()) {
        TEST_CHECK(settingsDirectory.isValid());
        OpenMeeting::MeetingCoordinatorTestAccess::bindSession(
            *coordinator, room, runtime);
        listener = OpenMeeting::MeetingCoordinatorTestAccess::listener(*coordinator, runtime);
        room->AddListener(listener);
        room->AddListener(bridge);
    }

    ~Fixture() {
        room->RemoveListener(bridge);
        destroyCoordinator();
        DrainQt();
    }

    void destroyCoordinator() {
        room->RemoveListener(listener);
        coordinator.reset();
    }

    void replaceSession() {
        coordinator->leaveMeetingAsync(false);
        room->RemoveListener(listener);
        runtime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(
            io, runtime->generation() + 1, QStringLiteral("local-user"));
        OpenMeeting::MeetingCoordinatorTestAccess::bindSession(*coordinator, room, runtime);
        OpenMeeting::MeetingCoordinatorTestAccess::markMeetingActive(*coordinator);
        listener = OpenMeeting::MeetingCoordinatorTestAccess::listener(*coordinator, runtime);
        room->AddListener(listener);
    }

    QTemporaryDir settingsDirectory;
    OpenMeeting::SessionManagerTestAccess::ScopedSession session;
    asio::io_context io;
    std::shared_ptr<livekit::Room> room;
    std::shared_ptr<OpenMeeting::MeetingSessionRuntime> runtime;
    std::unique_ptr<OpenMeeting::MeetingCoordinator> coordinator;
    std::shared_ptr<ValueBridge> bridge;
    std::shared_ptr<livekit::RoomListener> listener;
};

std::vector<uint8_t> JsonPayload(const QJsonObject &object) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return {bytes.begin(), bytes.end()};
}

bool IsSessionOnlyNotify(const openmeeting::meeting::NotifyMeetingData &notify) {
    return !notify.has_kickoffmeetingdata() ||
        notify.kickoffmeetingdata().reasoncode() != openmeeting::meeting::KickOffReason::DuplicatedLogin;
}

std::vector<uint8_t> SessionOnlyNotifyBytes(const openmeeting::meeting::NotifyMeetingData &notify) {
    // These core scenarios exercise room-only notifications. Account invalidation
    // (including proto3's default 0) is covered by the isolated Qt window fixture.
    TEST_CHECK(IsSessionOnlyNotify(notify));
    const auto bytes = notify.SerializeAsString();
    return {bytes.begin(), bytes.end()};
}

void CompletedLeaveHasOneTerminal() {
    Fixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::markMeetingActive(*fixture.coordinator);
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_TERMINAL", "terminal-peer", "terminal-peer",
        livekit::proto::ParticipantInfo::ACTIVE, false));
    DrainNative(fixture.io);
    DrainQt();

    int started = 0;
    int completed = 0;
    int failed = 0;
    int messages = 0;
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &, const QString &, const QString &,
                         const QString &, qint64, int64_t) { ++started; });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &, const QString &, const QString &,
                         const QString &, const QByteArray &) {
        ++completed;
        fixture.coordinator->leaveMeetingAsync(false);
    });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &) { ++failed; });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaMessageReceived,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &, const QString &, const QString &,
                         const QByteArray &) { ++messages; });

    QJsonObject packet;
    packet[QStringLiteral("om_type")] = QStringLiteral("media_start");
    packet[QStringLiteral("transferId")] = QStringLiteral("terminal-wire");
    packet[QStringLiteral("totalChunks")] = 1;
    packet[QStringLiteral("mediaType")] = QStringLiteral("file");
    packet[QStringLiteral("fileName")] = QStringLiteral("terminal.bin");
    packet[QStringLiteral("totalSize")] = 5;
    fixture.room->OnIncomingDataPacket(JsonPayload(packet), "PA_TERMINAL", "chat");
    packet[QStringLiteral("om_type")] = QStringLiteral("media_chunk");
    packet[QStringLiteral("chunkIndex")] = 0;
    packet[QStringLiteral("chunkData")] = QStringLiteral("aGVsbG8=");
    fixture.room->OnIncomingDataPacket(JsonPayload(packet), "PA_TERMINAL", "chat");
    DrainNative(fixture.io); // Room listener -> Qt value delivery.
    DrainQt();              // Coordinator -> transfer strand.
    DrainNative(fixture.io); // Real aggregation -> Qt completion.
    DrainQt();              // completed slot synchronously leaves the session.

    std::cout << "completed-leave: started=" << started << " completed=" << completed
              << " failed=" << failed << " messages=" << messages << std::endl;
    TEST_CHECK(started == 1);
    TEST_CHECK(completed == 1);
    TEST_CHECK(failed == 0);
    TEST_CHECK(messages == 0);
    TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(
                   *fixture.coordinator) == 0);
}

void PumpPipeline(Fixture &fixture) {
    // Deterministic queue stages, with no sleep or wall-clock race.
    for (int stage = 0; stage != 4; ++stage) {
        DrainNative(fixture.io);
        DrainQt();
    }
}

void AddSender(Fixture &fixture) {
    OpenMeeting::MeetingCoordinatorTestAccess::markMeetingActive(*fixture.coordinator);
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_REENTRY", "reentry-peer", "reentry-peer",
        livekit::proto::ParticipantInfo::ACTIVE, false));
    PumpPipeline(fixture);
}

void SendMedia(Fixture &fixture, bool startOnly = false) {
    QJsonObject packet;
    packet[QStringLiteral("om_type")] = QStringLiteral("media_start");
    packet[QStringLiteral("transferId")] = QStringLiteral("same-wire");
    packet[QStringLiteral("totalChunks")] = 1;
    packet[QStringLiteral("mediaType")] = QStringLiteral("file");
    packet[QStringLiteral("fileName")] = QStringLiteral("reentry.bin");
    packet[QStringLiteral("totalSize")] = 5;
    fixture.room->OnIncomingDataPacket(JsonPayload(packet), "PA_REENTRY", "chat");
    if (startOnly) return;
    packet[QStringLiteral("om_type")] = QStringLiteral("media_chunk");
    packet[QStringLiteral("chunkIndex")] = 0;
    packet[QStringLiteral("chunkData")] = QStringLiteral("aGVsbG8=");
    fixture.room->OnIncomingDataPacket(JsonPayload(packet), "PA_REENTRY", "chat");
}

enum class ReentryAction { None, Leave, Destroy, ReplaceSession };
enum class TransferEffect { Started, Progress99, Progress100, Completed, Message };

void ApplyReentry(Fixture &fixture, ReentryAction action) {
    if (action == ReentryAction::Leave) fixture.coordinator->leaveMeetingAsync(false);
    if (action == ReentryAction::Destroy) fixture.destroyCoordinator();
    if (action == ReentryAction::ReplaceSession) fixture.replaceSession();
}

void TransferReentry(TransferEffect boundary, ReentryAction action) {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    bool acted = false;
    int started = 0, progress99 = 0, progress100 = 0, completed = 0, failed = 0, messages = 0;
    const auto effect = [&](TransferEffect current) {
        if (!acted && current == boundary && action != ReentryAction::None) {
            acted = true;
            ApplyReentry(fixture, action);
        }
    };
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted, &observer,
        [&](const QString &, const QString &, const QString &, const QString &,
            const QString &, qint64, int64_t) { ++started; effect(TransferEffect::Started); });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingProgress, &observer,
        [&](const QString &, int progress) {
            if (progress == 100) { ++progress100; effect(TransferEffect::Progress100); }
            else { ++progress99; effect(TransferEffect::Progress99); }
        });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted, &observer,
        [&](const QString &, const QString &, const QString &, const QString &,
            const QString &, const QByteArray &bytes) {
            TEST_CHECK(bytes == QByteArray("hello"));
            ++completed; effect(TransferEffect::Completed);
        });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed, &observer,
        [&](const QString &, const QString &) { ++failed; });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaMessageReceived, &observer,
        [&](const QString &, const QString &, const QString &, const QString &, const QByteArray &) {
            ++messages; effect(TransferEffect::Message);
        });
    SendMedia(fixture);
    PumpPipeline(fixture);
    const bool successful = action == ReentryAction::None ||
        boundary == TransferEffect::Completed || boundary == TransferEffect::Message;
    TEST_CHECK(started == 1);
    TEST_CHECK(completed == (successful ? 1 : 0));
    TEST_CHECK(failed == (successful ? 0 : 1));
    TEST_CHECK(messages == ((action == ReentryAction::None || boundary == TransferEffect::Message) ? 1 : 0));
    if (action != ReentryAction::None) TEST_CHECK(acted);
    if (fixture.coordinator) {
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 0);
        if (action == ReentryAction::Leave) {
            TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::sessionReleased(*fixture.coordinator));
            TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
        }
    } else TEST_CHECK(action == ReentryAction::Destroy);
    // A new session remains usable after the old continuation was rejected.
    if (action == ReentryAction::ReplaceSession) {
        fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
            "PA_REENTRY", "reentry-peer", "successor",
            livekit::proto::ParticipantInfo::ACTIVE, false));
        PumpPipeline(fixture);
        SendMedia(fixture);
        PumpPipeline(fixture);
        TEST_CHECK(completed == (successful ? 2 : 1));
        TEST_CHECK(messages == 1);
        TEST_CHECK(failed == (successful ? 0 : 1));
    }
    std::cout << "transfer-reentry boundary=" << static_cast<int>(boundary)
              << " action=" << static_cast<int>(action) << " PASS" << std::endl;
}

void RosterReentry(bool replacementGeneration, ReentryAction action) {
    Fixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::markMeetingActive(*fixture.coordinator);
    // Explicit immutable roster fixtures exercise the production listener's
    // generation reset branch. They do not claim a real transport reconnect.
    auto membership = std::make_shared<livekit::MembershipState>(
        livekit::ParticipantKey{1, 101, "PA_ROSTER", "roster-peer"});
    livekit::ParticipantEvent roster;
    roster.kind = livekit::ParticipantEventKind::Upsert;
    roster.native_room_generation = 1;
    roster.event_sequence = 1;
    roster.participant.key = membership->key;
    roster.participant.ticket = membership;
    roster.participant.state.sid = "PA_ROSTER";
    roster.participant.state.identity = "roster-peer";
    roster.participant.state.name = "roster";
    if (replacementGeneration) {
        fixture.listener->OnParticipantEvent(roster);
        DrainQt();
        membership->active.store(false);
        membership = std::make_shared<livekit::MembershipState>(
            livekit::ParticipantKey{2, 102, "PA_ROSTER", "roster-peer"});
        roster.native_room_generation = 2;
        roster.event_sequence = 2;
        roster.participant.key = membership->key;
        roster.participant.ticket = membership;
    }
    QObject observer;
    int updates = 0, joined = 0, left = 0;
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::participantsUpdated, &observer,
        [&](const std::vector<OpenMeeting::ParticipantInfo> &) {
            ++updates;
            if (updates == 1) ApplyReentry(fixture, action);
        });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::participantJoined, &observer,
        [&](const QString &, const QString &) { ++joined; });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::participantLeft, &observer,
        [&](const QString &) { ++left; });
    fixture.listener->OnParticipantEvent(roster);
    DrainQt();
    TEST_CHECK(updates == 1);
    TEST_CHECK(joined == 0);
    TEST_CHECK(left == 0);
    if (fixture.coordinator) {
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::sessionReleased(*fixture.coordinator));
    }
    std::cout << "roster-reentry replacement=" << replacementGeneration
              << " action=" << static_cast<int>(action) << " PASS" << std::endl;
}

void CancelRetiredInstancePreservesSuccessor() {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    std::vector<QString> startedIds, failedIds, completedIds;
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted, &observer,
        [&](const QString &id, const QString &, const QString &, const QString &,
            const QString &, qint64, int64_t) { startedIds.push_back(id); });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed, &observer,
        [&](const QString &id, const QString &) { failedIds.push_back(id); });
    QObject::connect(fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted, &observer,
        [&](const QString &id, const QString &, const QString &, const QString &,
            const QString &, const QByteArray &) { completedIds.push_back(id); });
    SendMedia(fixture, true);
    PumpPipeline(fixture);
    TEST_CHECK(startedIds.size() == 1);
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_REENTRY", "reentry-peer", "", livekit::proto::ParticipantInfo::DISCONNECTED, false));
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_REENTRY", "reentry-peer", "successor", livekit::proto::ParticipantInfo::ACTIVE, false));
    DrainNative(fixture.io);
    DrainQt(); // Old cancellation is still on the strand while successor is visible.
    SendMedia(fixture);
    PumpPipeline(fixture);
    TEST_CHECK(startedIds.size() == 2);
    TEST_CHECK(startedIds[0] != startedIds[1]);
    TEST_CHECK(failedIds.size() == 1 && failedIds[0] == startedIds[0]);
    TEST_CHECK(completedIds.size() == 1 && completedIds[0] == startedIds[1]);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 0);
    std::cout << "retired-instance-cancellation PASS" << std::endl;
}

void SendWrappedData(Fixture &fixture, const std::string &sid, const std::string &identity,
                     const std::vector<uint8_t> &payload);

void DataEffectReentry(bool logBoundary, ReentryAction action) {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    int effects = 0;
    if (logBoundary) {
        MeetingUI::participantSnapshotLogHook = [&](const QString &tag) {
            if (tag == QStringLiteral("KICK_OFF")) {
                MeetingUI::participantSnapshotLogHook = {};
                ++effects;
                ApplyReentry(fixture, action);
            }
        };
    } else {
        QObject::connect(fixture.coordinator.get(),
            &OpenMeeting::MeetingCoordinator::participantsUpdated, &observer,
            [&](const std::vector<OpenMeeting::ParticipantInfo> &) {
                ++effects;
                ApplyReentry(fixture, action);
            });
    }
    int downstream = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::kickedOff,
        &observer, [&](const QString &, int) { ++downstream; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::hostRoleChanged,
        &observer, [&](const QString &, const QString &) { ++downstream; });
    openmeeting::meeting::NotifyMeetingData notify;
    if (logBoundary) {
        auto *kick = notify.mutable_kickoffmeetingdata();
        kick->set_userid("local-user");
        kick->set_reason("test-only");
        kick->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);
    } else notify.mutable_meetinghostdata()->set_userid("reentry-peer");
    // Match PublishData's real wire envelope. A bare Notify has field numbers
    // that collide with DataPacket metadata and is not a transport packet.
    SendWrappedData(fixture, "PA_REENTRY", "reentry-peer", SessionOnlyNotifyBytes(notify));
    PumpPipeline(fixture);
    MeetingUI::participantSnapshotLogHook = {};
    TEST_CHECK(effects == 1);
    TEST_CHECK(downstream == 0);
    std::cout << "data-effect-reentry log=" << logBoundary
              << " action=" << static_cast<int>(action) << " PASS" << std::endl;
}

void DisconnectedLogCannotChangeSuccessor() {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    bool replaced = false;
    int staleLeave = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingLeft,
        &observer, [&] { if (replaced) ++staleLeave; });
    MeetingUI::participantSnapshotLogHook = [&](const QString &tag) {
        if (tag != QStringLiteral("DISCONNECTED")) return;
        MeetingUI::participantSnapshotLogHook = {};
        fixture.replaceSession();
        replaced = true;
    };
    fixture.listener->OnDisconnected(livekit::RoomDisconnectReason::NetworkError, "red-test-only");
    DrainQt();
    MeetingUI::participantSnapshotLogHook = {};
    std::cout << "disconnected-log-replace: replaced=" << replaced
              << " state=" << static_cast<int>(fixture.coordinator->state())
              << " staleLeave=" << staleLeave << std::endl;
    TEST_CHECK(replaced);
    TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(staleLeave == 0);
}

enum class ListenerEffect {
    DisconnectedLog, DisconnectedState, ConnectedInfo, ConnectedEmptyMetadata,
    ChangedMetadata, ReconnectingLog, ReconnectedState, DuplicateIdentityLog
};

void ListenerOwnerReentry(ListenerEffect boundary, ReentryAction action) {
    Fixture fixture;
    AddSender(fixture);
    if (boundary == ListenerEffect::ReconnectedState) {
        OpenMeeting::MeetingCoordinatorTestAccess::markCommittedReconnect(*fixture.coordinator);
    }
    QObject observer;
    bool acted = false;
    bool performingAction = false;
    int staleEffects = 0;
    const auto reenter = [&] {
        if (acted) return;
        acted = true;
        performingAction = true;
        ApplyReentry(fixture, action);
        performingAction = false;
    };
    const auto isStaleEffect = [&] { return acted && !performingAction; };
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
        &observer, [&](OpenMeeting::MeetingState state, const QString &) {
            if (!acted &&
                ((boundary == ListenerEffect::DisconnectedState && state == OpenMeeting::MeetingState::Idle) ||
                 (boundary == ListenerEffect::ReconnectedState && state == OpenMeeting::MeetingState::InMeeting))) {
                reenter();
            } else if (isStaleEffect()) ++staleEffects;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::roomInfoUpdated,
        &observer, [&](const OpenMeeting::MeetingRoomInfo &) {
            if (boundary == ListenerEffect::ConnectedInfo) reenter();
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
        &observer, [&](const std::vector<OpenMeeting::ParticipantInfo> &) {
            if (boundary == ListenerEffect::ConnectedEmptyMetadata ||
                boundary == ListenerEffect::ChangedMetadata) reenter();
            else if (isStaleEffect()) ++staleEffects;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDetailUpdated,
        &observer, [&](const OpenMeeting::MeetingDetail &) {
            if (isStaleEffect()) ++staleEffects;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingLeft,
        &observer, [&] { if (isStaleEffect()) ++staleEffects; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingKickOff,
        &observer, [&](livekit::RoomDisconnectReason) { if (isStaleEffect()) ++staleEffects; });
    MeetingUI::participantSnapshotLogHook = [&](const QString &tag) {
        if (!acted &&
            ((boundary == ListenerEffect::DisconnectedLog && tag == QStringLiteral("DISCONNECTED")) ||
             (boundary == ListenerEffect::ReconnectingLog && tag == QStringLiteral("RECONNECTING")) ||
             (boundary == ListenerEffect::DuplicateIdentityLog && tag == QStringLiteral("DUPLICATE_IDENTITY")))) {
            reenter();
        } else if (isStaleEffect() && tag == QStringLiteral("RECONNECTED")) ++staleEffects;
    };
    switch (boundary) {
    case ListenerEffect::DisconnectedLog:
    case ListenerEffect::DisconnectedState:
        fixture.listener->OnDisconnected(livekit::RoomDisconnectReason::NetworkError, "listener-owner-test");
        break;
    case ListenerEffect::ConnectedInfo:
    case ListenerEffect::ConnectedEmptyMetadata:
        fixture.listener->OnConnected();
        break;
    case ListenerEffect::ChangedMetadata:
        fixture.listener->OnRoomMetadataChanged(livekit::RoomInfo{}, "",
            "{\"detail\":{\"info\":{\"hostUserID\":\"reentry-peer\"}}}");
        break;
    case ListenerEffect::ReconnectingLog:
        fixture.listener->OnReconnecting();
        break;
    case ListenerEffect::ReconnectedState:
        fixture.listener->OnReconnected();
        break;
    case ListenerEffect::DuplicateIdentityLog:
        fixture.listener->OnDisconnected(livekit::RoomDisconnectReason::DuplicateIdentity,
                                         "duplicate-identity-test");
        break;
    }
    DrainQt();
    MeetingUI::participantSnapshotLogHook = {};
    TEST_CHECK(acted);
    TEST_CHECK(staleEffects == 0);
    if (action == ReentryAction::Destroy) TEST_CHECK(!fixture.coordinator);
    else if (action == ReentryAction::ReplaceSession) {
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    } else {
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::sessionReleased(*fixture.coordinator));
    }
    std::cout << "listener-owner boundary=" << static_cast<int>(boundary)
              << " action=" << static_cast<int>(action) << " PASS" << std::endl;
}

void DuplicateLogInvalidationStillCleansResources() {
    Fixture fixture;
    AddSender(fixture);
    bool invalidated = false;
    MeetingUI::participantSnapshotLogHook = [&](const QString &tag) {
        if (tag != QStringLiteral("DUPLICATE_IDENTITY")) return;
        MeetingUI::participantSnapshotLogHook = {};
        OpenMeeting::MeetingCoordinatorTestAccess::invalidateAdmissionOnly(*fixture.coordinator);
        invalidated = true;
    };
    fixture.listener->OnDisconnected(livekit::RoomDisconnectReason::DuplicateIdentity,
                                     "admission-only-reentry");
    DrainQt();
    MeetingUI::participantSnapshotLogHook = {};
    TEST_CHECK(invalidated);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::sessionReleased(*fixture.coordinator));
    std::cout << "duplicate-log-admission-invalidation-cleanup PASS" << std::endl;
}

void DisconnectDiagnosticBoundaryUsesSafeCopy() {
    Fixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::markMeetingActive(*fixture.coordinator);
    const QString rawDetail = QStringLiteral(
        "heartbeat timer error: access_token=coordinator-secret Authorization: Bearer ui-secret");
    QString disconnectedLog;
    QString stateDetail;
    QObject::connect(
        fixture.coordinator.get(),
        &OpenMeeting::MeetingCoordinator::stateChanged,
        fixture.coordinator.get(),
        [&](OpenMeeting::MeetingState, const QString &detail) { stateDetail = detail; });
    MeetingUI::participantSnapshotSecurityLogHook =
        [&](const QString &tag, const QString &message) {
            if (tag == QStringLiteral("DISCONNECTED")) disconnectedLog = message;
        };

    fixture.listener->OnDisconnected(
        livekit::RoomDisconnectReason::NetworkError,
        rawDetail.toStdString());
    DrainQt();
    MeetingUI::participantSnapshotSecurityLogHook = {};

    TEST_CHECK(!disconnectedLog.isEmpty());
    TEST_CHECK(!stateDetail.isEmpty());
    TEST_CHECK(!disconnectedLog.contains(QStringLiteral("coordinator-secret")));
    TEST_CHECK(!disconnectedLog.contains(QStringLiteral("ui-secret")));
    TEST_CHECK(!stateDetail.contains(QStringLiteral("coordinator-secret")));
    TEST_CHECK(!stateDetail.contains(QStringLiteral("ui-secret")));
    TEST_CHECK(disconnectedLog.contains(QStringLiteral("opaque{kind=room_disconnect,detail=[omitted]}")));
    TEST_CHECK(stateDetail == QStringLiteral("opaque{kind=room_disconnect,detail=[omitted]}"));
}

void ListenerOwnerRegression() {
    DisconnectDiagnosticBoundaryUsesSafeCopy();
    DisconnectedLogCannotChangeSuccessor();
    for (const auto boundary : {ListenerEffect::DisconnectedLog, ListenerEffect::DisconnectedState,
                               ListenerEffect::ConnectedInfo, ListenerEffect::ConnectedEmptyMetadata,
                               ListenerEffect::ChangedMetadata, ListenerEffect::ReconnectingLog,
                               ListenerEffect::ReconnectedState, ListenerEffect::DuplicateIdentityLog}) {
        // Once OnDisconnected has set Idle, leaveMeetingAsync is intentionally
        // a no-op. Destroy and a successor session test its remaining effect.
        if (boundary != ListenerEffect::DisconnectedState) {
            ListenerOwnerReentry(boundary, ReentryAction::Leave);
        }
        ListenerOwnerReentry(boundary, ReentryAction::Destroy);
        ListenerOwnerReentry(boundary, ReentryAction::ReplaceSession);
    }
    DuplicateLogInvalidationStillCleansResources();
    std::cout << "LISTENER_OWNER_CASES=25 PASS" << std::endl;
}

struct StartupReconnectSignals final {
    std::vector<OpenMeeting::MeetingState> states;
    std::vector<bool> audioMuted;
    std::vector<bool> videoEnabled;
    int errors = 0;
};

void ObserveStartupReconnect(Fixture &fixture, QObject &observer,
                             StartupReconnectSignals &observed) {
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
        &observer, [&](OpenMeeting::MeetingState state, const QString &) { observed.states.push_back(state); });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::localAudioMuteChanged,
        &observer, [&](bool muted) { observed.audioMuted.push_back(muted); });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::localVideoEnableChanged,
        &observer, [&](bool enabled) { observed.videoEnabled.push_back(enabled); });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::errorOccurred,
        &observer, [&](const QString &, const QString &) { ++observed.errors; });
}

void StartupReconnectOrder(bool degraded, bool reconnectedFirst) {
    Fixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*fixture.coordinator);
    const auto generation = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*fixture.coordinator);
    QObject observer;
    StartupReconnectSignals observed;
    ObserveStartupReconnect(fixture, observer, observed);

    fixture.listener->OnReconnecting();
    DrainQt();
    TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Reconnecting);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*fixture.coordinator));

    const auto queueTerminal = [&] {
        if (degraded) {
            OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(*fixture.coordinator, generation);
        } else {
            OpenMeeting::MeetingCoordinatorTestAccess::queueSuccessfulStartup(*fixture.coordinator, generation);
        }
    };

    if (reconnectedFirst) {
        fixture.listener->OnReconnected();
        DrainQt();
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Reconnecting);
        TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*fixture.coordinator));
        queueTerminal();
        DrainQt();
    } else {
        queueTerminal();
        DrainQt();
        TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::Reconnecting);
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*fixture.coordinator));
        fixture.listener->OnReconnected();
        DrainQt();
    }

    TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::startupCommitted(*fixture.coordinator));
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*fixture.coordinator));
    if (degraded) {
        TEST_CHECK(fixture.coordinator->isLocalAudioMuted());
        TEST_CHECK(!fixture.coordinator->isLocalVideoEnabled());
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::hasNoLocalTracks(*fixture.coordinator));
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::localProjection(*fixture.coordinator, true, false));
        TEST_CHECK(observed.audioMuted == std::vector<bool>{true});
        TEST_CHECK(observed.videoEnabled == std::vector<bool>{false});
        TEST_CHECK(observed.errors == 1);
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::startupListenOnly(*fixture.coordinator));
        fixture.coordinator->setLocalAudioMuted(false);
        fixture.coordinator->setLocalVideoEnabled(true);
        TEST_CHECK(fixture.coordinator->isLocalAudioMuted());
        TEST_CHECK(!fixture.coordinator->isLocalVideoEnabled());
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::localProjection(*fixture.coordinator, true, false));
        TEST_CHECK((observed.audioMuted == std::vector<bool>{true, true}));
        TEST_CHECK((observed.videoEnabled == std::vector<bool>{false, false}));
    }
    TEST_CHECK(std::count(observed.states.begin(), observed.states.end(),
                          OpenMeeting::MeetingState::Reconnecting) == 1);
    TEST_CHECK(std::count(observed.states.begin(), observed.states.end(),
                          OpenMeeting::MeetingState::InMeeting) == 1);
}

void StartupReconnectNormalSuccess() {
    Fixture fixture;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*fixture.coordinator, true, false);
    const auto generation = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*fixture.coordinator);
    QObject observer;
    StartupReconnectSignals observed;
    ObserveStartupReconnect(fixture, observer, observed);

    OpenMeeting::MeetingCoordinatorTestAccess::queueSuccessfulStartup(*fixture.coordinator, generation);
    DrainQt();

    TEST_CHECK(fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::startupCommitted(*fixture.coordinator));
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*fixture.coordinator));
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::startupListenOnly(*fixture.coordinator));
    TEST_CHECK(observed.audioMuted == std::vector<bool>{true});
    TEST_CHECK(observed.videoEnabled == std::vector<bool>{false});
}

void StartupReconnectRejectsStaleAndStoppedTerminals() {
    Fixture replacement;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*replacement.coordinator);
    const auto oldGeneration = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*replacement.coordinator);
    auto oldListener = replacement.listener;
    oldListener->OnReconnecting();
    DrainQt();
    oldListener->OnReconnected();
    OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(*replacement.coordinator, oldGeneration);
    replacement.replaceSession();
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*replacement.coordinator, true, false);
    DrainQt();
    TEST_CHECK(replacement.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(replacement.coordinator->isLocalAudioMuted());
    TEST_CHECK(!replacement.coordinator->isLocalVideoEnabled());
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::startupCommitted(*replacement.coordinator));
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*replacement.coordinator));

    Fixture stopped;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*stopped.coordinator);
    const auto stoppedGeneration = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*stopped.coordinator);
    stopped.listener->OnReconnecting();
    DrainQt();
    stopped.listener->OnReconnected();
    OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(*stopped.coordinator, stoppedGeneration);
    stopped.coordinator->leaveMeetingAsync(false);
    DrainQt();
    TEST_CHECK(stopped.coordinator->state() == OpenMeeting::MeetingState::Idle);
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::startupCommitted(*stopped.coordinator));
    TEST_CHECK(!OpenMeeting::MeetingCoordinatorTestAccess::reconnectPending(*stopped.coordinator));
}

void StartupReconnectTerminalIdempotenceAndReentry() {
    Fixture idempotent;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*idempotent.coordinator);
    const auto generation = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*idempotent.coordinator);
    QObject observer;
    StartupReconnectSignals observed;
    ObserveStartupReconnect(idempotent, observer, observed);
    idempotent.listener->OnReconnecting();
    idempotent.listener->OnReconnecting();
    DrainQt();
    idempotent.listener->OnReconnected();
    idempotent.listener->OnReconnected();
    OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(*idempotent.coordinator, generation);
    OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(*idempotent.coordinator, generation);
    DrainQt();
    TEST_CHECK(idempotent.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(observed.audioMuted == std::vector<bool>{true});
    TEST_CHECK(observed.videoEnabled == std::vector<bool>{false});
    TEST_CHECK(observed.errors == 1);
    TEST_CHECK(std::count(observed.states.begin(), observed.states.end(),
                          OpenMeeting::MeetingState::Reconnecting) == 1);
    TEST_CHECK(std::count(observed.states.begin(), observed.states.end(),
                          OpenMeeting::MeetingState::InMeeting) == 1);

    Fixture reentrant;
    OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*reentrant.coordinator);
    const auto reentrantGeneration = OpenMeeting::MeetingCoordinatorTestAccess::sessionGeneration(*reentrant.coordinator);
    reentrant.listener->OnReconnecting();
    DrainQt();
    reentrant.listener->OnReconnected();
    DrainQt();
    QObject reentryObserver;
    int staleCapabilitySignals = 0;
    int staleErrors = 0;
    bool replaced = false;
    QObject::connect(reentrant.coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
        &reentryObserver, [&](OpenMeeting::MeetingState state, const QString &) {
            if (state == OpenMeeting::MeetingState::InMeeting && !replaced) {
                replaced = true;
                reentrant.replaceSession();
                OpenMeeting::MeetingCoordinatorTestAccess::prepareStartup(*reentrant.coordinator, true, false);
            }
        });
    QObject::connect(reentrant.coordinator.get(), &OpenMeeting::MeetingCoordinator::localAudioMuteChanged,
        &reentryObserver, [&](bool) { ++staleCapabilitySignals; });
    QObject::connect(reentrant.coordinator.get(), &OpenMeeting::MeetingCoordinator::localVideoEnableChanged,
        &reentryObserver, [&](bool) { ++staleCapabilitySignals; });
    QObject::connect(reentrant.coordinator.get(), &OpenMeeting::MeetingCoordinator::errorOccurred,
        &reentryObserver, [&](const QString &, const QString &) { ++staleErrors; });
    OpenMeeting::MeetingCoordinatorTestAccess::queueDegradedStartup(
        *reentrant.coordinator, reentrantGeneration);
    DrainQt();
    TEST_CHECK(replaced);
    TEST_CHECK(reentrant.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    TEST_CHECK(reentrant.coordinator->isLocalAudioMuted());
    TEST_CHECK(!reentrant.coordinator->isLocalVideoEnabled());
    TEST_CHECK(staleCapabilitySignals == 0);
    TEST_CHECK(staleErrors == 0);
}

void StartupReconnectRegression() {
    StartupReconnectNormalSuccess();
    for (bool degraded : {false, true}) {
        for (bool reconnectedFirst : {false, true}) {
            StartupReconnectOrder(degraded, reconnectedFirst);
        }
    }
    StartupReconnectRejectsStaleAndStoppedTerminals();
    StartupReconnectTerminalIdempotenceAndReentry();
    std::cout << "STARTUP_RECONNECT_CASES=9 PASS" << std::endl;
}

void OwnerTerminalRegression() {
    CompletedLeaveHasOneTerminal();
    TransferReentry(TransferEffect::Completed, ReentryAction::None);
    for (const auto boundary : {TransferEffect::Started, TransferEffect::Progress99,
                               TransferEffect::Progress100, TransferEffect::Completed,
                               TransferEffect::Message}) {
        TransferReentry(boundary, ReentryAction::Leave);
        TransferReentry(boundary, ReentryAction::Destroy);
    }
    TransferReentry(TransferEffect::Progress100, ReentryAction::ReplaceSession);
    TransferReentry(TransferEffect::Completed, ReentryAction::ReplaceSession);
    for (bool replacement : {false, true}) {
        RosterReentry(replacement, ReentryAction::Leave);
        RosterReentry(replacement, ReentryAction::Destroy);
    }
    CancelRetiredInstancePreservesSuccessor();
    for (bool log : {false, true}) {
        DataEffectReentry(log, ReentryAction::Leave);
        DataEffectReentry(log, ReentryAction::Destroy);
        DataEffectReentry(log, ReentryAction::ReplaceSession);
    }
    std::cout << "OWNER_TERMINAL_CASES=25 PASS" << std::endl;
}

template <typename Message>
std::vector<uint8_t> PacketBytes(const Message &message) {
    static_assert(!std::is_same_v<std::decay_t<Message>, openmeeting::meeting::NotifyMeetingData>,
                  "Notify fixtures must use the session-only account-path guard");
    const auto bytes = message.SerializeAsString();
    return {bytes.begin(), bytes.end()};
}

OpenMeeting::ParticipantInfo Projected(Fixture &fixture, const QString &identity) {
    const auto values = fixture.coordinator->participants();
    const auto *value = FindParticipant(values, identity);
    TEST_CHECK(value != nullptr);
    return *value;
}

livekit::proto::ParticipantUpdate DetailedParticipant() {
    auto update = MakeParticipantUpdate("PA_STATE", "state-peer", "native-name",
        livekit::proto::ParticipantInfo::ACTIVE, false);
    auto *participant = update.mutable_participants(0);
    participant->set_metadata("{\"userInfo\":{\"nickname\":\"snapshot-nickname\"}}");
    for (const auto &sid : {"TR_STATE_A", "TR_STATE_B"}) {
        auto *track = participant->add_tracks();
        track->set_sid(sid);
        track->set_name(sid);
        track->set_type(livekit::proto::TrackType::VIDEO);
        track->set_muted(false);
    }
    return update;
}

void SetStreamState(Fixture &fixture, const std::string &sid, bool paused) {
    livekit::proto::SignalResponse response;
    auto *stream = response.mutable_stream_state_update()->add_stream_states();
    stream->set_participant_sid("PA_STATE");
    stream->set_track_sid(sid);
    stream->set_state(paused ? livekit::proto::StreamState::PAUSED : livekit::proto::StreamState::ACTIVE);
    fixture.room->HandleSignalMessageForTesting(response);
}

void CheckGuiThread() {
    TEST_CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
}

void AkCaseA() {
    Fixture fixture;
    QObject observer;
    int joined = 0, qualityEvents = 0, streamEvents = 0, muteEvents = 0, permissionEvents = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
        &observer, [&](const std::vector<OpenMeeting::ParticipantInfo> &) { CheckGuiThread(); });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantJoined,
        &observer, [&](const QString &, const QString &) { CheckGuiThread(); ++joined; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantConnectionQualityChanged,
        &observer, [&](const QString &, int, float) { CheckGuiThread(); ++qualityEvents; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantTrackStreamStateChanged,
        &observer, [&](const QString &, const QString &, bool video, bool) {
            CheckGuiThread(); TEST_CHECK(video); ++streamEvents;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteTrackMuted,
        &observer, [&](const QString &, bool video, bool muted) {
            CheckGuiThread(); TEST_CHECK(video && muted); ++muteEvents;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::trackSubscriptionPermissionChanged,
        &observer, [&](const QString &, const QString &, const QString &, bool allowed) {
            CheckGuiThread(); TEST_CHECK(!allowed); ++permissionEvents;
        });
    auto update = DetailedParticipant();
    fixture.room->UpdateParticipantsForTesting(update);
    PumpPipeline(fixture);
    TEST_CHECK(Projected(fixture, "state-peer").name == "snapshot-nickname");
    TEST_CHECK(joined == 1);
    fixture.room->UpdateParticipantsForTesting(update);
    PumpPipeline(fixture);
    TEST_CHECK(joined == 1);

    livekit::proto::SignalResponse quality;
    auto *qualityValue = quality.mutable_connection_quality()->add_updates();
    qualityValue->set_participant_sid("PA_STATE");
    qualityValue->set_quality(livekit::proto::ConnectionQuality::GOOD);
    qualityValue->set_score(0.75f);
    fixture.room->HandleSignalMessageForTesting(quality);
    fixture.room->HandleSignalMessageForTesting(quality);
    PumpPipeline(fixture);
    const auto current = Projected(fixture, "state-peer");
    TEST_CHECK(current.connectionQuality == livekit::ConnectionQuality::Good);
    TEST_CHECK(current.connectionQualityScore == 0.75f && qualityEvents == 1);
    SetStreamState(fixture, "TR_STATE_A", true);
    SetStreamState(fixture, "TR_STATE_B", true);
    SetStreamState(fixture, "TR_STATE_B", true);
    PumpPipeline(fixture);
    TEST_CHECK(Projected(fixture, "state-peer").isVideoStreamPaused && streamEvents == 2);
    SetStreamState(fixture, "TR_STATE_A", false);
    PumpPipeline(fixture);
    TEST_CHECK(Projected(fixture, "state-peer").isVideoStreamPaused && streamEvents == 3);
    SetStreamState(fixture, "TR_STATE_B", false);
    PumpPipeline(fixture);
    TEST_CHECK(!Projected(fixture, "state-peer").isVideoStreamPaused && streamEvents == 4);
    livekit::proto::SignalResponse permission;
    auto *allowed = permission.mutable_subscription_permission_update();
    allowed->set_participant_sid("PA_STATE");
    allowed->set_track_sid("TR_STATE_A");
    allowed->set_allowed(false);
    fixture.room->HandleSignalMessageForTesting(permission);
    fixture.room->HandleSignalMessageForTesting(permission);
    PumpPipeline(fixture);
    TEST_CHECK(permissionEvents == 1);
    update.mutable_participants(0)->mutable_permission()->set_can_publish(false);
    update.mutable_participants(0)->mutable_tracks(0)->set_muted(true);
    update.mutable_participants(0)->mutable_tracks(1)->set_muted(true);
    fixture.room->UpdateParticipantsForTesting(update);
    PumpPipeline(fixture);
    TEST_CHECK(!Projected(fixture, "state-peer").permissions.can_publish);
    TEST_CHECK(!Projected(fixture, "state-peer").isVideoEnabled && muteEvents == 2);
    std::cout << "AK_CASE_A multifield/aggregate/per-track/gui/idempotence PASS" << std::endl;
}

void AkCaseB() {
    Fixture fixture;
    auto update = DetailedParticipant();
    fixture.room->UpdateParticipantsForTesting(update);
    PumpPipeline(fixture);
    QObject observer;
    std::vector<OpenMeeting::ParticipantInfo> projected;
    std::vector<float> speakerLevels;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
        &observer, [&](const std::vector<OpenMeeting::ParticipantInfo> &values) {
            CheckGuiThread();
            if (const auto *participant = FindParticipant(values, "state-peer")) projected.push_back(*participant);
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::activeSpeakersChanged,
        &observer, [&](const std::vector<livekit::ActiveSpeakerInfo> &values) {
            CheckGuiThread(); if (!values.empty()) speakerLevels.push_back(values.front().audio_level);
        });
    SetStreamState(fixture, "TR_STATE_A", true);
    update.mutable_participants(0)->set_metadata("{\"userInfo\":{\"nickname\":\"frozen-one\"}}");
    fixture.room->UpdateParticipantsForTesting(update);
    livekit::proto::SpeakersChanged speakers;
    auto *speaker = speakers.add_speakers();
    speaker->set_sid("PA_STATE"); speaker->set_active(true); speaker->set_level(0.25f);
    fixture.room->HandleActiveSpeakerUpdateForTesting(speakers);
    DrainNative(fixture.io); // Qt remains deliberately undrained.
    const auto frozenEvents = fixture.bridge->events();
    const auto frozen = *std::find_if(frozenEvents.rbegin(), frozenEvents.rend(), [](const auto &event) {
        return event.kind == livekit::ParticipantEventKind::Upsert;
    });
    update.mutable_participants(0)->set_metadata("{\"userInfo\":{\"nickname\":\"latest-two\"}}");
    update.mutable_participants(0)->clear_tracks();
    fixture.room->UpdateParticipantsForTesting(update);
    speaker->set_level(0.875f);
    fixture.room->HandleActiveSpeakerUpdateForTesting(speakers);
    DrainNative(fixture.io);
    TEST_CHECK(projected.empty() && speakerLevels.empty());
    TEST_CHECK(frozen.participant.state.metadata.find("frozen-one") != std::string::npos);
    TEST_CHECK(frozen.participant.state.publications.size() == 2);
    TEST_CHECK(frozen.participant.state.publications[0].stream_state == livekit::TrackPublication::StreamState::Paused);
    TEST_CHECK(fixture.room->remote_participants().at("PA_STATE")->SnapshotState().publications.empty());
    DrainQt();
    TEST_CHECK(std::any_of(projected.begin(), projected.end(), [](const auto &value) {
        return value.name == "frozen-one" && value.isVideoStreamPaused;
    }));
    TEST_CHECK(Projected(fixture, "state-peer").name == "latest-two");
    TEST_CHECK(!Projected(fixture, "state-peer").isVideoStreamPaused);
    TEST_CHECK(speakerLevels.size() == 2 && speakerLevels[0] == 0.25f && speakerLevels[1] == 0.875f);
    std::cout << "AK_CASE_B frozen-publication/metadata/speakers PASS" << std::endl;
}

struct TransferCounters {
    int started = 0, progress = 0, completed = 0, failed = 0, messages = 0, streams = 0;
};

void ObserveTransfer(Fixture &fixture, QObject &observer, TransferCounters &counts) {
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
        &observer, [&](const QString &, const QString &, const QString &, const QString &,
                      const QString &, qint64, int64_t) { ++counts.started; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingProgress,
        &observer, [&](const QString &, int) { ++counts.progress; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
        &observer, [&](const QString &, const QString &, const QString &, const QString &,
                      const QString &, const QByteArray &) { ++counts.completed; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
        &observer, [&](const QString &, const QString &) { ++counts.failed; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaMessageReceived,
        &observer, [&](const QString &, const QString &, const QString &, const QString &, const QByteArray &) { ++counts.messages; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::textStreamReceived,
        &observer, [&](std::shared_ptr<livekit::TextStreamReader>, const QString &) { ++counts.streams; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::byteStreamReceived,
        &observer, [&](std::shared_ptr<livekit::ByteStreamReader>, const QString &) { ++counts.streams; });
}

void RetireSender(Fixture &fixture) {
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate("PA_REENTRY", "reentry-peer", "",
        livekit::proto::ParticipantInfo::DISCONNECTED, false));
}

void SendChunkOnly(Fixture &fixture, const std::string &sid = "PA_REENTRY") {
    QJsonObject chunk{{"om_type", "media_chunk"}, {"transferId", "same-wire"}, {"totalChunks", 1},
        {"chunkIndex", 0}, {"mediaType", "file"}, {"fileName", "reentry.bin"},
        {"totalSize", 5}, {"chunkData", "aGVsbG8="}};
    fixture.room->OnIncomingDataPacket(JsonPayload(chunk), sid, "chat");
}

void AkCaseC(int boundary) {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    TransferCounters counts;
    ObserveTransfer(fixture, observer, counts);
    if (boundary == 3) {
        SendMedia(fixture, true);
        PumpPipeline(fixture);
        TEST_CHECK(counts.started == 1 && OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 1);
        SendChunkOnly(fixture);
    } else SendMedia(fixture);
    if (boundary >= 1) { DrainNative(fixture.io); DrainQt(); }
    if (boundary >= 2) {
        DrainNative(fixture.io);
        bool checkedEmpty = false;
        asio::post(fixture.runtime->strand(), [&] {
            TEST_CHECK(fixture.runtime->transfersOnStrand().empty()); checkedEmpty = true;
        });
        DrainNative(fixture.io);
        TEST_CHECK(checkedEmpty); // The map was erased; UI completion is still queued.
    }
    RetireSender(fixture); // Real canonical retirement, not a validity flag test stub.
    PumpPipeline(fixture);
    TEST_CHECK(counts.started == (boundary == 3 ? 1 : 0));
    TEST_CHECK(counts.progress == 0 && counts.completed == 0 && counts.messages == 0);
    TEST_CHECK(counts.failed == (boundary == 3 ? 1 : 0));
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 0);
    std::cout << "AK_CASE_C transfer-boundary=" << boundary << " PASS" << std::endl;
}

void AkCaseCStream(bool nativeAlreadyDelivered) {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    TransferCounters counts;
    ObserveTransfer(fixture, observer, counts);
    for (bool text : {false, true}) {
        livekit::proto::DataPacket packet;
        packet.set_participant_sid("PA_REENTRY");
        packet.set_participant_identity("reentry-peer");
        auto *header = packet.mutable_stream_header();
        header->set_stream_id(text ? "old-text" : "old-bytes");
        header->set_topic("test-retire");
        header->set_total_length(1);
        if (text) header->mutable_text_header(); else header->mutable_byte_header()->set_name("old.bin");
        fixture.room->OnIncomingDataPacket(PacketBytes(packet), "", "");
    }
    if (nativeAlreadyDelivered) DrainNative(fixture.io);
    RetireSender(fixture);
    PumpPipeline(fixture);
    TEST_CHECK(counts.streams == 0);
    std::cout << "AK_CASE_C stream-open-native-delivered=" << nativeAlreadyDelivered << " PASS" << std::endl;
}

void AkCaseD(bool reuseSid) {
    Fixture fixture;
    AddSender(fixture);
    QObject observer;
    TransferCounters counts;
    ObserveTransfer(fixture, observer, counts);
    const auto oldEvents = fixture.bridge->events();
    const auto oldUpsert = *std::find_if(oldEvents.rbegin(), oldEvents.rend(), [](const auto &event) {
        return event.kind == livekit::ParticipantEventKind::Upsert;
    });
    const auto heldTicket = oldUpsert.participant.ticket.lock();
    const auto heldParticipant = fixture.room->remote_participants().at("PA_REENTRY");
    SendMedia(fixture, true);
    PumpPipeline(fixture);
    RetireSender(fixture);
    const std::string replacementSid = reuseSid ? "PA_REENTRY" : "PA_REPLACEMENT";
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(replacementSid, "reentry-peer", "new-instance",
        livekit::proto::ParticipantInfo::ACTIVE, false));
    DrainNative(fixture.io); DrainQt(); // Old cancellation pending; successor already projected.
    const auto replacement = Projected(fixture, "reentry-peer");
    TEST_CHECK(replacement.participantKey != oldUpsert.participant.key);
    TEST_CHECK(heldTicket && !heldTicket->active.load() && heldParticipant->identity() == "reentry-peer");
    fixture.listener->OnParticipantEvent(oldUpsert);
    const auto events = fixture.bridge->events();
    const auto departure = *std::find_if(events.rbegin(), events.rend(), [](const auto &event) {
        return event.kind == livekit::ParticipantEventKind::Departure;
    });
    fixture.listener->OnParticipantEvent(departure);
    SendChunkOnly(fixture, replacementSid); // Implicit start, deliberately reuse wire ID.
    PumpPipeline(fixture);
    TEST_CHECK(Projected(fixture, "reentry-peer").participantKey == replacement.participantKey);
    TEST_CHECK(Projected(fixture, "reentry-peer").name == "new-instance");
    TEST_CHECK(counts.started == 2 && counts.failed == 1 && counts.completed == 1 && counts.messages == 1);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 0);
    std::cout << "AK_CASE_D same-identity/reuse-sid=" << reuseSid << " PASS" << std::endl;
}

template <typename Future>
void WaitBounded(Future &future) {
    TEST_CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

class ConcurrentValueObserver final : public livekit::RoomListener {
public:
    std::weak_ptr<livekit::Room> room;
    std::promise<void> firstEntered;
    std::shared_future<void> releaseFirst;
    std::atomic<int> active{0}, maximumActive{0}, callbacks{0}, reentrantReads{0};
    bool ConsumesParticipantEvents() const override { return true; }
    void OnParticipantEvent(const livekit::ParticipantEvent &) override {
        const int inProgress = active.fetch_add(1) + 1;
        int previous = maximumActive.load();
        while (previous < inProgress && !maximumActive.compare_exchange_weak(previous, inProgress)) {}
        struct Exit { std::atomic<int> &value; ~Exit() { --value; } } exit{active};
        const int count = ++callbacks;
        if (count == 1) {
            firstEntered.set_value();
            WaitBounded(releaseFirst);
        }
        if (auto value = room.lock()) {
            const auto participants = value->remote_participants();
            for (const auto &[sid, participant] : participants) {
                TEST_CHECK(participant->SnapshotState().sid == sid);
            }
            ++reentrantReads;
            if (count == 1) {
                value->UpdateParticipantsForTesting(MakeParticipantUpdate("PA_CALLBACK", "callback-peer", "callback",
                    livekit::proto::ParticipantInfo::ACTIVE, false));
                throw std::runtime_error("intentional listener exception after reentrant native update");
            }
        }
    }
};

void AkCaseE() {
    Fixture fixture;
    auto observer = std::make_shared<ConcurrentValueObserver>();
    observer->room = fixture.room;
    std::promise<void> releaseCallback;
    observer->releaseFirst = releaseCallback.get_future().share();
    auto callbackEntered = observer->firstEntered.get_future();
    fixture.room->AddListener(observer);
    auto work = asio::make_work_guard(fixture.io);
    std::promise<void> startWorkers, entered0, entered1, exited0, exited1;
    auto workersGo = startWorkers.get_future().share();
    auto enteredFuture0 = entered0.get_future(), enteredFuture1 = entered1.get_future();
    auto exitedFuture0 = exited0.get_future(), exitedFuture1 = exited1.get_future();
    std::thread::id worker0, worker1;
    asio::post(fixture.io, [&] { worker0 = std::this_thread::get_id(); entered0.set_value(); WaitBounded(workersGo); });
    asio::post(fixture.io, [&] { worker1 = std::this_thread::get_id(); entered1.set_value(); WaitBounded(workersGo); });
    std::thread first([&] { fixture.io.run(); exited0.set_value(); });
    std::thread second([&] { fixture.io.run(); exited1.set_value(); });
    WaitBounded(enteredFuture0); WaitBounded(enteredFuture1);
    TEST_CHECK(worker0 != worker1);
    startWorkers.set_value();
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate("PA_SEED", "seed-peer", "seed",
        livekit::proto::ParticipantInfo::ACTIVE, false));
    WaitBounded(callbackEntered); // One drainer is actively blocked inside a real listener.
    std::promise<void> continueProducers, half0, half1;
    auto producersGo = continueProducers.get_future().share();
    auto halfFuture0 = half0.get_future(), halfFuture1 = half1.get_future();
    const auto produce = [&](int index, std::promise<void> &half) {
        const std::string sid = "PA_CONCURRENT_REAL_" + std::to_string(index);
        const std::string identity = "concurrent-real-" + std::to_string(index);
        for (int revision = 1; revision <= 70; ++revision) {
            if (revision == 36) {
                half.set_value(); WaitBounded(producersGo);
                fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(sid, identity, "",
                    livekit::proto::ParticipantInfo::DISCONNECTED, false));
            }
            fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(sid, identity,
                "revision-" + std::to_string(revision), livekit::proto::ParticipantInfo::ACTIVE, false));
        }
    };
    std::thread producer0([&] { produce(0, half0); });
    std::thread producer1([&] { produce(1, half1); });
    WaitBounded(halfFuture0); WaitBounded(halfFuture1);
    std::promise<void> otherWorkerProgress;
    auto otherProgress = otherWorkerProgress.get_future();
    asio::post(fixture.io, [&] { otherWorkerProgress.set_value(); });
    WaitBounded(otherProgress); // The second IO worker progresses during the blocked callback.
    TEST_CHECK(observer->active.load() == 1);
    releaseCallback.set_value();
    continueProducers.set_value();
    producer0.join(); producer1.join();
    work.reset();
    WaitBounded(exitedFuture0); WaitBounded(exitedFuture1);
    first.join(); second.join();
    PumpPipeline(fixture);
    TEST_CHECK(observer->maximumActive.load() == 1);
    TEST_CHECK(observer->callbacks.load() > 128 && observer->reentrantReads.load() == observer->callbacks.load());
    TEST_CHECK(Projected(fixture, "concurrent-real-0").name == "revision-70");
    TEST_CHECK(Projected(fixture, "concurrent-real-1").name == "revision-70");
    TEST_CHECK(Projected(fixture, "callback-peer").name == "callback");
    uint64_t previousSequence = 0;
    for (const auto &event : fixture.bridge->events()) {
        TEST_CHECK(event.event_sequence > previousSequence); previousSequence = event.event_sequence;
    }
    fixture.room->RemoveListener(observer);
    std::cout << "AK_CASE_E two-io-workers/concurrent-producers/single-drainer/exception/reentry PASS callbacks="
              << observer->callbacks.load() << std::endl;
}

void SendWrappedData(Fixture &fixture, const std::string &sid, const std::string &identity,
                     const std::vector<uint8_t> &payload) {
    livekit::proto::DataPacket packet;
    packet.set_participant_sid(sid);
    packet.set_participant_identity(identity);
    packet.mutable_user()->set_payload(payload.data(), payload.size());
    packet.mutable_user()->set_topic("chat");
    fixture.room->OnIncomingDataPacket(PacketBytes(packet), "", "");
}

void AkCaseI() {
    Fixture fixture;
    AddSender(fixture);
    RetireSender(fixture);
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate("PA_NEW", "reentry-peer", "new-sender",
        livekit::proto::ParticipantInfo::ACTIVE, false));
    PumpPipeline(fixture);
    QObject observer;
    int chats = 0, kicked = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageReceived,
        &observer, [&](const QString &identity, const QString &, const QString &text, int64_t) {
            CheckGuiThread(); TEST_CHECK(identity == "reentry-peer" && text == "accepted"); ++chats;
        });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::kickedOff,
        &observer, [&](const QString &, int) { ++kicked; });
    const auto message = JsonPayload(QJsonObject{{"om_type", "chat_text"}, {"text", "accepted"}});
    SendWrappedData(fixture, "PA_REENTRY", "reentry-peer", message); // Explicit stale SID must not fall back.
    SendWrappedData(fixture, "", "unknown-peer", message); // Cannot become server origin.
    SendWrappedData(fixture, "PA_NEW", "reentry-peer", message);
    SendWrappedData(fixture, "", "reentry-peer", message);
    PumpPipeline(fixture);
    TEST_CHECK(chats == 2);
    std::vector<livekit::SenderOrigin> origins;
    for (const auto &event : fixture.bridge->events()) {
        if (event.kind == livekit::ParticipantEventKind::DataReceived) origins.push_back(event.sender.origin);
    }
    TEST_CHECK(origins.size() == 4);
    TEST_CHECK(origins[0] == livekit::SenderOrigin::Unresolved && origins[1] == livekit::SenderOrigin::Unresolved);
    TEST_CHECK(origins[2] == livekit::SenderOrigin::Remote && origins[3] == livekit::SenderOrigin::Remote);
    openmeeting::meeting::NotifyMeetingData notify;
    notify.mutable_kickoffmeetingdata()->set_userid("local-user");
    notify.mutable_kickoffmeetingdata()->set_reason("server-origin-test");
    // Proto3 defaults to DuplicatedLogin (global account invalidation), not a
    // normal Room kick. Select the intended session-only reason explicitly.
    notify.mutable_kickoffmeetingdata()->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);
    SendWrappedData(fixture, "PA_REENTRY", "reentry-peer", SessionOnlyNotifyBytes(notify));
    PumpPipeline(fixture);
    TEST_CHECK(kicked == 0 && fixture.coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    SendWrappedData(fixture, "", "", SessionOnlyNotifyBytes(notify));
    PumpPipeline(fixture);
    TEST_CHECK(kicked == 1 && fixture.coordinator->state() == OpenMeeting::MeetingState::Idle);
    const auto events = fixture.bridge->events();
    const auto server = std::find_if(events.rbegin(), events.rend(), [](const auto &event) {
        return event.kind == livekit::ParticipantEventKind::DataReceived;
    });
    TEST_CHECK(server != events.rend() && server->sender.origin == livekit::SenderOrigin::Server);
    std::cout << "AK_CASE_I sid/identity/unresolved/server-notify PASS" << std::endl;
}

void AkCaseJ() {
    auto video = std::make_shared<livekit::Track>("TR_COPY", "video", livekit::TrackKind::Video);
    auto audio = std::make_shared<livekit::Track>("TR_AUDIO", "audio", livekit::TrackKind::Audio);
    auto publication = std::make_shared<livekit::TrackPublication>(video, "TR_COPY", "publication");
    publication->set_stream_state(livekit::TrackPublication::StreamState::Paused);
    publication->set_subscription_allowed(false);
    livekit::TrackPublication publicationCopy(*publication), publicationAssigned(audio, "other", "other");
    publicationAssigned = *publication;
    for (auto *copy : {&publicationCopy, &publicationAssigned}) {
        const auto snapshot = copy->SnapshotState();
        TEST_CHECK(snapshot.track == video && snapshot.sid == "TR_COPY" &&
            !snapshot.subscription_allowed && snapshot.stream_state == livekit::TrackPublication::StreamState::Paused);
    }
    livekit::Participant participant("PA_COPY", "copy-peer"), assigned("other", "other");
    participant.set_name("copy-name"); participant.set_metadata("copy-metadata");
    participant.set_attributes({{"revision", "0"}, {"parity", "0"}});
    participant.add_publication(publication);
    using LegacyGet = std::shared_ptr<livekit::TrackPublication>
        (livekit::Participant::*)(const std::string&);
    using ConstGet = std::shared_ptr<livekit::TrackPublication>
        (livekit::Participant::*)(const std::string&) const;
    LegacyGet legacyGet = &livekit::Participant::get_publication;
    ConstGet constGet = &livekit::Participant::get_publication;
    const livekit::Participant& constParticipant = participant;
    TEST_CHECK((participant.*legacyGet)("TR_COPY") == publication);
    TEST_CHECK((constParticipant.*constGet)("TR_COPY") == publication);
    TEST_CHECK((participant.*legacyGet)("missing-publication") == nullptr);
    TEST_CHECK((constParticipant.*constGet)("missing-publication") == nullptr);
    std::cout << "AK_CASE_J legacy/const-get-publication-member-pointer PASS" << std::endl;
    livekit::Participant copied(participant);
    assigned = participant;
    for (auto *copy : {&copied, &assigned}) {
        const auto snapshot = copy->SnapshotState();
        TEST_CHECK(snapshot.sid == "PA_COPY" && snapshot.identity == "copy-peer" && snapshot.name == "copy-name");
        TEST_CHECK(snapshot.metadata == "copy-metadata" && snapshot.publications.size() == 1);
        TEST_CHECK(snapshot.publications[0].track == video);
    }
    copied.set_name("independent-copy");
    TEST_CHECK(participant.name() == "copy-name");
    std::promise<void> goPromise;
    auto go = goPromise.get_future().share();
    auto writer = std::async(std::launch::async, [&] {
        WaitBounded(go);
        for (int revision = 1; revision <= 2000; ++revision) {
            participant.set_attributes({{"revision", std::to_string(revision)}, {"parity", std::to_string(revision % 2)}});
            publication->set_track((revision % 2) ? video : audio);
            publication->set_subscription_allowed(revision % 2 == 0);
            publication->set_stream_state((revision % 2) ? livekit::TrackPublication::StreamState::Paused : livekit::TrackPublication::StreamState::Active);
            video->set_muted(revision % 2 == 0);
            if (revision % 2) participant.remove_publication("TR_COPY");
            else participant.add_publication(publication);
        }
    });
    auto reader = std::async(std::launch::async, [&] {
        WaitBounded(go);
        for (int iteration = 0; iteration < 2000; ++iteration) {
            livekit::Participant copy(participant);
            const auto snapshot = copy.SnapshotState();
            TEST_CHECK(std::stoi(snapshot.attributes.at("revision")) % 2 == std::stoi(snapshot.attributes.at("parity")));
            for (const auto &pub : snapshot.publications) {
                TEST_CHECK(pub.sid == "TR_COPY" && pub.track && pub.kind == pub.track->kind());
            }
            livekit::TrackPublication pubCopy(*publication);
            TEST_CHECK(pubCopy.sid() == "TR_COPY" && pubCopy.track());
        }
    });
    goPromise.set_value();
    WaitBounded(writer); WaitBounded(reader); writer.get(); reader.get();
    auto assignForward = std::async(std::launch::async, [&] { for (int i = 0; i < 1000; ++i) copied = assigned; });
    auto assignBackward = std::async(std::launch::async, [&] { for (int i = 0; i < 1000; ++i) assigned = copied; });
    WaitBounded(assignForward); WaitBounded(assignBackward); assignForward.get(); assignBackward.get();
    int sends = 0, dataCallbacks = 0, publishCallbacks = 0;
    std::unique_ptr<livekit::LocalParticipant> local;
    local = std::make_unique<livekit::LocalParticipant>("PA_LOCAL_J", "local-j", [&](const livekit::proto::SignalRequest &) {
        TEST_CHECK(local->SnapshotState().sid == "PA_LOCAL_J");
        local->set_attribute("callback", "outside-lock"); ++sends;
    });
    local->add_publication(std::make_shared<livekit::TrackPublication>(video, "TR_LOCAL_J", "local"));
    local->SetPublishDataHandler([&](const auto &, bool, const auto &, const auto &) {
        TEST_CHECK(local->SnapshotState().identity == "local-j"); local->set_name("callback-name"); ++dataCallbacks;
    });
    local->SetPublishTrackHandler([&](std::shared_ptr<livekit::Track>) {
        TEST_CHECK(local->SnapshotState().name == "callback-name"); ++publishCallbacks;
    });
    auto callbacks = std::async(std::launch::async, [&] {
        local->SetAttributes({{"key", "value"}});
        local->SetMuted("TR_LOCAL_J", true);
        local->PublishData({1, 2, 3});
        local->PublishTrack(audio);
    });
    WaitBounded(callbacks); callbacks.get();
    TEST_CHECK(sends >= 2 && dataCallbacks == 1 && publishCallbacks == 1);
    std::cout << "AK_CASE_J copy/map/publication/concurrent-assignment/lock-free-callback PASS" << std::endl;
}

class CountingAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
public:
    SourceState state() const override { return kLive; }
    bool remote() const override { return true; }
    void AddSink(webrtc::AudioTrackSinkInterface *sink) override {
        TEST_CHECK(std::this_thread::get_id() == thread);
        TEST_CHECK(sink && std::find(sinks.begin(), sinks.end(), sink) == sinks.end());
        sinks.push_back(sink); added.push_back(sink);
        auto callback = std::move(onAdd); onAdd = {};
        if (callback) callback();
    }
    void RemoveSink(webrtc::AudioTrackSinkInterface *sink) override {
        TEST_CHECK(std::this_thread::get_id() == thread);
        const auto current = std::find(sinks.begin(), sinks.end(), sink);
        TEST_CHECK(current != sinks.end());
        sinks.erase(current); removed.push_back(sink);
        auto callback = std::move(onRemove); onRemove = {};
        if (callback) callback();
    }
    std::thread::id thread = std::this_thread::get_id();
    std::vector<webrtc::AudioTrackSinkInterface *> sinks, added, removed;
    std::function<void()> onAdd, onRemove;
};

class AttachFixture final : public Fixture {
public:
    explicit AttachFixture(bool autoSubscribe = true)
        : sourceA(webrtc::make_ref_counted<CountingAudioSource>()),
          sourceB(webrtc::make_ref_counted<CountingAudioSource>()),
          rtcA(webrtc::AudioTrack::Create("rtc-attach-A", sourceA)),
          rtcB(webrtc::AudioTrack::Create("rtc-attach-B", sourceB)) {
        TEST_CHECK(rtcA && rtcB);
        // H is a resolved-attach local test. This is NOT a real Connect/F test.
        livekit::ParticipantSnapshotRoomTestAccess::establishConnectedAttachPrecondition(
            *room, autoSubscribe);
        addParticipant();
        participantA = room->remote_participants().at("PA_ATTACH");
        publicationA = participantA->get_remote_publication("TR_ATTACH");
        TEST_CHECK(publicationA);
        trackA = publicationA->track();
        PumpPipeline(*this);
    }
    ~AttachFixture() {
        room->SetLogHandler({});
        sourceA->onAdd = {}; sourceA->onRemove = {};
        sourceB->onAdd = {}; sourceB->onRemove = {};
        room->Disconnect(); // Real cleanup while all observer storage is alive.
        PumpPipeline(*this);
    }
    void addParticipant() {
        auto update = MakeParticipantUpdate("PA_ATTACH", "attach-peer", "attach-participant",
            livekit::proto::ParticipantInfo::ACTIVE, false);
        auto *track = update.mutable_participants(0)->add_tracks();
        track->set_sid("TR_ATTACH"); track->set_name("audio"); track->set_type(livekit::proto::TrackType::AUDIO);
        room->UpdateParticipantsForTesting(update);
    }
    void retireParticipant() {
        room->UpdateParticipantsForTesting(MakeParticipantUpdate("PA_ATTACH", "attach-peer", "",
            livekit::proto::ParticipantInfo::DISCONNECTED, false));
    }
    void attachA() {
        livekit::ParticipantSnapshotRoomTestAccess::attach(*room, participantA, rtcA, "TR_ATTACH");
    }
    void attachB(const std::shared_ptr<livekit::RemoteParticipant> &participant) {
        livekit::ParticipantSnapshotRoomTestAccess::attach(*room, participant, rtcB, "TR_ATTACH");
    }
    std::size_t availableCount(uint64_t incarnation = 0) const {
        const auto events = bridge->events();
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const auto &event) {
            return event.kind == livekit::ParticipantEventKind::TrackAvailable &&
                (incarnation == 0 || event.participant.key.incarnation == incarnation);
        }));
    }
    webrtc::scoped_refptr<CountingAudioSource> sourceA, sourceB;
    webrtc::scoped_refptr<webrtc::AudioTrack> rtcA, rtcB;
    std::shared_ptr<livekit::RemoteParticipant> participantA;
    std::shared_ptr<livekit::RemoteTrackPublication> publicationA;
    std::shared_ptr<livekit::Track> trackA;
};

void AkAttachBaseline() {
    AttachFixture fixture;
    fixture.attachA();
    PumpPipeline(fixture);
    TEST_CHECK(fixture.sourceA->added.size() == 1 && fixture.sourceA->sinks.size() == 1);
    TEST_CHECK(fixture.sourceA->removed.empty());
    TEST_CHECK(fixture.trackA->rtc_track().get() == fixture.rtcA.get());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get()) != 0);
    TEST_CHECK(fixture.availableCount() == 1);
    TEST_CHECK(!Projected(fixture, "attach-peer").isAudioMuted);
    fixture.retireParticipant(); PumpPipeline(fixture);
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.empty());
    TEST_CHECK(fixture.sourceA->removed.front() == fixture.sourceA->added.front());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);
    std::cout << "AK_CASE_H1 real-AudioTrack/AddSink/retire/RemoveSink PASS" << std::endl;
}

void AkAttachPreRetain(bool replace) {
    AttachFixture fixture;
    const auto oldKey = Projected(fixture, "attach-peer").participantKey;
    std::shared_ptr<livekit::RemoteParticipant> successor;
    fixture.sourceA->onAdd = [&] {
        fixture.retireParticipant();
        if (replace) {
            fixture.addParticipant(); successor = fixture.room->remote_participants().at("PA_ATTACH");
            fixture.attachB(successor);
        }
    };
    fixture.attachA(); PumpPipeline(fixture);
    TEST_CHECK(fixture.sourceA->added.size() == 1 && fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.empty());
    TEST_CHECK(!fixture.trackA->rtc_track());
    TEST_CHECK(fixture.availableCount(oldKey.incarnation) == 0);
    if (replace) {
        const auto publication = successor->get_remote_publication("TR_ATTACH");
        TEST_CHECK(publication->track()->rtc_track().get() == fixture.rtcB.get());
        TEST_CHECK(fixture.sourceB->sinks.size() == 1 && fixture.sourceB->removed.empty());
        TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 1);
        TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, publication.get()) != 0);
        TEST_CHECK(fixture.availableCount() == 1);
    } else TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);
    std::cout << "AK_CASE_H" << (replace ? 3 : 2) << " pre-retain/replace=" << replace << " PASS" << std::endl;
}

void AkAttachRetainedTail() {
    AttachFixture fixture;
    const auto oldKey = Projected(fixture, "attach-peer").participantKey;
    std::shared_ptr<livekit::RemoteParticipant> successor;
    bool replaced = false;
    fixture.room->SetLogHandler([&](const std::string &, const std::string &tag, const std::string &) {
        if (tag != "AUDIO_ATTACH" || replaced) return;
        replaced = true;
        fixture.retireParticipant(); fixture.addParticipant();
        successor = fixture.room->remote_participants().at("PA_ATTACH");
        fixture.attachB(successor);
    });
    fixture.attachA(); PumpPipeline(fixture);
    TEST_CHECK(replaced && successor != fixture.participantA);
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.empty());
    TEST_CHECK(fixture.availableCount(oldKey.incarnation) == 0 && fixture.availableCount() == 1);
    TEST_CHECK(fixture.sourceB->sinks.size() == 1 && fixture.sourceB->removed.empty());
    TEST_CHECK(successor->get_remote_publication("TR_ATTACH")->track()->rtc_track().get() == fixture.rtcB.get());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 1);
    std::cout << "AK_CASE_H4 retained-tail/same-SID-successor PASS" << std::endl;
}

void AkAttachSupersededSerial() {
    AttachFixture fixture;
    fixture.attachA();
    const auto original = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    fixture.attachA(); // Same RTC id reaches retain's superseded-binding branch.
    const auto replacement = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    PumpPipeline(fixture);
    TEST_CHECK(original != 0 && replacement > original);
    TEST_CHECK(fixture.sourceA->added.size() == 2 && fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.size() == 1);
    TEST_CHECK(fixture.sourceA->sinks.front() == fixture.sourceA->added.back());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 1);
    std::cout << "AK_CASE_H4 superseded-binding-serial PASS" << std::endl;
}

void AkAttachStaleCleanup() {
    AttachFixture fixture;
    fixture.attachA();
    const auto original = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    fixture.attachB(fixture.participantA);
    const auto replacement = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    livekit::ParticipantSnapshotRoomTestAccess::detach(*fixture.room, fixture.publicationA.get(), original);
    TEST_CHECK(replacement > original && fixture.trackA->rtc_track().get() == fixture.rtcB.get());
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceB->removed.empty() && fixture.sourceB->sinks.size() == 1);
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get()) == replacement);
    livekit::ParticipantSnapshotRoomTestAccess::detach(*fixture.room, fixture.publicationA.get(), replacement);
    TEST_CHECK(fixture.sourceB->removed.size() == 1 && fixture.sourceB->sinks.empty());
    TEST_CHECK(!fixture.trackA->rtc_track());
    std::cout << "AK_CASE_H5 stale-serial-cleanup/no-successor-damage PASS" << std::endl;
}

void AkAttachRemoveReentry() {
    AttachFixture fixture;
    fixture.attachA();
    const auto original = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    uint64_t replacement = 0;
    fixture.sourceA->onRemove = [&] {
        fixture.attachB(fixture.participantA);
        replacement = livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get());
    };
    livekit::ParticipantSnapshotRoomTestAccess::detach(*fixture.room, fixture.publicationA.get(), original);
    const bool successorHandleIntact = fixture.trackA->rtc_track().get() == fixture.rtcB.get();
    std::cout << "AK_CASE_H6 current-serial=" << replacement << " old-serial=" << original
              << " B-sinks=" << fixture.sourceB->sinks.size() << " successor-rtc-intact=" << successorHandleIntact << std::endl;
    TEST_CHECK(replacement > original);
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceB->sinks.size() == 1 && fixture.sourceB->removed.empty());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(*fixture.room, fixture.publicationA.get()) == replacement);
    TEST_CHECK(successorHandleIntact);
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 1);
    PumpPipeline(fixture);
    std::cout << "AK_CASE_H6 external-RemoveSink/reentrant-successor PASS" << std::endl;
}

bool SyncHasSubscriptionException(const livekit::proto::SyncState &state,
                                  const std::string &participantSid,
                                  const std::string &trackSid) {
    const auto &subscription = state.subscription();
    const bool flat = std::find(subscription.track_sids().begin(),
        subscription.track_sids().end(), trackSid) != subscription.track_sids().end();
    const bool scoped = std::any_of(
        subscription.participant_tracks().begin(),
        subscription.participant_tracks().end(),
        [&](const auto &participant) {
            return participant.participant_sid() == participantSid &&
                std::find(participant.track_sids().begin(),
                    participant.track_sids().end(), trackSid) !=
                    participant.track_sids().end();
        });
    return flat && scoped;
}

void GapSubscriptionLateAttachAndSyncState() {
    {
        AttachFixture fixture;
        TEST_CHECK(fixture.publicationA->SetSubscribed(false));
        TEST_CHECK(!fixture.publicationA->is_subscribed());
        const auto sync = livekit::ParticipantSnapshotRoomTestAccess::syncState(*fixture.room);
        TEST_CHECK(!sync.subscription().subscribe());
        TEST_CHECK(SyncHasSubscriptionException(sync, "PA_ATTACH", "TR_ATTACH"));

        fixture.attachA();
        PumpPipeline(fixture);
        TEST_CHECK(fixture.sourceA->added.empty() && fixture.sourceA->sinks.empty());
        TEST_CHECK(fixture.availableCount() == 0);

        TEST_CHECK(fixture.publicationA->SetSubscribed(true));
        fixture.attachA();
        PumpPipeline(fixture);
        TEST_CHECK(fixture.sourceA->added.size() == 1 && fixture.sourceA->sinks.size() == 1);
        TEST_CHECK(fixture.availableCount() == 1);
    }
    {
        AttachFixture fixture(false);
        TEST_CHECK(!fixture.publicationA->is_subscribed());
        auto sync = livekit::ParticipantSnapshotRoomTestAccess::syncState(*fixture.room);
        TEST_CHECK(sync.subscription().subscribe());
        TEST_CHECK(!SyncHasSubscriptionException(sync, "PA_ATTACH", "TR_ATTACH"));
        TEST_CHECK(fixture.publicationA->SetSubscribed(true));
        sync = livekit::ParticipantSnapshotRoomTestAccess::syncState(*fixture.room);
        TEST_CHECK(sync.subscription().subscribe());
        TEST_CHECK(SyncHasSubscriptionException(sync, "PA_ATTACH", "TR_ATTACH"));
    }
    std::cout << "GAP_P1_03_CASE_1 late-attach/auto-subscribe-matrix/SyncState PASS" << std::endl;
}

void GapSubscriptionAcquireInterleave() {
    AttachFixture fixture;
    fixture.sourceA->onAdd = [&] {
        TEST_CHECK(fixture.publicationA->SetSubscribed(false));
    };
    fixture.attachA();
    PumpPipeline(fixture);
    TEST_CHECK(!fixture.publicationA->is_subscribed());
    TEST_CHECK(fixture.sourceA->added.size() == 1);
    TEST_CHECK(fixture.sourceA->removed.size() == 1);
    TEST_CHECK(fixture.sourceA->sinks.empty());
    TEST_CHECK(!fixture.trackA->rtc_track());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);
    TEST_CHECK(fixture.availableCount() == 0);
    std::cout << "GAP_P1_03_CASE_2 unsubscribe-between-AddSink-and-retain PASS" << std::endl;
}

void GapSubscriptionQueuedDeliveryAndStaleCleanup() {
    AttachFixture fixture;
    livekit::ParticipantSnapshotRoomTestAccess::pauseParticipantDrain(*fixture.room);
    fixture.attachA();
    const auto firstSerial =
        livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(
            *fixture.room, fixture.publicationA.get());
    TEST_CHECK(firstSerial != 0);
    TEST_CHECK(fixture.publicationA->SetSubscribed(false));
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.empty());
    TEST_CHECK(fixture.publicationA->SetSubscribed(true));
    fixture.attachB(fixture.participantA);
    const auto successorSerial =
        livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(
            *fixture.room, fixture.publicationA.get());
    TEST_CHECK(successorSerial > firstSerial);
    TEST_CHECK(fixture.sourceB->sinks.size() == 1 && fixture.sourceB->removed.empty());

    livekit::ParticipantSnapshotRoomTestAccess::resumeParticipantDrain(*fixture.room);
    PumpPipeline(fixture);
    TEST_CHECK(fixture.availableCount() == 1);
    TEST_CHECK(fixture.trackA->rtc_track().get() == fixture.rtcB.get());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingSerial(
        *fixture.room, fixture.publicationA.get()) == successorSerial);
    TEST_CHECK(fixture.sourceB->sinks.size() == 1 && fixture.sourceB->removed.empty());
    std::cout << "GAP_P1_03_CASE_3 revoked-Available/stale-Unavailable/new-binding PASS" << std::endl;
}

class ReentrantUnsubscribeListener final : public livekit::RoomListener {
public:
    explicit ReentrantUnsubscribeListener(std::shared_ptr<livekit::Room> room)
        : room_(std::move(room)) {}

    void OnTrackUnsubscribed(
            std::shared_ptr<livekit::Track>,
            std::shared_ptr<livekit::TrackPublication> publication,
            std::shared_ptr<livekit::RemoteParticipant>) override {
        ++callbacks;
        const auto room = room_.lock();
        TEST_CHECK(room && !room->remote_participants().empty());
        const auto remote =
            std::dynamic_pointer_cast<livekit::RemoteTrackPublication>(publication);
        TEST_CHECK(remote && remote->SetSubscribed(true));
        resubscribed = true;
    }

    int callbacks = 0;
    bool resubscribed = false;

private:
    std::weak_ptr<livekit::Room> room_;
};

void GapSubscriptionReentrantAndRepeatedCleanup() {
    AttachFixture fixture;
    auto listener =
        std::make_shared<ReentrantUnsubscribeListener>(fixture.room);
    fixture.room->AddListener(listener);
    fixture.attachA();
    TEST_CHECK(fixture.publicationA->SetSubscribed(false));
    TEST_CHECK(listener->callbacks == 1 && listener->resubscribed);
    TEST_CHECK(fixture.publicationA->is_subscribed());
    TEST_CHECK(fixture.sourceA->removed.size() == 1 && fixture.sourceA->sinks.empty());

    fixture.attachB(fixture.participantA);
    TEST_CHECK(fixture.sourceB->sinks.size() == 1);
    fixture.room->RemoveListener(listener);
    TEST_CHECK(fixture.publicationA->SetSubscribed(false));
    TEST_CHECK(fixture.publicationA->SetSubscribed(false));
    TEST_CHECK(fixture.sourceB->removed.size() == 1 && fixture.sourceB->sinks.empty());
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::bindingCount(*fixture.room) == 0);
    std::cout << "GAP_P1_03_CASE_10 listener-reentry/repeated-false/no-double-cleanup PASS" << std::endl;
}

struct OwnedStopWitness {
    std::weak_ptr<OpenMeeting::MeetingSessionRuntime> runtime;
    std::shared_ptr<std::atomic<bool>> workerExited = std::make_shared<std::atomic<bool>>(false);
    int contextShutdowns = 0;
    int contextDestructions = 0;
    int disconnectObservations = 0;
    bool runtimeExpiredAtContextShutdown = true;
    bool workerExitedAtContextShutdown = true;
    bool transfersEmptyAfterBarrier = false;
    bool admissionClosedAfterBarrier = false;
};

// The witness stores only a weak runtime. It cannot prolong the strand's
// lifetime into execution_context destruction and make that order appear safe.
class OwnedContextWitnessService final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;
    explicit OwnedContextWitnessService(asio::execution_context &context)
        : asio::execution_context::service(context) {}
    ~OwnedContextWitnessService() override {
        if (witness) ++witness->contextDestructions;
    }
    std::shared_ptr<OwnedStopWitness> witness;
private:
    void shutdown() override {
        TEST_CHECK(witness);
        ++witness->contextShutdowns;
        const bool runtimeExpired = witness->runtime.expired();
        const bool workerExited = witness->workerExited->load(std::memory_order_acquire);
        // ASIO calls service shutdown from both io_context and its base
        // destructor. A later success must never hide an earlier order failure.
        witness->runtimeExpiredAtContextShutdown &= runtimeExpired;
        witness->workerExitedAtContextShutdown &= workerExited;
        std::cout << "AK_CONTEXT_SHUTDOWN call=" << witness->contextShutdowns
                  << " runtime-expired=" << runtimeExpired << " worker-exited=" << workerExited << std::endl;
        TEST_CHECK(runtimeExpired && workerExited);
    }
};
asio::execution_context::id OwnedContextWitnessService::id;

class StopBarrierObserver final : public livekit::RoomListener {
public:
    explicit StopBarrierObserver(std::shared_ptr<OwnedStopWitness> value) : witness(std::move(value)) {}
    void OnDisconnected(livekit::RoomDisconnectReason, const std::string &) override {
        // Disconnect happens after the production cleanup barrier and before
        // io_context::stop. The worker remains free to execute this observation.
        auto runtime = witness->runtime.lock();
        TEST_CHECK(runtime);
        std::promise<void> checked;
        auto future = checked.get_future();
        asio::post(runtime->strand(), [runtime, witness = witness, &checked] {
            runtime->assertOnStrand();
            witness->transfersEmptyAfterBarrier = runtime->transfersOnStrand().empty();
            witness->admissionClosedAfterBarrier = !runtime->acceptsDataOnStrand();
            ++witness->disconnectObservations;
            checked.set_value();
        });
        WaitBounded(future); future.get();
    }
private:
    std::shared_ptr<OwnedStopWitness> witness;
};

class OwnedShutdownFixture final {
public:
    OwnedShutdownFixture()
        : session(OpenMeeting::SessionManagerTestAccess::create(
              std::make_unique<QSettings>(settingsDirectory.filePath("settings.ini"), QSettings::IniFormat))),
          coordinator(OpenMeeting::MeetingCoordinatorTestAccess::create(*session)),
          witness(std::make_shared<OwnedStopWitness>()),
          bridge(std::make_shared<ValueBridge>()), observer(std::make_shared<StopBarrierObserver>(witness)) {
        TEST_CHECK(settingsDirectory.isValid());
        listener = OpenMeeting::MeetingCoordinatorTestAccess::createOwnedSession(*coordinator);
        room = OpenMeeting::MeetingCoordinatorTestAccess::ownedRoom(*coordinator);
        runtime = OpenMeeting::MeetingCoordinatorTestAccess::ownedRuntime(*coordinator);
        witness->runtime = runtime;
        auto &context = OpenMeeting::MeetingCoordinatorTestAccess::ownedContext(*coordinator);
        asio::use_service<OwnedContextWitnessService>(context).witness = witness;
        {
            auto owner = room.lock();
            TEST_CHECK(owner);
            // Local connected precondition only: no claim of network Connect.
            livekit::ParticipantSnapshotRoomTestAccess::establishConnectedAttachPrecondition(*owner);
            owner->AddListener(bridge);
            owner->AddListener(observer);
        }
        OpenMeeting::MeetingCoordinatorTestAccess::startOwnedWorker(*coordinator, witness->workerExited);
        nativeBarrier();
    }
    ~OwnedShutdownFixture() {
        coordinator.reset();
        bridge->clear();
        DrainQt();
    }
    void nativeBarrier() {
        std::promise<void> reached;
        auto future = reached.get_future();
        asio::post(OpenMeeting::MeetingCoordinatorTestAccess::ownedContext(*coordinator), [&] { reached.set_value(); });
        WaitBounded(future); future.get();
    }
    void onStrand(std::function<void(OpenMeeting::MeetingSessionRuntime &)> action) {
        auto value = runtime.lock();
        TEST_CHECK(value);
        std::promise<void> reached;
        auto future = reached.get_future();
        asio::post(value->strand(), [value, action = std::move(action), &reached] {
            action(*value);
            reached.set_value();
        });
        WaitBounded(future); future.get();
    }
    void deliverQt() { QCoreApplication::sendPostedEvents(coordinator.get(), QEvent::MetaCall); }
    void pump() {
        nativeBarrier(); deliverQt();
        onStrand([](auto &) {}); deliverQt();
        nativeBarrier(); deliverQt();
    }
    void addParticipant(bool video = true) {
        auto value = room.lock();
        TEST_CHECK(value);
        value->UpdateParticipantsForTesting(MakeParticipantUpdate(
            "PA_SHUTDOWN", "shutdown-peer", "shutdown-name", livekit::proto::ParticipantInfo::ACTIVE, video));
    }
    std::weak_ptr<livekit::Track> track() {
        auto value = room.lock();
        TEST_CHECK(value);
        auto participant = value->remote_participants().at("PA_SHUTDOWN");
        return participant->get_publication("TR_PA_SHUTDOWN")->track();
    }
    void stop() {
        coordinator->leaveMeetingAsync(false);
        TEST_CHECK(coordinator->state() == OpenMeeting::MeetingState::Idle);
        TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::ownedSessionReleased(*coordinator));
    }
    void checkStopOrder() const {
        std::cout << "AK_STOP_WITNESS disconnect=" << witness->disconnectObservations
                  << " map-empty=" << witness->transfersEmptyAfterBarrier
                  << " admission-closed=" << witness->admissionClosedAfterBarrier
                  << " service-shutdowns=" << witness->contextShutdowns
                  << " service-destructions=" << witness->contextDestructions
                  << " worker-exited-at-shutdown=" << witness->workerExitedAtContextShutdown
                  << " runtime-expired-at-shutdown=" << witness->runtimeExpiredAtContextShutdown
                  << " runtime-expired-now=" << runtime.expired() << std::endl;
        TEST_CHECK(witness->disconnectObservations == 1);
        TEST_CHECK(witness->transfersEmptyAfterBarrier && witness->admissionClosedAfterBarrier);
        // This bundled ASIO invokes shutdown in io_context::~io_context and
        // execution_context::~execution_context; service destruction is once.
        TEST_CHECK(witness->contextShutdowns == 2 && witness->contextDestructions == 1);
        TEST_CHECK(witness->workerExitedAtContextShutdown && witness->runtimeExpiredAtContextShutdown);
        TEST_CHECK(runtime.expired());
    }

    QTemporaryDir settingsDirectory;
    OpenMeeting::SessionManagerTestAccess::ScopedSession session;
    std::unique_ptr<OpenMeeting::MeetingCoordinator> coordinator;
    std::shared_ptr<OwnedStopWitness> witness;
    std::shared_ptr<ValueBridge> bridge;
    std::shared_ptr<StopBarrierObserver> observer;
    std::weak_ptr<livekit::Room> room;
    std::weak_ptr<OpenMeeting::MeetingSessionRuntime> runtime;
    std::weak_ptr<livekit::RoomListener> listener;
};

void AkShutdownQtPending(bool destroyOwner) {
    OwnedShutdownFixture fixture;
    int joins = 0, updates = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantJoined,
        fixture.coordinator.get(), [&] { ++joins; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
        fixture.coordinator.get(), [&] { ++updates; });
    fixture.addParticipant();
    fixture.nativeBarrier(); // Native value listener has posted the real Qt functors.
    auto track = fixture.track();
    TEST_CHECK(!track.expired() && !fixture.bridge->events().empty());
    {
        auto room = fixture.room.lock();
        TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::pendingParticipantEvents(*room) == 0);
    }
    fixture.bridge->clear(); // Do not let our observation history retain Track.
    TEST_CHECK(joins == 0 && updates == 0);
    if (destroyOwner) fixture.coordinator.reset();
    else fixture.stop();
    fixture.checkStopOrder();
    TEST_CHECK(fixture.room.expired() && fixture.listener.expired());
    if (!destroyOwner) TEST_CHECK(!track.expired()); // Only the unconsumed Qt payload now retains it.
    const int updatesAtStop = updates;
    DrainQt();
    TEST_CHECK(joins == 0 && updates == updatesAtStop);
    TEST_CHECK(track.expired());
    std::cout << "AK_CASE_K2 Qt-pending/destroy-owner=" << destroyOwner
              << " runtime-before-context/no-business-effect/payload-released PASS" << std::endl;
}

void AkShutdownTransfers() {
    OwnedShutdownFixture fixture;
    fixture.addParticipant(false); fixture.pump();
    livekit::ParticipantKey senderKey;
    for (const auto &event : fixture.bridge->events()) {
        if (event.kind == livekit::ParticipantEventKind::Upsert) senderKey = event.participant.key;
    }
    TEST_CHECK(senderKey.incarnation != 0);
    fixture.bridge->clear();
    std::map<QString, int> starts, failures;
    int completions = 0, messages = 0;
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
        fixture.coordinator.get(), [&](const QString &id) { ++starts[id]; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
        fixture.coordinator.get(), [&](const QString &id) { ++failures[id]; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
        fixture.coordinator.get(), [&] { ++completions; });
    QObject::connect(fixture.coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaMessageReceived,
        fixture.coordinator.get(), [&] { ++messages; });
    const auto send = [&](const QString &id, bool chunk) {
        QJsonObject packet{{"om_type", chunk ? "media_chunk" : "media_start"}, {"transferId", id},
            {"totalChunks", 1}, {"mediaType", "file"}, {"fileName", "shutdown.bin"}, {"totalSize", 5}};
        if (chunk) { packet["chunkIndex"] = 0; packet["chunkData"] = "aGVsbG8="; }
        fixture.room.lock()->OnIncomingDataPacket(JsonPayload(packet), "PA_SHUTDOWN", "chat");
    };
    send("receiving", false); send("aggregated", false);
    fixture.pump();
    TEST_CHECK(starts.size() == 2 && OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 2);
    fixture.onStrand([](auto &runtime) { TEST_CHECK(runtime.transfersOnStrand().size() == 2); });
    send("aggregated", true);
    fixture.nativeBarrier(); // Chunk is now queued to the GUI, not yet aggregated.
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    {
        auto runtime = fixture.runtime.lock();
        asio::post(runtime->strand(), [&] { entered.set_value(); WaitBounded(releaseFuture); });
    }
    WaitBounded(enteredFuture);
    fixture.deliverQt(); // Real DataReceived routes the chunk behind the controlled gate.
    release.set_value(); // Release BEFORE stop can wait on the same strand.
    fixture.onStrand([](auto &runtime) {
        TEST_CHECK(runtime.transfersOnStrand().size() == 1);
        TEST_CHECK(runtime.transfersOnStrand().begin()->first.wireTransferId == "receiving");
    });
    TEST_CHECK(completions == 0 && messages == 0 && failures.empty());
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 2);
    fixture.bridge->clear();
    fixture.stop(); fixture.checkStopOrder();
    TEST_CHECK(failures.size() == 2);
    for (const auto &[id, count] : starts) { TEST_CHECK(count == 1 && failures[id] == 1); }
    TEST_CHECK(completions == 0 && messages == 0);
    OpenMeeting::MeetingCoordinatorTestAccess::stopAgain(*fixture.coordinator);
    OpenMeeting::MeetingCoordinatorTestAccess::cancel(*fixture.coordinator, senderKey);
    DrainQt();
    TEST_CHECK(failures.size() == 2 && completions == 0 && messages == 0);
    for (const auto &[id, count] : failures) TEST_CHECK(count == 1);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(*fixture.coordinator) == 0);
    TEST_CHECK(fixture.room.expired() && fixture.listener.expired());
    std::cout << "AK_CASE_K3 receiving+aggregated/two-real-placeholders/stop-terminal-once/repeated-cleanup PASS" << std::endl;
}

void AkShutdownTickets(bool retainState) {
    OwnedShutdownFixture fixture;
    fixture.addParticipant();
    auto update = MakeParticipantUpdate("PA_SHUTDOWN", "shutdown-peer", "muted-name", livekit::proto::ParticipantInfo::ACTIVE);
    update.mutable_participants(0)->mutable_tracks(0)->set_muted(true);
    fixture.room.lock()->UpdateParticipantsForTesting(update);
    fixture.pump();
    livekit::ParticipantTicket participantTicket;
    livekit::TrackTicket trackTicket;
    livekit::ParticipantKey participantKey;
    livekit::TrackKey trackKey;
    {
        const auto events = fixture.bridge->events();
        for (const auto &event : events) {
            if (event.kind == livekit::ParticipantEventKind::TrackMuted) {
                participantTicket = event.participant.ticket; participantKey = event.participant.key;
                trackTicket = event.track_ticket; trackKey = event.track_key;
            }
        }
    }
    TEST_CHECK(livekit::IsParticipantTicketActive(participantTicket, participantKey));
    TEST_CHECK(livekit::IsTrackTicketActive(trackTicket, trackKey));
    auto participantState = retainState ? participantTicket.lock() : nullptr;
    auto trackState = retainState ? trackTicket.lock() : nullptr;
    auto track = fixture.track();
    fixture.bridge->clear();
    fixture.stop(); fixture.checkStopOrder(); DrainQt();
    TEST_CHECK(fixture.room.expired() && fixture.listener.expired() && track.expired());
    TEST_CHECK(!livekit::IsParticipantTicketActive(participantTicket, participantKey));
    TEST_CHECK(!livekit::IsTrackTicketActive(trackTicket, trackKey));
    if (retainState) {
        TEST_CHECK(participantState && trackState && !participantState->active.load() && !trackState->active.load());
        TEST_CHECK(!participantTicket.expired() && !trackTicket.expired());
    }
    participantState.reset(); trackState.reset();
    TEST_CHECK(participantTicket.expired() && trackTicket.expired());
    std::cout << "AK_CASE_K4 weak-ticket/no-cycle/retain-state=" << retainState << " PASS" << std::endl;
}

void AkShutdownNativePending() {
    OwnedShutdownFixture fixture;
    auto externalRoom = fixture.room.lock();
    TEST_CHECK(externalRoom);
    livekit::ParticipantSnapshotRoomTestAccess::pauseParticipantDrain(*externalRoom);
    fixture.addParticipant();
    fixture.nativeBarrier();
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::pausedDrainAttempts(*externalRoom) == 1);
    TEST_CHECK(livekit::ParticipantSnapshotRoomTestAccess::pendingParticipantEvents(*externalRoom) > 0);
    auto ticket = livekit::ParticipantSnapshotRoomTestAccess::firstPendingTicket(*externalRoom);
    auto track = fixture.track();
    const auto *listenerAddress = fixture.listener.lock().get();
    TEST_CHECK(listenerAddress && livekit::ParticipantSnapshotRoomTestAccess::containsListener(*externalRoom, listenerAddress));
    TEST_CHECK(fixture.bridge->events().empty() && !track.expired());
    fixture.stop(); fixture.checkStopOrder();
    const auto queuedAfterStop = livekit::ParticipantSnapshotRoomTestAccess::pendingParticipantEvents(*externalRoom);
    const bool payloadReleased = track.expired();
    const bool listenerStillRegistered = livekit::ParticipantSnapshotRoomTestAccess::containsListener(*externalRoom, listenerAddress);
    const bool listenerExpired = fixture.listener.expired();
    TEST_CHECK(ticket.expired());
    std::cout << "AK_CASE_K1 external-Room/native-pending queued-after-stop=" << queuedAfterStop
              << " payload-released=" << payloadReleased << " listener-registered=" << listenerStillRegistered
              << " listener-expired=" << listenerExpired << std::endl;
    // Do not call the retained raw-owner bridge after owner destruction to
    // manufacture a UAF. Release our external Room safely before assertions.
    externalRoom.reset();
    TEST_CHECK(fixture.room.expired() && fixture.listener.expired() && track.expired());
    DrainQt();
    TEST_CHECK(queuedAfterStop == 0);
    TEST_CHECK(payloadReleased);
    TEST_CHECK(!listenerStillRegistered && listenerExpired);
    std::cout << "AK_CASE_K1 external-Room/native-pending/payload+bridge-released PASS" << std::endl;
}

void AkShutdownRegression() {
    std::cout << "AK_SHUTDOWN_PLANNED=6 (real owned Coordinator stop; local Connected precondition)" << std::endl;
    AkShutdownQtPending(false);
    AkShutdownQtPending(true);
    AkShutdownTransfers();
    AkShutdownTickets(false);
    AkShutdownTickets(true);
    AkShutdownNativePending(); // Preserve the FIFO/listener candidate for safe RED last.
    std::cout << "AK_SHUTDOWN_EXECUTED=6 PASSED=6 FAILED=0" << std::endl;
}

void AkAttachRegression() {
    AkAttachBaseline();
    AkAttachPreRetain(false);
    AkAttachPreRetain(true);
    AkAttachRetainedTail();
    AkAttachSupersededSerial();
    AkAttachStaleCleanup();
    AkAttachRemoveReentry();
    GapSubscriptionLateAttachAndSyncState();
    GapSubscriptionAcquireInterleave();
    GapSubscriptionQueuedDeliveryAndStaleCleanup();
    GapSubscriptionReentrantAndRepeatedCleanup();
    std::cout << "AK_ATTACH_EXECUTED=11 PASS (resolved-attach local precondition; no F Connect claim)" << std::endl;
}

void AkCoreRegression() {
    int executed = 0;
    AkCaseA(); ++executed;
    AkCaseB(); ++executed;
    for (int boundary = 0; boundary < 4; ++boundary) { AkCaseC(boundary); ++executed; }
    for (bool nativeDelivered : {false, true}) { AkCaseCStream(nativeDelivered); ++executed; }
    for (bool reuseSid : {false, true}) { AkCaseD(reuseSid); ++executed; }
    AkCaseE(); ++executed;
    AkCaseI(); ++executed;
    AkCaseJ(); ++executed;
    std::cout << "AK_CORE_EXECUTED=" << executed << " PASS (A-E/I/J first batch; F/H/K not claimed)" << std::endl;
}

void VerifyOpenMeetingInitialMediaProjection() {
    OpenMeeting::MediaPreferences requested;
    requested.enableMicrophone = true;
    requested.enableVideo = true;
    const std::string nested = R"({
        "detail": {
            "info": {
                "systemGenerated": {
                    "meetingID": "nested-meeting",
                    "creatorUserID": "creator-user",
                    "startTime": 1700000000
                },
                "creatorDefinedMeeting": {
                    "title": "Nested title",
                    "hostUserID": "host-user",
                    "scheduledTime": 1700000100,
                    "meetingDuration": 3600
                }
            },
            "setting": {
                "disableCameraOnJoin": true,
                "disableMicrophoneOnJoin": true,
                "canParticipantJoinMeetingEarly": false
            }
        }
    })";
    const auto nestedState = OpenMeeting::MeetingCoordinator::resolveInitialMediaState(
        nested, requested);
    TEST_CHECK(nestedState.hasOpenMeetingDetail && nestedState.metadataValid);
    TEST_CHECK(!nestedState.microphoneEnabled && !nestedState.videoEnabled);
    TEST_CHECK(nestedState.detail.meetingId == QStringLiteral("nested-meeting"));
    TEST_CHECK(nestedState.detail.meetingName == QStringLiteral("Nested title"));
    TEST_CHECK(nestedState.detail.creatorUserId == QStringLiteral("creator-user"));
    TEST_CHECK(nestedState.detail.hostUserId == QStringLiteral("host-user"));
    TEST_CHECK(nestedState.detail.startTime == 1700000000);
    TEST_CHECK(nestedState.detail.endTime == 1700003600);

    OpenMeeting::MediaPreferences userDisabled;
    userDisabled.enableMicrophone = false;
    userDisabled.enableVideo = false;
    const auto unrestricted = OpenMeeting::MeetingCoordinator::resolveInitialMediaState(
        R"({"detail":{"info":{},"setting":{}}})", userDisabled);
    TEST_CHECK(unrestricted.hasOpenMeetingDetail && unrestricted.metadataValid);
    TEST_CHECK(!unrestricted.microphoneEnabled && !unrestricted.videoEnabled);

    const auto unrelated = OpenMeeting::MeetingCoordinator::resolveInitialMediaState(
        R"({"custom":"manual-livekit"})", requested);
    TEST_CHECK(!unrelated.hasOpenMeetingDetail && unrelated.metadataValid);
    TEST_CHECK(unrelated.microphoneEnabled && unrelated.videoEnabled);

    const auto invalid = OpenMeeting::MeetingCoordinator::resolveInitialMediaState(
        R"({"detail":{"setting":{"disableCameraOnJoin":"false"}}})", requested);
    TEST_CHECK(invalid.hasOpenMeetingDetail && !invalid.metadataValid);
    TEST_CHECK(!invalid.microphoneEnabled && !invalid.videoEnabled);

    const std::string legacy = R"({
        "detail": {
            "info": {
                "meetingID": "legacy-meeting",
                "meetingName": "Legacy title",
                "creatorUserID": "legacy-creator",
                "hostUserID": "legacy-host",
                "startTime": 1800000000,
                "endTime": 1800007200
            },
            "setting": {"disableCameraOnJoin": true}
        }
    })";
    const auto legacyState = OpenMeeting::MeetingCoordinator::resolveInitialMediaState(
        legacy, requested);
    TEST_CHECK(legacyState.hasOpenMeetingDetail && legacyState.metadataValid);
    TEST_CHECK(legacyState.microphoneEnabled && !legacyState.videoEnabled);
    TEST_CHECK(legacyState.detail.meetingName == QStringLiteral("Legacy title"));

    Fixture metadataFixture;
    metadataFixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_HOST", "host-user", "Host", livekit::proto::ParticipantInfo::ACTIVE));
    DrainNative(metadataFixture.io);
    DrainQt();
    OpenMeeting::MeetingCoordinatorTestAccess::parseMetadata(
        *metadataFixture.coordinator, nested);
    TEST_CHECK(metadataFixture.coordinator->meetingDetail().meetingId ==
        QStringLiteral("nested-meeting"));
    const auto projected = OpenMeeting::MeetingCoordinatorTestAccess::participants(
        *metadataFixture.coordinator);
    const auto *host = FindParticipant(projected, QStringLiteral("host-user"));
    TEST_CHECK(host != nullptr && host->isHost);

    OpenMeeting::MeetingCoordinatorTestAccess::parseMetadata(
        *metadataFixture.coordinator, legacy);
    TEST_CHECK(metadataFixture.coordinator->meetingDetail().meetingId ==
        QStringLiteral("legacy-meeting"));
    TEST_CHECK(metadataFixture.coordinator->meetingDetail().hostUserId ==
        QStringLiteral("legacy-host"));
    std::cout << "P4_METADATA_MEDIA_POLICY PASS" << std::endl;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    // Validate the explicit temporary IniFormat storage used by our injected
    // fixtures. Native singleton settings cannot be redirected by Qt's
    // setDefaultFormat; singleton entry paths are prohibited instead.
    QTemporaryDir accountSettings;
    TEST_CHECK(accountSettings.isValid());
    QSettings settingsProbe(accountSettings.filePath("account-probe.ini"), QSettings::IniFormat);
    TEST_CHECK(settingsProbe.format() == QSettings::IniFormat);
    TEST_CHECK(QDir::cleanPath(settingsProbe.fileName()).startsWith(QDir::cleanPath(accountSettings.path()) + "/"));
    openmeeting::meeting::NotifyMeetingData defaultKick;
    defaultKick.mutable_kickoffmeetingdata()->set_userid("local-user");
    TEST_CHECK(!IsSessionOnlyNotify(defaultKick));
    defaultKick.mutable_kickoffmeetingdata()->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);
    TEST_CHECK(IsSessionOnlyNotify(defaultKick));
    if (application.arguments().contains(QStringLiteral("--ak-core"))) {
        AkCoreRegression();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ak-attach"))) {
        AkAttachRegression();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ak-shutdown"))) {
        AkShutdownRegression();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--owner-terminal-red"))) {
        CompletedLeaveHasOneTerminal();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--listener-owner-red"))) {
        DisconnectedLogCannotChangeSuccessor();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--listener-owner"))) {
        ListenerOwnerRegression();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--owner-terminal"))) {
        OwnerTerminalRegression();
        return 0;
    }
    VerifyOpenMeetingInitialMediaProjection();
    Fixture fixture;

    std::vector<QString> projectedNames;
    int participantLeftCount = 0;
    int mediaStartedCount = 0;
    int mediaCompletedCount = 0;
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::participantsUpdated,
                     fixture.coordinator.get(),
                     [&](const std::vector<OpenMeeting::ParticipantInfo> &participants) {
        if (const auto *participant = FindParticipant(
                participants, QStringLiteral("shared-identity"))) {
            projectedNames.push_back(participant->name);
        }
    });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::participantLeft,
                     fixture.coordinator.get(),
                     [&](const QString &) { ++participantLeftCount; });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &, const QString &, const QString &,
                         const QString &, qint64, int64_t) { ++mediaStartedCount; });
    QObject::connect(fixture.coordinator.get(),
                     &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
                     fixture.coordinator.get(),
                     [&](const QString &, const QString &, const QString &, const QString &,
                         const QString &, const QByteArray &) { ++mediaCompletedCount; });

    // Case A: a normal native participant update reaches the Qt projection as
    // a copied value and includes the participant-instance credential.
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_SHARED", "shared-identity", "initial-name",
        livekit::proto::ParticipantInfo::ACTIVE));
    DrainNative(fixture.io);
    DrainQt();
    auto participants = OpenMeeting::MeetingCoordinatorTestAccess::participants(
        *fixture.coordinator);
    const auto *initial = FindParticipant(participants, QStringLiteral("shared-identity"));
    TEST_CHECK(initial != nullptr);
    TEST_CHECK(initial->name == QStringLiteral("initial-name"));
    TEST_CHECK(initial->isVideoEnabled);
    TEST_CHECK(initial->participantKey.incarnation != 0);
    const livekit::ParticipantKey firstKey = initial->participantKey;

    // Case B: two native values are frozen before the Qt queue runs. The first
    // queued delivery must retain its original name instead of rereading the
    // now-mutated Participant.
    projectedNames.clear();
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_SHARED", "shared-identity", "snapshot-one",
        livekit::proto::ParticipantInfo::ACTIVE));
    DrainNative(fixture.io);
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_SHARED", "shared-identity", "snapshot-two",
        livekit::proto::ParticipantInfo::ACTIVE));
    DrainNative(fixture.io);
    TEST_CHECK(projectedNames.empty());
    DrainQt();
    TEST_CHECK(projectedNames.size() >= 2);
    TEST_CHECK(projectedNames[projectedNames.size() - 2] == QStringLiteral("snapshot-one"));
    TEST_CHECK(projectedNames.back() == QStringLiteral("snapshot-two"));

    // Case C: media completion work is queued, then the participant leaves.
    // The inactive ticket must reject every delayed UI effect.
    QJsonObject start;
    start[QStringLiteral("om_type")] = QStringLiteral("media_start");
    start[QStringLiteral("transferId")] = QStringLiteral("wire-reused");
    start[QStringLiteral("totalChunks")] = 1;
    start[QStringLiteral("mediaType")] = QStringLiteral("file");
    start[QStringLiteral("fileName")] = QStringLiteral("old.bin");
    start[QStringLiteral("totalSize")] = 5;
    fixture.room->OnIncomingDataPacket(
        JsonPayload(start), "PA_SHARED", "chat");
    QJsonObject chunk = start;
    chunk[QStringLiteral("om_type")] = QStringLiteral("media_chunk");
    chunk[QStringLiteral("chunkIndex")] = 0;
    chunk[QStringLiteral("chunkData")] = QStringLiteral("aGVsbG8=");
    fixture.room->OnIncomingDataPacket(
        JsonPayload(chunk), "PA_SHARED", "chat");
    DrainNative(fixture.io);
    DrainQt();
    DrainNative(fixture.io);

    const auto eventsBeforeDeparture = fixture.bridge->events();
    const auto oldUpsert = *std::find_if(
        eventsBeforeDeparture.rbegin(), eventsBeforeDeparture.rend(),
        [](const livekit::ParticipantEvent &event) {
            return event.kind == livekit::ParticipantEventKind::Upsert;
        });
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_SHARED", "shared-identity", "", livekit::proto::ParticipantInfo::DISCONNECTED,
        false));
    DrainNative(fixture.io);
    DrainQt();
    TEST_CHECK(mediaStartedCount == 0);
    TEST_CHECK(mediaCompletedCount == 0);
    TEST_CHECK(OpenMeeting::MeetingCoordinatorTestAccess::inboundLedgerSize(
                   *fixture.coordinator) == 0);
    TEST_CHECK(FindParticipant(
                   OpenMeeting::MeetingCoordinatorTestAccess::participants(*fixture.coordinator),
                   QStringLiteral("shared-identity")) == nullptr);
    TEST_CHECK(!livekit::IsParticipantTicketActive(
        oldUpsert.participant.ticket, oldUpsert.participant.key));

    const auto eventsAfterDeparture = fixture.bridge->events();
    const auto departure = *std::find_if(
        eventsAfterDeparture.rbegin(), eventsAfterDeparture.rend(),
        [](const livekit::ParticipantEvent &event) {
            return event.kind == livekit::ParticipantEventKind::Departure;
        });

    // Case D: the same SID and identity create a new incarnation. Replaying
    // the old value and tombstone cannot overwrite or remove the new instance.
    fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
        "PA_SHARED", "shared-identity", "replacement",
        livekit::proto::ParticipantInfo::ACTIVE));
    DrainNative(fixture.io);
    DrainQt();
    participants = OpenMeeting::MeetingCoordinatorTestAccess::participants(
        *fixture.coordinator);
    const auto *replacement = FindParticipant(
        participants, QStringLiteral("shared-identity"));
    TEST_CHECK(replacement != nullptr);
    TEST_CHECK(replacement->name == QStringLiteral("replacement"));
    TEST_CHECK(replacement->participantKey.incarnation != firstKey.incarnation);
    const auto replacementKey = replacement->participantKey;

    QMetaObject::invokeMethod(fixture.coordinator.get(), [&fixture, oldUpsert] {
        OpenMeeting::MeetingCoordinatorTestAccess::apply(
            *fixture.coordinator, kCoordinatorGeneration, oldUpsert);
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(fixture.coordinator.get(), [&fixture, departure] {
        OpenMeeting::MeetingCoordinatorTestAccess::apply(
            *fixture.coordinator, kCoordinatorGeneration, departure);
    }, Qt::QueuedConnection);
    DrainQt();
    participants = OpenMeeting::MeetingCoordinatorTestAccess::participants(
        *fixture.coordinator);
    replacement = FindParticipant(participants, QStringLiteral("shared-identity"));
    TEST_CHECK(replacement != nullptr);
    TEST_CHECK(replacement->participantKey == replacementKey);
    TEST_CHECK(replacement->name == QStringLiteral("replacement"));

    // Case E: concurrent producers are serialized by Room. The single native
    // drainer preserves event sequence and the Qt projection keeps every peer.
    constexpr int kConcurrentParticipants = 12;
    std::vector<std::thread> producers;
    producers.reserve(kConcurrentParticipants);
    for (int index = 0; index != kConcurrentParticipants; ++index) {
        producers.emplace_back([&fixture, index] {
            const std::string suffix = std::to_string(index);
            fixture.room->UpdateParticipantsForTesting(MakeParticipantUpdate(
                "PA_CONCURRENT_" + suffix,
                "concurrent-" + suffix,
                "peer-" + suffix,
                livekit::proto::ParticipantInfo::ACTIVE,
                false));
        });
    }
    for (auto &producer : producers) producer.join();
    DrainNative(fixture.io);
    DrainQt();

    participants = OpenMeeting::MeetingCoordinatorTestAccess::participants(
        *fixture.coordinator);
    for (int index = 0; index != kConcurrentParticipants; ++index) {
        TEST_CHECK(FindParticipant(
            participants,
            QStringLiteral("concurrent-%1").arg(index)) != nullptr);
    }
    const auto allEvents = fixture.bridge->events();
    uint64_t previousSequence = 0;
    for (const auto &event : allEvents) {
        TEST_CHECK(event.event_sequence > previousSequence);
        previousSequence = event.event_sequence;
    }

    // Snapshot access and whole-map mutation share the same lock; correlated
    // attributes can never be observed as a torn map under concurrent access.
    livekit::RemoteParticipant snapshotParticipant("PA_SNAPSHOT", "snapshot-peer");
    std::atomic<bool> writerDone{false};
    std::thread writer([&] {
        for (int revision = 1; revision <= 2000; ++revision) {
            snapshotParticipant.set_attributes({
                {"revision", std::to_string(revision)},
                {"parity", std::to_string(revision % 2)},
            });
        }
        writerDone.store(true, std::memory_order_release);
    });
    while (!writerDone.load(std::memory_order_acquire)) {
        const auto snapshot = snapshotParticipant.SnapshotState();
        const auto revision = snapshot.attributes.find("revision");
        const auto parity = snapshot.attributes.find("parity");
        if (revision != snapshot.attributes.end() && parity != snapshot.attributes.end()) {
            TEST_CHECK(std::stoi(revision->second) % 2 == std::stoi(parity->second));
        }
    }
    writer.join();

    // The locking migration preserves the legacy no-SID behavior: when no
    // canonical TR_ publication exists, every visited local track is muted
    // before the request is rejected for lacking a signaling SID.
    livekit::LocalParticipant localParticipant(
        "PA_LOCAL", "local-user", [](const livekit::proto::SignalRequest &) {});
    auto localAudio = std::make_shared<livekit::Track>(
        "legacy-audio", "audio", livekit::TrackKind::Audio);
    auto localVideo = std::make_shared<livekit::Track>(
        "legacy-video", "video", livekit::TrackKind::Video);
    localParticipant.add_publication(std::make_shared<livekit::TrackPublication>(
        localAudio, "legacy-audio", "audio"));
    localParticipant.add_publication(std::make_shared<livekit::TrackPublication>(
        localVideo, "legacy-video", "video"));
    localParticipant.SetMuted("", true);
    TEST_CHECK(localAudio->muted());
    TEST_CHECK(localVideo->muted());

    TEST_CHECK(participantLeftCount >= 1);
    OwnerTerminalRegression();
    ListenerOwnerRegression();
    StartupReconnectRegression();
    AkCoreRegression();
    AkAttachRegression();
    AkShutdownRegression();
    return 0;
}

#endif // IDA2_WINDOW_ACCEPTANCE
