#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include "base/basic_types.h"
#include <QtWidgets/QApplication>
#include <QtGui/QIcon>
#include <QtCore/QDir>
#include <QtPlugin>
#include "crl/crl.h"
#include <rpl/rpl.h>
#include "ui/style/style_core.h"
#include "src/ui/meeting_ui_integration.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_branding.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_main_window.h"
#include "src/ui/login_dialog.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "src/rtc/webrtc_manager.h"
#include "src/app/debug_login_options.h"

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
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

	// 初始化 CRL (Concurrency & Reactive Library)
	crl::details::init();

	QApplication app(argc, argv);
	app.setApplicationName(MeetingUI::AppBranding::name());
	app.setWindowIcon(QIcon(QStringLiteral(":/meeting-ui/icons/cohavora.svg")));
	app.setApplicationVersion(QStringLiteral(COHAVORA_VERSION));
	MeetingUI::AppTranslation::install(app,
		MeetingUI::AppTranslation::startupLocale(app.arguments()));
	app.setApplicationDisplayName(MeetingUI::AppBranding::displayName());
	auto debugLogin = MeetingApp::ParseDebugLoginOptions(app.arguments());
	if (debugLogin.status == MeetingApp::DebugLoginOptionStatus::Invalid) {
		qCritical().noquote() << debugLogin.error;
		return 2;
	}
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

	// 创建并展示现代会议主界面
	MeetingUI::MeetingMainWindow mainWindow;
	mainWindow.show();

	const int result = app.exec();

	// 退出时显式释放 WebRTC 资源与样式系统
	livekit::WebRTCManager::Instance().Deinitialize();
	style::StopManager();

	return result;
}
