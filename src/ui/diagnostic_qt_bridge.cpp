#include "src/ui/diagnostic_qt_bridge.h"

#include "src/ui/meeting_log_console.h"

#include <QtCore/QDebug>

#include <atomic>

namespace MeetingUI {
namespace {

std::atomic<std::shared_ptr<livekit::diagnostic::DiagnosticPipeline>>& ActivePipeline() {
	static auto* pipeline = new std::atomic<std::shared_ptr<livekit::diagnostic::DiagnosticPipeline>>;
	return *pipeline;
}

bool IsTemplate(const QString& message, const QString& text) {
	return message == text ||
		(message.size() == text.size() + 2 && message.front() == QLatin1Char('"') &&
		 message.back() == QLatin1Char('"') &&
		 message.mid(1, text.size()) == text);
}

void SafeQtMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
	thread_local bool handling = false;
	if (handling) return;
	handling = true;
	try {
		const auto pipeline = ActivePipeline().load(std::memory_order_acquire);
		if (pipeline) {
			using namespace livekit::diagnostic;
			if (type == QtFatalMsg) {
				pipeline->TryEmit(Event::Issue(IssueCode::QtFatal));
				pipeline->TryEmit(Event::Terminal(Outcome::Failure, DrainResult::Failed));
			} else if (IsTemplate(message, QStringLiteral(
				"The local stability ledger is unavailable."))) {
				pipeline->TryEmit(Event::FileFailed(
					FailureReason::LedgerUnavailable, 0, SinkKind::Telemetry));
			} else if (IsTemplate(message, QStringLiteral(
				"Native cleanup failed; retaining resources and keeping shutdown pending."))) {
				pipeline->TryEmit(Event::Issue(IssueCode::NativeCleanupFailed));
			} else if (message.startsWith(QStringLiteral(
				"[SessionManager] Invalidate complete local session, reason=")) ||
				message.startsWith(QStringLiteral(
				"\"[SessionManager] Invalidate complete local session, reason="))) {
				pipeline->TryEmit(Event::Issue(IssueCode::SessionInvalidated));
			} else if (IsTemplate(message, QStringLiteral(
				"Invalid debug sign-in options."))) {
				pipeline->TryEmit(Event::Issue(IssueCode::InvalidSignInOptions));
			} else if (IsTemplate(message, QStringLiteral(
				"Automated debug sign-in failed or timed out."))) {
				pipeline->TryEmit(Event::Issue(IssueCode::SignInFailed));
			} else {
				pipeline->CountSuppressed();
			}
		}
	} catch (...) {}
	handling = false;
}

} // namespace

DiagnosticQtBridge::DiagnosticQtBridge(
		std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline)
: pipeline_(std::move(pipeline)) {
	if (!pipeline_) return;
	pipeline_->SetMirror(MeetingLogConsoleWindow::diagnosticMirror());
	ActivePipeline().store(pipeline_, std::memory_order_release);
	qInstallMessageHandler(SafeQtMessage);
}

DiagnosticQtBridge::~DiagnosticQtBridge() {
	if (pipeline_) pipeline_->SetMirror({});
	ActivePipeline().store({}, std::memory_order_release);
	// Keep the safe handler installed through Qt's static teardown.
}

} // namespace MeetingUI
