#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/crash_evidence_provider.h"
#include "src/ui/diagnostic_qt_bridge.h"
#include "src/ui/meeting_log_console.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSettings>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtGui/QClipboard>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace {
QtMessageHandler safe_handler = nullptr;
}

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Cohavora"));
    const auto qt_data_root = std::filesystem::path(
        QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation).toStdWString());
    TEST_CHECK(livekit::telemetry::CrashEvidenceProvider::DefaultRoot()
        .parent_path().lexically_normal() == qt_data_root.lexically_normal());
    app.setOrganizationName(QStringLiteral("CohavoraTest"));
    QTemporaryDir settingsDirectory;
    TEST_CHECK(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());
    TEST_CHECK(QDir().mkpath(settingsDirectory.path() +
                             QStringLiteral("/CohavoraTest")));
    TEST_CHECK(QSettings().status() == QSettings::NoError);
    auto pipeline = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
    livekit::diagnostic::InstallBusinessPipeline(pipeline);
    MeetingUI::DiagnosticQtBridge bridge(pipeline);
    TEST_CHECK(MeetingUI::MeetingLogConsoleWindow::Active() == nullptr);

    if (app.arguments().contains(QStringLiteral("--fatal-child"))) {
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS |
                     SEM_NOOPENFILEERRORBOX);
        safe_handler = qInstallMessageHandler([](QtMsgType type,
                const QMessageLogContext& context, const QString& message) {
            safe_handler(type, context, message);
            if (type == QtFatalMsg) std::_Exit(91);
        });
        qFatal("private fatal canary 7753");
        return 0;
    }

    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    TEST_CHECK(pipeline->StartWriter(
        std::filesystem::path(directory.path().toStdWString())));
    std::thread first_producer([&] {
        pipeline->TryEmit(livekit::diagnostic::Event::Started("test-build"));
    });
    first_producer.join();
    TEST_CHECK(MeetingUI::MeetingLogConsoleWindow::Active() == nullptr);
    qWarning().noquote() << "private unknown Qt canary 5934";
    TEST_CHECK(pipeline->GetStatus().suppressed == 1);
    qWarning() << "The local stability ledger is unavailable.";
    qCritical() << "Invalid debug sign-in options.";
    TEST_CHECK(pipeline->Close() == livekit::diagnostic::DrainResult::Completed);

    bool ledger_failure = false, login_failure = false;
    for (const auto& run : std::filesystem::directory_iterator(
             std::filesystem::path(directory.path().toStdWString()))) {
        for (const auto& file : std::filesystem::directory_iterator(run.path())) {
            if (file.path().extension() != ".jsonl") continue;
            std::ifstream input(file.path());
            std::string line;
            while (std::getline(input, line)) {
                const auto record = nlohmann::json::parse(line);
                if (record.at("event_name") == "diagnostics.sink.failed" &&
                    record.at("attributes").value("sink_kind", "") == "telemetry")
                    ledger_failure = true;
                if (record.at("event_name") == "process.issue" &&
                    record.at("attributes").value("reason_code", "") ==
                        "invalid_sign_in_options")
                    login_failure = true;
                TEST_CHECK(line.find("private unknown Qt canary 5934") == std::string::npos);
            }
        }
    }
    TEST_CHECK(ledger_failure && login_failure);

    auto &console = MeetingUI::MeetingLogConsoleWindow::Instance();
    console.drainPending();
    auto *view = console.findChild<QPlainTextEdit*>();
    TEST_CHECK(view);
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("private unknown Qt canary 5934")));
    livekit::diagnostic::Event filtered = livekit::diagnostic::Event::Issue(
        livekit::diagnostic::IssueCode::InvalidSignInOptions);
    TEST_CHECK(filtered.context.anonymous_session_id.Assign(
        "0123456789abcdef0123456789abcdef"));
    TEST_CHECK(filtered.context.operation_id.Assign("room_connect_42"));
    MeetingUI::MeetingLogConsoleWindow::diagnosticMirror()(filtered);
    console.drainPending();
    auto *severity = console.findChild<QComboBox*>(
        QStringLiteral("consoleSeverityFilter"));
    auto *component = console.findChild<QComboBox*>(
        QStringLiteral("consoleComponentFilter"));
    auto *session = console.findChild<QLineEdit*>(
        QStringLiteral("consoleSessionFilter"));
    auto *operation = console.findChild<QLineEdit*>(
        QStringLiteral("consoleOperationFilter"));
    auto *copyScope = console.findChild<QComboBox*>(
        QStringLiteral("consoleCopyScope"));
    TEST_CHECK(severity && component && session && operation && copyScope);
    auto *saveLogs = console.findChild<QCheckBox*>(
        QStringLiteral("consoleSaveLogs"));
    TEST_CHECK(saveLogs && saveLogs->isEnabled());
    saveLogs->setChecked(false);
    TEST_CHECK(!pipeline->GetStatus().retention_enabled);
    TEST_CHECK(!QSettings().value(QStringLiteral("diagnostics/historyEnabled"),
                                 true).toBool());
    auto restartedPipeline = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
    restartedPipeline->SetRetentionEnabled(QSettings().value(
        QStringLiteral("diagnostics/historyEnabled"), true).toBool());
    TEST_CHECK(!restartedPipeline->GetStatus().retention_enabled);
    saveLogs->setChecked(true);
    TEST_CHECK(pipeline->GetStatus().retention_enabled);
    TEST_CHECK(QSettings().value(QStringLiteral("diagnostics/historyEnabled"),
                                false).toBool());
    TEST_CHECK(console.findChild<QPushButton*>(
        QStringLiteral("consoleClearPreviousLogs")));
    TEST_CHECK(console.findChild<QCheckBox*>(
        QStringLiteral("consoleCrashCollection")));
    TEST_CHECK(console.findChild<QCheckBox*>(
        QStringLiteral("consoleDiagnosticMode")));
    severity->setCurrentText(QStringLiteral("error"));
    component->setCurrentText(QStringLiteral("app"));
    session->setText(QStringLiteral("01234567"));
    operation->setText(QStringLiteral("room_connect_42"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("process.issue")));
    livekit::diagnostic::Event failedChat;
    failedChat.kind = livekit::diagnostic::EventKind::ChatSendTerminal;
    failedChat.outcome = livekit::diagnostic::Outcome::Failure;
    failedChat.context = filtered.context;
    MeetingUI::MeetingLogConsoleWindow::diagnosticMirror()(failedChat);
    console.drainPending();
    component->setCurrentText(QStringLiteral("meeting_ui"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("chat.send.terminal")));
    component->setCurrentText(QStringLiteral("app"));
    session->setText(QStringLiteral("nonmatching"));
    TEST_CHECK(view->toPlainText().isEmpty());
    session->setText(QStringLiteral("01234567"));
    copyScope->setCurrentIndex(0);
    console.copyAllLogs();
    TEST_CHECK(QApplication::clipboard()->text().contains(
        QStringLiteral("room_connect_42")));
    TEST_CHECK(!QApplication::clipboard()->text().contains(
        QStringLiteral("private unknown Qt canary 5934")));

    auto blockedPipeline = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
    blockedPipeline->SetMirror(
        MeetingUI::MeetingLogConsoleWindow::diagnosticMirror());
    const auto blockedRoot = std::filesystem::path(directory.path().toStdWString()) /
        "blocked-ui";
    TEST_CHECK(blockedPipeline->StartWriter(blockedRoot));
    std::thread blockedProducer([&] {
        for (int index = 0; index != 3000; ++index)
            TEST_CHECK(blockedPipeline->TryEmit(
                livekit::diagnostic::Event::Received(
                    livekit::diagnostic::ChatKind::Text, 17)));
    });
    blockedProducer.join();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    TEST_CHECK(blockedPipeline->Close() ==
        livekit::diagnostic::DrainResult::Completed);
    TEST_CHECK(blockedPipeline->GetStatus().written >= 3000);
    console.drainPending();
    bool showedUiLoss = false;
    for (auto* label : console.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("UI loss=")) &&
            !label->text().contains(QStringLiteral("UI loss=0")))
            showedUiLoss = true;
    }
    TEST_CHECK(showedUiLoss);

    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({QStringLiteral("--fatal-child")});
    child.start();
    TEST_CHECK(child.waitForStarted(5000));
    if (!child.waitForFinished(10000)) {
        child.kill();
        child.waitForFinished(5000);
        TEST_CHECK(false);
    }
    TEST_CHECK(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0);
    const auto output = child.readAllStandardError() + child.readAllStandardOutput();
    TEST_CHECK(!output.contains("private fatal canary 7753"));
    return 0;
}
