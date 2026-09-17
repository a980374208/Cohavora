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

    const auto heartbeat_rendered = view->toPlainText().toStdString();
    TEST_CHECK(heartbeat_rendered.find(kHeartbeatSecret) == std::string::npos);
    TEST_CHECK(heartbeat_rendered.find("[redacted: sensitive log field]") != std::string::npos);
    console.onFilterChanged(QString::fromLatin1(kHeartbeatSecret));
    TEST_CHECK(view->toPlainText().isEmpty());
    console.onFilterChanged(QStringLiteral("redacted"));
    TEST_CHECK(view->toPlainText().contains(QStringLiteral("sensitive log field")));

    console.clearLogs();
    return 0;
}
