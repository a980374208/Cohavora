#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QCheckBox>
#include <QtCore/QDateTime>
#include <QtCore/QMutex>
#include <memory>

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
	~MeetingLogConsoleWindow() override = default;

	void appendLog(LogCategory category, const QString &tag, const QString &message);

public slots:
	void clearLogs();
	void copyAllLogs();
	void onFilterChanged(const QString &filterText);

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
	};

	void initUi();
	QString formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message, QString *outCatName = nullptr);
	void rebuildLogView();

	QPlainTextEdit *_logView = nullptr;
	QPushButton *_clearBtn = nullptr;
	QPushButton *_copyBtn = nullptr;
	QCheckBox *_autoScrollBox = nullptr;
	QLabel *_statusLabel = nullptr;
	QLineEdit *_filterInput = nullptr;

	std::vector<LogEntry> _logEntries;
	QString _currentFilter;
	static constexpr size_t kMaxLogEntries = 5000;

	QMutex _mutex;
};

// 全局便捷日志输出宏
void LogToConsole(LogCategory cat, const QString &tag, const QString &msg);

} // namespace MeetingUI
