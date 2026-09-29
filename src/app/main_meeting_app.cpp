#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include "base/basic_types.h"
#include "base/platform/win/base_windows_winrt.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtGui/QIcon>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QSettings>
#include <QtCore/QCryptographicHash>
#include <QtPlugin>
#include "crl/crl.h"
#include <rpl/rpl.h>
#include "ui/style/style_core.h"
#include "src/ui/meeting_ui_integration.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_branding.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_main_window.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/login_dialog.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "src/rtc/webrtc_manager.h"
#include "src/app/debug_login_options.h"
#include "src/app/pilot_diagnostics_probe.h"
#include "src/app/async_shutdown_guard.h"
#include "src/core/session_shutdown_service.h"
#include "src/telemetry/stability_ledger.h"
#include "src/telemetry/crash_evidence_provider.h"
#include "src/telemetry/build_identity.h"
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/diagnostic_spdlog_bridge.h"
#include "src/ui/diagnostic_qt_bridge.h"

#include <filesystem>
#include <memory>

// 静态链接 Qt 必须显式导入平台与图像插件
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace crl {
rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}
} // namespace crl

namespace {

constexpr auto kDebugLoginTimeout = 30000;

struct DebugLoginCompletion {
	bool completed = false;
	bool success = false;
};

struct DiagnosticCloseGuard final {
	std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline;
	livekit::diagnostic::ShutdownReason reason =
		livekit::diagnostic::ShutdownReason::UserExit;
	~DiagnosticCloseGuard() { if (pipeline) pipeline->Close(reason); }
};

bool LoginWithDebugCredentials(
		OpenMeeting::SessionManager &session,
		const QString &account,
		const QString &password) {
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	const auto completion = std::make_shared<DebugLoginCompletion>();
	const QPointer<QEventLoop> loopGuard(&loop);

	QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
		session.cancelPendingLogin();
		loop.quit();
	});
	session.loginWithPassword(
		account,
		password,
		false,
		false,
		[completion, loopGuard](bool success, const QString &) {
			completion->completed = true;
			completion->success = success;
			if (loopGuard) loopGuard->quit();
		});

	if (!completion->completed) {
		timeout.start(kDebugLoginTimeout);
		loop.exec();
	}
	return completion->completed && completion->success;
}

} // namespace

int main(int argc, char *argv[]) {
	auto diagnostics = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
	const auto buildId = livekit::telemetry::CurrentExecutableBuildId();
	const auto crashRoot = livekit::telemetry::CrashEvidenceProvider::DefaultRoot();
	const auto crashEnabled =
		livekit::telemetry::CrashEvidenceProvider::CollectionEnabled(crashRoot);
	auto crashScan = livekit::telemetry::CrashEvidenceProvider::Scan(
		crashRoot, crashEnabled);
	auto crashProvider = livekit::telemetry::CrashEvidenceProvider::Install(
		crashRoot, std::string(diagnostics->run_id()), buildId, crashEnabled);
	crashScan.recovery.provider_configured = crashProvider->installed();
	std::shared_ptr<livekit::telemetry::StabilityLedger> stabilityLedger;
	std::shared_ptr<livekit::telemetry::ScopedProcessRun> processRun;
	if (!crashRoot.empty()) {
		stabilityLedger = std::make_shared<livekit::telemetry::StabilityLedger>(
			crashRoot.parent_path() / L"telemetry" / L"stability-ledger-v1.json");
		processRun = std::make_shared<livekit::telemetry::ScopedProcessRun>(
			stabilityLedger, crashScan.recovery,
			std::string(diagnostics->run_id()), buildId);
		livekit::telemetry::InstallStabilityLedger(stabilityLedger);
	}
	DiagnosticCloseGuard diagnosticClose{diagnostics};
	livekit::diagnostic::InstallBusinessPipeline(diagnostics);
	diagnostics->TryEmit(livekit::diagnostic::Event::Started(
		buildId, livekit::telemetry::CurrentExecutablePdbIdentity()));
	if (!livekit::diagnostic::InstallSafeSpdlogAdapter(diagnostics)) return 4;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

	// 初始化 CRL (Concurrency & Reactive Library)
	crl::details::init();

	QApplication app(argc, argv);
	// Capture can be the first WinRT caller on a worker thread; initialize the
	// desktop toolkit's dynamic WinRT imports before meeting actions start.
	if (!base::WinRT::Supported()) {
		qCritical() << "Required Windows Runtime APIs are unavailable.";
		return 4;
	}
	app.setOrganizationName(QStringLiteral("Cohavora"));
	app.setApplicationName(MeetingUI::AppBranding::name());
	app.setApplicationVersion(QStringLiteral(COHAVORA_VERSION));
	auto debugLogin = MeetingApp::ParseDebugLoginOptions(app.arguments());
	if (debugLogin.status == MeetingApp::DebugLoginOptionStatus::Invalid ||
		!MeetingApp::ConfigureDebugSettingsRoot(debugLogin.debugEnabled,
			qEnvironmentVariable("LIVEKIT_UIA_SETTINGS_ROOT"))) {
		qCritical() << "Invalid debug options or settings root.";
		diagnosticClose.reason = livekit::diagnostic::ShutdownReason::LoginRejected;
		return 2;
	}
	MeetingUI::DiagnosticQtBridge diagnosticQtBridge(diagnostics);
	const auto diagnosticRoot = QDir(
		QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
		.filePath(QStringLiteral("diagnostics"));
	QSettings diagnosticSettings;
	const bool requestedLogRetention = diagnosticSettings.value(
		QStringLiteral("diagnostics/historyEnabled"), true).toBool();
	diagnostics->SetRetentionEnabled(
		diagnosticSettings.status() == QSettings::NoError && requestedLogRetention);
	diagnostics->StartWriter(std::filesystem::path(diagnosticRoot.toStdWString()));
	if (!processRun) {
		const auto stabilityPath = QDir(
			QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
			.filePath(QStringLiteral("telemetry/stability-ledger-v1.json"));
		stabilityLedger = std::make_shared<livekit::telemetry::StabilityLedger>(
			std::filesystem::path(stabilityPath.toStdWString()));
		processRun = std::make_shared<livekit::telemetry::ScopedProcessRun>(
			stabilityLedger, std::move(crashScan.recovery),
			std::string(diagnostics->run_id()), buildId);
		livekit::telemetry::InstallStabilityLedger(stabilityLedger);
	}
	if (!processRun->started()) {
		qWarning() << "The local stability ledger is unavailable.";
	}
	const auto telemetryRoot = QDir(
		QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
		.filePath(QStringLiteral("telemetry/reports"));
	auto telemetryHistory =
		std::make_shared<livekit::telemetry::TelemetryHistoryStore>(
			std::filesystem::path(telemetryRoot.toStdWString()),
			std::string(diagnostics->run_id()),
			std::filesystem::path(diagnosticRoot.toStdWString()));
	livekit::telemetry::InstallTelemetryHistoryStore(telemetryHistory);
	telemetryHistory->SetHistoryEnabled(
		QSettings().value(QStringLiteral("telemetry/historyEnabled"), true).toBool());
	std::unique_ptr<MeetingApp::PilotDiagnosticsProbe> pilotProbe;
	auto pilotParticipant = std::make_shared<std::atomic<std::shared_ptr<const std::string>>>();
	auto& shutdownService = OpenMeeting::SessionShutdownService::Instance();
	const auto pilotPath = qEnvironmentVariable("LIVEKIT_UIA_PILOT_PROBE");
	const auto pilotRun = qEnvironmentVariable("LIVEKIT_UIA_RUN_ID");
	if (debugLogin.debugEnabled && !pilotPath.isEmpty() && pilotRun.size() == 32 &&
		std::all_of(pilotRun.begin(), pilotRun.end(), [](QChar c) {
			return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
				(c >= QLatin1Char('a') && c <= QLatin1Char('f')); })) {
		livekit::telemetry::SetFineRenderStatisticsEnabled(true);
		pilotProbe = std::make_unique<MeetingApp::PilotDiagnosticsProbe>(
			std::filesystem::path(pilotPath.toStdWString()), pilotRun.toStdString(),
			telemetryHistory, diagnostics, std::filesystem::path(telemetryRoot.toStdWString()),
			std::filesystem::path(diagnosticRoot.toStdWString()),
			[&shutdownService] { return shutdownService.pending(); },
			[pilotParticipant] { return pilotParticipant->load(std::memory_order_acquire); });
	}
	app.setWindowIcon(QIcon(QStringLiteral(":/meeting-ui/icons/cohavora.svg")));
	MeetingUI::AppTranslation::install(app,
		MeetingUI::AppTranslation::startupLocale(app.arguments()));
	// Windows appends this value to owned top-level window captions. Resolve it
	// only after translators are installed so diagnostic titles stay localized.
	app.setApplicationDisplayName(MeetingUI::AppBranding::displayName());
	OpenMeeting::initializeServiceEndpointPolicy(
		debugLogin.debugEnabled);

	// 设置 UI 抽象层 Integration
	MeetingUI::MeetingUiIntegration integration;
	Ui::Integration::Set(&integration);

	// 初始化 Telegram Desktop lib_ui 样式系统
	style::StartManager(100);
	MeetingUI::AppTheme::install(app);

	// 初始化会话与用户认证
	auto &session = OpenMeeting::SessionManager::instance();
	if (debugLogin.status == MeetingApp::DebugLoginOptionStatus::Enabled) {
		const bool loggedIn = LoginWithDebugCredentials(
			session, debugLogin.account, debugLogin.password);
		debugLogin.clearPassword();
		if (!loggedIn) {
			qCritical() << "Automated debug sign-in failed or timed out.";
			diagnosticClose.reason = livekit::diagnostic::ShutdownReason::LoginRejected;
			style::StopManager();
			return 3;
		}
	} else {
		session.resumeSavedSession(true);
	}

	// Only a successfully restored, explicitly enabled session skips login.
	if (!session.isLoggedIn() ||
		(debugLogin.status != MeetingApp::DebugLoginOptionStatus::Enabled &&
		 !session.isAutoLogin())) {
		MeetingUI::LoginDialog loginDlg;
		if (loginDlg.exec() != QDialog::Accepted) {
			// 用户主动退出登录对话框，直接退出程序
			style::StopManager();
			return 0;
		}
	}

	if (pilotProbe) {
		// Bind the native authenticated account to the independent receiver
		// without exporting the account ID or relying on a display-name override.
		const auto fingerprint = QCryptographicHash::hash(
			(pilotRun + QLatin1Char(':') + session.userId()).toUtf8(),
			QCryptographicHash::Sha256).toHex().toStdString();
		pilotParticipant->store(std::make_shared<const std::string>(fingerprint),
			std::memory_order_release);
	}
	// 创建并展示现代会议主界面
	auto mainWindow = std::make_unique<MeetingUI::MeetingMainWindow>();
	mainWindow->show();
	app.setQuitOnLastWindowClosed(false);
	MeetingApp::AsyncShutdownGuard shutdownGuard(app,
		[&](std::function<void()> finished) {
			// Close producers before closing cleanup admission. Meeting windows
			// are heap-owned, WA_DeleteOnClose widgets; explicitly destroy any
			// remaining hidden ones so their coordinators cannot enqueue later.
			if (mainWindow) mainWindow->close();
			for (auto* widget : QApplication::topLevelWidgets()) {
				if (auto* room = qobject_cast<MeetingUI::MeetingRoomWindow*>(widget)) {
					room->close();
					delete room;
				}
			}
			mainWindow.reset();
			MeetingUI::MeetingLogConsoleWindow::DestroyInstance();
			shutdownService.DrainAsync([&, finished = std::move(finished)]() mutable {
				shutdownService.SubmitCleanup(
					[history = std::move(telemetryHistory), run = std::move(processRun),
					 diagnostics]() mutable {
						// All final session snapshots have entered the history queue.
						const auto historyClose = history->Close();
                        if (historyClose.state != livekit::telemetry::TelemetryCloseState::Completed) {
                            diagnostics->TryEmit(livekit::diagnostic::Event::Issue(
                                livekit::diagnostic::IssueCode::NativeCleanupFailed));
                        }
						livekit::telemetry::InstallTelemetryHistoryStore({});
						history.reset();
						run.reset();
						livekit::telemetry::InstallStabilityLedger({});
						diagnostics->Close(livekit::diagnostic::ShutdownReason::UserExit);
					});
				shutdownService.ShutdownAsync(std::move(finished));
			});
		}, [] {
			// Reject the current modal without deleting its parent. The guard
			// resumes shutdown only after all nested exec() calls have returned.
			if (auto* modal = QApplication::activeModalWidget()) {
				if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
				else modal->close();
			}
		});
	QObject::connect(&app, &QGuiApplication::lastWindowClosed,
		&shutdownGuard, [&] {
            shutdownGuard.RequestIfStillNeeded([] {
                // Reauthentication temporarily closes the last visible dialog
                // before its caller shows the main window again.
                for (auto *window : QApplication::topLevelWidgets()) {
                    if (!window->parentWidget() && window->isVisible() &&
                        window->testAttribute(Qt::WA_QuitOnClose)) return false;
                }
                return true;
            });
        });

	const int result = app.exec();

	// 退出时显式释放 WebRTC 资源与样式系统
	livekit::WebRTCManager::Instance().Deinitialize();
	style::StopManager();

	return result;
}
