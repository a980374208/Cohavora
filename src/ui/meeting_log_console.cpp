#include <QtCore/QCoreApplication>
#include "src/ui/meeting_log_console.h"
#include "src/ui/app_theme.h"
#include "src/telemetry/log_redaction.h"
#include "src/telemetry/crash_evidence_provider.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QScrollBar>
#include <QtGui/QClipboard>
#include <QtGui/QIcon>
#include <QtGui/QTextCursor>
#include <QtGui/QTextBlock>
#include <QtGui/QShowEvent>
#include <algorithm>
#include <array>
#include <vector>
#include <QtGui/QFont>
#include <atomic>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace {

struct ConsoleStorageRoots final {
	std::filesystem::path diagnostics;
	std::filesystem::path crashEvidence;
};
std::unique_ptr<const ConsoleStorageRoots> g_consoleStorageRoots;

struct LogBlockId final : QTextBlockUserData {
    explicit LogBlockId(quint64 value) : id(value) {}
    quint64 id;
};
std::atomic<MeetingUI::MeetingLogConsoleWindow*> g_activeConsole{nullptr};

QPointer<MeetingUI::MeetingLogConsoleWindow>& SingletonConsole() {
	static QPointer<MeetingUI::MeetingLogConsoleWindow> instance;
	return instance;
}

QString SafeLegacyMessage(const QString &tag, const QString &message) {
	const auto scrubbed = QString::fromStdString(
		livekit::secure_log::SanitizeForOutput(message.toStdString()));
	if (scrubbed != message)
		return QStringLiteral("[redacted: sensitive log field]");
	if ((tag == QStringLiteral("SHUTDOWN") &&
		 message == QStringLiteral("native_cleanup_failed")) ||
		(tag == QStringLiteral("SESSION") &&
		 message == QStringLiteral("session_invalidated")) ||
		(tag == QStringLiteral("LOGIN") &&
		 (message == QStringLiteral("debug_login_options_invalid") ||
		  message == QStringLiteral("debug_login_failed")))) return message;
	if (tag.startsWith(QStringLiteral("CHAT_"))) {
		if (message == QStringLiteral("transfer_interrupted")) return message;
		const auto parts = message.split(' ');
		bool sizeOk = false;
		if (parts.size() == 1 && parts[0].startsWith(QStringLiteral("bytes="))) {
			const auto size = parts[0].mid(6).toULongLong(&sizeOk);
			if (sizeOk) return QStringLiteral("bytes=%1").arg(size);
		}
		if (parts.size() == 2 &&
			(parts[0] == QStringLiteral("type=image") ||
			 parts[0] == QStringLiteral("type=file")) &&
			parts[1].startsWith(QStringLiteral("bytes="))) {
			const auto size = parts[1].mid(6).toULongLong(&sizeOk);
			if (sizeOk) return QStringLiteral("%1 bytes=%2").arg(parts[0]).arg(size);
		}
	}
	return {};
}

QString SafeTag(const QString &tag) {
	if (tag.isEmpty() || tag.size() > 64) return QStringLiteral("UNKNOWN");
	for (const auto ch : tag) {
		if (!ch.isUpper() && !ch.isDigit() && ch != '_')
			return QStringLiteral("UNKNOWN");
	}
	return tag;
}

} // namespace

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;
namespace {
constexpr std::array ConsoleColorRoles = {
    Theme::ColorRole::TextMuted, Theme::ColorRole::TextSecondary,
    Theme::ColorRole::Accent, Theme::ColorRole::Success,
    Theme::ColorRole::Warning, Theme::ColorRole::Danger,
    Theme::ColorRole::Text,
};
auto ConsoleThemeColors() {
    std::array<QColor, ConsoleColorRoles.size()> result;
    for (size_t i = 0; i != result.size(); ++i) result[i] = Theme::color(ConsoleColorRoles[i]);
    return result;
}
QString ConsoleCategoryName(LogCategory category) {
	switch (category) {
	case LogCategory::General: return QStringLiteral("GEN");
	case LogCategory::Connection: return QStringLiteral("CONN");
	case LogCategory::Signal: return QStringLiteral("SIGNAL");
	case LogCategory::WebRTC: return QStringLiteral("WEBRTC");
	case LogCategory::Media: return QStringLiteral("MEDIA");
	case LogCategory::Track: return QStringLiteral("TRACK");
	case LogCategory::Participant: return QStringLiteral("USER");
	case LogCategory::Error: return QStringLiteral("ERROR");
	}
	return QStringLiteral("INFO");
}
QString ConsoleContext(const QString &sessionId, const QString &operationId) {
	QString context;
	if (!sessionId.isEmpty()) context += QStringLiteral(" session=") + sessionId;
	if (!operationId.isEmpty()) context += QStringLiteral(" operation=") + operationId;
	return context;
}
void SetLabelTextIfChanged(QLabel *label, const QString &text) {
	if (label && label->text() != text) label->setText(text);
}
} // namespace

bool MeetingLogConsoleWindow::ConfigureStorageRoots(
		std::filesystem::path diagnostics, std::filesystem::path crashEvidence) {
	if (!qApp || qApp->thread() != QThread::currentThread() ||
		g_consoleStorageRoots || Active() || diagnostics.empty()) return false;
	g_consoleStorageRoots = std::make_unique<const ConsoleStorageRoots>(
		ConsoleStorageRoots{std::move(diagnostics), std::move(crashEvidence)});
	return true;
}

MeetingLogConsoleWindow& MeetingLogConsoleWindow::Instance() {
	Q_ASSERT(qApp && qApp->thread() == QThread::currentThread());
	auto& instance = SingletonConsole();
	if (!instance) {
		instance = new MeetingLogConsoleWindow;
		// QApplication calls post routines before accessibility/platform teardown.
		// A function-static QWidget instead survives until CRT static teardown.
		qAddPostRoutine(&MeetingLogConsoleWindow::DestroyInstance);
	}
	return *instance;
}

void MeetingLogConsoleWindow::DestroyInstance() {
	qRemovePostRoutine(&MeetingLogConsoleWindow::DestroyInstance);
	delete SingletonConsole().data();
}

void LogToConsole(LogCategory cat, const QString &tag, const QString &msg) {
	MeetingLogConsoleWindow::enqueueLog(cat, tag, msg);
}

MeetingLogConsoleWindow *MeetingLogConsoleWindow::Active() noexcept {
	return g_activeConsole.load(std::memory_order_acquire);
}

std::shared_ptr<MeetingLogConsoleWindow::SharedQueue>
MeetingLogConsoleWindow::sharedQueue() {
	static auto* queue = new std::shared_ptr<SharedQueue>(
		std::make_shared<SharedQueue>());
	return *queue;
}

MeetingLogConsoleWindow::MeetingLogConsoleWindow(QWidget *parent)
	: QDialog(parent)
	, _diagnosticRoot(g_consoleStorageRoots ? g_consoleStorageRoots->diagnostics
		: std::filesystem::path(QDir(QStandardPaths::writableLocation(
			QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("diagnostics")).toStdWString()))
	, _crashRoot(g_consoleStorageRoots ? g_consoleStorageRoots->crashEvidence
		: livekit::telemetry::CrashEvidenceProvider::DefaultRoot()) {
	setObjectName(QStringLiteral("meetingLogConsole"));
	AppTheme::configureModelessWindow(*this);
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora Console / Debug Logs"));
	resize(780, 520);
	setMinimumSize(600, 380);
	initUi();
	_refreshThemeProjection = [this, previousColors = ConsoleThemeColors()]() mutable {
		const auto colors = ConsoleThemeColors();
		if (colors == previousColors) return;
		const auto selection = _logView->textCursor();
		const auto scrollPosition = _logView->verticalScrollBar()->value();
		struct ColorRange { int position; int length; size_t role; };
		std::vector<ColorRange> ranges;
		for (auto block = _logView->document()->begin(); block.isValid(); block = block.next()) {
			for (auto it = block.begin(); !it.atEnd(); ++it) {
				const auto fragment = it.fragment();
				if (!fragment.isValid()) continue;
				const auto foreground = fragment.charFormat().foreground().color();
				for (size_t i = 0; i != previousColors.size(); ++i) {
					if (foreground == previousColors[i]) {
						ranges.push_back({fragment.position(), fragment.length(), i});
						break;
					}
				}
			}
		}
		// Change formatting in place so switching themes retains selection and scroll position.
		QTextCursor cursor(_logView->document());
		cursor.beginEditBlock();
		for (const auto &range : ranges) {
			cursor.setPosition(range.position);
			cursor.setPosition(range.position + range.length, QTextCursor::KeepAnchor);
			QTextCharFormat format;
			format.setForeground(colors[range.role]);
			cursor.mergeCharFormat(format);
		}
		cursor.endEditBlock();
		_logView->setTextCursor(selection);
		_logView->verticalScrollBar()->setValue(scrollPosition);
		previousColors = colors;
	};
	Theme::observeChanges(*this, [this] {
		if (isVisible()) _refreshThemeProjection();
	});
	AppTheme::makeDialogAdaptive(*this, QSize(780, 520));
	g_activeConsole.store(this, std::memory_order_release);
	_drainTimer = new QTimer(this);
	_drainTimer->setInterval(50);
	connect(_drainTimer, &QTimer::timeout, this, &MeetingLogConsoleWindow::drainPending);
	_drainTimer->start();
}

MeetingLogConsoleWindow::~MeetingLogConsoleWindow() {
	g_activeConsole.store(nullptr, std::memory_order_release);
}

void MeetingLogConsoleWindow::initUi() {
	MeetingUI::AppTheme::setStyleVariant(*this, "meeting-log-console-this");

	auto mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(10);

	// 顶部工具条
	auto topLayout = new QHBoxLayout();
	_statusLabel = new QLabel(QCoreApplication::translate("MeetingUI", "● Console Ready"), this);
	_statusLabel->setObjectName(QStringLiteral("consoleLossStatus"));
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-log-console-statuslabel");
	topLayout->addWidget(_statusLabel);

	topLayout->addStretch();

	_filterInput = new QLineEdit(this);
	_filterInput->setObjectName(QStringLiteral("consoleTextFilter"));
	_filterInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Search or filter logs..."));
	_filterInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search or filter logs..."));
	_filterInput->setClearButtonEnabled(true);
	_filterInput->setMinimumWidth(200);
	topLayout->addWidget(_filterInput);

	_autoScrollBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Auto-scroll"), this);
	_autoScrollBox->setObjectName(QStringLiteral("consoleAutoScroll"));
	_autoScrollBox->setChecked(true);
	topLayout->addWidget(_autoScrollBox);

	_copyScope = new QComboBox(this);
	_copyScope->setObjectName(QStringLiteral("consoleCopyScope"));
	_copyScope->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copy scope"));
	_copyScope->addItem(QCoreApplication::translate("MeetingUI", "Visible results"));
	_copyScope->addItem(QCoreApplication::translate("MeetingUI", "Selection"));
	_copyBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")),
		QCoreApplication::translate("MeetingUI", "Copy"), this);
	_clearBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear"), this);
	_copyBtn->setObjectName(QStringLiteral("consoleCopy"));
	_clearBtn->setObjectName(QStringLiteral("consoleClear"));
	topLayout->addWidget(_copyScope);
	topLayout->addWidget(_copyBtn);
	topLayout->addWidget(_clearBtn);

	mainLayout->addLayout(topLayout);
	auto *filters = new QHBoxLayout();
	_severityFilter = new QComboBox(this);
	_severityFilter->setObjectName(QStringLiteral("consoleSeverityFilter"));
	_severityFilter->setAccessibleName(QCoreApplication::translate("MeetingUI", "Severity filter"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "All levels"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Trace"), QStringLiteral("trace"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Debug"), QStringLiteral("debug"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Info"), QStringLiteral("info"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Warning"), QStringLiteral("warning"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Error"), QStringLiteral("error"));
	_severityFilter->addItem(QCoreApplication::translate("MeetingUI", "Fatal"), QStringLiteral("fatal"));
	_componentFilter = new QComboBox(this);
	_componentFilter->setObjectName(QStringLiteral("consoleComponentFilter"));
	_componentFilter->setAccessibleName(QCoreApplication::translate("MeetingUI", "Component filter"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "All components"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Legacy"), QStringLiteral("legacy"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Application"), QStringLiteral("app"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Diagnostic pipeline"), QStringLiteral("diagnostic_pipeline"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Network"), QStringLiteral("net"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Meeting coordinator"), QStringLiteral("meeting_coordinator"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Room"), QStringLiteral("room"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Participant"), QStringLiteral("participant"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Session runtime"), QStringLiteral("session_runtime"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Session telemetry"), QStringLiteral("session_telemetry"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Meeting UI"), QStringLiteral("meeting_ui"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Media"), QStringLiteral("media"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "Render"), QStringLiteral("render"));
	_componentFilter->addItem(QCoreApplication::translate("MeetingUI", "RTC"), QStringLiteral("rtc"));
	_sessionInput = new QLineEdit(this);
	_sessionInput->setObjectName(QStringLiteral("consoleSessionFilter"));
	_sessionInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Session ID"));
	_sessionInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Session ID"));
	_sessionInput->setClearButtonEnabled(true);
	_operationInput = new QLineEdit(this);
	_operationInput->setObjectName(QStringLiteral("consoleOperationFilter"));
	_operationInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Operation ID"));
	_operationInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Operation ID"));
	_operationInput->setClearButtonEnabled(true);
	filters->addWidget(_severityFilter);
	filters->addWidget(_componentFilter);
	filters->addWidget(_sessionInput);
	filters->addWidget(_operationInput);
	mainLayout->addLayout(filters);
	auto *retention = new QHBoxLayout();
	const auto pipeline = livekit::diagnostic::InstalledBusinessPipeline();
	const std::weak_ptr<livekit::diagnostic::DiagnosticPipeline> weakPipeline =
		pipeline;
	_saveLogsBox = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Save local logs"), this);
	_saveLogsBox->setObjectName(QStringLiteral("consoleSaveLogs"));
	_saveLogsBox->setChecked(pipeline && pipeline->GetStatus().retention_enabled);
	_saveLogsBox->setEnabled(pipeline != nullptr);
	retention->addWidget(_saveLogsBox);
	if (qApp->arguments().contains(QStringLiteral("--debug")) &&
		qEnvironmentVariable("LIVEKIT_UIA_LOG_PAIR") == QStringLiteral("1")) {
		auto* production = new QCheckBox(QCoreApplication::translate(
			"MeetingUI", "Collect diagnostic events"), this);
		production->setObjectName(QStringLiteral("consoleCollectDiagnostics"));
		production->setChecked(true);
		production->setEnabled(pipeline != nullptr);
		retention->addWidget(production);
		auto* restore = new QTimer(production);
		restore->setSingleShot(true);
		connect(restore, &QTimer::timeout, production, [production] { production->setChecked(true); });
		connect(production, &QCheckBox::toggled, production, [weakPipeline, restore](bool enabled) {
			if (const auto active = weakPipeline.lock())
				active->PauseProductionForBenchmark(std::chrono::milliseconds(enabled ? 0 : 60000));
			if (enabled) restore->stop(); else restore->start(60000);
		});
		connect(production, &QObject::destroyed, [weakPipeline] {
			if (const auto active = weakPipeline.lock())
				active->PauseProductionForBenchmark(std::chrono::milliseconds::zero());
		});
	}
	_clearSavedBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Clear previous logs"), this);
	_clearSavedBtn->setObjectName(QStringLiteral("consoleClearPreviousLogs"));
	retention->addWidget(_clearSavedBtn);
	_crashCollectionBox = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Crash metadata next run"), this);
	_crashCollectionBox->setObjectName(QStringLiteral("consoleCrashCollection"));
	const auto crashRoot = _crashRoot;
	_crashCollectionBox->setChecked(!crashRoot.empty() &&
		livekit::telemetry::CrashEvidenceProvider::CollectionEnabled(crashRoot));
	_crashCollectionBox->setEnabled(!crashRoot.empty());
	retention->addWidget(_crashCollectionBox);
	_diagnosticModeBox = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Diagnostic mode"), this);
	_diagnosticModeBox->setObjectName(QStringLiteral("consoleDiagnosticMode"));
	_diagnosticModeBox->setChecked(pipeline && pipeline->DiagnosticWindowActive());
	_diagnosticModeBox->setEnabled(pipeline != nullptr);
	retention->addWidget(_diagnosticModeBox);
	retention->addStretch();
	mainLayout->addLayout(retention);
	_storageStatusLabel = new QLabel(
		QCoreApplication::translate("MeetingUI",
			"Capture: all levels and components"), this);
	_storageStatusLabel->setWordWrap(true);
	mainLayout->addWidget(_storageStatusLabel);
	connect(_saveLogsBox, &QCheckBox::toggled, this,
		[this, weakPipeline](bool enabled) {
			if (const auto active = weakPipeline.lock()) {
				QSettings settings;
				settings.setValue(QStringLiteral("diagnostics/historyEnabled"), enabled);
				settings.sync();
				if (settings.status() != QSettings::NoError) {
					const QSignalBlocker blocked(_saveLogsBox);
					_saveLogsBox->setChecked(!enabled);
					showStorageMessage(QCoreApplication::translate(
						"MeetingUI", "Log retention setting could not be saved"));
					return;
				}
				active->SetRetentionEnabled(enabled);
			}
		});
	connect(_clearSavedBtn, &QPushButton::clicked, this, [this] {
		if (_queue->clearFuture.valid()) return;
		const auto root = _diagnosticRoot;
		try {
			_queue->clearFuture = std::async(std::launch::async, [root] {
				return livekit::diagnostic::DiagnosticFileSink::ClearInactiveHistory(root);
			});
			_clearSavedBtn->setEnabled(false);
			showStorageMessage(QCoreApplication::translate(
				"MeetingUI", "Clearing previous logs..."));
		} catch (...) {
			showStorageMessage(QCoreApplication::translate(
				"MeetingUI", "Log cleanup could not start"));
		}
	});
	connect(_crashCollectionBox, &QCheckBox::toggled, this,
		[this, crashRoot](bool enabled) {
			if (livekit::telemetry::CrashEvidenceProvider::SetCollectionEnabled(
					crashRoot, enabled)) return;
			const QSignalBlocker blocked(_crashCollectionBox);
			_crashCollectionBox->setChecked(!enabled);
			showStorageMessage(QCoreApplication::translate(
				"MeetingUI", "Crash metadata setting could not be saved"));
		});
	connect(_diagnosticModeBox, &QCheckBox::toggled, this,
		[weakPipeline](bool enabled) {
			if (const auto active = weakPipeline.lock())
				active->OpenDiagnosticWindow(enabled
					? std::chrono::minutes(10) : std::chrono::milliseconds::zero());
		});

	// 控制台文本区
	_logView = new QPlainTextEdit(this);
	_logView->setObjectName(QStringLiteral("consoleLogView"));
	_logView->setAccessibleName(QCoreApplication::translate("MeetingUI", "Debug Logs"));
	_logView->setReadOnly(true);
	_logView->setMaximumBlockCount(3000);
	mainLayout->addWidget(_logView);
	connect(_autoScrollBox, &QCheckBox::toggled, this, [this](bool enabled) {
		if (enabled) {
			auto *scroll = _logView->verticalScrollBar();
			scroll->setValue(scroll->maximum());
		}
	});

	_filterTimer = new QTimer(this);
	_filterTimer->setSingleShot(true);
	_filterTimer->setInterval(120);
	connect(_filterTimer, &QTimer::timeout, this, [this] { rebuildLogView(); });
	connect(_filterInput, &QLineEdit::textChanged, this, &MeetingLogConsoleWindow::onFilterChanged);
	connect(_sessionInput, &QLineEdit::textChanged, this,
		[this] { _filterTimer->start(); });
	connect(_operationInput, &QLineEdit::textChanged, this,
		[this] { _filterTimer->start(); });
	connect(_severityFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this] { rebuildLogView(); });
	connect(_componentFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this] { rebuildLogView(); });
	connect(_clearBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::clearLogs);
	connect(_copyBtn, &QPushButton::clicked, this, &MeetingLogConsoleWindow::copyAllLogs);

}

void MeetingLogConsoleWindow::appendLog(LogCategory category, const QString &tag, const QString &message) {
	enqueueLog(category, tag, message);
}

void MeetingLogConsoleWindow::enqueueLog(LogCategory category, const QString &tag,
		const QString &message) {
	PendingEntry entry;
	entry.category = category;
	entry.tag = SafeTag(tag);
	entry.message = SafeLegacyMessage(entry.tag, message);
	// Unknown legacy text has no useful safe payload. Do not let its placeholder
	// flood the bounded queue; retain an error marker without exposing raw text.
	if (entry.message.isEmpty()) {
		if (category != LogCategory::Error) return;
		entry.message = QStringLiteral("[suppressed: unregistered diagnostic]");
	}
	entry.timeStr = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
	entry.severity = category == LogCategory::Error
		? QStringLiteral("error") : QStringLiteral("info");
	entry.component = QStringLiteral("legacy");
	offer(sharedQueue(), std::move(entry));
}

void MeetingLogConsoleWindow::offer(const std::shared_ptr<SharedQueue> &queue,
		PendingEntry entry) {
	entry.chargeBytes = sizeof(PendingEntry) +
		2 * (entry.tag.capacity() + entry.message.capacity() +
		entry.timeStr.capacity() + entry.severity.capacity() +
		entry.component.capacity() + entry.sessionId.capacity() +
		entry.operationId.capacity());
	std::lock_guard lock(queue->mutex);
	if (queue->closed || entry.chargeBytes > kMaxPendingBytes) {
		++queue->dropped;
		return;
	}
	while (!queue->pending.empty() &&
		(queue->pending.size() >= kMaxPendingEntries ||
		 queue->pendingBytes + entry.chargeBytes > kMaxPendingBytes)) {
		queue->pendingBytes -= queue->pending.front().chargeBytes;
		queue->pending.pop_front();
		++queue->dropped;
	}
	queue->pendingBytes += entry.chargeBytes;
	queue->pending.push_back(std::move(entry));
}

std::function<void(const livekit::diagnostic::Event&)>
MeetingLogConsoleWindow::diagnosticMirror() {
	return [queue = sharedQueue()](const livekit::diagnostic::Event &event) {
		using namespace livekit::diagnostic;
		const auto level = EventSeverity(event);
		// Counts remain in the structured file sink. They are not actionable UI
		// transitions and otherwise crowd out connection/media failures.
		if (event.kind == EventKind::SignalMessageSummary && level <= Severity::Info)
			return;
		PendingEntry entry;
		entry.category = level >= Severity::Error || event.kind == EventKind::SinkFailed
			? LogCategory::Error : LogCategory::General;
		const auto name = livekit::diagnostic::EventName(event.kind);
		entry.tag = QString::fromLatin1(name.data(), static_cast<int>(name.size()));
		const auto component = livekit::diagnostic::ComponentName(event.kind);
		entry.component = QString::fromLatin1(component.data(),
			static_cast<int>(component.size()));
		const auto severity = livekit::diagnostic::SeverityName(
			livekit::diagnostic::EventSeverity(event));
		entry.severity = QString::fromLatin1(severity.data(),
			static_cast<int>(severity.size()));
		const auto session = event.context.anonymous_session_id.View();
		entry.sessionId = QString::fromLatin1(session.data(),
			static_cast<int>(session.size()));
		const auto operation = event.context.operation_id.View();
		entry.operationId = QString::fromLatin1(operation.data(),
			static_cast<int>(operation.size()));
		entry.timeStr = QDateTime::fromMSecsSinceEpoch(event.occurred_at_utc_ms)
			.toString("hh:mm:ss.zzz");
		switch (event.kind) {
		case livekit::diagnostic::EventKind::ProcessStarted:
			entry.message = QStringLiteral("build=%1").arg(event.build_id.data()); break;
		case livekit::diagnostic::EventKind::ProcessStopping:
			entry.message = QString::fromLatin1(livekit::diagnostic::ShutdownReasonName(
				event.shutdown_reason).data()); break;
		case livekit::diagnostic::EventKind::ProcessTerminal:
			entry.message = QStringLiteral("outcome=%1 drain=%2")
				.arg(QString::fromLatin1(livekit::diagnostic::OutcomeName(event.outcome).data()))
				.arg(QString::fromLatin1(livekit::diagnostic::DrainResultName(event.drain_result).data()));
			break;
		case livekit::diagnostic::EventKind::QueueSummary:
			entry.message = QStringLiteral("accepted=%1 dropped=%2 high_water=%3")
				.arg(event.accepted).arg(event.dropped).arg(event.queue_high_water); break;
		case livekit::diagnostic::EventKind::SinkFailed:
			entry.message = QString::fromLatin1(livekit::diagnostic::FailureReasonName(
				event.failure_reason).data()); break;
		case livekit::diagnostic::EventKind::SinkRecovered:
			entry.message = QStringLiteral("sink_recovered"); break;
		case livekit::diagnostic::EventKind::ChatReceived:
			entry.message = QStringLiteral("type=%1 bytes=%2")
				.arg(QString::fromLatin1(livekit::diagnostic::ChatKindName(event.chat_kind).data()))
				.arg(event.bytes); break;
		case livekit::diagnostic::EventKind::ProcessIssue:
			entry.category = LogCategory::Error;
			entry.message = QString::fromLatin1(
				livekit::diagnostic::IssueCodeName(event.issue_code).data());
			break;
		case EventKind::RtcSdpStep:
			entry.message = QStringLiteral("%1 %2 pc=%3 seq=%4 type=%5 state=%6->%7 reason=%8 late=%9 rtc_error=%10")
				.arg(QString::fromLatin1(SdpActionName(event.sdp_action).data()))
				.arg(QString::fromLatin1(SdpPhaseName(event.sdp_phase).data()))
				.arg(QString::fromLatin1(SdpRoleName(event.sdp_role).data()))
				.arg(event.sdp_sequence)
				.arg(QString::fromLatin1(SdpDescriptionName(event.sdp_description).data()))
				.arg(QString::fromLatin1(SdpStateName(event.signaling_before).data()))
				.arg(QString::fromLatin1(SdpStateName(event.signaling_after).data()))
				.arg(QString::fromLatin1(SdpReasonName(event.sdp_reason).data()))
				.arg(event.sdp_after_terminal).arg(event.rtc_error_type);
			break;
		case EventKind::RtcLifecycle:
			entry.message = QStringLiteral("status=%1").arg(QString::fromLatin1(RtcStatusName(event.rtc_status).data()));
			break;
		case EventKind::RenderBackendChanged:
			entry.message = QStringLiteral("%1 -> %2 reason=%3")
				.arg(QString::fromLatin1(RenderBackendName(event.from_render_backend).data()),
					QString::fromLatin1(RenderBackendName(event.to_render_backend).data()),
					QString::fromLatin1(RenderReasonName(event.render_reason).data()));
			break;
		case EventKind::MediaSubscriptionChanged:
			entry.message = QStringLiteral("media=%1 state=%2")
				.arg(QString::fromLatin1(MediaKindName(event.media_kind).data()),
					QString::fromLatin1(SubscriptionStateName(event.subscription_state).data()));
			break;
		case EventKind::DeviceSwitchTerminal:
		case EventKind::DeviceCaptureTerminal:
			entry.message = QStringLiteral("media=%1 reason=%2")
				.arg(QString::fromLatin1(MediaKindName(event.media_kind).data()),
					QString::fromLatin1(DeviceSwitchReasonName(event.device_switch_reason).data()));
			break;
		case EventKind::SettingsFirstPaint:
			entry.message = QStringLiteral("measurement_point=click_to_first_qt_paint_completed");
			break;
		case EventKind::SettingsDeviceProbe:
			entry.message = QStringLiteral("media=%1 cache_hit=%2 device_count=%3")
				.arg(QString::fromLatin1(MediaKindName(event.media_kind).data()))
				.arg(event.cache_hit).arg(event.device_count);
			break;
		default: break;
		}
		// Only closed, typed fields may enter the presentation. Never include raw
		// HTTP bodies, exception strings, SDP, or arbitrary producer attributes.
		auto addField = [&entry](const char *key, QString value) {
			if (!entry.message.isEmpty()) entry.message += QLatin1Char(' ');
			entry.message += QString::fromLatin1(key) + QLatin1Char('=') + value;
		};
		if (event.route != Route::Unknown)
			addField("route", QString::fromLatin1(RouteName(event.route).data()));
		if (event.outcome != Outcome::Unknown && event.kind != EventKind::ProcessTerminal)
			addField("outcome", QString::fromLatin1(OutcomeName(event.outcome).data()));
		if (event.stage != Stage::Unknown)
			addField("stage", QString::fromLatin1(StageName(event.stage).data()));
		if (event.error_code != ErrorCode::None)
			addField("error", QString::fromLatin1(ErrorCodeName(event.error_code).data()));
		if (event.http_status != 0)
			addField("http", QString::number(event.http_status));
		if (event.duration_ms != 0 || event.kind == EventKind::SettingsFirstPaint ||
			event.kind == EventKind::SettingsDeviceProbe)
			addField("duration_ms", QString::number(event.duration_ms));
		if (event.kind == livekit::diagnostic::EventKind::QueueSummary) {
			std::lock_guard lock(queue->mutex);
			const auto previousDropped = queue->pipelineDropped;
			queue->pipelineDropped = event.dropped;
			if (previousDropped == event.dropped) return;
		}
		offer(queue, std::move(entry));
	};
}

void MeetingLogConsoleWindow::appendVisible(PendingEntry pending) {
	LogEntry entry;
	entry.id = _nextEntryId++;
	entry.timeStr = std::move(pending.timeStr);
	entry.category = pending.category;
	entry.tag = std::move(pending.tag);
	entry.message = std::move(pending.message);
	entry.severity = std::move(pending.severity);
	entry.component = std::move(pending.component);
	entry.sessionId = std::move(pending.sessionId);
	entry.operationId = std::move(pending.operationId);
	entry.chargeBytes = sizeof(LogEntry) + 2 * (entry.timeStr.capacity() +
		entry.tag.capacity() + entry.message.capacity() +
		entry.severity.capacity() + entry.component.capacity() +
		entry.sessionId.capacity() + entry.operationId.capacity());
	_cacheBytes += entry.chargeBytes;
	_logEntries.push_back(std::move(entry));
	while (_logEntries.size() > kMaxLogEntries || _cacheBytes > kMaxCacheBytes) {
		_cacheBytes -= _logEntries.front().chargeBytes;
		_logEntries.pop_front();
	}
	if (isVisible()) trimVisible();
}

// Block identity survives Qt's own block cap and multiline entries. Eviction
// deletes only the expired prefix, never re-parses all retained HTML.
void MeetingLogConsoleWindow::trimVisible() {
	if (!_logView) return;
	if (_logEntries.empty()) {
		_logView->clear();
		_visibleIds.clear();
	} else {
		auto block = _logView->document()->firstBlock();
		while (block.isValid()) {
			const auto *id = static_cast<LogBlockId*>(block.userData());
			if (!id || id->id >= _logEntries.front().id) break;
			block = block.next();
		}
		if (!block.isValid()) {
			_logView->clear();
			_visibleIds.clear();
		} else if (block.position() > 0) {
			const auto retainedId = static_cast<LogBlockId*>(block.userData())->id;
			QTextCursor cursor(_logView->document());
			cursor.setPosition(block.position(), QTextCursor::KeepAnchor);
			cursor.removeSelectedText();
			_logView->document()->firstBlock().setUserData(new LogBlockId(retainedId));
		}
		const auto *first = static_cast<LogBlockId*>(_logView->document()->firstBlock().userData());
		if (first) while (!_visibleIds.empty() && _visibleIds.front() < first->id)
			_visibleIds.pop_front();
	}
	_visibleCount = _visibleIds.size();
}

void MeetingLogConsoleWindow::renderVisible(QElapsedTimer &elapsed, bool allowHidden) {
	// Preserve the previous projection until the text filter settles.
	if (!_logView || (!isVisible() && !allowHidden) || _pendingRebuild ||
		(_filterTimer && _filterTimer->isActive())) return;
	auto *scroll = _logView->verticalScrollBar();
	const auto previousPosition = scroll->value();
	const bool followTail = _autoScrollBox && _autoScrollBox->isChecked()
		&& previousPosition == scroll->maximum() && !_logView->textCursor().hasSelection();
	bool appended = false;
	auto it = std::lower_bound(_logEntries.begin(), _logEntries.end(), _nextVisibleId,
		[](const LogEntry &entry, quint64 id) { return entry.id < id; });
	for (int count = 0; it != _logEntries.end() && count < 128 && elapsed.elapsed() < 4; ++it, ++count) {
		_nextVisibleId = it->id + 1;
		if (!matchesFilter(*it)) continue;
		_logView->appendHtml(formatLogHtml(it->timeStr, it->category, it->tag,
			it->message + ConsoleContext(it->sessionId, it->operationId)));
		appended = true;
		for (auto block = _logView->document()->lastBlock(); block.isValid() && !block.userData(); block = block.previous())
			block.setUserData(new LogBlockId(it->id));
		_visibleIds.push_back(it->id);
	}
	trimVisible();
	// Idle refreshes must not move the viewport or destroy a text selection.
	// appendHtml can scroll by itself, so also restore the position when paused.
	if (appended) scroll->setValue(followTail ? scroll->maximum() : previousPosition);
}

void MeetingLogConsoleWindow::drainPending() {
	if (_queue->clearFuture.valid() && _queue->clearFuture.wait_for(
			std::chrono::milliseconds::zero()) == std::future_status::ready) {
		const auto result = _queue->clearFuture.get();
		_clearSavedBtn->setEnabled(true);
		showStorageMessage(result.success
			? QCoreApplication::translate("MeetingUI",
				"Cleared %1 saved segments; %2 active runs kept")
				.arg(result.removed_segments).arg(result.active_runs_skipped)
			: QCoreApplication::translate("MeetingUI", "Log cleanup failed: %1")
				.arg(QString::fromStdString(result.reason)));
	}
	QElapsedTimer elapsed;
	elapsed.start();
	for (int count = 0; count < 128 && elapsed.elapsed() < 2; ++count) {
		PendingEntry entry;
		{
			std::lock_guard lock(_queue->mutex);
			if (_queue->pending.empty()) break;
			entry = std::move(_queue->pending.front());
			_queue->pendingBytes -= entry.chargeBytes;
			_queue->pending.pop_front();
		}
		appendVisible(std::move(entry));
	}
	renderVisible(elapsed);
	refreshStatus();
}

void MeetingLogConsoleWindow::refreshStatus() {
	if (!isVisible()) return;
	auto storageText = std::move(_pendingStorageMessage);
	_pendingStorageMessage.clear();
	if (_diagnosticModeBox) {
		if (const auto pipeline = livekit::diagnostic::InstalledBusinessPipeline()) {
			const bool active = pipeline->DiagnosticWindowActive();
			if (_diagnosticModeBox->isChecked() != active) {
				const QSignalBlocker blocked(_diagnosticModeBox);
				_diagnosticModeBox->setChecked(active);
			}
			if (active && !_queue->clearFuture.valid() &&
				std::chrono::steady_clock::now() >= _storageMessageUntil)
				storageText = QCoreApplication::translate(
					"MeetingUI", "Diagnostic mode: %1 min remaining")
					.arg((pipeline->DiagnosticWindowRemaining().count() + 59999) /
						60000);
		}
	}
	if (!storageText.isEmpty()) SetLabelTextIfChanged(_storageStatusLabel, storageText);
	if (_statusLabel) {
		quint64 dropped, pipelineDropped;
		{
			std::lock_guard lock(_queue->mutex);
			dropped = _queue->dropped;
			pipelineDropped = _queue->pipelineDropped;
		}
		SetLabelTextIfChanged(_statusLabel, QCoreApplication::translate("MeetingUI",
			"%1/%2 matched; UI loss=%3, pipeline loss=%4")
			.arg(_visibleCount).arg(_logEntries.size()).arg(dropped).arg(pipelineDropped));
	}
}

void MeetingLogConsoleWindow::showStorageMessage(QString message) {
	if (isVisible()) SetLabelTextIfChanged(_storageStatusLabel, message);
	else _pendingStorageMessage = std::move(message);
	_storageMessageUntil = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
}

void MeetingLogConsoleWindow::onFilterChanged(const QString &filterText) {
	_currentFilter = filterText.trimmed();
	_filterTimer->start();
}

bool MeetingLogConsoleWindow::matchesFilter(const LogEntry &entry) const {
	if (_severityFilter && _severityFilter->currentIndex() > 0 &&
		entry.severity != _severityFilter->currentData().toString()) return false;
	if (_componentFilter && _componentFilter->currentIndex() > 0 &&
		entry.component != _componentFilter->currentData().toString()) return false;
	if (_sessionInput && !_sessionInput->text().trimmed().isEmpty() &&
		!entry.sessionId.contains(_sessionInput->text().trimmed(),
			Qt::CaseInsensitive)) return false;
	if (_operationInput && !_operationInput->text().trimmed().isEmpty() &&
		!entry.operationId.contains(_operationInput->text().trimmed(),
			Qt::CaseInsensitive)) return false;
	if (!_currentFilter.isEmpty()) {
		const auto fullText = QStringLiteral("[%1] [%2] [%3] %4%5")
			.arg(entry.timeStr, ConsoleCategoryName(entry.category), entry.tag,
				entry.message, ConsoleContext(entry.sessionId, entry.operationId));
		if (!fullText.contains(_currentFilter, Qt::CaseInsensitive)) return false;
	}
	return true;
}

void MeetingLogConsoleWindow::rebuildLogView(bool allowHidden) {
	if (_filterTimer) _filterTimer->stop();
	if (!_logView) return;
	if (!isVisible() && !allowHidden) {
		_pendingRebuild = true;
		return;
	}
	_pendingRebuild = false;
	_logView->clear();
	_visibleIds.clear();
	_visibleCount = 0;
	_nextVisibleId = _logEntries.empty() ? _nextEntryId : _logEntries.front().id;
	QElapsedTimer elapsed;
	elapsed.start();
	renderVisible(elapsed, allowHidden); // Remaining work resumes on the regular drain timer.
}

QString MeetingLogConsoleWindow::formatLogHtml(const QString &timeStr, LogCategory category, const QString &tag, const QString &message) {
	QString color = Theme::color(Theme::ColorRole::Text).name();
	const auto catName = ConsoleCategoryName(category);

	switch (category) {
	case LogCategory::General:
		color = Theme::color(Theme::ColorRole::TextSecondary).name(); break;
	case LogCategory::Connection:
		color = Theme::color(Theme::ColorRole::Accent).name(); break;
	case LogCategory::Signal:
		color = Theme::color(Theme::ColorRole::Accent).name(); break;
	case LogCategory::WebRTC:
		color = Theme::color(Theme::ColorRole::Accent).name(); break;
	case LogCategory::Media:
		color = Theme::color(Theme::ColorRole::Success).name(); break;
	case LogCategory::Track:
		color = Theme::color(Theme::ColorRole::Warning).name(); break;
	case LogCategory::Participant:
		color = Theme::color(Theme::ColorRole::Accent).name(); break;
	case LogCategory::Error:
		color = Theme::color(Theme::ColorRole::Danger).name(); break;
	}

	return QString(R"(<span style="color:%6;">[%1]</span> <span style="color:%2; font-weight:bold;">[%3]</span> <span style="color:%7;">[%4]</span> <span style="color:%2;">%5</span>)")
		.arg(timeStr, color, catName, tag.toHtmlEscaped(), message.toHtmlEscaped(),
			Theme::color(Theme::ColorRole::TextMuted).name(),
			Theme::color(Theme::ColorRole::TextSecondary).name());
}

void MeetingLogConsoleWindow::clearLogs() {
	++_clipboardCopyGeneration;
	_clipboardCopyPending = false;
	if (_filterTimer) _filterTimer->stop();
	_pendingRebuild = false;
	{
		std::lock_guard lock(_queue->mutex);
		_queue->pending.clear();
		_queue->pendingBytes = 0;
	}
	_logEntries.clear();
	_visibleIds.clear();
	_nextVisibleId = _nextEntryId;
	_cacheBytes = 0;
	_visibleCount = 0;
	if (_logView) {
		_logView->clear();
	}
	if (_statusLabel) {
		SetLabelTextIfChanged(_statusLabel, QCoreApplication::translate("MeetingUI", "● Console Ready (0 entries)"));
	}
}

void MeetingLogConsoleWindow::copyAllLogs() {
	// A newer explicit action invalidates any delayed copy, including an empty
	// selection. Never let an older retry overwrite the user's newer action.
	++_clipboardCopyGeneration;
	_clipboardCopyPending = false;
	if (!_logView) return;
	if (_copyScope && _copyScope->currentIndex() == 1) {
		const auto selected = _logView->textCursor().selectedText()
			.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
		if (!selected.isEmpty()) copyTextToClipboard(selected);
		return;
	}
	// Explicit copy can request one bounded projection slice while hidden. The
	// selection scope above always reads Qt's selection without altering it.
	// Keep the existing blocks and the remembered projected palette in sync
	// before appending/rebuilding with the current theme while hidden.
	if (_refreshThemeProjection) _refreshThemeProjection();
	if (_pendingRebuild || (_filterTimer && _filterTimer->isActive())) rebuildLogView(true);
	else {
		QElapsedTimer elapsed;
		elapsed.start();
		renderVisible(elapsed, true);
	}
	copyTextToClipboard(_logView->toPlainText());
}

quint64 MeetingLogConsoleWindow::clipboardRevision() const {
#ifdef Q_OS_WIN
	// Native sequence changes are visible before Qt dispatches dataChanged.
	return GetClipboardSequenceNumber();
#else
	return _clipboardRevision;
#endif
}

void MeetingLogConsoleWindow::copyTextToClipboard(QString text) {
	if (!_clipboardChangeConnection) {
		_clipboardChangeConnection = connect(QApplication::clipboard(),
			&QClipboard::dataChanged, this, [this] { ++_clipboardRevision; });
	}
	_clipboardCopyPending = true;
	tryCopyTextToClipboard(std::move(text), _clipboardCopyGeneration,
		clipboardRevision(), 0);
}

void MeetingLogConsoleWindow::tryCopyTextToClipboard(QString text,
		quint64 generation, quint64 revision, int attempt) {
	if (generation != _clipboardCopyGeneration) return;
	const auto finish = [this](QString message) {
		_clipboardCopyPending = false;
		showStorageMessage(std::move(message));
	};
	if (clipboardRevision() != revision) {
		finish(QCoreApplication::translate("MeetingUI", "Clipboard changed. Try copying again."));
		return;
	}
	auto *clipboard = QApplication::clipboard();
	QPointer<MeetingLogConsoleWindow> alive(this);
	clipboard->setText(text);
	if (!alive || generation != _clipboardCopyGeneration) return;
	const bool copied = clipboard->ownsClipboard() && clipboard->text() == text;
	if (!alive || generation != _clipboardCopyGeneration) return;
	if (copied) {
		finish(QCoreApplication::translate("MeetingUI", "Copied logs to clipboard."));
		return;
	}
	const auto after = clipboardRevision();
	// Be conservative even when the owner PID is ours: another window in this
	// process can represent a newer user choice too.
	if (after != revision) {
		finish(QCoreApplication::translate("MeetingUI", "Clipboard changed. Try copying again."));
		return;
	}
	if (attempt >= 2) {
		finish(QCoreApplication::translate("MeetingUI", "Clipboard is busy. Try copying again."));
		return;
	}
	// Qt's Windows OLE setter can fail after its own bounded retries. Yield the
	// UI thread before retrying, so clipboard readers can finish their requests.
	// Keep the snapshot bounded to this action, and cancel if another writer wins.
	showStorageMessage(QCoreApplication::translate("MeetingUI", "Retrying clipboard copy..."));
	QTimer::singleShot(50, this,
		[this, text = std::move(text), generation, after, attempt]() mutable {
			tryCopyTextToClipboard(std::move(text), generation, after, attempt + 1);
		});
}

void MeetingLogConsoleWindow::resizeEvent(QResizeEvent *e) {
	QDialog::resizeEvent(e);
}

void MeetingLogConsoleWindow::showEvent(QShowEvent *e) {
	QDialog::showEvent(e);
	trimVisible();
	if (_refreshThemeProjection) _refreshThemeProjection();
	if (_pendingRebuild || (_filterTimer && _filterTimer->isActive())) rebuildLogView();
	else {
		QElapsedTimer elapsed;
		elapsed.start();
		renderVisible(elapsed);
	}
	refreshStatus();
}

void MeetingLogConsoleWindow::closeEvent(QCloseEvent *e) {
	++_clipboardCopyGeneration;
	_clipboardCopyPending = false;
	hide();
	e->ignore();
}

} // namespace MeetingUI
