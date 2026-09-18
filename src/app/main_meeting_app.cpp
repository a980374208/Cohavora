#include "base/basic_types.h"
#include <QtWidgets/QApplication>
#include <QtCore/QDir>
#include <QtPlugin>
#include "crl/crl.h"
#include <rpl/rpl.h>
#include "ui/style/style_core.h"
#include "src/ui/meeting_ui_integration.h"
#include "src/ui/meeting_main_window.h"
#include "src/ui/login_dialog.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "src/rtc/webrtc_manager.h"

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

int main(int argc, char *argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

	// 初始化 CRL (Concurrency & Reactive Library)
	crl::details::init();

	QApplication app(argc, argv);
	app.setApplicationName(QString::fromUtf8("LiveKitMeetingClient"));
	app.setApplicationDisplayName(QString::fromUtf8("音视频会议客户端 - LiveKit Powered"));
	OpenMeeting::initializeServiceEndpointPolicy(
		app.arguments().contains(QStringLiteral("--debug")));

	// 设置 UI 抽象层 Integration
	MeetingUI::MeetingUiIntegration integration;
	Ui::Integration::Set(&integration);

	// 初始化 Telegram Desktop lib_ui 样式系统
	style::StartManager(100);

	// 初始化会话与用户认证
	auto &session = OpenMeeting::SessionManager::instance();
	session.resumeSavedSession(true);

	// Only a successfully restored, explicitly enabled session skips login.
	if (!session.isLoggedIn() || !session.isAutoLogin()) {
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
