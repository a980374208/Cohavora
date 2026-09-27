#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/app_translation.h"
#include <QtWidgets/QApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QApplication::platformName() != QStringLiteral("windows")) return 77;
    const auto args = app.arguments();
    const int stateArgument = args.indexOf(QStringLiteral("--state-file"));
    if (stateArgument < 0 || stateArgument + 1 >= args.size()) return 2;
    const QString stateFile = args[stateArgument + 1];
    MeetingUI::AppTranslation::install(app, MeetingUI::AppTranslation::startupLocale(args));
    MeetingUI::WhiteboardPanel panel;
    namespace wb = livekit::whiteboard;
    // Startup-only test data injection into the non-const panel's model.
    // After show(), the fixture only observes; all operations come from UIA.
    auto &document = const_cast<wb::Document &>(panel.document());
    wb::Command seed;
    seed.id = "uia-clear-seed-command";
    seed.actor = document.owner();
    seed.context = document.context();
    seed.kind = wb::CommandKind::Add;
    seed.object.id = "uia-clear-seed-object";
    seed.object.author = document.owner();
    seed.object.kind = wb::ObjectKind::Line;
    seed.object.points = {{100, 100}, {300, 200}};
    if (!document.apply(seed).changed()) return 2;
    panel.canvas()->refreshFromDocument();
    emit panel.canvas()->documentChanged();
    panel.resize(1200, 760);
    panel.show();
    int sample = 0;
    auto observe = [&] {
        const auto &state = panel.document();
        QJsonObject value;
        value["pid"] = static_cast<double>(QCoreApplication::applicationPid());
        value["sample"] = ++sample;
        value["objects"] = static_cast<int>(state.page().objects.size());
        value["epoch"] = static_cast<double>(state.page().epoch);
        value["pageId"] = QString::fromStdString(state.page().id);
        value["canUndo"] = state.canUndo(state.owner());
        value["canRedo"] = state.canRedo(state.owner());
        value["snapshot"] = QString::fromStdString(state.toJson());
        // Immutable numbered samples: readers never compete with replacement
        // of an existing Windows file. Only completed atomic writes are read.
        QSaveFile file(stateFile + QStringLiteral(".%1").arg(sample, 8, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly)) { qWarning() << file.errorString(); app.exit(2); return; }
        const auto bytes = QJsonDocument(value).toJson();
        if (file.write(bytes) != bytes.size() || !file.commit()) { qWarning() << file.errorString(); app.exit(2); }
    };
    QTimer observer;
    QObject::connect(&observer, &QTimer::timeout, &app, observe);
    observer.start(200);
    QTimer::singleShot(0, &app, observe);
    QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
    return app.exec();
}
