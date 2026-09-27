#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/app_translation.h"
#include <QtWidgets/QApplication>
#include <QtCore/QTimer>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QApplication::platformName() != QStringLiteral("windows")) return 77;
    MeetingUI::AppTranslation::install(app,
        MeetingUI::AppTranslation::startupLocale(app.arguments()));
    MeetingUI::WhiteboardPanel panel;
    // Deterministic initial zoom lets UIA test Fit without canvas input.
    panel.canvas()->setZoom(1.5);
    panel.resize(1200, 760);
    panel.show();
    // No Room, transport, synthetic input or UI-operation backdoor.
    QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
    return app.exec();
}
