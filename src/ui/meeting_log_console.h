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

protected:
	void resizeEvent(QResizeEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

private:
	void initUi();
	QString formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message);

	QPlainTextEdit *_logView = nullptr;
	QPushButton *_clearBtn = nullptr;
	QPushButton *_copyBtn = nullptr;
	QCheckBox *_autoScrollBox = nullptr;
	QLabel *_statusLabel = nullptr;
	QLineEdit *_filterInput = nullptr;

	QMutex _mutex;
};

// 全局便捷日志输出宏
void LogToConsole(LogCategory cat, const QString &tag, const QString &msg);

} // namespace MeetingUI
