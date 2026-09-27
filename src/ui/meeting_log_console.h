#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include "src/telemetry/diagnostic_event.h"
#include "src/telemetry/diagnostic_file_sink.h"
#include <deque>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace MeetingUI {

enum class LogCategory {
	General,
	Connection,
	Signal,
	WebRTC,
	Media,
	Track,
	Participant,
	Error
};

class MeetingLogConsoleWindow : public QDialog {
	Q_OBJECT
public:
	static MeetingLogConsoleWindow& Instance();

	explicit MeetingLogConsoleWindow(QWidget *parent = nullptr);
	~MeetingLogConsoleWindow() override;

	void appendLog(LogCategory category, const QString &tag, const QString &message);
	static std::function<void(const livekit::diagnostic::Event&)> diagnosticMirror();
	static void enqueueLog(LogCategory category, const QString &tag, const QString &message);
	static MeetingLogConsoleWindow *Active() noexcept;

public slots:
	void clearLogs();
	void copyAllLogs();
	void onFilterChanged(const QString &filterText);
	void drainPending();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

private:
	struct LogEntry {
		QString timeStr;
		LogCategory category;
		QString tag;
		QString message;
		QString catName;
		QString formattedHtml;
		QString fullText;
		QString severity;
		QString component;
		QString sessionId;
		QString operationId;
		size_t chargeBytes = 0;
	};
	struct PendingEntry {
		LogCategory category = LogCategory::General;
		QString tag;
		QString message;
		QString timeStr;
		QString severity;
		QString component;
		QString sessionId;
		QString operationId;
		size_t chargeBytes = 0;
	};
	struct SharedQueue {
		std::mutex mutex;
		std::deque<PendingEntry> pending;
		std::future<livekit::diagnostic::DiagnosticClearResult> clearFuture;
		size_t pendingBytes = 0;
		quint64 dropped = 0;
		quint64 pipelineDropped = 0;
		bool closed = false;
	};
	static std::shared_ptr<SharedQueue> sharedQueue();

	void initUi();
	static void offer(const std::shared_ptr<SharedQueue> &queue, PendingEntry entry);
	bool appendVisible(PendingEntry entry);
	QString formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message, QString *outCatName = nullptr);
	void rebuildLogView();
	bool matchesFilter(const LogEntry &entry) const;
	void showStorageMessage(QString message);

	QPlainTextEdit *_logView = nullptr;
	QPushButton *_clearBtn = nullptr;
	QPushButton *_copyBtn = nullptr;
	QCheckBox *_autoScrollBox = nullptr;
	QLabel *_statusLabel = nullptr;
	QLineEdit *_filterInput = nullptr;
	QLineEdit *_sessionInput = nullptr;
	QLineEdit *_operationInput = nullptr;
	QComboBox *_severityFilter = nullptr;
	QComboBox *_componentFilter = nullptr;
	QComboBox *_copyScope = nullptr;
	QCheckBox *_saveLogsBox = nullptr;
	QCheckBox *_crashCollectionBox = nullptr;
	QCheckBox *_diagnosticModeBox = nullptr;
	QPushButton *_clearSavedBtn = nullptr;
	QLabel *_storageStatusLabel = nullptr;
	std::chrono::steady_clock::time_point _storageMessageUntil{};

	std::deque<LogEntry> _logEntries;
	size_t _cacheBytes = 0;
	size_t _visibleCount = 0;
	QString _currentFilter;
	static constexpr size_t kMaxLogEntries = 5000;
	static constexpr size_t kMaxCacheBytes = 8 * 1024 * 1024;
	static constexpr size_t kMaxPendingEntries = 2048;
	static constexpr size_t kMaxPendingBytes = 2 * 1024 * 1024;

	std::shared_ptr<SharedQueue> _queue = sharedQueue();
	QTimer *_drainTimer = nullptr;
};

// 全局便捷日志输出宏
void LogToConsole(LogCategory cat, const QString &tag, const QString &msg);

} // namespace MeetingUI
