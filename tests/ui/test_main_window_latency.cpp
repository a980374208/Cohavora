#include "src/ui/meeting_main_window.h"
#include "src/ui/meeting_detail_dialog.h"
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

namespace MeetingUI {
// Only inject the account/credential/detail preconditions. The actual main
// window entry and encryption dialog control flow remains production code.
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
        OpenMeeting::MeetingCatalogDetail detail;
        detail.record.meetingId = "public-entry-fixture";
        dialog._detail = detail;
        dialog._joinRequested = true;
        dialog.accept();
    }
};
}

void TestEncryptionEntryCancellation(QApplication& app) {
    OpenMeeting::initializeServiceEndpointPolicy(true);
    auto& session = OpenMeeting::SessionManager::instance();
    TEST_CHECK(session.setServerBaseUrl("http://127.0.0.1:9"));
    session.loginAsGuest("Public entry fixture", "public-entry-fixture");
    TEST_CHECK(session.isLoggedIn());
    MeetingUI::MeetingMainWindow window;
    for (const auto& route : {"ordinary", "quick", "detail", "direct", "quick-share", "join-share"}) {
        for (int attempt = 0; attempt != 2; ++attempt) {
            int prompts = 0;
            QTimer driver;
            driver.setInterval(0);
            QObject::connect(&driver, &QTimer::timeout, &window, [&] {
                auto* modal = app.activeModalWidget();
                if (auto* encryption = dynamic_cast<MeetingUI::MeetingEncryptionDialog*>(modal)) {
                    ++prompts;
                    auto* required = encryption->findChild<QCheckBox*>("e2eeRequired");
                    auto* input = encryption->findChild<QLineEdit*>("e2eeKeyInput");
                    TEST_CHECK(required && input && !required->isChecked() && input->text().isEmpty());
                    required->setChecked(true);
                    input->setText("public-cancelled-entry-key");
                    encryption->reject();
                    TEST_CHECK(input->text().isEmpty() && !encryption->takeRequest());
                } else if (auto* join = dynamic_cast<MeetingUI::JoinMeetingDialog*>(modal)) {
                    MeetingUI::MeetingEntryEncryptionTestAccess::credentialsReady(*join, QString(route) == "direct");
                } else if (auto* detail = dynamic_cast<MeetingUI::MeetingDetailDialog*>(modal)) {
                    MeetingUI::MeetingEntryEncryptionTestAccess::detailReady(*detail);
                }
            });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &window, [] { TEST_CHECK(false && "entry dialog timeout"); });
            deadline.start(10000);
            driver.start();
            MeetingUI::MeetingEntryEncryptionTestAccess::enter(window, QString::fromLatin1(route));
            driver.stop(); deadline.stop();
            TEST_CHECK(prompts == 1);
            TEST_CHECK(MeetingUI::MeetingEntryEncryptionTestAccess::reservationReleased(window));
            for (auto* top : app.topLevelWidgets())
                TEST_CHECK(!dynamic_cast<MeetingUI::MeetingRoomWindow*>(top));
            std::printf("E2EE_ENTRY_CANCEL route=%s attempt=%d PASS\n", route, attempt + 1);
        }
    }
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
