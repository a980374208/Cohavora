#include "src/ui/app_theme.h"
#include "src/ui/directory_icon_provider.h"
#include <QApplication>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QtPlugin>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <thread>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

namespace {
QJsonObject sample() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool memoryOk = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    bool yun = false;
    int count = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool modulesOk = snapshot != INVALID_HANDLE_VALUE && Module32FirstW(snapshot, &entry);
    if (modulesOk) do {
        ++count;
        yun |= _wcsicmp(entry.szModule, L"YunShellExtV164.dll") == 0;
    } while (Module32NextW(snapshot, &entry));
    if (modulesOk) modulesOk = GetLastError() == ERROR_NO_MORE_FILES;
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return {{"private_bytes", memoryOk ? QJsonValue(double(memory.PrivateUsage)) : QJsonValue()},
        {"memory_query_ok", memoryOk}, {"module_query_ok", modulesOk},
        {"module_count", count}, {"yun_shell_loaded", yun}};
}
}

int main(int argc, char **argv) {
    // Independent hard deadline also covers a blocked GUI/Shell call.
    std::thread([] { Sleep(18000); TerminateProcess(GetCurrentProcess(), 124); }).detach();
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QString mode = app.arguments().value(1);
    const QString profile = app.arguments().value(2);
    if ((mode != "baseline" && mode != "generic") || QApplication::platformName() != "windows"
        || profile.isEmpty() || !QFileInfo(profile).isAbsolute() || !QFileInfo(profile).isDir()) {
        std::puts("{\"status\":\"INVALID_MODE_OR_NON_WINDOWS_PLATFORM\"}");
        return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("LiveKitDirectoryIconProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DirectoryIconProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile);
    MeetingUI::AppTheme::install(app);
    QJsonObject result{{"mode", mode}, {"platform", QApplication::platformName()},
        {"status", "DIAGNOSTIC_ONLY"}, {"qualification_credit", 0}};
    result.insert("before_provider", sample());
    MeetingUI::DirectoryIconProvider icons(*QApplication::style());
    result.insert("after_provider", sample());
    QFileDialog *chooser = nullptr;
    bool controlledClose = false;
    bool contaminated = false;
    QTimer::singleShot(1000, &app, [&] {
        result.insert("baseline", sample());
        chooser = new QFileDialog(nullptr, "Export telemetry report");
        QObject::connect(chooser, &QDialog::finished, &app, [&](int) {
            if (!controlledClose) contaminated = true;
        });
        result.insert("after_constructor", sample());
        chooser->setOption(QFileDialog::DontUseNativeDialog);
        chooser->setOption(QFileDialog::ShowDirsOnly);
        chooser->setFileMode(QFileDialog::Directory);
        if (mode == "generic") chooser->setIconProvider(&icons);
        chooser->setDirectory(app.arguments().value(3, QDir::currentPath()));
        MeetingUI::AppTheme::setTone(*chooser, MeetingUI::AppTheme::Tone::Light);
        MeetingUI::AppTheme::styleChoiceControls(*chooser, MeetingUI::AppTheme::Tone::Light);
        MeetingUI::AppTheme::makeDialogAdaptive(*chooser, {760, 520});
        chooser->show();
        result.insert("after_show", sample());
        QTimer::singleShot(5000, &app, [&] {
            result.insert("opened", sample());
            result.insert("opened_visible", chooser->isVisible());
            controlledClose = true;
            chooser->reject();
            delete chooser;
            chooser = nullptr;
            result.insert("after_close", sample());
            QTimer::singleShot(2000, &app, [&] {
                result.insert("closed_quiet", sample());
                result.insert("status", contaminated ? "CONTAMINATED" : "CAPTURED_DIAGNOSTIC_ONLY");
                const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
                std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
                std::putchar('\n');
                std::fflush(stdout);
                app.quit();
            });
        });
    });
    return app.exec();
}
