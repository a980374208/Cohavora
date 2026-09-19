#if defined(IDA2_WINDOW_ACCEPTANCE)

#include "base/basic_types.h"
#include "crl/crl.h"
#include "rpl/rpl.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/remote_track_publication.h"
#include "src/render/owned_i420_frame.h"
#include "src/ui/meeting_room_window.h"
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
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtPlugin>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
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
    static ScopedSession create(std::unique_ptr<QSettings> settings) {
        return ScopedSession(new SessionManager(std::move(settings)), [](SessionManager *value) { delete value; });
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
    static void establishLocalConnectedPrecondition(Room &room) {
        std::lock_guard lock(room.room_mutex_);
        room.connection_state_ = ConnectionState::Connected;
    }
    static void attach(Room &room, const std::shared_ptr<RemoteParticipant> &participant,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::string &sid) {
        room.AttachRemoteTrackToParticipant(participant, std::move(track), nullptr, sid);
    }
};
} // namespace livekit

class ParticipantWindowTestAccess final {
public:
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
    static void render(MeetingUI::MeetingRoomWindow &window) {
        TEST_CHECK(QThread::currentThread() == window.thread());
        window.onRemoteRenderTick();
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

livekit::proto::ParticipantUpdate WindowParticipant(const std::string &name, bool active = true, bool video = true) {
    livekit::proto::ParticipantUpdate update;
    auto *participant = update.add_participants();
    participant->set_sid("PA_WINDOW"); participant->set_identity("window-peer"); participant->set_name(name);
    participant->set_state(active ? livekit::proto::ParticipantInfo::ACTIVE : livekit::proto::ParticipantInfo::DISCONNECTED);
    participant->mutable_permission()->set_can_subscribe(true);
    participant->mutable_permission()->set_can_publish(true);
    participant->mutable_permission()->set_can_publish_data(true);
    if (active && video) {
        auto *track = participant->add_tracks();
        track->set_sid("TR_PA_WINDOW"); track->set_name("window-video"); track->set_type(livekit::proto::TrackType::VIDEO);
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
    explicit WindowFixture(bool localConnectedPrecondition = true)
        : session(OpenMeeting::SessionManagerTestAccess::create(
              std::make_unique<QSettings>(settingsDirectory.filePath("settings.ini"), QSettings::IniFormat))),
          room(livekit::Room::Create(io.get_executor())),
          runtime(std::make_shared<OpenMeeting::MeetingSessionRuntime>(io, 71, QStringLiteral("local-user"))),
          coordinator(OpenMeeting::MeetingCoordinatorTestAccess::create(*session)),
          observer(std::make_shared<WindowValueObserver>()) {
        TEST_CHECK(settingsDirectory.isValid());
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
    WindowMedia attachExisting(const std::string &rtcId) {
        auto participant = room->remote_participants().at("PA_WINDOW");
        WindowMedia media;
        media.source = webrtc::make_ref_counted<WindowMemoryVideoSource>();
        media.rtc = webrtc::VideoTrack::Create(rtcId, media.source, webrtc::Thread::Current());
        TEST_CHECK(media.rtc);
        livekit::ParticipantSnapshotRoomTestAccess::attach(*room, participant, media.rtc, "TR_PA_WINDOW");
        media.track = participant->get_publication("TR_PA_WINDOW")->track();
        TEST_CHECK(media.track && media.track->rtc_track().get() == media.rtc.get());
        pump();
        return media;
    }
    void open() { window = ParticipantWindowTestAccess::create(coordinator); }
    void open(MeetingUI::MeetingRoomWindow::Config config) {
        window = ParticipantWindowTestAccess::create(coordinator, std::move(config));
    }

    QTemporaryDir settingsDirectory;
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
    bool protocolFailure = false;
    int joins = 0;
    int resumes = 0;
    int rejectedResumes = 0;
    int syncStates = 0;
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
            join->mutable_room()->set_sid("RM_WINDOW_LOOPBACK");
            join->mutable_room()->set_name("window-loopback");
            auto *local = join->mutable_participant();
            local->set_sid("PA_WINDOW_LOCAL"); local->set_identity("local-user");
            local->set_name("local-user"); local->set_state(livekit::proto::ParticipantInfo::ACTIVE);
            local->mutable_permission()->set_can_subscribe(true);
            local->mutable_permission()->set_can_publish(true);
            local->mutable_permission()->set_can_publish_data(true);
            *join->add_other_participants() = WindowParticipant(
                joins == 1 ? "join-window-peer" : "restart-window-peer").participants(0);
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
            if (signal.has_sync_state()) ++syncStates;
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

void WindowConnect(WindowFixture &fixture, const std::shared_ptr<WindowLoopbackServer> &server) {
    const std::string url = "ws://127.0.0.1:" + std::to_string(server->port());
    const std::string token = "ida2-window-local-test-token";
    livekit::SignalOptions options;
    options.allow_insecure_transport = true;
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
    TEST_CHECK(server->joins == 1 && !server->protocolFailure && fixture.observer->connected == 1);
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
    if (application.arguments().contains("--pr-sec-005")) {
        PrSec005InvitationContract();
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
        std::cout << "AK_WINDOW_NETWORK_EXECUTED=3 PASSED=3 FAILED=0" << std::endl;
    } else {
        std::cout << "AK_WINDOW_PLANNED=8 (PR-SEC-005 invitation; F4/F5 local precondition; F1/F2/F3 real loopback)" << std::endl;
        PrSec005InvitationContract();
        AkWindowAliveLate();
        AkWindowOldTrack();
        AkWindowRetiredPresentation(false);
        AkWindowRetiredPresentation(true);
        AkWindowInitialRoster();
        AkWindowSoftResume();
        AkWindowFullRestart();
        std::cout << "AK_WINDOW_EXECUTED=8 PASSED=8 FAILED=0" << std::endl;
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
    static void establishConnectedAttachPrecondition(Room &room) {
        std::lock_guard lock(room.room_mutex_);
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
            target->completeRoomStartupOnUiThread(sessionGeneration, {}, {});
        }, Qt::QueuedConnection);
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
    // The production singleton owns native account settings. This target uses
    // injected temporary SessionManager instances and must never enter that
    // singleton's account-invalidation branch, including proto3's default 0.
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
    fixture.room->OnIncomingDataPacket(SessionOnlyNotifyBytes(notify), "PA_REENTRY", "chat");
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
    AttachFixture()
        : sourceA(webrtc::make_ref_counted<CountingAudioSource>()),
          sourceB(webrtc::make_ref_counted<CountingAudioSource>()),
          rtcA(webrtc::AudioTrack::Create("rtc-attach-A", sourceA)),
          rtcB(webrtc::AudioTrack::Create("rtc-attach-B", sourceB)) {
        TEST_CHECK(rtcA && rtcB);
        // H is a resolved-attach local test. This is NOT a real Connect/F test.
        livekit::ParticipantSnapshotRoomTestAccess::establishConnectedAttachPrecondition(*room);
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
    std::cout << "AK_ATTACH_EXECUTED=7 PASS (resolved-attach local precondition; no F Connect claim)" << std::endl;
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
