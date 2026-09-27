#include "src/ui/meeting_log_console.h"
#include "src/ui/app_translation.h"
#include <QtWidgets/QApplication>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QApplication::platformName() != QStringLiteral("windows")) return 77;
    QCoreApplication::setOrganizationName(QStringLiteral("CohavoraUiaFixture"));
    QCoreApplication::setApplicationName(QStringLiteral("ConsoleFixture"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    MeetingUI::AppTranslation::install(app,
        MeetingUI::AppTranslation::startupLocale(app.arguments()));
    MeetingUI::MeetingLogConsoleWindow console;
    console.appendLog(MeetingUI::LogCategory::General,
        QStringLiteral("SESSION"), QStringLiteral("session_invalidated"));
    console.appendLog(MeetingUI::LogCategory::Error,
        QStringLiteral("SHUTDOWN"), QStringLiteral("native_cleanup_failed"));
    console.show();
    // Hard lifetime bound even if the external UIA client is terminated.
    QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
    return app.exec();
}
