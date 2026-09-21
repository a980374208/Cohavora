#include <QtCore/QCoreApplication>
#include "src/ui/meeting_log_console.h"
#include "src/ui/app_theme.h"
#include "src/telemetry/log_redaction.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QApplication>
#include <QtGui/QClipboard>
#include <QtGui/QTextCursor>
#include <QtGui/QFont>

namespace MeetingUI {

MeetingLogConsoleWindow& MeetingLogConsoleWindow::Instance() {
	static MeetingLogConsoleWindow instance;
	return instance;
}

void LogToConsole(LogCategory cat, const QString &tag, const QString &msg) {
	MeetingLogConsoleWindow::Instance().appendLog(cat, tag, msg);
}

MeetingLogConsoleWindow::MeetingLogConsoleWindow(QWidget *parent)
	: QDialog(parent) {
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	setWindowTitle(QCoreApplication::translate("MeetingUI", "LiveKit Console / Debug Logs"));
	resize(780, 520);
	setMinimumSize(600, 380);
	initUi();
	AppTheme::makeDialogAdaptive(*this, QSize(780, 520));
}

void MeetingLogConsoleWindow::initUi() {
	MeetingUI::AppTheme::setStyleVariant(*this, "meeting-log-console-this");

	auto mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(10);

	// 顶部工具条
	auto topLayout = new QHBoxLayout();
	_statusLabel = new QLabel(QCoreApplication::translate("MeetingUI", "● Console Ready"), this);
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-log-console-statuslabel");
	topLayout->addWidget(_statusLabel);

	topLayout->addStretch();

	_filterInput = new QLineEdit(this);
	_filterInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search or filter logs..."));
	_filterInput->setClearButtonEnabled(true);
	_filterInput->setMinimumWidth(200);
	topLayout->addWidget(_filterInput);

	_autoScrollBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Auto-scroll"), this);
	_autoScrollBox->setChecked(true);
	topLayout->addWidget(_autoScrollBox);

	_copyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Copy All"), this);
	_clearBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear"), this);
	topLayout->addWidget(_copyBtn);
	topLayout->addWidget(_clearBtn);

	mainLayout->addLayout(topLayout);

	// 控制台文本区
	_logView = new QPlainTextEdit(this);
	_logView->setReadOnly(true);
	_logView->setMaximumBlockCount(3000); // 限制最多保留 3000 行
	mainLayout->addWidget(_logView);

	connect(_filterInput, &QLineEdit::textChanged, this, &MeetingLogConsoleWindow::onFilterChanged);
	connect(_clearBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::clearLogs);
	connect(_copyBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::copyAllLogs);

	// 欢迎信息
	appendLog(LogCategory::General, "SYSTEM", QCoreApplication::translate("MeetingUI", "LiveKit console started. Listening for signaling, WebRTC media, and device events..."));
}

void MeetingLogConsoleWindow::appendLog(LogCategory category, const QString &tag, const QString &message) {
	const QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
	const QString safeTag = QString::fromStdString(
		livekit::secure_log::SanitizeForOutput(tag.toStdString()));
	const QString safeMessage = QString::fromStdString(
		livekit::secure_log::SanitizeForOutput(message.toStdString()));
	QString catName;
	const QString formatted = formatLogHtml(timeStr, category, safeTag, safeMessage, &catName);

	LogEntry entry;
	entry.timeStr = timeStr;
	entry.category = category;
	entry.tag = safeTag;
	entry.message = safeMessage;
	entry.catName = catName;
	entry.formattedHtml = formatted;
	entry.fullText = QString("[%1] [%2] [%3] %4").arg(timeStr, catName, safeTag, safeMessage);

	QMetaObject::invokeMethod(this, [this, entry = std::move(entry)]() {
		QMutexLocker locker(&_mutex);
		_logEntries.push_back(entry);
		if (_logEntries.size() > kMaxLogEntries) {
			_logEntries.erase(_logEntries.begin());
		}

		bool match = true;
		if (!_currentFilter.isEmpty()) {
			match = entry.fullText.contains(_currentFilter, Qt::CaseInsensitive);
		}

		if (match && _logView) {
			_logView->appendHtml(entry.formattedHtml);
			if (_autoScrollBox && _autoScrollBox->isChecked()) {
				_logView->moveCursor(QTextCursor::End);
			}
		}
	}, Qt::QueuedConnection);
}

void MeetingLogConsoleWindow::onFilterChanged(const QString &filterText) {
	QMutexLocker locker(&_mutex);
	_currentFilter = filterText.trimmed();
	rebuildLogView();
}

void MeetingLogConsoleWindow::rebuildLogView() {
	if (!_logView) return;

	_logView->clear();
	int matchedCount = 0;

	for (const auto &entry : _logEntries) {
		if (_currentFilter.isEmpty() || entry.fullText.contains(_currentFilter, Qt::CaseInsensitive)) {
			_logView->appendHtml(entry.formattedHtml);
			matchedCount++;
		}
	}

	if (_statusLabel) {
		if (_currentFilter.isEmpty()) {
			_statusLabel->setText(QCoreApplication::translate("MeetingUI", "● Console Ready (%1 entries)").arg(_logEntries.size()));
		} else {
			_statusLabel->setText(QCoreApplication::translate("MeetingUI", "● Filtered: %1/%2 entries").arg(matchedCount).arg(_logEntries.size()));
		}
	}

	if (_autoScrollBox && _autoScrollBox->isChecked()) {
		_logView->moveCursor(QTextCursor::End);
	}
}

QString MeetingLogConsoleWindow::formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message, QString *outCatName) {
	QString color = "#d1d5db"; // 默认浅白
	QString catName = "INFO";

	switch (category) {
	case LogCategory::General:
		color = "#86909c"; catName = "GEN"; break;
	case LogCategory::Connection:
		color = "#14C9C9"; catName = "CONN"; break;
	case LogCategory::Signal:
		color = "#165DFF"; catName = "SIGNAL"; break;
	case LogCategory::WebRTC:
		color = "#722ED1"; catName = "WEBRTC"; break;
	case LogCategory::Media:
		color = "#00B42A"; catName = "MEDIA"; break;
	case LogCategory::Track:
		color = "#F7BA1E"; catName = "TRACK"; break;
	case LogCategory::Participant:
		color = "#3491FA"; catName = "USER"; break;
	case LogCategory::Error:
		color = "#F53F3F"; catName = "ERROR"; break;
	}

	if (outCatName) {
		*outCatName = catName;
	}

	return QString(R"(<span style="color:#595e6d;">[%1]</span> <span style="color:%2; font-weight:bold;">[%3]</span> <span style="color:#86909c;">[%4]</span> <span style="color:%2;">%5</span>)")
		.arg(timeStr)
		.arg(color)
		.arg(catName)
		.arg(tag.toHtmlEscaped())
		.arg(message.toHtmlEscaped());
}

void MeetingLogConsoleWindow::clearLogs() {
	QMutexLocker locker(&_mutex);
	_logEntries.clear();
	if (_logView) {
		_logView->clear();
	}
	if (_statusLabel) {
		_statusLabel->setText(QCoreApplication::translate("MeetingUI", "● Console Ready (0 entries)"));
	}
}

void MeetingLogConsoleWindow::copyAllLogs() {
	QMutexLocker locker(&_mutex);
	if (_logView) {
		QApplication::clipboard()->setText(_logView->toPlainText());
	}
}

void MeetingLogConsoleWindow::resizeEvent(QResizeEvent *e) {
	QDialog::resizeEvent(e);
}

void MeetingLogConsoleWindow::closeEvent(QCloseEvent *e) {
	hide();
	e->ignore();
}

} // namespace MeetingUI
