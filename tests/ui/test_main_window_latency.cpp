#include "src/ui/meeting_main_window.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_ui_integration.h"
#include "tests/support/test_check.h"
#include "crl/crl.h"
#include "rpl/never.h"
#include "ui/style/style_core.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtPlugin>
#include <cstdio>
#include "src/ui/app_icons.h"
#include <QtGui/QPainter>

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

int main(int argc, char **argv) {
    QElapsedTimer startup;
    startup.start();
    crl::details::init();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;
    TEST_CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("MainWindowLatencyTest");
    app.setApplicationName("Isolated");
    MeetingUI::MeetingUiIntegration integration;
    Ui::Integration::Set(&integration);
    const auto applicationMs = startup.nsecsElapsed() / 1e6;
    style::StartManager(100);
    const auto styleMs = startup.nsecsElapsed() / 1e6;
    MeetingUI::AppTranslation::install(app, QLocale("zh_CN"));
    MeetingUI::AppTheme::install(app);
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
