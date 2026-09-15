#include "src/ui/meeting_main_window.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/login_dialog.h"
#include "src/ui/shadow_helper.h"
#include "src/ui/video_test_widget.h"
#include "src/core/meeting_coordinator.h"
#include "src/net/session_manager.h"
#include "styles/style_widgets.h"
#include <QtCore/QPointer>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QFont>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace MeetingUI {

// ----------------------------------------------------
// WindowControlsWidget 窗口右上角控制按钮
// ----------------------------------------------------

WindowControlsWidget::WindowControlsWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedSize(120, 32);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void WindowControlsWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const int w = width();
	const int h = height();
	const int btnW = 40;

	_minRect = QRect(0, 0, btnW, h);
	_maxRect = QRect(btnW, 0, btnW, h);
	_closeRect = QRect(btnW * 2, 0, btnW, h);

	// 最小化按钮
	if (_hoverBtn == HoverBtn::Min) {
		p.fillRect(_minRect, QColor(0xe5, 0xe8, 0xef));
	}
	p.setPen(QPen(QColor(0x60, 0x62, 0x66), 1.2));
	p.drawLine(_minRect.center().x() - 5, _minRect.center().y(), _minRect.center().x() + 5, _minRect.center().y());

	// 最大化按钮
	if (_hoverBtn == HoverBtn::Max) {
		p.fillRect(_maxRect, QColor(0xe5, 0xe8, 0xef));
	}
	p.setPen(QPen(QColor(0x60, 0x62, 0x66), 1.2));
	p.drawRect(_maxRect.center().x() - 5, _maxRect.center().y() - 5, 10, 10);

	// 关闭按钮
	if (_hoverBtn == HoverBtn::Close) {
		// 右上角带圆角的红色悬浮背景
		QPainterPath closePath;
		closePath.addRoundedRect(_closeRect, 0, 0);
		p.fillPath(closePath, QColor(0xf5, 0x3f, 0x3f));
		p.setPen(QPen(Qt::white, 1.3));
	} else {
		p.setPen(QPen(QColor(0x60, 0x62, 0x66), 1.2));
	}
	const int ccx = _closeRect.center().x();
	const int ccy = _closeRect.center().y();
	p.drawLine(ccx - 5, ccy - 5, ccx + 5, ccy + 5);
	p.drawLine(ccx + 5, ccy - 5, ccx - 5, ccy + 5);
}

void WindowControlsWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	HoverBtn next = HoverBtn::None;
	if (_minRect.contains(pos)) next = HoverBtn::Min;
	else if (_maxRect.contains(pos)) next = HoverBtn::Max;
	else if (_closeRect.contains(pos)) next = HoverBtn::Close;

	if (next != _hoverBtn) {
		_hoverBtn = next;
		update();
	}
}

void WindowControlsWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		if (_minRect.contains(e->pos())) {
			_minClicks.fire({});
		} else if (_maxRect.contains(e->pos())) {
			_maxClicks.fire({});
		} else if (_closeRect.contains(e->pos())) {
			_closeClicks.fire({});
		}
	}
}

void WindowControlsWidget::leaveEventHook(QEvent *e) {
	_hoverBtn = HoverBtn::None;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

// ----------------------------------------------------
// JoinMeetingDialog 加入会议对话框
// ----------------------------------------------------

JoinMeetingDialog::JoinMeetingDialog(QWidget *parent)
	: QDialog(parent) {
	setWindowTitle(QString::fromUtf8("加入会议"));
	setFixedSize(460, 520);
	setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground, true);

	setStyleSheet(R"(
		QDialog {
			background: transparent;
		}
		#dialogContainer {
			background-color: #ffffff;
			border-radius: 14px;
			border: 1px solid #e1e4ea;
		}
		QLabel {
			color: #1f2329;
			font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
			font-size: 13px;
		}
		QLineEdit {
			border: 1px solid #dcdfe6;
			border-radius: 8px;
			padding: 8px 12px;
			font-size: 13px;
			background: #f8f9fa;
		}
		QLineEdit:focus {
			border: 1px solid #1677ff;
			background: #ffffff;
		}
		QPushButton#joinBtn {
			background-color: #1677ff;
			color: #ffffff;
			border-radius: 8px;
			padding: 9px 24px;
			font-size: 14px;
			font-weight: bold;
			border: none;
		}
		QPushButton#joinBtn:hover {
			background-color: #4096ff;
		}
		QPushButton#joinBtn:disabled {
			background-color: #b7d6ff;
		}
		QPushButton#cancelBtn {
			background-color: #f2f3f5;
			color: #4e5969;
			border-radius: 8px;
			padding: 9px 20px;
			font-size: 14px;
			border: none;
		}
		QPushButton#cancelBtn:hover {
			background-color: #e5e6eb;
		}
		QPushButton#linkBtn {
			background: transparent;
			color: #1677ff;
			font-size: 12px;
			border: none;
			padding: 0;
			text-align: left;
		}
		QPushButton#linkBtn:hover {
			color: #4096ff;
			text-decoration: underline;
		}
		QCheckBox {
			font-size: 13px;
			color: #4e5969;
		}
	)");

	auto rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(12, 12, 12, 12);

	auto container = new QWidget(this);
	container->setObjectName("dialogContainer");
	rootLayout->addWidget(container);

	auto mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(28, 24, 28, 24);
	mainLayout->setSpacing(12);

	// 标题栏
	auto titleLayout = new QHBoxLayout();
	auto titleLabel = new QLabel(QString::fromUtf8("加入会议"), container);
	QFont tf = titleLabel->font();
	tf.setPixelSize(18);
	tf.setBold(true);
	titleLabel->setFont(tf);
	titleLayout->addWidget(titleLabel);
	titleLayout->addStretch();

	_closeBtn = new QPushButton(QString::fromUtf8("✕"), container);
	_closeBtn->setStyleSheet("border:none; color:#8c8c8c; font-size:14px;");
	_closeBtn->setFixedSize(24, 24);
	connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
	titleLayout->addWidget(_closeBtn);
	mainLayout->addLayout(titleLayout);

	// 会议号输入框
	auto idLabel = new QLabel(QString::fromUtf8("会议号"), container);
	idLabel->setStyleSheet("font-weight: bold; color: #303133; font-size: 12px;");
	mainLayout->addWidget(idLabel);

	_meetingIdInput = new QLineEdit(container);
	_meetingIdInput->setPlaceholderText(QString::fromUtf8("请输入 9 位会议号 (如 847-123-456)"));
	mainLayout->addWidget(_meetingIdInput);

	// 入会密码输入框
	auto pwdLabel = new QLabel(QString::fromUtf8("会议密码 (选填)"), container);
	pwdLabel->setStyleSheet("font-weight: bold; color: #303133; font-size: 12px;");
	mainLayout->addWidget(pwdLabel);

	_passwordInput = new QLineEdit(container);
	_passwordInput->setPlaceholderText(QString::fromUtf8("如果会议加密请输入密码"));
	_passwordInput->setEchoMode(QLineEdit::Password);
	mainLayout->addWidget(_passwordInput);

	// 参会昵称输入框
	auto nameLabel = new QLabel(QString::fromUtf8("参会昵称"), container);
	nameLabel->setStyleSheet("font-weight: bold; color: #303133; font-size: 12px;");
	mainLayout->addWidget(nameLabel);

	_displayNameInput = new QLineEdit(container);
	_displayNameInput->setPlaceholderText(QString::fromUtf8("请输入入会后显示的昵称"));
	auto &session = OpenMeeting::SessionManager::instance();
	_displayNameInput->setText(session.nickname());
	mainLayout->addWidget(_displayNameInput);

	// 入会音视频设置
	auto optLayout = new QHBoxLayout();
	_audioMuteBox = new QCheckBox(QString::fromUtf8("入会开启麦克风"), container);
	_audioMuteBox->setChecked(session.mediaPreferences().enableMicrophone);
	_videoMuteBox = new QCheckBox(QString::fromUtf8("入会开启摄像头"), container);
	_videoMuteBox->setChecked(session.mediaPreferences().enableVideo);
	optLayout->addWidget(_audioMuteBox);
	optLayout->addWidget(_videoMuteBox);
	mainLayout->addLayout(optLayout);

	// 手动/高级直连设置折叠栏
	_manualToggleBtn = new QPushButton(QString::fromUtf8("⚙ 高级 LiveKit 直连设置 ▾"), container);
	_manualToggleBtn->setObjectName("linkBtn");
	connect(_manualToggleBtn, &QPushButton::clicked, this, &JoinMeetingDialog::toggleManualServer);
	mainLayout->addWidget(_manualToggleBtn);

	_manualWidget = new QWidget(container);
	auto manLayout = new QVBoxLayout(_manualWidget);
	manLayout->setContentsMargins(0, 2, 0, 2);
	manLayout->setSpacing(4);

	_serverUrlInput = new QLineEdit(_manualWidget);
	_serverUrlInput->setPlaceholderText(QString::fromUtf8("服务器地址 (如 ws://127.0.0.1:7880)"));
	_serverUrlInput->setText("ws://127.0.0.1:7880");

	_tokenInput = new QLineEdit(_manualWidget);
	_tokenInput->setPlaceholderText(QString::fromUtf8("手动指定 LiveKit Token (选填)"));

	manLayout->addWidget(_serverUrlInput);
	manLayout->addWidget(_tokenInput);
	_manualWidget->setVisible(false);
	mainLayout->addWidget(_manualWidget);

	// 状态/错误提示
	_statusLabel = new QLabel(container);
	_statusLabel->setStyleSheet("color: #f53f3f; font-size: 12px; padding: 2px 4px;");
	_statusLabel->setAlignment(Qt::AlignCenter);
	_statusLabel->setWordWrap(true);
	_statusLabel->setMinimumHeight(32);
	_statusLabel->setVisible(false);
	mainLayout->addWidget(_statusLabel);

	// 底部按钮栏
	auto btnLayout = new QHBoxLayout();
	btnLayout->addStretch();
	_cancelBtn = new QPushButton(QString::fromUtf8("取消"), container);
	_cancelBtn->setObjectName("cancelBtn");
	_joinBtn = new QPushButton(QString::fromUtf8("加入会议"), container);
	_joinBtn->setObjectName("joinBtn");

	btnLayout->addWidget(_cancelBtn);
	btnLayout->addWidget(_joinBtn);
	mainLayout->addLayout(btnLayout);

	connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
	connect(_joinBtn, &QPushButton::clicked, this, &JoinMeetingDialog::onJoinClicked);
	connect(_meetingIdInput, &QLineEdit::returnPressed, this, &JoinMeetingDialog::onJoinClicked);
}

void JoinMeetingDialog::toggleManualServer() {
	bool isVisible = _manualWidget->isVisible();
	_manualWidget->setVisible(!isVisible);
	_manualToggleBtn->setText(!isVisible ? QString::fromUtf8("⚙ 高级 LiveKit 直连设置 ▴") : QString::fromUtf8("⚙ 高级 LiveKit 直连设置 ▾"));
	adjustSize();
}

void JoinMeetingDialog::reject() {
	_isCancelled = true;
	_isLoading = false;
	QDialog::reject();
}

void JoinMeetingDialog::closeEvent(QCloseEvent *e) {
	_isCancelled = true;
	_isLoading = false;
	QDialog::closeEvent(e);
}

void JoinMeetingDialog::setLoading(bool loading, const QString &statusText) {
	_isLoading = loading;
	if (_joinBtn) _joinBtn->setEnabled(!loading);
	if (_cancelBtn) _cancelBtn->setEnabled(!loading);
	if (_closeBtn) _closeBtn->setEnabled(!loading);
	if (_meetingIdInput) _meetingIdInput->setEnabled(!loading);
	if (_passwordInput) _passwordInput->setEnabled(!loading);
	if (_displayNameInput) _displayNameInput->setEnabled(!loading);

	if (loading) {
		if (_joinBtn) _joinBtn->setText(QString::fromUtf8("正在入会..."));
		if (_statusLabel) {
			_statusLabel->setStyleSheet("color: #1677ff; font-size: 12px;");
			_statusLabel->setText(statusText.isEmpty() ? QString::fromUtf8("正在处理入会请求...") : statusText);
			_statusLabel->setVisible(true);
		}
	} else {
		if (_joinBtn) _joinBtn->setText(QString::fromUtf8("加入会议"));
	}
}

void JoinMeetingDialog::showError(const QString &msg) {
	if (!_statusLabel) return;
	_statusLabel->setStyleSheet("color: #f53f3f; font-size: 12px;");
	_statusLabel->setText(msg);
	_statusLabel->setVisible(!msg.isEmpty());
}

void JoinMeetingDialog::onJoinClicked() {
	if (_isLoading) return;

	// 1. 检查是否启用了高级手动直连模式
	if (_manualWidget && _manualWidget->isVisible() && _tokenInput && !_tokenInput->text().trimmed().isEmpty()) {
		_resolvedServerUrl = _serverUrlInput ? _serverUrlInput->text().trimmed() : QString();
		_resolvedToken = _tokenInput ? _tokenInput->text().trimmed() : QString();
		_cleanMeetingId = _meetingIdInput ? _meetingIdInput->text().trimmed() : QString();
		if (_cleanMeetingId.isEmpty()) {
			_cleanMeetingId = "livekit_room";
		}
		accept();
		return;
	}

	// 2. 正常业务入会流程
	QString rawId = _meetingIdInput ? _meetingIdInput->text().trimmed() : QString();
	_cleanMeetingId = rawId;
	_cleanMeetingId.remove('-').remove(' ');

	if (_cleanMeetingId.isEmpty()) {
		showError(QString::fromUtf8("请输入有效的会议号"));
		if (_meetingIdInput) _meetingIdInput->setFocus();
		return;
	}

	const QString password = _passwordInput ? _passwordInput->text() : QString();
	auto &session = OpenMeeting::SessionManager::instance();

	setLoading(true, QString::fromUtf8("正在校验会议权限..."));

	QPointer<JoinMeetingDialog> self = this;

	// 第一阶段：加入会议校验
	session.httpClient().joinMeeting(_cleanMeetingId, password, [self](bool ok, bool pass, const OpenMeeting::HttpError &err) {
		if (!self || self->_isCancelled) {
			return;
		}
		if (!ok) {
			self->setLoading(false);
			if (err.message.contains("user already in meeting", Qt::CaseInsensitive) || err.code == 200001) {
				self->showError(QString::fromUtf8("该账号已在当前会议中，不能重复入会。请使用【访客体验】或换一个账号登录加入！"));
			} else {
				self->showError(QString::fromUtf8("入会校验失败: %1").arg(err.message.isEmpty() ? QString::fromUtf8("会议不存在或网络不可达") : err.message));
			}
			return;
		}

		self->setLoading(true, QString::fromUtf8("正在换取 LiveKit 视讯凭据..."));

		// 第二阶段：换取 LiveKit Token 与 URL
		auto &sess = OpenMeeting::SessionManager::instance();
		sess.httpClient().getMeetingToken(self->_cleanMeetingId, [self](bool tokenOk, const OpenMeeting::LiveKitAuthInfo &auth, const OpenMeeting::HttpError &tokenErr) {
			if (!self || self->_isCancelled) {
				return;
			}
			self->setLoading(false);
			if (!tokenOk || auth.url.isEmpty() || auth.token.isEmpty()) {
				self->showError(QString::fromUtf8("获取凭据失败: %1").arg(tokenErr.message.isEmpty() ? QString::fromUtf8("凭据解析异常") : tokenErr.message));
				return;
			}

			self->_resolvedServerUrl = auth.url;
			self->_resolvedToken = auth.token;

			// 保存偏好设置
			auto &s = OpenMeeting::SessionManager::instance();
			if (self->_audioMuteBox) s.setEnableMicrophone(self->_audioMuteBox->isChecked());
			if (self->_videoMuteBox) s.setEnableVideo(self->_videoMuteBox->isChecked());

			self->accept();
		});
	});
}

QString JoinMeetingDialog::serverUrl() const {
	return _resolvedServerUrl;
}

QString JoinMeetingDialog::token() const {
	return _resolvedToken;
}

QString JoinMeetingDialog::meetingId() const {
	return _cleanMeetingId;
}

QString JoinMeetingDialog::password() const {
	return _passwordInput ? _passwordInput->text().trimmed() : QString();
}

QString JoinMeetingDialog::displayName() const {
	const QString name = _displayNameInput ? _displayNameInput->text().trimmed() : QString();
	return name.isEmpty() ? QString::fromUtf8("参会者") : name;
}

bool JoinMeetingDialog::isAudioMuted() const {
	return _audioMuteBox && !_audioMuteBox->isChecked();
}

bool JoinMeetingDialog::isVideoMuted() const {
	return _videoMuteBox && !_videoMuteBox->isChecked();
}

void JoinMeetingDialog::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_isDragging = true;
		_dragPosition = e->globalPos() - frameGeometry().topLeft();
		e->accept();
	}
}

void JoinMeetingDialog::mouseMoveEvent(QMouseEvent *e) {
	if (_isDragging && (e->buttons() & Qt::LeftButton)) {
		move(e->globalPos() - _dragPosition);
		e->accept();
	}
}

// ----------------------------------------------------
// MeetingMainWindow 主界面 (基于 TDeskTop 原生 WindowHelper 架构)
// ----------------------------------------------------

MeetingMainWindow::MeetingMainWindow(QWidget *parent)
	: Ui::RpWidget(parent) {
	setObjectName("MeetingMainWindow");
	setWindowTitle(QString::fromUtf8("会议客户端 - LiveKit Powered"));
	resize(1040, 660);
	setMinimumSize(900, 580);
	setMouseTracking(true);

	// 设置无边框但保留系统窗口特性
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);

	initLayout();
	connect(&OpenMeeting::SessionManager::instance(),
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingMainWindow::onSessionInvalidated,
	        Qt::QueuedConnection);
}

void MeetingMainWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	setupNativeWindow();
}

void MeetingMainWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
	if (!_handle) {
		_handle = reinterpret_cast<HWND>(winId());
	}
	if (!_handle) return;

	// 1. 设置 WS_CAPTION | WS_THICKFRAME，让 Windows 系统内核开启 8 方向边缘拉伸与 Aero Snap
	LONG_PTR style = GetWindowLongPtr(_handle, GWL_STYLE);
	SetWindowLongPtr(_handle, GWL_STYLE, style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

	// 2. 启用 DWM 边框扩展，让 Windows 渲染系统级平滑高斯弥散阴影
	MARGINS margins = { 1, 1, 1, 1 };
	DwmExtendFrameIntoClientArea(_handle, &margins);

	// 3. 启用 Windows 11 DWM 原生抗锯齿圆角
	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(_handle, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &preference, sizeof(preference));

	SetWindowPos(_handle, nullptr, 0, 0, 0, 0,
		SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void MeetingMainWindow::initLayout() {
	_sidebar = new SidebarWidget(this);
	_actionGrid = new ActionGridContainer(this);
	_scheduleWidget = new ScheduleWidget(this);
	_windowControls = new WindowControlsWidget(this);

	// 窗口控制按钮事件
	_windowControls->minimizeClicked() | rpl::on_next([this] {
		showMinimized();
	}, lifetime());

	_windowControls->maximizeClicked() | rpl::on_next([this] {
		if (isMaximized()) {
			showNormal();
		} else {
			showMaximized();
		}
	}, lifetime());

	_windowControls->closeClicked() | rpl::on_next([this] {
		close();
	}, lifetime());

	// 卡片点击事件
	_actionGrid->cardClicked() | rpl::on_next([this](ActionCardType t) {
		onCardClicked(t);
	}, lifetime());

	// 全部会议与添加日程事件
	_scheduleWidget->allMeetingsClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("全部会议"), QString::fromUtf8("当前已与您的日程系统同步，暂无待进行的预定会议。"));
	}, lifetime());

	_scheduleWidget->addScheduleClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("预定日程"), QString::fromUtf8("点击创建新的会议日程，邀请参会人！"));
	}, lifetime());

	// 侧边栏用户头像点击菜单
	_sidebar->avatarClicked() | rpl::on_next([this] {
		auto &session = OpenMeeting::SessionManager::instance();
		QMenu menu(this);
		QString statusStr = session.isLoggedIn()
			? QString::fromUtf8("当前用户: %1 (%2)").arg(session.nickname(), session.userId())
			: QString::fromUtf8("当前未登录");
		menu.addAction(statusStr)->setEnabled(false);
		menu.addSeparator();

		auto *switchAction = menu.addAction(QString::fromUtf8("切换账号 / 登录"));
		auto *logoutAction = menu.addAction(QString::fromUtf8("退出登录"));

		QAction *selected = menu.exec(QCursor::pos());
		if (selected == switchAction || selected == logoutAction) {
			session.logout();
			LoginDialog loginDlg(this);
			if (loginDlg.exec() == QDialog::Accepted) {
				_sidebar->update();
			}
		}
	}, lifetime());
}

void MeetingMainWindow::onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason) {
	if (_sessionInvalidationDialogActive) {
		return;
	}
	_sessionInvalidationDialogActive = true;

	const bool duplicatedLogin =
		reason == OpenMeeting::SessionInvalidationReason::DuplicatedLogin;
	qWarning() << "[UI] Show session invalidation dialog, reason="
	           << static_cast<int>(reason);

	// sessionInvalidated 对象间使用 QueuedConnection，不能依赖投递顺序保证
	// 会议窗口先于本窗口执行。这里在展示全局登录弹窗前同步关闭全部顶层会议，
	// 使媒体和 Room 生命周期先收敛，且不会产生每个会议各自的重复弹窗。
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		auto *roomWindow = qobject_cast<MeetingRoomWindow *>(widget);
		if (roomWindow) {
			roomWindow->onSessionInvalidated(reason);
		}
	}

	QMessageBox::warning(this,
	                     duplicatedLogin ? QString::fromUtf8("账号已下线")
	                                     : QString::fromUtf8("登录失效"),
	                     duplicatedLogin
	                         ? QString::fromUtf8("您的账号已在其他设备登录，当前客户端已退出。")
	                         : QString::fromUtf8("登录状态已失效，请重新登录。"));

	// SessionManager 在发射 sessionInvalidated 前已复用 logout(false) 清理 token
	// 和本地 user 设置；这里仅负责让用户回到可重新认证的界面。
	LoginDialog loginDlg(this);
	if (loginDlg.exec() == QDialog::Accepted && _sidebar) {
		_sidebar->update();
	}
	_sessionInvalidationDialogActive = false;
}

void MeetingMainWindow::onCardClicked(ActionCardType type) {
	if (type == ActionCardType::JoinMeeting) {
		JoinMeetingDialog dlg(this);
		if (dlg.exec() == QDialog::Accepted) {
			auto coordinator = OpenMeeting::MeetingCoordinator::create();
			OpenMeeting::MediaPreferences prefs;
			prefs.enableMicrophone = !dlg.isAudioMuted();
			prefs.enableVideo = !dlg.isVideoMuted();

			MeetingRoomWindow::Config cfg;
			cfg.serverUrl = dlg.serverUrl();
			cfg.token = dlg.token();
			cfg.meetingId = dlg.meetingId();
			cfg.displayName = dlg.displayName();
			cfg.audioMuted = dlg.isAudioMuted();
			cfg.videoEnabled = !dlg.isVideoMuted();

			if (!dlg.serverUrl().isEmpty() && !dlg.token().isEmpty()) {
				// 高级直连模式
				coordinator->connectDirectlyAsync(dlg.serverUrl(), dlg.token(), dlg.meetingId(), dlg.displayName(), prefs);
			} else {
				// 标准两阶段入会
				coordinator->joinMeetingAsync(dlg.meetingId(), dlg.password(), dlg.displayName(), prefs);
			}

			auto *roomWindow = new MeetingRoomWindow(cfg, coordinator);
			roomWindow->setAttribute(Qt::WA_DeleteOnClose);
			roomWindow->show();
		}
	} else if (type == ActionCardType::QuickMeeting) {
		auto &session = OpenMeeting::SessionManager::instance();
		if (!session.isLoggedIn()) {
			LoginDialog loginDlg(this);
			if (loginDlg.exec() != QDialog::Accepted) {
				return;
			}
		}

		auto coordinator = OpenMeeting::MeetingCoordinator::create();
		auto prefs = session.mediaPreferences();

		MeetingRoomWindow::Config cfg;
		cfg.displayName = session.nickname();
		cfg.audioMuted = !prefs.enableMicrophone;
		cfg.videoEnabled = prefs.enableVideo;

		coordinator->createAndJoinQuickMeetingAsync(
			QString::fromUtf8("%1 的快速会议").arg(session.nickname()),
			3600,
			prefs);

		auto *roomWindow = new MeetingRoomWindow(cfg, coordinator);
		roomWindow->setAttribute(Qt::WA_DeleteOnClose);
		roomWindow->show();
	} else if (type == ActionCardType::ScheduleMeeting) {
		QMessageBox::information(this, QString::fromUtf8("预定会议"),
			QString::fromUtf8("已打开会议预定面板，您可以设定会议主题、时间、周期与参会密码。"));
	} else if (type == ActionCardType::ShareScreen) {
		QMessageBox::information(this, QString::fromUtf8("共享屏幕"),
			QString::fromUtf8("正在枚举可用桌面与应用视窗，可选择全屏或指定视窗进行超清低延迟屏幕共享。"));
	} else if (type == ActionCardType::SimulcastTest) {
		MeetingTestWindow testDlg(this);
		testDlg.exec();
	}
}

void MeetingMainWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	// 左侧导航栏 (宽 68)
	const int sidebarW = 68;
	_sidebar->setGeometry(0, 0, sidebarW, h);
	_sidebar->setCornerRadius(isMaximized() ? 0 : kWindowCornerRadius);

	// 右上角窗口控制按钮
	_windowControls->move(w - _windowControls->width() - 4, 4);

	// 中间操作区与右侧日程区
	const int remainW = w - sidebarW;
	const int gridW = remainW * 46 / 100;
	const int scheduleW = remainW - gridW;

	_actionGrid->setGeometry(sidebarW, 36, gridW, h - 36);
	_scheduleWidget->setGeometry(sidebarW + gridW, 36, scheduleW, h - 36);
}

void MeetingMainWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const int w = width();
	const int h = height();
	const int radius = isMaximized() ? 0 : kWindowCornerRadius;

	// 1. 绘制主体圆角容器背景
	if (radius > 0) {
		QPainterPath path;
		path.addRoundedRect(QRectF(0, 0, w, h), radius, radius);
		p.fillPath(path, Qt::white);
	} else {
		p.fillRect(rect(), Qt::white);
	}

	// 2. 绘制中间与右侧之间的浅灰纵向分割线
	const int sidebarW = 68;
	const int remainW = w - sidebarW;
	const int gridW = remainW * 46 / 100;
	const int splitX = sidebarW + gridW;

	p.setPen(QColor(0xf0, 0xf2, 0xf5));
	p.drawLine(splitX, 36, splitX, h - 36);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MeetingMainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
#else
bool MeetingMainWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
#endif
{
#if defined(Q_OS_WIN)
	auto msg = reinterpret_cast<MSG*>(message);
	if (!msg) {
		return Ui::RpWidget::nativeEvent(eventType, message, result);
	}
	HWND handle = msg->hwnd ? msg->hwnd : _handle;

	switch (msg->message) {
	case WM_NCCALCSIZE: {
		if (msg->wParam == TRUE) {
			// 消除 Windows 默认系统边框，使客户区占满整个窗口
			*result = 0;
			return true;
		}
	} break;

	case WM_NCHITTEST: {
		if (!handle) {
			break;
		}
		POINT p{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		ScreenToClient(handle, &p);

		const qreal ratio = devicePixelRatioF();
		const int x = static_cast<int>(p.x / ratio);
		const int y = static_cast<int>(p.y / ratio);

		const int w = width();
		const int h = height();
		const int border = 8; // 边缘 8 像素为系统拉伸感应带

		if (!isMaximized() && !isFullScreen()) {
			const bool left = (x < border);
			const bool right = (x >= w - border);
			const bool top = (y < border);
			const bool bottom = (y >= h - border);

			if (top && left) { *result = HTTOPLEFT; return true; }
			if (top && right) { *result = HTTOPRIGHT; return true; }
			if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
			if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
			if (left) { *result = HTLEFT; return true; }
			if (right) { *result = HTRIGHT; return true; }
			if (top) { *result = HTTOP; return true; }
			if (bottom) { *result = HTBOTTOM; return true; }
		}

		// 标题栏拖拽区域（排除右上角 130px 窗口控制按钮）
		if (y < 42 && x < w - 130) {
			*result = HTCAPTION;
			return true;
		}

		*result = HTCLIENT;
		return true;
	} break;
	}
#endif
	return Ui::RpWidget::nativeEvent(eventType, message, result);
}

} // namespace MeetingUI
