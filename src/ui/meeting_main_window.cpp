#include "src/ui/meeting_main_window.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/shadow_helper.h"
#include "src/ui/video_test_widget.h"
#include "styles/style_widgets.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
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
	setFixedSize(420, 320);
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
			font-family: "Microsoft YaHei";
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
	mainLayout->setSpacing(14);

	auto titleLayout = new QHBoxLayout();
	auto titleLabel = new QLabel(QString::fromUtf8("加入会议"), container);
	QFont tf = titleLabel->font();
	tf.setPixelSize(18);
	tf.setBold(true);
	titleLabel->setFont(tf);
	titleLayout->addWidget(titleLabel);
	titleLayout->addStretch();
	mainLayout->addLayout(titleLayout);

	// 第一个输入框：服务器地址
	_serverUrlInput = new QLineEdit(container);
	_serverUrlInput->setPlaceholderText(QString::fromUtf8("请输入服务器地址 (如 ws://127.0.0.1:7880)"));
	_serverUrlInput->setText("ws://127.0.0.1:7880");
	mainLayout->addWidget(_serverUrlInput);

	// 第二个输入框：Token
	_tokenInput = new QLineEdit(container);
	_tokenInput->setPlaceholderText(QString::fromUtf8("请输入Token"));
	_tokenInput->setText("");
	mainLayout->addWidget(_tokenInput);

	auto optLayout = new QHBoxLayout();
	_audioMuteBox = new QCheckBox(QString::fromUtf8("入会开启麦克风"), container);
	_audioMuteBox->setChecked(true);
	_videoMuteBox = new QCheckBox(QString::fromUtf8("入会开启摄像头"), container);
	_videoMuteBox->setChecked(true);
	optLayout->addWidget(_audioMuteBox);
	optLayout->addWidget(_videoMuteBox);
	mainLayout->addLayout(optLayout);

	mainLayout->addSpacing(8);

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
	connect(_joinBtn, &QPushButton::clicked, this, &QDialog::accept);
}

QString JoinMeetingDialog::serverUrl() const {
	return _serverUrlInput ? _serverUrlInput->text().trimmed() : QString();
}

QString JoinMeetingDialog::token() const {
	return _tokenInput ? _tokenInput->text().trimmed() : QString();
}

QString JoinMeetingDialog::meetingId() const {
	return serverUrl();
}

QString JoinMeetingDialog::displayName() const {
	return QString::fromUtf8("LiveKit用户");
}

bool JoinMeetingDialog::isAudioMuted() const {
	return _audioMuteBox && !_audioMuteBox->isChecked();
}

bool JoinMeetingDialog::isVideoMuted() const {
	return _videoMuteBox && !_videoMuteBox->isChecked();
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
}

void MeetingMainWindow::onCardClicked(ActionCardType type) {
	if (type == ActionCardType::JoinMeeting) {
		JoinMeetingDialog dlg(this);
		if (dlg.exec() == QDialog::Accepted) {
			MeetingRoomWindow::Config cfg;
			cfg.serverUrl = dlg.serverUrl();
			cfg.token = dlg.token();
			cfg.displayName = dlg.displayName();
			cfg.audioMuted = dlg.isAudioMuted();
			cfg.videoEnabled = !dlg.isVideoMuted();

			auto *roomWindow = new MeetingRoomWindow(cfg);
			roomWindow->setAttribute(Qt::WA_DeleteOnClose);
			roomWindow->show();
		}
	} else if (type == ActionCardType::QuickMeeting) {
		MeetingRoomWindow::Config cfg;
		cfg.serverUrl = "wss://project-a-ofnzcsum.livekit.cloud";
		cfg.token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJBUEl0ZXhzeFQ5QnJ0MzgiLCJzdWIiOiJ3aW4iLCJleHAiOjE3ODg4NDE4MTksIm5iZiI6MTc4Nzk0MTgxOSwiaWF0IjoxNzg3OTQxODE5LCJpZGVudGl0eSI6IndpbiIsInZpZGVvIjp7InJvb21Kb2luIjp0cnVlLCJyb29tIjoidGVzdCIsImNhblB1Ymxpc2giOnRydWUsImNhblN1YnNjcmliZSI6dHJ1ZSwiY2FuUHVibGlzaERhdGEiOnRydWV9fQ.3yhpA3erjcqXXLVKks7pVCCFo3bwfyQoSCepN1wuRNU";
		cfg.displayName = QString::fromUtf8("快速会议主持人");
		cfg.audioMuted = false;
		cfg.videoEnabled = true;

		auto *roomWindow = new MeetingRoomWindow(cfg);
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
