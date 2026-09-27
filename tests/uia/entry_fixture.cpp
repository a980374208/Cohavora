#include "src/ui/meeting_main_window.h"
#include "src/ui/settings_dialog.h"
#include "src/ui/app_translation.h"
#include "src/ui/app_theme.h"
#include "src/net/session_manager.h"
#include "src/ui/meeting_ui_integration.h"
#include "crl/crl.h"
#include "rpl/rpl.h"
#include "ui/style/style_core.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtPlugin>

#include <memory>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

namespace crl {
rpl::producer<> on_main_update_requests() { return rpl::never<>(); }
}

namespace OpenMeeting {
class SessionManagerTestAccess {
public:
    static SessionManager *create(const QString &path) {
        return new SessionManager(std::make_unique<QSettings>(path, QSettings::IniFormat));
    }
    static void destroy(SessionManager *session) { delete session; }
};
}

int main(int argc, char **argv) {
    crl::details::init();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (QApplication::platformName() != QStringLiteral("windows")) return 77;
    const auto args = app.arguments();
    const bool settingsMode = args.contains(QStringLiteral("--settings"));
    QTemporaryDir isolatedSettings;
    if (!isolatedSettings.isValid()) return 2;
    const auto settingsPath = isolatedSettings.filePath(QStringLiteral("settings.ini"));
    MeetingUI::AppTranslation::install(app, MeetingUI::AppTranslation::startupLocale(args));
    MeetingUI::MeetingUiIntegration integration;
    Ui::Integration::Set(&integration);
    style::StartManager(100);
    MeetingUI::AppTheme::install(app);
    std::unique_ptr<OpenMeeting::SessionManager,
        decltype(&OpenMeeting::SessionManagerTestAccess::destroy)> session(
            OpenMeeting::SessionManagerTestAccess::create(settingsPath),
            &OpenMeeting::SessionManagerTestAccess::destroy);
    if (settingsMode) {
        const auto stateArg = args.indexOf(QStringLiteral("--state-file"));
        if (stateArg < 0 || stateArg + 1 >= args.size()) return 2;
        MeetingUI::SettingsDialog dialog(*session);
        dialog.show();
        const auto statePath = args[stateArg + 1];
        int sample = 0;
        QTimer observer;
        const auto observe = [&] {
            const auto value = session->mediaPreferences();
            QSettings stored(settingsPath, QSettings::IniFormat);
            stored.sync();
            QJsonObject state;
            state["pid"] = static_cast<double>(QCoreApplication::applicationPid());
            state["sample"] = ++sample;
            state["camera"] = value.enableVideo;
            state["microphone"] = value.enableMicrophone;
            state["aec"] = value.echoCancellation;
            state["settingsFileExists"] = QFileInfo::exists(stored.fileName());
            state["settingsIsolated"] = QDir::cleanPath(stored.fileName()).startsWith(
                QDir::cleanPath(isolatedSettings.path()) + QLatin1Char('/'));
            state["storedCamera"] = stored.value(QStringLiteral("media/enableVideo")).toBool();
            state["storedAec"] = stored.value(QStringLiteral("media/echoCancellation")).toBool();
            state["settingsPath"] = stored.fileName();
            state["settingsFormat"] = static_cast<int>(stored.format());
            state["isolatedDirectory"] = isolatedSettings.path();
            QSaveFile file(statePath + QStringLiteral(".%1").arg(sample, 8, 10, QLatin1Char('0')));
            const auto bytes = QJsonDocument(state).toJson();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) app.exit(2);
        };
        QObject::connect(&observer, &QTimer::timeout, &app, observe);
        observer.start(200);
        QTimer::singleShot(0, &app, observe);
        QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
        const auto result = app.exec();
        style::StopManager();
        return result;
    }
    MeetingUI::JoinMeetingDialog dialog(nullptr, {}, std::nullopt, session.get());
    dialog.show();
    QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
    const auto result = app.exec();
    style::StopManager();
    return result;
}
