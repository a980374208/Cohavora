#include "src/ui/meeting_log_console.h"
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
	setWindowTitle(QString::fromUtf8("LiveKit 实时控制台 / 调试日志"));
	resize(780, 520);
	setMinimumSize(600, 380);
	initUi();
}

void MeetingLogConsoleWindow::initUi() {
	setStyleSheet(R"(
		QDialog {
			background-color: #18191f;
			color: #e5e6eb;
			font-family: "Consolas", "Courier New", "Microsoft YaHei", monospace;
		}
		QPlainTextEdit {
			background-color: #121316;
			color: #d1d5db;
			border: 1px solid #2d3039;
			border-radius: 6px;
			font-size: 12px;
			line-height: 1.4;
			padding: 8px;
			selection-background-color: #1677ff;
		}
		QPushButton {
			background-color: #272a34;
			color: #e5e6eb;
			border: 1px solid #3c404d;
			border-radius: 6px;
			padding: 6px 14px;
			font-size: 12px;
		}
		QPushButton:hover {
			background-color: #363a47;
		}
		QLineEdit {
			background-color: #1e2027;
			border: 1px solid #2d3039;
			border-radius: 6px;
			padding: 4px 10px;
			color: #ffffff;
			font-size: 12px;
		}
		QCheckBox {
			color: #86909c;
			font-size: 12px;
		}
	)");

	auto mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(10);

	// 顶部工具条
	auto topLayout = new QHBoxLayout();
	_statusLabel = new QLabel(QString::fromUtf8("● 控制台就绪"), this);
	_statusLabel->setStyleSheet("color: #00b42a; font-weight: bold; font-size: 13px;");
	topLayout->addWidget(_statusLabel);

	topLayout->addStretch();

	_filterInput = new QLineEdit(this);
	_filterInput->setPlaceholderText(QString::fromUtf8("过滤日志关键词..."));
	_filterInput->setFixedWidth(160);
	topLayout->addWidget(_filterInput);

	_autoScrollBox = new QCheckBox(QString::fromUtf8("自动滚屏"), this);
	_autoScrollBox->setChecked(true);
	topLayout->addWidget(_autoScrollBox);

	_copyBtn = new QPushButton(QString::fromUtf8("复制全部"), this);
	_clearBtn = new QPushButton(QString::fromUtf8("清空"), this);
	topLayout->addWidget(_copyBtn);
	topLayout->addWidget(_clearBtn);

	mainLayout->addLayout(topLayout);

	// 控制台文本区
	_logView = new QPlainTextEdit(this);
	_logView->setReadOnly(true);
	_logView->setMaximumBlockCount(3000); // 限制最多保留 3000 行
	mainLayout->addWidget(_logView);

	connect(_clearBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::clearLogs);
	connect(_copyBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::copyAllLogs);

	// 欢迎信息
	appendLog(LogCategory::General, "SYSTEM", QString::fromUtf8("LiveKit 客户端控制台已启动，实时监听信令、WebRTC 媒体与设备事件..."));
}

void MeetingLogConsoleWindow::appendLog(LogCategory category, const QString &tag, const QString &message) {
	const QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
	const QString formatted = formatLogHtml(timeStr, category, tag, message);

	QMetaObject::invokeMethod(this, [this, formatted]() {
		QMutexLocker locker(&_mutex);
		if (_logView) {
			_logView->appendHtml(formatted);
			if (_autoScrollBox && _autoScrollBox->isChecked()) {
				_logView->moveCursor(QTextCursor::End);
			}
		}
	}, Qt::QueuedConnection);
}

QString MeetingLogConsoleWindow::formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message) {
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

	return QString(R"(<span style="color:#595e6d;">[%1]</span> <span style="color:%2; font-weight:bold;">[%3]</span> <span style="color:#86909c;">[%4]</span> <span style="color:%2;">%5</span>)")
		.arg(timeStr)
		.arg(color)
		.arg(catName)
		.arg(tag.toHtmlEscaped())
		.arg(message.toHtmlEscaped());
}

void MeetingLogConsoleWindow::clearLogs() {
	QMutexLocker locker(&_mutex);
	if (_logView) {
		_logView->clear();
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
