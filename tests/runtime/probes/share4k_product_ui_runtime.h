#pragma once

// Compiled only into the existing Qt acceptance fixture, after its test access
// classes. Commands activate real production controls; capture/publication and
// quality snapshots are never injected. Pixels and credentials are not saved.
#include "src/core/local_video_track.h"
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtWidgets/QComboBox>
#include <QtGui/QAccessible>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace share4k_product_ui {

class OwnedWindow final {
public:
    OwnedWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = Procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"CohavoraShare4kOwnedWindow";
        RegisterClassW(&type);
        title_ = L"Cohavora share4k owned animation " + std::to_wstring(GetCurrentProcessId());
        hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, title_.c_str(),
            WS_POPUP, 0, 0, 3840, 2160, nullptr, nullptr, type.hInstance, this);
        if (!hwnd_) throw std::runtime_error("owned_window_create_failed");
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 3840, 2160, SWP_NOACTIVATE);
        SetTimer(hwnd_, 1, 33, nullptr);
        UpdateWindow(hwnd_);
        const auto size = clientSize();
        if (size != QSize(3840, 2160)) {
            DestroyWindow(hwnd_); hwnd_ = nullptr;
            throw std::runtime_error("owned_window_not_actual_4k");
        }
    }
    ~OwnedWindow() { Close(); }
    void Close() {
        if (hwnd_) {
            KillTimer(hwnd_, 1);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_); hwnd_ = nullptr;
        }
    }
    intptr_t id() const { return reinterpret_cast<intptr_t>(hwnd_); }
    QString title() const { return QString::fromStdWString(title_); }
    QSize clientSize() const {
        RECT bounds{};
        return GetClientRect(hwnd_, &bounds) ? QSize(bounds.right, bounds.bottom) : QSize{};
    }
    quint64 paints() const { return paints_; }
private:
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<OwnedWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<OwnedWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_TIMER && self) {
            ++self->sequence_;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT bounds{}; GetClientRect(hwnd, &bounds);
            HBRUSH base = CreateSolidBrush(self->sequence_ % 2 ? RGB(20, 60, 100) : RGB(24, 85, 50));
            FillRect(dc, &bounds, base); DeleteObject(base);
            RECT moving{int(self->sequence_ * 41 % 3760), 400, int(self->sequence_ * 41 % 3760) + 80, 1600};
            FillRect(dc, &moving, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 230, 30));
            const std::wstring label = L"Task-owned 3840 x 2160 WGC source  |  frame " + std::to_wstring(self->sequence_);
            TextOutW(dc, 120, 120, label.c_str(), int(label.size()));
            EndPaint(hwnd, &paint); ++self->paints_;
            return 0;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
    HWND hwnd_ = nullptr;
    std::wstring title_;
    quint64 sequence_ = 0, paints_ = 0;
};

// Production capture intentionally excludes this process's own HWNDs. Keep
// the task-owned animation in a separate fixture process instead of changing
// that production shutdown/deadlock boundary.
inline int RunOwnedWindow(QApplication &application) {
    const auto args = application.arguments();
    const int directoryArgument = args.indexOf("--share4k-directory");
    if (directoryArgument < 0 || directoryArgument + 1 >= args.size()) return 2;
    const QString directory = QFileInfo(args[directoryArgument + 1]).canonicalFilePath();
    if (directory.isEmpty()) return 2;
    QLockFile lock(QDir(directory).filePath("owned.lock"));
    if (!lock.tryLock(0)) return 2;
    application.setQuitOnLastWindowClosed(false);
    OwnedWindow window;
    QElapsedTimer lifetime; lifetime.start();
    quint64 sequence = 0;
    QTimer timer;
    QObject callbackContext;
    timer.setInterval(250);
    const auto publish = [&] {
        return meeting_soak::WriteJson(QDir(directory).filePath("status.json"), {
            {"schema", 1}, {"pid", double(GetCurrentProcessId())},
            {"hwnd", QString::number(qulonglong(window.id()))}, {"title", window.title()},
            {"client_width", window.clientSize().width()}, {"client_height", window.clientSize().height()},
            {"paints", double(window.paints())}, {"heartbeat_seq", double(++sequence)},
            {"elapsed_s", lifetime.elapsed() / 1000.0}, {"utc_ms", double(QDateTime::currentMSecsSinceEpoch())},
            {"source_scope", "task_owned_separate_process_3840x2160_window"}});
    };
    QObject::connect(&timer, &QTimer::timeout, &callbackContext, [&] {
        if (!publish()) application.exit(2);
        else if (QFileInfo::exists(QDir(directory).filePath("owned.stop"))) application.exit(0);
        else if (lifetime.elapsed() > 420000) application.exit(3);
    });
    if (!publish()) return 2;
    timer.start();
    const int result = application.exec(); timer.stop(); window.Close();
    meeting_soak::WriteJson(QDir(directory).filePath("final.json"), {
        {"status", result == 0 ? "PASS" : "FAIL"}, {"exit_code", result}, {"window_closed", true},
        {"paints", double(window.paints())}, {"scope", "task_owned_animation_window_only"}});
    return result;
}

class ExternalWindow final {
public:
    ExternalWindow() {
        bool valid = false;
        const auto value = qEnvironmentVariable("SHARE4K_SOURCE_HWND").toULongLong(&valid);
        if (!valid || value == 0 || value > std::numeric_limits<uintptr_t>::max())
            throw std::runtime_error("owned_external_hwnd_invalid");
        hwnd_ = reinterpret_cast<HWND>(uintptr_t(value));
        title_ = qEnvironmentVariable("SHARE4K_SOURCE_TITLE");
        if (title_.isEmpty() || !GetWindowThreadProcessId(hwnd_, &pid_) || pid_ == GetCurrentProcessId() || !Validate())
            throw std::runtime_error("owned_external_window_invalid");
    }
    intptr_t id() const { return reinterpret_cast<intptr_t>(hwnd_); }
    QString title() const { return title_; }
    DWORD pid() const { return pid_; }
    QSize clientSize() const {
        RECT bounds{};
        return GetClientRect(hwnd_, &bounds) ? QSize(bounds.right, bounds.bottom) : QSize{};
    }
    bool Validate() const {
        DWORD observed = 0;
        const int length = GetWindowTextLengthW(hwnd_);
        if (length <= 0 || length > 512) return false;
        std::vector<wchar_t> text(length + 1);
        GetWindowTextW(hwnd_, text.data(), int(text.size()));
        return IsWindow(hwnd_) && IsWindowVisible(hwnd_) &&
            GetWindowThreadProcessId(hwnd_, &observed) && observed == pid_ && observed != GetCurrentProcessId() &&
            QString::fromWCharArray(text.data()) == title_ && clientSize() == QSize(3840, 2160);
    }
private:
    HWND hwnd_ = nullptr;
    DWORD pid_ = 0;
    QString title_;
};

inline bool Activate(QWidget &owner, const char *id) {
    auto *button = owner.findChild<QPushButton*>(QString::fromLatin1(id));
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    auto *accessible = QAccessible::queryAccessibleInterface(button);
    auto *action = accessible ? accessible->actionInterface() : nullptr;
    if (!action) return false;
    const auto names = action->actionNames();
    const auto name = names.contains(QAccessibleActionInterface::toggleAction())
        ? QAccessibleActionInterface::toggleAction() : QAccessibleActionInterface::pressAction();
    if (!names.contains(name)) return false;
    action->doAction(name);
    return true;
}

inline bool SelectQuality(QDialog &dialog, const char *resolutionName, const char *fpsName,
        livekit::ScreenShareQuality quality) {
    auto *resolution = dialog.findChild<QComboBox*>(QString::fromLatin1(resolutionName));
    auto *fps = dialog.findChild<QComboBox*>(QString::fromLatin1(fpsName));
    if (!resolution || !fps) return false;
    const int index = resolution->findData(static_cast<int>(quality.resolution));
    const int rate = fps->findData(quality.fps);
    if (index < 0 || rate < 0) return false;
    // Exercise the production combo selections and their accepted handlers.
    resolution->setCurrentIndex(index); fps->setCurrentIndex(rate);
    return resolution->currentData().toInt() == int(quality.resolution) && fps->currentData().toInt() == quality.fps;
}

inline int Run(QApplication &application) {
    using namespace OpenMeeting;
    const auto args = application.arguments();
    const int directoryArgument = args.indexOf("--share4k-directory");
    if (directoryArgument < 0 || directoryArgument + 1 >= args.size()) return 2;
    const QString directory = QFileInfo(args[directoryArgument + 1]).canonicalFilePath();
    if (directory.isEmpty()) return 2;
    meeting_soak::Protocol protocol(directory);
    if (!protocol.Open()) return 2;
    const auto url = qEnvironmentVariable("LIVEKIT_URL");
    const auto token = qEnvironmentVariable("LIVEKIT_TOKEN");
    const auto roomName = qEnvironmentVariable("SHARE4K_ROOM");
    if (url.isEmpty() || token.isEmpty() || roomName != protocol.runId()) return 2;
    // Match product main: the desktop toolkit supplies WINRT_IMPL_* imports.
    // Resolve its process-wide function table before capture can be the first
    // WinRT caller on a worker. This import initialization has no Finish API.
    if (!base::WinRT::Supported()) {
        protocol.Complete(false, "winrt_imports_unavailable");
        protocol.Publish({{"state", "failed"}, {"error_code", "winrt_imports_unavailable"},
            {"winrt_imports_resolved", false}});
        return 4;
    }
    application.setQuitOnLastWindowClosed(false);
    initializeServiceEndpointPolicy(qEnvironmentVariable("LIVEKIT_TEST_ALLOW_INSECURE") == "1");
    MeetingUI::AppTranslation::install(application, MeetingUI::AppTranslation::startupLocale(args));
    MeetingUI::AppTheme::install(application);
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    auto session = SessionManagerTestAccess::create(std::make_unique<QSettings>(settings.filePath("share4k.ini"), QSettings::IniFormat));
    auto coordinator = MeetingCoordinatorTestAccess::create(*session);
    ExternalWindow source;
    MeetingUI::MeetingRoomWindow::Config config;
    config.audioMuted = true; config.videoEnabled = false;
    config.displayName = QStringLiteral("4K product UI publisher");
    auto window = ParticipantWindowTestAccess::create(coordinator, std::move(config));
    ParticipantWindowTestAccess::startGpu(*window, true);
    window->show();
    MediaPreferences preferences;
    preferences.enableMicrophone = false; preferences.enableVideo = false;
    preferences.screenShareVideoCodec = QStringLiteral("vp8");
    coordinator->connectDirectlyAsync(url, token, roomName, QStringLiteral("4K product UI publisher"), preferences);
    QFile samples(protocol.Path("samples.jsonl")), events(protocol.Path("events.jsonl"));
    if (!samples.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !events.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    const auto record = [&](QJsonObject value) {
        value.insert("utc_ms", double(QDateTime::currentMSecsSinceEpoch()));
        value.insert("command_seq", double(protocol.commandSeq()));
        events.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n'); events.flush();
    };
    bool failed = false, finishing = false, shutdownComplete = false, pickerSelected = false;
    int ownedIndex = -1, ownedMatches = 0;
    std::function<bool()> pending;
    QElapsedTimer pendingTime, lifetime;
    lifetime.start();
    QString pendingAction;
    livekit::ScreenShareQuality desired;
    std::weak_ptr<livekit::Track> initialTrack;
    std::weak_ptr<livekit::VideoSource> initialSource;
    std::weak_ptr<livekit::render::VideoRenderRouter> initialPreview;
    std::string initialSid, initialRtcId;
    auto callbackContext = std::make_unique<QObject>();
    QObject::connect(coordinator.get(), &MeetingCoordinator::screenShareSourcesReady, callbackContext.get(),
        [&](const std::vector<livekit::DesktopSource> &sources) {
            ownedIndex = -1; ownedMatches = 0;
            for (int i = 0; i < int(sources.size()); ++i)
                if (sources[i].kind == livekit::DesktopSourceKind::Window && sources[i].id == source.id() &&
                    QString::fromStdString(sources[i].title) == source.title()) { ownedIndex = i; ++ownedMatches; }
            record({{"event", "sources_enumerated"}, {"owned_matches", ownedMatches}, {"owned_index", ownedIndex}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::errorOccurred, callbackContext.get(),
        [&](const QString &, const QString &) { failed = true; record({{"event", "coordinator_error"}}); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::sessionShutdownFinished, callbackContext.get(),
        [&] { shutdownComplete = true; if (finishing) application.exit(failed ? 1 : 0); });
    QTimer tick;
    tick.setInterval(250);
    QObject::connect(&tick, &QTimer::timeout, callbackContext.get(), [&] {
        if (!source.Validate()) { failed = true; protocol.Complete(false, "owned_external_window_changed"); }
        const auto snapshot = coordinator->screenShareSnapshot();
        std::shared_ptr<livekit::LocalVideoTrack> track;
        int localShares = 0;
        if (const auto room = coordinator->room()) if (const auto local = room->local_participant())
            for (const auto &[sid, publication] : local->tracks()) {
                const auto value = publication->track();
                if (value && value->Track::source() == livekit::TrackSource::ScreenShareVideo) {
                    ++localShares; track = std::dynamic_pointer_cast<livekit::LocalVideoTrack>(value);
                }
            }
        if (snapshot.state == livekit::ScreenShareState::Active && track && initialSid.empty()) {
            initialSid = snapshot.track_sid; initialTrack = track;
            initialSource = track->source(); initialPreview = snapshot.preview;
            if (track->rtc_track()) initialRtcId = track->rtc_track()->id();
        }
        const auto capture = livekit::ObserveDesktopCapture();
        QJsonObject state{{"state", int(coordinator->state())}, {"share_state", int(snapshot.state)},
            {"winrt_imports_resolved", true},
            {"utc_ms", double(QDateTime::currentMSecsSinceEpoch())}, {"elapsed_s", lifetime.elapsed() / 1000.0},
            {"quality_status", int(snapshot.quality_status)}, {"local_share_tracks", localShares},
            {"track_sid", QString::fromStdString(snapshot.track_sid)},
            {"sid_hash", snapshot.track_sid.empty() ? QString{} : QString::fromLatin1(QCryptographicHash::hash(
                QByteArray::fromStdString(snapshot.track_sid), QCryptographicHash::Sha256).toHex().left(16))},
            {"capture_backend", QString::fromLatin1(capture.backend)},
            {"capture_frames", double(capture.frames)}, {"capture_failures", double(capture.failures)},
            {"source_client_width", source.clientSize().width()}, {"source_client_height", source.clientSize().height()},
            {"owned_window_pid", double(source.pid())}, {"owned_source_matches", ownedMatches},
            {"source_scope", "owned_3840x2160_native_window"}, {"ui_action_backend", "Qt QAccessible buttons and actual QComboBox selection"},
            {"source_frames", track ? double(track->source()->captured_frame_count()) : 0},
            {"preview_frames", snapshot.preview ? double(snapshot.preview->statistics().submitted) : 0},
            {"same_sid", !initialSid.empty() && snapshot.track_sid == initialSid},
            {"same_track", track && track == initialTrack.lock()},
            {"same_rtc_track", track && track->rtc_track() && !initialRtcId.empty() && track->rtc_track()->id() == initialRtcId},
            {"same_source", track && track->source() == initialSource.lock()},
            {"same_preview", snapshot.preview && snapshot.preview == initialPreview.lock()},
            {"exact_capture_instance_identity", "UNKNOWN"}, {"metadata_origin", "production ScreenShareSnapshot after sender/metadata transaction"}};
        if (snapshot.applied_quality) {
            const auto &profile = *snapshot.applied_quality;
            state.insert("resolution", int(profile.quality.resolution)); state.insert("fps", profile.quality.fps);
            state.insert("source_width", profile.source_width); state.insert("source_height", profile.source_height);
            state.insert("applied_width", profile.width); state.insert("applied_height", profile.height);
            state.insert("quality_revision", double(profile.revision));
        }
        if (pending && pending()) {
            pending = {}; protocol.Complete(true); record({{"event", "ui_command_applied"}, {"action", pendingAction}});
        } else if (pending && pendingTime.elapsed() > 60000) {
            pending = {}; protocol.Complete(false, "ui_command_timeout"); failed = true;
            record({{"event", "ui_command_failed"}, {"action", pendingAction}, {"code", "ui_command_timeout"}});
        }
        if (failed || lifetime.elapsed() > 360000) {
            if (!finishing) { failed = true; finishing = true; coordinator->leaveMeetingAsync(false); }
        }
        const auto action = protocol.Poll();
        if (!action.isEmpty()) {
            const auto command = meeting_soak::ReadJson(protocol.Path("command.json"));
            desired = {static_cast<livekit::ScreenShareResolution>(command.value("resolution").toInt(-1)), command.value("fps").toInt(-1)};
            pendingAction = action; pendingTime.restart();
            record({{"event", "ui_command_requested"}, {"action", action}});
            const auto applied = [&] {
                const auto value = coordinator->screenShareSnapshot();
                return value.state == livekit::ScreenShareState::Active && value.applied_quality &&
                    value.applied_quality->quality == desired && value.quality_status == livekit::ScreenShareQualityStatus::Applied;
            };
            if (action == "ui_start" && desired.valid() && coordinator->state() == MeetingState::InMeeting &&
                snapshot.state == livekit::ScreenShareState::Idle && Activate(*window, "meetingShareScreen")) {
                ownedIndex = -1; ownedMatches = 0; pickerSelected = false;
                pending = [&, applied] {
                    if (pickerSelected) return applied();
                    auto *picker = window->findChild<QDialog*>("screen-share-picker");
                    if (!picker || ownedMatches != 1 || ownedIndex < 0) return false;
                    auto *combo = picker->findChild<QComboBox*>("screenShareSource");
                    if (!combo || ownedIndex >= combo->count() || !combo->itemText(ownedIndex).contains(source.title())) return false;
                    combo->setCurrentIndex(ownedIndex);
                    if (!SelectQuality(*picker, "screenShareStartResolution", "screenShareStartFps", desired)) return false;
                    record({{"event", "picker_owned_source_selected"}, {"width", 3840}, {"height", 2160},
                        {"resolution", int(desired.resolution)}, {"fps", desired.fps}});
                    pickerSelected = Activate(*picker, "screenShareAccept");
                    return false;
                };
            } else if (action == "ui_switch_quality" && desired.valid() && snapshot.state == livekit::ScreenShareState::Active &&
                Activate(*window, "screenShareQuality")) {
                pickerSelected = false;
                pending = [&, applied] {
                    if (pickerSelected) return applied();
                    auto *dialog = window->findChild<QDialog*>("screenShareQualityDialog");
                    if (!dialog || !SelectQuality(*dialog, "activeScreenShareResolution", "activeScreenShareFps", desired)) return false;
                    pickerSelected = Activate(*dialog, "screenShareQualityApply"); return false;
                };
            } else if (action == "ui_stop" && snapshot.state == livekit::ScreenShareState::Active && Activate(*window, "meetingShareScreen")) {
                pending = [&] {
                    if (coordinator->screenShareSnapshot().state != livekit::ScreenShareState::Idle) return false;
                    if (const auto room = coordinator->room()) if (const auto local = room->local_participant())
                        for (const auto &[sid, publication] : local->tracks())
                            if (const auto track = publication->track(); track && track->source() == livekit::TrackSource::ScreenShareVideo) return false;
                    return true;
                };
            } else if (action == "stop" || action == "leave") {
                pending = {}; protocol.Complete(true);
                if (!finishing) {
                    finishing = true;
                    record({{"event", "leave_requested"}}); coordinator->leaveMeetingAsync(false);
                }
            } else { failed = true; protocol.Complete(false, "ui_command_rejected"); }
        }
        if (!protocol.Publish(state)) { failed = true; application.exit(2); }
        state.insert("utc_ms", double(QDateTime::currentMSecsSinceEpoch()));
        state.insert("command_seq", double(protocol.commandSeq())); state.insert("command_status", protocol.commandStatus());
        samples.write(QJsonDocument(state).toJson(QJsonDocument::Compact) + '\n'); samples.flush();
        if (finishing && shutdownComplete && !coordinator->room()) application.exit(failed ? 1 : 0);
    });
    tick.start();
    QTimer::singleShot(380000, callbackContext.get(), [&] { application.exit(3); });
    const int result = application.exec();
    tick.stop(); pending = {}; callbackContext.reset();
    record({{"event", "teardown_callbacks_detached"}});
    const auto capture = livekit::ObserveDesktopCapture();
    const bool retired = shutdownComplete && !coordinator->room();
    window.reset(); record({{"event", "teardown_window_released"}});
    coordinator.reset(); record({{"event", "teardown_coordinator_released"}});
    livekit::WebRTCManager::Instance().Deinitialize(); record({{"event", "teardown_webrtc_deinitialized"}});
    meeting_soak::WriteJson(protocol.Path("final.json"), {{"status", result == 0 ? "PASS" : "FAIL"},
        {"exit_code", result}, {"room_retired", retired}, {"shutdown_acknowledged", shutdownComplete},
        {"callback_context_detached", true}, {"webrtc_deinitialized", true}, {"winrt_imports_resolved", true},
        {"capture_frames", double(capture.frames)}, {"scope", "product UI operation chain only"}});
    return result;
}
} // namespace share4k_product_ui
