#include "src/ui/meeting_main_window.h"
#include "src/ui/meeting_detail_dialog.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/ui/meeting_encryption_dialog.h"
#include "src/ui/meeting_encryption_panel.h"
#include "tests/runtime/probes/e2ee_product_runtime.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_ui_integration.h"
#include "tests/support/test_check.h"
#include "crl/crl.h"
#include "rpl/never.h"
#include "ui/style/style_core.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QMessageBox>
#include <QtPlugin>
#include <cstdio>
#include "src/ui/app_icons.h"
#include <QtGui/QPainter>
#include <QtGui/QAccessible>
#include <QtGui/QKeyEvent>

class MeasuredMainWindow final : public MeetingUI::MeetingMainWindow {
protected:
    void showEvent(QShowEvent *event) override {
        QElapsedTimer timer;
        timer.start();
        MeetingUI::MeetingMainWindow::showEvent(event);
        std::printf("main_native_setup ms=%.3f\n", timer.nsecsElapsed() / 1e6);
    }
};

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)
namespace crl { rpl::producer<> on_main_update_requests() { return rpl::never<>(); } }

class PaintProbe final : public QObject {
public:
    bool completed = false;
    bool scheduled = false;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint && !scheduled) {
            scheduled = true;
            QTimer::singleShot(0, this, [this] { completed = true; });
        }
        return false;
    }
};

// Retain the production entry/preparation/admission control flow while
// preventing any device or service startup through existing test friends.
class CameraOwnerTestAccess final {
public:
    static std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator(MeetingUI::MeetingRoomWindow& window) {
        return window._coordinator;
    }
    static MeetingUI::MeetingRoomWindow::Config config(const MeetingUI::MeetingRoomWindow& window) {
        return window._config;
    }
};
namespace OpenMeeting {
class MeetingCoordinatorTestAccess final {
public:
    static void interceptAdmission(MeetingCoordinator& coordinator,
            std::function<void(const QString&, const QString&)> joined, bool quickMeeting) {
        coordinator._admissionBackend = {};
        coordinator._admissionBackend.joinMeeting =
            [joined, quickMeeting](const QString& id, const QString& password, ResultCallback<bool>) {
                TEST_CHECK(!quickMeeting);
                joined(id, password); // Pending admission never creates a native Room.
            };
        coordinator._admissionBackend.leaveMeeting = [](const QString& id, ResultCallback<bool> callback) {
            TEST_CHECK(!id.isEmpty());
            callback(true, true, {});
        };
        coordinator._admissionBackend.endMeeting = coordinator._admissionBackend.leaveMeeting;
        coordinator._admissionBackend.getMeetingToken = [](const QString&, ResultCallback<LiveKitAuthInfo>) {
            TEST_CHECK(false && "pending detail admission must not fetch a token");
        };
        coordinator._admissionBackend.createImmediateMeeting = [joined, quickMeeting](const QString& title, int duration, ResultCallback<LiveKitAuthInfo>) {
            TEST_CHECK(quickMeeting && duration == 3600);
            joined(title, {}); // Keep quick admission pending before native Room startup.
        };
    }
    static const MediaPreferences& preferences(const MeetingCoordinator& coordinator) { return coordinator._mediaPrefs; }
    static std::shared_ptr<livekit::MeetingSecretHandle> secret(const MeetingCoordinator& coordinator) {
        return coordinator._admissionEncryption.secret;
    }
};
}

namespace MeetingUI {
// Only inject the account/credential/detail preconditions. The actual main
// window entry and join-dialog control flow remains production code.
class MeetingEntryEncryptionTestAccess final {
public:
    static void enter(MeetingMainWindow& window, const QString& route) {
        if (route == "detail") { window.showMeetingDetail("public-entry-fixture"); return; }
        auto reservation = window._meetingEntryGuard.tryAcquire();
        TEST_CHECK(reservation);
        if (route == "quick" || route == "quick-share")
            window.openQuickMeeting(std::move(reservation), route == "quick-share");
        else window.openJoinMeetingDialog(std::move(reservation), "public-entry-fixture", {}, route == "join-share");
    }
    static bool reservationReleased(MeetingMainWindow& window) {
        return bool(window._meetingEntryGuard.tryAcquire());
    }
    static void credentialsReady(JoinMeetingDialog& dialog, bool direct) {
        dialog._resolvedServerUrl = "ws://127.0.0.1:9";
        dialog._resolvedToken = "public-entry-test-token";
        dialog._isManualConnection = direct;
        dialog.accept();
    }
    static void detailReady(MeetingDetailDialog& dialog) {
        dialog.requestJoin();
        TEST_CHECK(dialog.result() == QDialog::Accepted);
    }
    static void catalog(MeetingMainWindow& window, OpenMeeting::MeetingCatalogController::Backend backend) {
        delete window._meetingCatalog;
        window._meetingCatalog = new OpenMeeting::MeetingCatalogController(
            OpenMeeting::SessionManager::instance(), std::move(backend), &window);
        QObject::connect(window._meetingCatalog, &OpenMeeting::MeetingCatalogController::detailChanged,
            &window, [&window] { window.handlePendingMeetingEntryDetail(); });
        QObject::connect(window._meetingCatalog, &OpenMeeting::MeetingCatalogController::upcomingChanged,
            &window, [&window] { window.syncSchedule(); });
    }
    static OpenMeeting::MeetingCatalogController& catalog(MeetingMainWindow& window) { return *window._meetingCatalog; }
    static std::unique_ptr<QObject> reserve(MeetingMainWindow& window) { return window._meetingEntryGuard.tryAcquire(); }
    static void detailJoin(MeetingDetailDialog& dialog) { dialog.requestJoin(); }
    static bool detailJoinEnabled(const MeetingDetailDialog& dialog) { return dialog._joinButton->isEnabled(); }
    static void join(JoinMeetingDialog& dialog) { dialog.onJoinClicked(); }
    static bool loading(const JoinMeetingDialog& dialog) { return dialog._isLoading; }
    static void loading(JoinMeetingDialog& dialog, bool value) { dialog.setLoading(value); }
    static std::shared_ptr<livekit::MeetingSecretHandle> retainPreparedSecret(JoinMeetingDialog& dialog) {
        TEST_CHECK(dialog.prepareEncryptionRequest());
        return dialog._encryptionRequest->secret;
    }
};
}

void TestEncryptionEntryCancellation(QApplication& app) {
    OpenMeeting::initializeServiceEndpointPolicy(true);
    auto& session = OpenMeeting::SessionManager::instance();
    TEST_CHECK(session.setServerBaseUrl("http://127.0.0.1:9"));
    session.loginAsGuest("Public entry fixture", "public-entry-fixture");
    TEST_CHECK(session.isLoggedIn());
    session.setMeetingSecurityPreferences({true, true, true, false});
    session.clearMeetingEncryptionKey();
    MeetingUI::MeetingMainWindow window;
    for (const auto& route : {"ordinary", "direct", "join-share"}) {
        for (int attempt = 0; attempt != 2; ++attempt) {
            std::fprintf(stderr, "E2EE_ENTRY_CANCEL route=%s attempt=%d BEGIN\n", route, attempt + 1);
            std::fflush(stderr);
            int prompts = 0;
            int joins = 0;
            int keyErrors = 0;
            QTimer driver;
            driver.setInterval(0);
            QObject::connect(&driver, &QTimer::timeout, &window, [&] {
                auto* modal = app.activeModalWidget();
                if (auto* encryption = dynamic_cast<MeetingUI::MeetingEncryptionDialog*>(modal)) {
                    ++prompts;
                    encryption->reject();
                } else if (auto* join = dynamic_cast<MeetingUI::JoinMeetingDialog*>(modal)) {
                    ++joins;
                    auto* toggle = join->findChild<QPushButton*>("joinEncryptionToggle");
                    auto* required = join->findChild<QCheckBox*>("e2eeRequired");
                    auto* input = join->findChild<QLineEdit*>("e2eeKeyInput");
                    TEST_CHECK(toggle && required && input && toggle->isEnabled());
                    toggle->click();
                    required->setChecked(true);
                    input->setText("public-cancelled-entry-key");
                    const auto retained = MeetingUI::MeetingEntryEncryptionTestAccess::retainPreparedSecret(*join);
                    join->reject();
                    TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable());
                    TEST_CHECK(retained && !retained->available() && !join->takeEncryptionRequest());
                } else if (auto* error = qobject_cast<QMessageBox*>(modal)) {
                    ++keyErrors;
                    error->reject();
                }
            });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &window, [] { TEST_CHECK(false && "entry dialog timeout"); });
            deadline.start(10000);
            driver.start();
            MeetingUI::MeetingEntryEncryptionTestAccess::enter(window, QString::fromLatin1(route));
            driver.stop(); deadline.stop();
            TEST_CHECK(prompts == 0 && joins == 1 && keyErrors == 0);
            TEST_CHECK(MeetingUI::MeetingEntryEncryptionTestAccess::reservationReleased(window));
            for (auto* top : app.topLevelWidgets())
                TEST_CHECK(!dynamic_cast<MeetingUI::MeetingRoomWindow*>(top));
            std::printf("E2EE_ENTRY_CANCEL route=%s attempt=%d PASS\n", route, attempt + 1);
        }
    }
    session.setMeetingSecurityPreferences({});
    session.logout(false);
}

class EntryAdmissionObserver final : public QObject {
public:
    bool quickMeeting = false;
    int windows = 0;
    int admissions = 0;
    QString id;
    QString password;
    MeetingUI::MeetingRoomWindow::Config config;
    QPointer<MeetingUI::MeetingRoomWindow> window;
    std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator;
    std::function<void()> onShow;
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* room = dynamic_cast<MeetingUI::MeetingRoomWindow*>(object);
        if (room && event->type() == QEvent::Show && !window) {
            ++windows;
            window = room;
            config = CameraOwnerTestAccess::config(*room);
            coordinator = CameraOwnerTestAccess::coordinator(*room);
            OpenMeeting::MeetingCoordinatorTestAccess::interceptAdmission(*coordinator,
                [this](const QString& joinedId, const QString& joinedPassword) {
                    ++admissions; id = joinedId; password = joinedPassword;
                }, quickMeeting);
            if (onShow) onShow();
        }
        return false;
    }
};

void TestEntryDefaultsWithoutKey(QApplication& app) {
    using namespace MeetingUI;
    using namespace OpenMeeting;
    auto& session = SessionManager::instance();
    session.loginAsGuest("Public default fixture", "public-default-fixture");
    session.setMeetingSecurityPreferences({true, true, true, true});
    session.clearMeetingEncryptionKey();
    auto preferences = session.mediaPreferences();
    preferences.enableMicrophone = false;
    preferences.enableVideo = false;
    session.setMediaPreferences(preferences);
    const auto spin = [&](const std::function<bool()>& complete) {
        QElapsedTimer timer; timer.start();
        while (!complete() && timer.elapsed() < 3000) {
            app.processEvents(QEventLoop::AllEvents, 5);
            app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(1);
        }
        TEST_CHECK(complete());
    };
    for (const auto* route : {"quick", "quick-share", "detail"}) {
        for (int attempt = 0; attempt != 2; ++attempt) {
            std::fprintf(stderr, "E2EE_ENTRY_NO_DEFAULT_KEY route=%s attempt=%d BEGIN\n", route, attempt + 1);
            std::fflush(stderr);
            MeetingMainWindow window;
            MeetingCatalogController::Backend backend;
            backend.getMeetingInfo = [](const QString& id, ResultCallback<MeetingCatalogDetail> callback) {
                MeetingCatalogDetail detail;
                detail.record.meetingId = id;
                detail.record.status = MeetingStatus::Scheduled;
                callback(true, detail, {});
            };
            MeetingEntryEncryptionTestAccess::catalog(window, std::move(backend));
            EntryAdmissionObserver observer;
            observer.quickMeeting = QString::fromLatin1(route) != "detail";
            app.installEventFilter(&observer);
            QTimer driver;
            driver.setInterval(0);
            QObject::connect(&driver, &QTimer::timeout, &window, [&] {
                if (auto* detail = dynamic_cast<MeetingDetailDialog*>(app.activeModalWidget())) {
                    MeetingEntryEncryptionTestAccess::detailReady(*detail);
                } else TEST_CHECK(!app.activeModalWidget()); // No key prompt or second Join dialog.
            });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &window, [] { TEST_CHECK(false && "default entry timeout"); });
            deadline.start(10000);
            driver.start();
            MeetingEntryEncryptionTestAccess::enter(window, QString::fromLatin1(route));
            spin([&] { return observer.admissions == 1; });
            driver.stop(); deadline.stop();
            TEST_CHECK(observer.windows == 1 && observer.window);
            TEST_CHECK(observer.config.audioMuted && !observer.config.videoEnabled);
            TEST_CHECK(!observer.coordinator->requiresEncryption());
            TEST_CHECK(!MeetingCoordinatorTestAccess::secret(*observer.coordinator));
            TEST_CHECK(!session.hasMeetingEncryptionKey());
            TEST_CHECK(!MeetingEntryEncryptionTestAccess::reservationReleased(window));
            observer.window->close();
            bool drained = false;
            SessionShutdownService::Instance().DrainAsync([&] { drained = true; });
            spin([&] { return drained && !observer.window; });
            observer.coordinator.reset();
            app.removeEventFilter(&observer);
            TEST_CHECK(MeetingEntryEncryptionTestAccess::reservationReleased(window));
            std::printf("E2EE_ENTRY_NO_DEFAULT_KEY route=%s attempt=%d PASS\n", route, attempt + 1);
        }
    }
    session.setMeetingSecurityPreferences({});
    session.logout(false);
}

void TestDetailDirectAdmission(QApplication& app) {
    using namespace MeetingUI;
    using namespace OpenMeeting;
    auto& session = SessionManager::instance();
    const QByteArray marker("public-detail-session-key");
    const auto spin = [&](const std::function<bool()>& complete) {
        QElapsedTimer timer; timer.start();
        while (!complete() && timer.elapsed() < 3000) {
            app.processEvents(QEventLoop::AllEvents, 5);
            app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(1);
        }
        TEST_CHECK(complete());
    };
    for (const auto& name : {"off", "required", "all", "missing", "busy", "refreshing",
            "refresh-error", "wrong-id", "completed", "auth-reset", "auth-preparation"}) {
        std::fprintf(stderr, "E2EE_DETAIL_DIRECT_ADMISSION case=%s BEGIN\n", name);
        std::fflush(stderr);
        const QString scenario = QString::fromLatin1(name);
        session.loginAsGuest("Public detail fixture", "public-detail-fixture");
        auto preferences = session.mediaPreferences();
        preferences.enableMicrophone = true;
        preferences.enableVideo = true;
        preferences.cameraVideoCodec = "auto";
        preferences.screenShareVideoCodec = "auto";
        session.setMediaPreferences(preferences);
        session.setMeetingSecurityPreferences({true, true, scenario == "required" || scenario == "missing", scenario == "all"});
        session.setMeetingEncryptionKey({marker.begin(), marker.end()});
        if (scenario == "missing") session.clearMeetingEncryptionKey();

        MeetingCatalogDetail detail;
        detail.record.meetingId = "public-entry-fixture";
        detail.record.status = scenario == "completed" ? MeetingStatus::Completed : MeetingStatus::Scheduled;
        detail.record.settings.disableMicrophoneOnJoin = true;
        detail.record.settings.disableCameraOnJoin = true;
        detail.password = "public-detail-password";
        MeetingMainWindow window;
        int queries = 0;
        MeetingCatalogController::Backend backend;
        backend.getMeetings = [](const std::vector<MeetingStatus>&, ResultCallback<MeetingList> callback) {
            callback(true, {}, {});
        };
        backend.getMeetingInfo = [&](const QString&, ResultCallback<MeetingCatalogDetail> callback) {
            ++queries;
            if (queries > 1 && scenario == "refreshing") return;
            if (queries > 1 && scenario == "refresh-error") {
                HttpError error; error.code = 1; error.message = "public-refresh-error";
                callback(false, {}, error);
                return;
            }
            auto response = detail;
            if (scenario == "wrong-id") response.record.meetingId = "public-other-meeting";
            callback(true, response, {});
        };
        MeetingEntryEncryptionTestAccess::catalog(window, std::move(backend));
        auto busyReservation = scenario == "busy" ? MeetingEntryEncryptionTestAccess::reserve(window) : nullptr;
        EntryAdmissionObserver observer;
        if (scenario == "auth-preparation") observer.onShow = [&] {
            session.loginAsGuest("Public replacement fixture", "public-replacement-fixture");
        };
        app.installEventFilter(&observer);
        int joinDialogs = 0;
        int errors = 0;
        QTimer driver;
        driver.setInterval(0);
        QObject::connect(&driver, &QTimer::timeout, &window, [&] {
            auto* modal = app.activeModalWidget();
            if (auto* dialog = dynamic_cast<MeetingDetailDialog*>(modal)) {
                const bool cannotJoin = scenario == "wrong-id" || scenario == "completed";
                TEST_CHECK(MeetingEntryEncryptionTestAccess::detailJoinEnabled(*dialog) != cannotJoin);
                MeetingEntryEncryptionTestAccess::detailJoin(*dialog);
                if (cannotJoin) {
                    TEST_CHECK(dialog->result() != QDialog::Accepted);
                    dialog->reject();
                } else {
                    TEST_CHECK(dialog->result() == QDialog::Accepted);
                    if (scenario == "refreshing" || scenario == "refresh-error")
                        MeetingEntryEncryptionTestAccess::catalog(window).loadMeetingDetail(detail.record.meetingId);
                    if (scenario == "auth-reset")
                        session.loginAsGuest("Public replacement fixture", "public-replacement-fixture");
                }
            } else if (auto* join = dynamic_cast<JoinMeetingDialog*>(modal)) {
                ++joinDialogs; join->reject();
            } else if (auto* error = qobject_cast<QMessageBox*>(modal)) {
                ++errors;
                TEST_CHECK(scenario == "busy");
                error->reject();
            }
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &window, [] { TEST_CHECK(false && "detail dialog timeout"); });
        deadline.start(10000);
        driver.start();
        MeetingEntryEncryptionTestAccess::enter(window, "detail");
        driver.stop(); deadline.stop();
        TEST_CHECK(joinDialogs == 0 && errors == (scenario == "busy" ? 1 : 0));
        const bool admitted = scenario == "off" || scenario == "required" || scenario == "all" || scenario == "missing";
        if (admitted || scenario == "auth-preparation") {
            TEST_CHECK(observer.windows == 1);
            TEST_CHECK(observer.config.meetingId == detail.record.meetingId && observer.config.displayName == "Public detail fixture");
            TEST_CHECK(observer.config.audioMuted && !observer.config.videoEnabled);
            TEST_CHECK(observer.config.invitationMode == InvitationMode::BusinessMeetingId);
            if (admitted) {
                spin([&] { return observer.admissions == 1; });
                TEST_CHECK(observer.id == detail.record.meetingId && observer.password == detail.password);
                TEST_CHECK(observer.coordinator->currentDisplayName() == "Public detail fixture");
                TEST_CHECK(!MeetingCoordinatorTestAccess::preferences(*observer.coordinator).enableMicrophone);
                TEST_CHECK(!MeetingCoordinatorTestAccess::preferences(*observer.coordinator).enableVideo);
                const bool required = scenario == "required" || scenario == "all";
                TEST_CHECK(observer.coordinator->requiresEncryption() == required);
                const auto secret = MeetingCoordinatorTestAccess::secret(*observer.coordinator);
                TEST_CHECK(required ? secret && secret->available() : !secret);
                TEST_CHECK(session.hasMeetingEncryptionKey() == (scenario != "missing"));
                TEST_CHECK(!MeetingEntryEncryptionTestAccess::reservationReleased(window));
                TEST_CHECK(session.mediaPreferences().enableMicrophone && session.mediaPreferences().enableVideo);
            } else {
                spin([&] { return !observer.window; });
                TEST_CHECK(observer.admissions == 0);
            }
            if (observer.window) observer.window->close();
        } else TEST_CHECK(observer.windows == 0 && observer.admissions == 0);
        busyReservation.reset();
        bool drained = false;
        SessionShutdownService::Instance().DrainAsync([&] { drained = true; });
        spin([&] { return drained && !observer.window; });
        observer.coordinator.reset();
        app.removeEventFilter(&observer);
        TEST_CHECK(MeetingEntryEncryptionTestAccess::reservationReleased(window));
        std::printf("E2EE_DETAIL_DIRECT_ADMISSION case=%s PASS\n", name);
    }
    session.clearMeetingEncryptionKey();
    session.setMeetingSecurityPreferences({});
    session.logout(false);
}

void TestJoinEncryptionSettings(QApplication& app) {
    using namespace MeetingUI;
    using namespace livekit;
    auto& session = OpenMeeting::SessionManager::instance();
    session.setMeetingSecurityPreferences({false, false, false, true});
    session.clearMeetingEncryptionKey();
    {
        JoinMeetingDialog dialog;
        dialog.show(); app.processEvents();
        auto* toggle = dialog.findChild<QPushButton*>("joinEncryptionToggle");
        auto* pane = dialog.findChild<QWidget*>("joinEncryptionWidget");
        auto* required = dialog.findChild<QCheckBox*>("e2eeRequired");
        auto* input = dialog.findChild<QLineEdit*>("e2eeKeyInput");
        auto* error = dialog.findChild<QLabel*>("e2eeInputError");
        TEST_CHECK(toggle && pane && required && input && error);
        TEST_CHECK(toggle->isEnabled() && pane->isHidden() && !required->isChecked());
        TEST_CHECK(input->echoMode() == QLineEdit::Password && !input->isEnabled());
        toggle->click();
        TEST_CHECK(!pane->isHidden());
        required->setChecked(true);
        dialog.findChild<QLineEdit*>("joinMeetingId")->setText("847123456");
        // Invalid material must be rejected before HTTP loading/admission.
        for (const auto& invalid : {QString(), QString::fromUtf8("中文密钥"), QString(4097, QLatin1Char('A'))}) {
            input->setText(invalid);
            MeetingEntryEncryptionTestAccess::join(dialog);
            TEST_CHECK(!MeetingEntryEncryptionTestAccess::loading(dialog));
            TEST_CHECK(dialog.result() != QDialog::Accepted && !dialog.takeEncryptionRequest());
            TEST_CHECK(!error->text().isEmpty() && input->isEnabled());
        }
        dialog.findChild<QPushButton*>("linkBtn")->click();
        dialog.findChild<QLineEdit*>("joinServerUrl")->setText("ws://127.0.0.1:9");
        dialog.findChild<QLineEdit*>("joinToken")->setText("public-join-encryption-token");
        input->clear();
        MeetingEntryEncryptionTestAccess::join(dialog);
        TEST_CHECK(dialog.result() != QDialog::Accepted && !dialog.takeEncryptionRequest());
        input->setText("public-join-encryption-key");
        auto* accessible = QAccessible::queryAccessibleInterface(input);
        TEST_CHECK(accessible && accessible->state().passwordEdit);
        TEST_CHECK(!accessible->text(QAccessible::Value).contains("public-join-encryption-key"));
        MeetingEntryEncryptionTestAccess::join(dialog);
        auto request = dialog.takeEncryptionRequest();
        TEST_CHECK(dialog.result() == QDialog::Accepted && dialog.isManualConnection());
        TEST_CHECK(request && request->mode == MeetingEncryptionMode::Required && request->secret->available());
        TEST_CHECK(!dialog.takeEncryptionRequest() && input->text().isEmpty() && !input->isUndoAvailable());
        request->Revoke();
        TEST_CHECK(!session.hasMeetingEncryptionKey()); // A one-meeting key is not a default.
    }
    {
        JoinMeetingDialog cancelled;
        auto* required = cancelled.findChild<QCheckBox*>("e2eeRequired");
        auto* input = cancelled.findChild<QLineEdit*>("e2eeKeyInput");
        required->setChecked(true);
        input->setText("public-join-cancelled-key");
        required->setChecked(false);
        TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable() && !input->isEnabled());
        required->setChecked(true);
        input->setText("public-join-cancelled-key");
        const auto retained = MeetingEntryEncryptionTestAccess::retainPreparedSecret(cancelled);
        cancelled.reject();
        TEST_CHECK(retained && !retained->available() && !cancelled.takeEncryptionRequest());
        TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable());
    }
    session.setMeetingSecurityPreferences({false, false, false, true});
    {
        JoinMeetingDialog missing;
        missing.show(); app.processEvents();
        auto* toggle = missing.findChild<QPushButton*>("joinEncryptionToggle");
        auto* required = missing.findChild<QCheckBox*>("e2eeRequired");
        auto* error = missing.findChild<QLabel*>("e2eeInputError");
        TEST_CHECK(toggle->isEnabled() && !required->isChecked() && required->isEnabled());
        TEST_CHECK(error->isHidden() && error->text().isEmpty());
        missing.findChild<QLineEdit*>("joinMeetingId")->setText("847123456");
        missing.findChild<QPushButton*>("linkBtn")->click();
        missing.findChild<QLineEdit*>("joinServerUrl")->setText("ws://127.0.0.1:9");
        missing.findChild<QLineEdit*>("joinToken")->setText("public-default-off-token");
        MeetingEntryEncryptionTestAccess::join(missing);
        TEST_CHECK(!MeetingEntryEncryptionTestAccess::loading(missing));
        auto off = missing.takeEncryptionRequest();
        TEST_CHECK(missing.result() == QDialog::Accepted && missing.isManualConnection());
        TEST_CHECK(off && off->mode == MeetingEncryptionMode::Off && !off->secret);
    }
    const QByteArray marker("public-global-encryption-key");
    {
        JoinMeetingDialog projected;
        auto* toggle = projected.findChild<QPushButton*>("joinEncryptionToggle");
        auto* required = projected.findChild<QCheckBox*>("e2eeRequired");
        auto* input = projected.findChild<QLineEdit*>("e2eeKeyInput");
        auto* error = projected.findChild<QLabel*>("e2eeInputError");
        TEST_CHECK(toggle->isEnabled() && !required->isChecked() && error->isHidden());
        required->setChecked(true);
        input->setText("public-uncommitted-key-editor");
        session.setMeetingEncryptionKey({marker.begin(), marker.end()});
        TEST_CHECK(!toggle->isEnabled() && required->isChecked() && !required->isEnabled());
        TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable() && !input->isEnabled());
        const auto retained = MeetingEntryEncryptionTestAccess::retainPreparedSecret(projected);
        TEST_CHECK(retained && retained->available());
        session.clearMeetingEncryptionKey();
        TEST_CHECK(!retained->available() && !projected.takeEncryptionRequest());
        TEST_CHECK(toggle->isEnabled() && !required->isChecked() && required->isEnabled());
        TEST_CHECK(input->text().isEmpty() && error->isHidden() && error->text().isEmpty());
        projected.accept();
        auto off = projected.takeEncryptionRequest();
        TEST_CHECK(projected.result() == QDialog::Accepted && off && off->mode == MeetingEncryptionMode::Off && !off->secret);
    }
    session.setMeetingEncryptionKey({marker.begin(), marker.end()});
    std::optional<MeetingEncryptionRequest> accepted;
    {
        JoinMeetingDialog global;
        auto* toggle = global.findChild<QPushButton*>("joinEncryptionToggle");
        auto* required = global.findChild<QCheckBox*>("e2eeRequired");
        auto* input = global.findChild<QLineEdit*>("e2eeKeyInput");
        TEST_CHECK(!toggle->isEnabled() && required->isChecked() && !required->isEnabled());
        TEST_CHECK(!input->isEnabled() && input->text().isEmpty());
        global.accept();
        accepted = global.takeEncryptionRequest();
        TEST_CHECK(accepted && accepted->mode == MeetingEncryptionMode::Required && accepted->secret->available());
        TEST_CHECK(!global.takeEncryptionRequest());
    }
    TEST_CHECK(accepted->secret->available()); // Taken request survives dialog destruction.
    accepted->Revoke();
    TEST_CHECK(session.hasMeetingEncryptionKey());
    {
        JoinMeetingDialog global;
        const auto retained = MeetingEntryEncryptionTestAccess::retainPreparedSecret(global);
        global.reject();
        TEST_CHECK(retained && !retained->available() && !global.takeEncryptionRequest());
        TEST_CHECK(session.hasMeetingEncryptionKey());
    }
    {
        JoinMeetingDialog updated;
        TEST_CHECK(!updated.findChild<QPushButton*>("joinEncryptionToggle")->isEnabled());
        session.setMeetingSecurityPreferences({true, false, false, false});
        TEST_CHECK(updated.findChild<QPushButton*>("joinEncryptionToggle")->isEnabled());
        TEST_CHECK(!updated.findChild<QCheckBox*>("e2eeRequired")->isChecked());
        updated.accept();
        auto off = updated.takeEncryptionRequest();
        TEST_CHECK(off && off->mode == MeetingEncryptionMode::Off && !off->secret);
    }
    {
        JoinMeetingDialog pending;
        TEST_CHECK(!MeetingEntryEncryptionTestAccess::retainPreparedSecret(pending));
        MeetingEntryEncryptionTestAccess::loading(pending, true);
        session.setMeetingSecurityPreferences({false, false, false, true});
        TEST_CHECK(!pending.findChild<QPushButton*>("joinEncryptionToggle")->isEnabled());
        pending.accept(); // Admission completes after the policy changed.
        auto required = pending.takeEncryptionRequest();
        TEST_CHECK(required && required->mode == MeetingEncryptionMode::Required && required->secret->available());
        required->Revoke();
    }
    session.setMeetingSecurityPreferences({true, false, false, false});
    {
        JoinMeetingDialog pending;
        TEST_CHECK(!MeetingEntryEncryptionTestAccess::retainPreparedSecret(pending));
        MeetingEntryEncryptionTestAccess::loading(pending, true);
        session.setMeetingSecurityPreferences({false, false, false, true});
        session.clearMeetingEncryptionKey();
        pending.accept();
        auto off = pending.takeEncryptionRequest();
        TEST_CHECK(pending.result() == QDialog::Accepted && off && off->mode == MeetingEncryptionMode::Off && !off->secret);
        TEST_CHECK(pending.findChild<QLabel*>("e2eeInputError")->isHidden());
    }
    session.clearMeetingEncryptionKey();
    session.setMeetingSecurityPreferences({});
    {
        session.loginAsGuest("Public reset fixture", "public-reset-fixture");
        JoinMeetingDialog reset;
        auto* input = reset.findChild<QLineEdit*>("e2eeKeyInput");
        reset.findChild<QCheckBox*>("e2eeRequired")->setChecked(true);
        input->setText("public-reset-uncommitted-key");
        const auto retained = MeetingEntryEncryptionTestAccess::retainPreparedSecret(reset);
        session.logout(false);
        TEST_CHECK(reset.result() == QDialog::Rejected && input->text().isEmpty() && !input->isUndoAvailable());
        TEST_CHECK(retained && !retained->available() && !reset.takeEncryptionRequest());
    }
    std::puts("E2EE_JOIN_SETTINGS_INPUT_GLOBAL_POLICY_AND_CANCELLATION PASS");
}

void SendEncryptionKey(QWidget* widget, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QApplication::sendEvent(widget, &release);
}

void TestEncryptionKeyboard(QApplication& app) {
    const auto tr = [](const char* text) { return QCoreApplication::translate("MeetingUI", text); };
    const bool chinese = QLocale().language() == QLocale::Chinese;
    TEST_CHECK((tr("Meeting Encryption") != QStringLiteral("Meeting Encryption")) == chinese);
    MeetingUI::MeetingEncryptionDialog dialog;
    dialog.show(); app.processEvents();
    auto* required = dialog.findChild<QCheckBox*>("e2eeRequired");
    auto* input = dialog.findChild<QLineEdit*>("e2eeKeyInput");
    auto* proceed = dialog.findChild<QPushButton*>("joinBtn");
    TEST_CHECK(required && input && proceed);
    auto* checkboxAccessible = QAccessible::queryAccessibleInterface(required);
    auto* continueAccessible = QAccessible::queryAccessibleInterface(proceed);
    TEST_CHECK(checkboxAccessible && checkboxAccessible->text(QAccessible::Name) == required->text());
    TEST_CHECK(continueAccessible && continueAccessible->text(QAccessible::Name) == tr("Continue"));
    required->setFocus();
    SendEncryptionKey(required, Qt::Key_Space);
    TEST_CHECK(required->isChecked() && input->isEnabled());
    SendEncryptionKey(required, Qt::Key_Tab);
    TEST_CHECK(dialog.focusWidget() == input);
    TEST_CHECK(input->accessibleName() == tr("Encryption key"));
    SendEncryptionKey(input, Qt::Key_Return);
    TEST_CHECK(!dialog.takeRequest() && dialog.isVisible()); // Empty key remains editable.
    input->setText(QStringLiteral("public-keyboard-fixture"));
    SendEncryptionKey(input, Qt::Key_Return);
    auto request = dialog.takeRequest();
    TEST_CHECK(dialog.result() == QDialog::Accepted && request && request->secret->available());
    TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable());
    request->Revoke();
    MeetingUI::MeetingEncryptionDialog cancelled(nullptr, true);
    cancelled.show(); app.processEvents();
    auto* cancelInput = cancelled.findChild<QLineEdit*>("e2eeKeyInput");
    cancelInput->setText(QStringLiteral("public-cancel-fixture"));
    SendEncryptionKey(cancelInput, Qt::Key_Escape);
    TEST_CHECK(cancelled.result() == QDialog::Rejected && !cancelled.takeRequest());
    TEST_CHECK(cancelInput->text().isEmpty() && !cancelInput->isUndoAvailable());
    std::puts("E2EE_UI_KEYBOARD_ACCESSIBILITY PASS");
}

void TestEncryptionDialog(QApplication& app) {
    TestEncryptionKeyboard(app);
    using namespace MeetingUI;
    using namespace livekit;
    {
        MeetingEncryptionDialog dialog;
        dialog.accept();
        auto request = dialog.takeRequest();
        TEST_CHECK(request && request->mode == MeetingEncryptionMode::Off && !request->secret);
        TEST_CHECK(!dialog.takeRequest());
    }
    MeetingEncryptionDialog dialog;
    auto* required = dialog.findChild<QCheckBox*>("e2eeRequired");
    auto* input = dialog.findChild<QLineEdit*>("e2eeKeyInput");
    TEST_CHECK(required && input && input->echoMode() == QLineEdit::Password);
    TEST_CHECK(!input->isEnabled());
    required->setChecked(true);
    dialog.accept();
    TEST_CHECK(dialog.result() != QDialog::Accepted && !dialog.takeRequest());
    auto* error = dialog.findChild<QLabel*>("e2eeInputError");
    TEST_CHECK(error && !error->text().isEmpty());
    input->setText(QString::fromUtf8("中文密钥🔒"));
    dialog.accept();
    TEST_CHECK(dialog.result() != QDialog::Accepted && !dialog.takeRequest());
    TEST_CHECK(!error->text().isEmpty() && input->isEnabled());
    input->setText(QString(5000, QLatin1Char('A')));
    TEST_CHECK(input->text().size() == 4097);
    dialog.accept();
    TEST_CHECK(dialog.result() != QDialog::Accepted && !dialog.takeRequest());
    input->clear();
    dialog.show(); app.processEvents();
    const auto imageOption = app.arguments().indexOf("--e2ee-screenshot");
    if (imageOption >= 0 && imageOption + 1 < app.arguments().size())
        TEST_CHECK(dialog.grab().save(app.arguments().at(imageOption + 1)));
    input->setText(QStringLiteral("public-fixture-marker"));
    auto* accessible = QAccessible::queryAccessibleInterface(input);
    TEST_CHECK(accessible && accessible->state().passwordEdit);
    TEST_CHECK(!accessible->text(QAccessible::Value).contains("public-fixture-marker"));
    if (auto* text = accessible->textInterface())
        TEST_CHECK(!text->text(0, text->characterCount()).contains("public-fixture-marker"));
    required->setChecked(false);
    TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable());
    required->setChecked(true);
    input->setText(QStringLiteral("public-fixture-marker"));
    dialog.accept();
    TEST_CHECK(input->text().isEmpty() && !input->isUndoAvailable());
    auto request = dialog.takeRequest();
    TEST_CHECK(request && request->mode == MeetingEncryptionMode::Required && request->secret->available());
    auto provider = request->secret->ConsumeProvider();
    TEST_CHECK(provider->options().shared_key && !request->secret->available());
    MeetingEncryptionDialog cancelled;
    auto* cancelRequired = cancelled.findChild<QCheckBox*>("e2eeRequired");
    auto* cancelInput = cancelled.findChild<QLineEdit*>("e2eeKeyInput");
    cancelRequired->setChecked(true); cancelInput->setText("public-fixture-marker");
    cancelled.reject();
    TEST_CHECK(cancelInput->text().isEmpty() && !cancelInput->isUndoAvailable() && !cancelled.takeRequest());
    TEST_CHECK(!livekit::ApplicationMemoryDumpAllowed()); // Cancel never restores memory capture.
    MeetingEncryptionPanel panel;
    panel.resize(700, 60); panel.show();
    int recoveries = 0;
    panel.setRecoveryHandler([&](std::shared_ptr<MeetingSecretHandle> secret) {
        ++recoveries;
        TEST_CHECK(secret && secret->available());
        secret->Revoke();
    });
    auto* recover = panel.findChild<QPushButton*>("reenterEncryptionKey");
    panel.setSession(true, true, false);
    TEST_CHECK(!recover->isEnabled());
    panel.setSession(true, true, true);
    TEST_CHECK(recover->isEnabled());
    recover->click(); app.processEvents();
    auto* recovery = dynamic_cast<MeetingEncryptionDialog*>(panel.findChild<QDialog*>());
    // No Q_OBJECT on the dialog: find by the concrete child list below.
    TEST_CHECK(recovery);
    auto* forced = recovery->findChild<QCheckBox*>("e2eeRequired");
    TEST_CHECK(forced->isChecked() && !forced->isEnabled());
    recovery->findChild<QLineEdit*>("e2eeKeyInput")->setText("public-fixture-marker");
    panel.setSession(true, false, false);
    TEST_CHECK(recoveries == 0 && !recover->isEnabled());
    app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel.setSession(true, true, true);
    recover->click(); app.processEvents();
    recovery = dynamic_cast<MeetingEncryptionDialog*>(panel.findChild<QDialog*>());
    TEST_CHECK(recovery);
    recovery->findChild<QLineEdit*>("e2eeKeyInput")->setText("public-fixture-marker");
    recovery->accept();
    TEST_CHECK(recoveries == 1 && !recover->isEnabled());
    panel.setInstallResult(true, true);
    TEST_CHECK(recover->isEnabled());
    auto* policyStatus = panel.findChild<QLabel*>("encryptionPolicyStatus");
    const auto installedText = policyStatus->text();
    panel.setSession(true, true, true);
    TEST_CHECK(policyStatus->text() == installedText);
    livekit::MediaEncryptionStatus mediaStatus;
    mediaStatus.enabled = true;
    livekit::MediaEncryptionTrackStatus failedTrack;
    failedTrack.receiving = true;
    failedTrack.report = livekit::MediaCryptorReport::MissingKey;
    mediaStatus.tracks.push_back(failedTrack);
    panel.setMediaStatus(mediaStatus);
    auto* mediaLabel = panel.findChild<QLabel*>("encryptionMediaStatus");
    TEST_CHECK(!mediaLabel->text().isEmpty());
    const auto activeMediaText = mediaLabel->text();
    panel.setSession(true, false, false);
    TEST_CHECK(mediaLabel->text().isEmpty());
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(mediaLabel->text().isEmpty());
    panel.setSession(true, true, true);
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(mediaLabel->text() == activeMediaText);
    mediaStatus.tracks[0].report = livekit::MediaCryptorReport::Ok;
    panel.setMediaStatus(mediaStatus);
    const auto unverifiedLabel = policyStatus->text();
    mediaStatus.tracks[0].protected_after_current_install = true;
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(policyStatus->text() != unverifiedLabel);
    mediaStatus.enabled = false;
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(policyStatus->text() == unverifiedLabel);
    mediaStatus.enabled = true;
    mediaStatus.tracks.clear();
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(policyStatus->text() == unverifiedLabel); // Empty is never all-verified.
    mediaStatus.tracks.push_back(failedTrack);
    panel.setMediaStatus(mediaStatus);
    auto* detailsButton = panel.findChild<QPushButton*>("encryptionProtectionDetails");
    TEST_CHECK(detailsButton && detailsButton->isEnabled());
    detailsButton->click(); app.processEvents();
    auto* detailsDialog = panel.findChild<QDialog*>("encryptionProtectionDetailsDialog");
    auto* tracksTable = panel.findChild<QTableWidget*>("encryptionProtectionTracks");
    TEST_CHECK(detailsDialog && tracksTable && tracksTable->rowCount() == 1);
    const auto failureDetail = tracksTable->item(0, 4)->text();
    mediaStatus.tracks[0].report = livekit::MediaCryptorReport::Ok;
    mediaStatus.tracks[0].protected_after_current_install = true;
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(tracksTable->item(0, 4)->text() != failureDetail);
    const auto protectedDetail = tracksTable->item(0, 4)->text();
    const auto allProtectedPolicy = policyStatus->text();
    mediaStatus.tracks[0].track_id = "good-track";
    mediaStatus.tracks[0].binding_id = 1;
    auto isolatedFailure = failedTrack;
    isolatedFailure.track_id = "missing-slot-track";
    isolatedFailure.binding_id = 2;
    mediaStatus.tracks.push_back(isolatedFailure);
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(tracksTable->rowCount() == 2);
    TEST_CHECK(tracksTable->item(0, 1)->text() == "good-track");
    TEST_CHECK(tracksTable->item(0, 4)->text() == protectedDetail);
    TEST_CHECK(tracksTable->item(1, 1)->text() == "missing-slot-track");
    TEST_CHECK(tracksTable->item(1, 4)->text() == failureDetail);
    TEST_CHECK(policyStatus->text() != allProtectedPolicy);
    mediaStatus.tracks.pop_back();
    panel.setSession(true, false, false);
    TEST_CHECK(tracksTable->rowCount() == 0 && !detailsButton->isEnabled());
    panel.setSession(true, true, true);
    mediaStatus.tracks[0] = failedTrack;
    mediaStatus.tracks[0].participant_identity = "<public-fixture-participant>";
    mediaStatus.tracks[0].track_id = "public-audio-track";
    panel.setMediaStatus(mediaStatus);
    TEST_CHECK(tracksTable->item(0, 0)->text() == "<public-fixture-participant>");
    const auto detailsScreenshot = app.arguments().indexOf("--e2ee-details-screenshot");
    if (detailsScreenshot >= 0 && detailsScreenshot + 1 < app.arguments().size()) {
        app.processEvents();
        TEST_CHECK(detailsDialog->grab().save(app.arguments().at(detailsScreenshot + 1)));
    }
    detailsDialog->close(); app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel.resize(1000, 80);
    const auto screenshot = app.arguments().indexOf("--e2ee-panel-screenshot");
    if (screenshot >= 0 && screenshot + 1 < app.arguments().size()) {
        app.processEvents();
        TEST_CHECK(panel.grab().save(app.arguments().at(screenshot + 1)));
    }
    panel.close();
    std::puts("E2EE_UI_INPUT_CONTRACT PASS");
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--e2ee-frame-signature") {
        using namespace e2ee_frame_test;
        const auto a = Sample(128, 64, [](int x, int) { return x < 64 ? 32 : 192; });
        const auto b = Sample(128, 64, [](int x, int) { return x < 64 ? 40 : 200; });
        const auto c = Sample(128, 64, [](int x, int) { return x < 64 ? 41 : 201; });
        const auto inverse = Sample(128, 64, [](int x, int) { return x < 64 ? 192 : 32; });
        const auto flat = Sample(128, 64, [](int, int) { return 100; });
        TEST_CHECK(a && b && c && inverse && flat);
        TEST_CHECK(Matches(*a, *a) && Matches(*a, *b));
        TEST_CHECK(!Matches(*a, *c) && !Matches(*a, *inverse) && !Matches(*flat, *flat));
        TEST_CHECK(!SampleLuma(nullptr, 128, 128, 64));
        TEST_CHECK(!Sample(32, 64, [](int, int) { return 0; }));
        std::puts("E2EE_FRAME_SIGNATURE PASS");
        return 0;
    }
    QElapsedTimer startup;
    startup.start();
    crl::details::init();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;
    TEST_CHECK(settings.isValid());
    auto settingsPath = settings.path();
    if (app.arguments().contains("--e2ee-product-runtime")) {
        const auto evidence = qEnvironmentVariable("E2EE_EVIDENCE_DIR");
        TEST_CHECK(!evidence.isEmpty());
        settingsPath = evidence + "/settings";
        TEST_CHECK(QDir().mkpath(settingsPath));
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
    if (app.arguments().contains("--e2ee-product-runtime")) {
        QSettings productSettings(QSettings::defaultFormat(), QSettings::UserScope, "Cohavora", "Cohavora");
        TEST_CHECK(QDir::cleanPath(productSettings.fileName()).startsWith(QDir::cleanPath(settingsPath) + "/"));
        productSettings.setValue("e2ee_acceptance/public_marker", "isolated-settings");
        productSettings.sync();
        TEST_CHECK(productSettings.status() == QSettings::NoError);
    }
    app.setOrganizationName("MainWindowLatencyTest");
    app.setApplicationName("Isolated");
    MeetingUI::MeetingUiIntegration integration;
    Ui::Integration::Set(&integration);
    const auto applicationMs = startup.nsecsElapsed() / 1e6;
    style::StartManager(100);
    const auto styleMs = startup.nsecsElapsed() / 1e6;
    MeetingUI::AppTranslation::install(app, MeetingUI::AppTranslation::startupLocale(app.arguments()));
    MeetingUI::AppTheme::install(app);
    if (app.arguments().contains("--e2ee-uia-fixture")) {
        MeetingUI::MeetingEncryptionDialog dialog;
        dialog.findChild<QCheckBox*>("e2eeRequired")->setChecked(true);
        dialog.findChild<QLineEdit*>("e2eeKeyInput")->setText("public-uia-password-marker");
        dialog.show();
        app.processEvents();
        QFile ready(qEnvironmentVariable("E2EE_UIA_READY"));
        TEST_CHECK(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        ready.write(QJsonDocument(QJsonObject{{"pid", QCoreApplication::applicationPid()},
            {"hwnd", static_cast<qint64>(dialog.winId())}}).toJson(QJsonDocument::Compact));
        ready.close();
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        const auto result = app.exec();
        dialog.reject();
        style::StopManager();
        return result;
    }
    if (app.arguments().contains("--e2ee-product-runtime")) {
        const int result = RunE2eeProductRuntime(app);
        style::StopManager();
        return result;
    }
    if (app.arguments().contains("--e2ee-only")) {
        TestEncryptionEntryCancellation(app);
        TestEntryDefaultsWithoutKey(app);
        TestDetailDirectAdmission(app);
        TestJoinEncryptionSettings(app);
        TestEncryptionDialog(app);
        style::StopManager();
        return 0;
    }
    std::printf("main_setup application_ms=%.3f style_ms=%.3f theme_translation_ms=%.3f\n",
        applicationMs, styleMs - applicationMs, startup.nsecsElapsed() / 1e6 - styleMs);
    for (int sample = 0; sample != 4; ++sample) {
        QElapsedTimer timer;
        timer.start();
        MeasuredMainWindow window;
        const auto constructed = timer.nsecsElapsed();
        PaintProbe paint;
        window.installEventFilter(&paint);
        if (app.arguments().contains("--no-activate")) window.setAttribute(Qt::WA_ShowWithoutActivating);
        window.show();
        const auto shown = timer.nsecsElapsed();
        while (!paint.completed && timer.elapsed() < 10000) {
            app.processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        TEST_CHECK(paint.completed);
        const auto painted = timer.nsecsElapsed();
        if (sample == 0 && app.arguments().contains("--snapshot")) {
            TEST_CHECK(window.grab().save("out/main-ui-latency-20260930/main.png"));
            QImage icons(300, 80, QImage::Format_ARGB32_Premultiplied);
            icons.fill(Qt::white);
            QPainter painter(&icons);
            int x = 8;
            for (auto kind : {MeetingUI::AppTheme::Icon::Close, MeetingUI::AppTheme::Icon::Details,
                    MeetingUI::AppTheme::Icon::Video, MeetingUI::AppTheme::Icon::Audio,
                    MeetingUI::AppTheme::Icon::Information}) {
                auto icon = MeetingUI::AppTheme::icon(kind);
                icon.paint(&painter, QRect(x, 4, 24, 24));
                icon.paint(&painter, QRect(x, 35, 40, 40));
                x += 58;
            }
            painter.end();
            TEST_CHECK(icons.save("out/main-ui-latency-20260930/icons.png"));
        }
        std::printf("main_benchmark sample=%d construct_ms=%.3f show_ms=%.3f first_qt_paint_ms=%.3f\n",
            sample, constructed / 1e6, (shown - constructed) / 1e6, painted / 1e6);
        if (app.arguments().contains("--no-menus")) continue;
        auto *avatar = window.findChild<QPushButton *>("mainAccountMenu");
        TEST_CHECK(avatar && avatar->isEnabled());
        timer.restart();
        avatar->click();
        QMenu *popup = nullptr;
        for (auto *menu : window.findChildren<QMenu *>()) if (menu->isVisible()) popup = menu;
        TEST_CHECK(popup);
        TEST_CHECK(popup->findChild<QAction *>("mainPostMeetingTelemetry"));
        std::printf("main_account_menu sample=%d popup_ms=%.3f\n", sample, timer.nsecsElapsed() / 1e6);
        popup->close();
        window.close();
    }
    std::fflush(stdout);
    style::StopManager();
}
