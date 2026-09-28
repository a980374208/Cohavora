#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/crash_evidence_provider.h"
#include "src/ui/diagnostic_qt_bridge.h"
#include "src/ui/meeting_log_console.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSettings>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollBar>
#include <QtGui/QClipboard>
#include <QtGui/QTextDocument>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>

namespace MeetingUI {
struct MeetingLogConsoleTestAccess {
    static void CheckScrollFollowing(MeetingLogConsoleWindow &console) {
        console.clearLogs();
        console.show();
        QCoreApplication::processEvents();
        for (int i = 0; i < 100; ++i)
            console.appendLog(LogCategory::General, QStringLiteral("CHAT_TEXT"),
                QStringLiteral("bytes=%1").arg(200000 + i));
        DrainUntilText(console, QStringLiteral("bytes=200099"));
        auto *scroll = console._logView->verticalScrollBar();
        TEST_CHECK(scroll->maximum() > 0);
        TEST_CHECK(scroll->value() == scroll->maximum());
        scroll->setValue(scroll->maximum() / 2);
        const auto readingPosition = scroll->value();
        auto selection = console._logView->textCursor();
        selection.setPosition(0);
        selection.setPosition(5, QTextCursor::KeepAnchor);
        console._logView->setTextCursor(selection);
        scroll->setValue(readingPosition);
        for (int i = 0; i < 5; ++i) console.drainPending();
        TEST_CHECK(scroll->value() == readingPosition);
        TEST_CHECK(console._logView->textCursor().selectedText() == selection.selectedText());
        selection.clearSelection();
        console._logView->setTextCursor(selection);
        scroll->setValue(readingPosition);
        console.appendLog(LogCategory::General, QStringLiteral("CHAT_TEXT"), QStringLiteral("bytes=200100"));
        DrainUntilText(console, QStringLiteral("bytes=200100"));
        TEST_CHECK(scroll->value() == readingPosition);
        scroll->setValue(scroll->maximum());
        console.appendLog(LogCategory::General, QStringLiteral("CHAT_TEXT"), QStringLiteral("bytes=200101"));
        DrainUntilText(console, QStringLiteral("bytes=200101"));
        TEST_CHECK(scroll->value() == scroll->maximum());
        console._autoScrollBox->setChecked(false);
        const auto pausedPosition = scroll->value();
        console.appendLog(LogCategory::General, QStringLiteral("CHAT_TEXT"), QStringLiteral("bytes=200102"));
        DrainUntilText(console, QStringLiteral("bytes=200102"));
        TEST_CHECK(scroll->value() == pausedPosition);
        console._autoScrollBox->setChecked(true);
        TEST_CHECK(scroll->value() == scroll->maximum());
        console.hide();
        console.clearLogs();
    }
    static void CheckNoiseFiltering(MeetingLogConsoleWindow &console) {
        using namespace livekit::diagnostic;
        console.clearLogs();
        const auto dropped = console._queue->dropped;
        for (int i = 0; i < 10000; ++i)
            console.appendLog(LogCategory::Signal, QStringLiteral("RAW_MSG"),
                QStringLiteral("private unregistered text"));
        const auto mirror = MeetingLogConsoleWindow::diagnosticMirror();
        Event signal;
        signal.kind = EventKind::SignalMessageSummary;
        signal.signal_message_count = 10;
        mirror(signal);
        mirror(Event::QueueHealth(100, 0, 10));
        TEST_CHECK(console._queue->pending.empty());
        TEST_CHECK(console._queue->dropped == dropped);

        console.appendLog(LogCategory::Error, QStringLiteral("FAILURE"),
            QStringLiteral("private error text"));
        Event terminal;
        terminal.kind = EventKind::RoomConnectTerminal;
        terminal.outcome = Outcome::Timeout;
        terminal.error_code = ErrorCode::JoinTimeout;
        terminal.duration_ms = 5000;
        mirror(terminal);
        DrainUntilText(console, QStringLiteral("duration_ms=5000"));
        const auto text = console._logView->toPlainText();
        TEST_CHECK(text.contains(QStringLiteral("[FAILURE]")));
        TEST_CHECK(text.contains(QStringLiteral("outcome=timeout")));
        TEST_CHECK(text.contains(QStringLiteral("error=join_timeout")));
        TEST_CHECK(!text.contains(QStringLiteral("private")));
        TEST_CHECK(!text.contains(QStringLiteral("session=unknown")));
        TEST_CHECK(!text.contains(QStringLiteral("operation=unknown")));
        console.clearLogs();
    }
    static void SettleFilter(MeetingLogConsoleWindow &console) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (console._filterTimer->isActive() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            Sleep(1);
        }
        TEST_CHECK(!console._filterTimer->isActive());
    }
    static void DrainUntilText(MeetingLogConsoleWindow &console, const QString &text) {
        SettleFilter(console);
        // Rendering is time-sliced (4ms), so a fixed number of calls is not a
        // completion condition on a slow/debug build. Keep the wait bounded.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            console.drainPending();
            if (console._logView->toPlainText().contains(text)) return;
        } while (std::chrono::steady_clock::now() < deadline);
        TEST_CHECK(console._logView->toPlainText().contains(text));
    }

    static void CheckByteEviction(MeetingLogConsoleWindow &console) {
        console.clearLogs();
        console.onFilterChanged({});
        const auto first = console._nextEntryId;
        for (int i = 0; i < 400; ++i) {
            MeetingLogConsoleWindow::PendingEntry entry;
            entry.message = QStringLiteral("row_%1_").arg(i) + QString(4096, QLatin1Char('x'));
            console.appendVisible(std::move(entry));
            console.drainPending();
        }
        DrainUntilText(console, QStringLiteral("row_399_"));
        TEST_CHECK(console._logEntries.front().id > first); // Byte cap, not the 5000-entry cap.
        TEST_CHECK(console._cacheBytes <= console.kMaxCacheBytes);
        TEST_CHECK(console._logView->toPlainText().contains(QStringLiteral("row_399_")));
        TEST_CHECK(!console._logView->toPlainText().contains(QStringLiteral("row_0_")));
        TEST_CHECK(console._visibleIds.front() == console._logEntries.front().id);
    }
};
}
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
    if (argc == 2 && std::string_view(argv[1]) == "--console-lifetime-child") {
        QPointer<MeetingUI::MeetingLogConsoleWindow> console;
        {
            QApplication application(argc, argv);
            application.setOrganizationName(QStringLiteral("CohavoraTest"));
            application.setApplicationName(QStringLiteral("ConsoleLifetime"));
            console = &MeetingUI::MeetingLogConsoleWindow::Instance();
            console->show();
            application.processEvents();
        }
        // A surviving QWidget would be destroyed after the platform plugin.
        // This checks ownership even when the late destructor happens not to crash.
        return console.isNull() && !MeetingUI::MeetingLogConsoleWindow::Active() ? 0 : 92;
    }
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
    livekit::diagnostic::Event signalSummary;
    signalSummary.kind = livekit::diagnostic::EventKind::SignalMessageSummary;
    signalSummary.signal_category = livekit::diagnostic::SignalCategory::Periodic;
    signalSummary.signal_message_count = 42;
    TEST_CHECK(pipeline->TryEmit(signalSummary));
    TEST_CHECK(pipeline->Close() == livekit::diagnostic::DrainResult::Completed);

    bool ledger_failure = false, login_failure = false, signal_saved = false;
    for (const auto& run : std::filesystem::directory_iterator(
             std::filesystem::path(directory.path().toStdWString()))) {
        for (const auto& file : std::filesystem::directory_iterator(run.path())) {
            if (file.path().extension() != ".jsonl") continue;
            std::ifstream input(file.path());
            std::string line;
            while (std::getline(input, line)) {
                const auto record = nlohmann::json::parse(line);
                if (record.at("event_name") == "signal.message.summary" &&
                    record.at("attributes").value("count", 0) == 42)
                    signal_saved = true;
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
    TEST_CHECK(ledger_failure && login_failure && signal_saved);

    auto &console = MeetingUI::MeetingLogConsoleWindow::Instance();
    console.drainPending();
    auto *view = console.findChild<QPlainTextEdit*>();
    TEST_CHECK(view);
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("private unknown Qt canary 5934")));
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("signal.message.summary")));
    MeetingUI::MeetingLogConsoleTestAccess::CheckNoiseFiltering(console);
    MeetingUI::MeetingLogConsoleTestAccess::CheckScrollFollowing(console);
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
    severity->setCurrentIndex(severity->findData(QStringLiteral("error")));
    component->setCurrentIndex(component->findData(QStringLiteral("app")));
    session->setText(QStringLiteral("01234567"));
    operation->setText(QStringLiteral("room_connect_42"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("process.issue")));
    livekit::diagnostic::Event failedChat;
    failedChat.kind = livekit::diagnostic::EventKind::ChatSendTerminal;
    failedChat.outcome = livekit::diagnostic::Outcome::Failure;
    failedChat.context = filtered.context;
    MeetingUI::MeetingLogConsoleWindow::diagnosticMirror()(failedChat);
    console.drainPending();
    component->setCurrentIndex(component->findData(QStringLiteral("meeting_ui")));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("chat.send.terminal")));
    component->setCurrentIndex(component->findData(QStringLiteral("app")));
    const auto previousText = view->toPlainText();
    session->setText(QStringLiteral("nonmatch"));
    console.drainPending();
    TEST_CHECK(view->toPlainText() == previousText);
    session->setText(QStringLiteral("nonmatching"));
    TEST_CHECK(view->toPlainText() == previousText);
    MeetingUI::MeetingLogConsoleTestAccess::SettleFilter(console);
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

    // A filter rebuild must yield after a bounded slice, then reach the newest
    // record. Eviction must not clear/reinsert the retained document.
    console.clearLogs();
    severity->setCurrentIndex(0);
    component->setCurrentIndex(0);
    session->clear();
    operation->clear();
    for (int i = 0; i < 6000; ++i) {
        console.appendLog(MeetingUI::LogCategory::General, QStringLiteral("CHAT_TEXT"),
            QStringLiteral("bytes=%1").arg(100000 + i));
        console.drainPending();
    }
    MeetingUI::MeetingLogConsoleTestAccess::DrainUntilText(
        console, QStringLiteral("bytes=105999"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("bytes=105999")));
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("bytes=100000")));
    console.onFilterChanged(QStringLiteral("CHAT_TEXT"));
    // Text changes defer rebuilding; DrainUntilText waits for the debounce.
    MeetingUI::MeetingLogConsoleTestAccess::DrainUntilText(
        console, QStringLiteral("bytes=105999"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("bytes=105999")));
    console.onFilterChanged(QStringLiteral("bytes=105999"));
    MeetingUI::MeetingLogConsoleTestAccess::DrainUntilText(
        console, QStringLiteral("bytes=105999"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("bytes=105999")));
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("bytes=105998")));

    MeetingUI::MeetingLogConsoleTestAccess::CheckByteEviction(console);
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
    child.setArguments({QStringLiteral("--console-lifetime-child")});
    child.start();
    TEST_CHECK(child.waitForStarted(5000));
    TEST_CHECK(child.waitForFinished(10000));
    TEST_CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
    return 0;
}
