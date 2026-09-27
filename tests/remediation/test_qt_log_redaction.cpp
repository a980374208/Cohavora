#include "src/ui/meeting_log_console.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>

#include <chrono>
#include <string>
#include <thread>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace {

void DrainEvents() {
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::thread early([] {
        MeetingUI::LogToConsole(MeetingUI::LogCategory::General,
            QStringLiteral("EARLY"), QStringLiteral("private early log 3172"));
    });
    early.join();
    TEST_CHECK(QApplication::topLevelWidgets().empty());
    auto& console = MeetingUI::MeetingLogConsoleWindow::Instance();
    console.clearLogs();

    constexpr const char* kSecret = "synthetic-qt-cache-secret";
    std::thread producer([]() {
        MeetingUI::LogToConsole(
            MeetingUI::LogCategory::Connection,
            "HANDSHAKE_FAILURE",
            "failure access_token=synthetic-qt-cache-secret");
    });
    producer.join();
    DrainEvents();
    console.drainPending();

    auto* view = console.findChild<QPlainTextEdit*>();
    TEST_CHECK(view != nullptr);
    const auto rendered = view->toPlainText().toStdString();
    TEST_CHECK(rendered.find(kSecret) == std::string::npos);
    TEST_CHECK(rendered.find("[redacted: sensitive log field]") != std::string::npos);

    // Rebuild from the internal LogEntry cache. A raw secret retained only in
    // the cache would become visible when filtering by that secret.
    console.onFilterChanged(QString::fromLatin1(kSecret));
    TEST_CHECK(view->toPlainText().isEmpty());
    console.onFilterChanged(QStringLiteral("redacted"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("sensitive log field")));

    console.clearLogs();
    constexpr const char* kHeartbeatSecret = "synthetic-heartbeat-ui-secret";
    MeetingUI::LogToConsole(
        MeetingUI::LogCategory::Connection,
        QStringLiteral("HEARTBEAT_FAILURE"),
        QStringLiteral("heartbeat timer error: Authorization: Bearer synthetic-heartbeat-ui-secret"));
    DrainEvents();
    console.drainPending();

    const auto heartbeat_rendered = view->toPlainText().toStdString();
    TEST_CHECK(heartbeat_rendered.find(kHeartbeatSecret) == std::string::npos);
    TEST_CHECK(heartbeat_rendered.find("[redacted: sensitive log field]") != std::string::npos);
    console.onFilterChanged(QString::fromLatin1(kHeartbeatSecret));
    TEST_CHECK(view->toPlainText().isEmpty());
    console.onFilterChanged(QStringLiteral("redacted"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("sensitive log field")));

    console.clearLogs();
    console.onFilterChanged({});
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant,
        QStringLiteral("CHAT_TEXT_SENT"), QStringLiteral("bytes=21"));
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection,
        QStringLiteral("HANDSHAKE_FAILURE"), QStringLiteral("private ordinary name 9281"));
    console.drainPending();
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("bytes=21")));
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("private ordinary name 9281")));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("unregistered diagnostic")));

    console.clearLogs();
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection,
        QStringLiteral("HANDSHAKE_FAILURE"),
        QStringLiteral("private participant 4827 access_token=synthetic-secret"));
    console.drainPending();
    TEST_CHECK(!view->toPlainText().contains(QStringLiteral("private participant 4827")));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("sensitive log field")));

    console.clearLogs();
    std::thread flood([] {
        for (int i = 0; i < 10000; ++i) {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::General,
                QStringLiteral("FLOOD"), QStringLiteral("safe fixed event"));
        }
    });
    flood.join();
    console.drainPending();
    auto *status = console.findChild<QLabel*>(QStringLiteral("consoleLossStatus"));
    TEST_CHECK(status && status->text().contains(QStringLiteral("UI loss=")));
    TEST_CHECK(!status->text().contains(QStringLiteral("UI loss=0,")));

    console.clearLogs();
    return 0;
}
