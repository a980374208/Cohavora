#include "src/ui/meeting_auto_share.h"
#include <QtCore/QCoreApplication>
#include "src/ui/app_branding.h"
#include "src/ui/meeting_main_window.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/update/update_service.h"
#include "src/ui/app_icons.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_booking_dialog.h"
#include "src/ui/meeting_detail_dialog.h"
#include "src/ui/meeting_list_dialog.h"
#include "src/ui/login_dialog.h"
#include "src/ui/settings_dialog.h"
#include "src/ui/ui_timing_observer.h"
#include "src/ui/shadow_helper.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/core/call_invite_controller.h"
#include "src/core/contact_catalog_controller.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/presence_watch_controller.h"
#include "src/media/desktop_capture.h"
#include "src/net/session_manager.h"
#include "src/net/user_event_stream_client.h"
#include "src/ui/call/incoming_call_window.h"
#include "src/ui/call/outgoing_call_overlay.h"
#include "src/ui/contacts/contacts_page_widget.h"
#include "src/ui/contacts/user_profile_dialog.h"
#include "src/telemetry/telemetry_report.h"
#include "styles/style_widgets.h"
#include <openssl/crypto.h>
#include <QtCore/QDate>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/qscopeguard.h>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QFont>
#include <QtGui/QScreen>
#include <algorithm>

#ifdef Q_OS_LINUX
#include "src/ui/linux_window_frame.h"
#endif

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {

void populateCopilotConfig(
		MeetingRoomWindow::Config &config,
		MeetingMainWindow *mainWin,
		const std::shared_ptr<OpenMeeting::AgentCoordinator> &agentCoordinator,
		OpenMeeting::ContactCatalogController *contacts = nullptr,
		OpenMeeting::PresenceWatchController *presence = nullptr,
		OpenMeeting::CallInviteController *callInvite = nullptr) {
	config.agentCoordinator = agentCoordinator;
	config.contacts = contacts;
	config.presence = presence;
	config.callInvite = callInvite;
	QPointer<MeetingMainWindow> owner(mainWin);
	config.openCopilotSettings = [owner](QWidget *dialogParent) {
		if (owner) {
			owner->openCopilotSettings(dialogParent);
		}
	};
	config.openSettingsPage = [owner](int pageIndex, int subTabIndex, QWidget *dialogParent) {
		if (owner) {
			owner->openSettingsAtPage(pageIndex, dialogParent, subTabIndex);
		}
	};
}

class BrandBadgeWidget final : public QWidget {
public:
	explicit BrandBadgeWidget(QWidget *parent = nullptr)
		: QWidget(parent) {
		setFixedSize(26, 26);
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(Theme::color(Theme::ColorRole::Accent));
		p.drawRoundedRect(rect(), 7, 7);
		const QIcon ic = Theme::icon(Theme::Icon::Video, Theme::ColorRole::OnAccent);
		ic.paint(&p, rect().adjusted(5, 5, -5, -5));
	}
};

QIcon buildAccountAvatarIcon(const QString &nickname, bool loggedIn, qreal dpr) {
	const int logical = 22;
	const qreal scale = std::max<qreal>(1.0, dpr);
	QPixmap pix(static_cast<int>(logical * scale), static_cast<int>(logical * scale));
	pix.setDevicePixelRatio(scale);
	pix.fill(Qt::transparent);

	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const QRectF avatarRect(0.5, 0.5, logical - 1.0, logical - 1.0);
	p.setPen(Qt::NoPen);
	p.setBrush(Theme::color(loggedIn ? Theme::ColorRole::Accent : Theme::ColorRole::TextMuted));
	p.drawEllipse(avatarRect);

	const QString trimmed = nickname.trimmed();
	const QString initial = trimmed.isEmpty()
		? QStringLiteral("?")
		: trimmed.left(1).toUpper();
	QFont f(QStringLiteral("Microsoft YaHei"));
	f.setPixelSize(11);
	f.setBold(true);
	p.setFont(f);
	p.setPen(Theme::color(Theme::ColorRole::OnAccent));
	p.drawText(avatarRect, Qt::AlignCenter, initial);

	// Bottom-right online indicator dot
	const QRectF dotRect(logical - 7.5, logical - 7.5, 7.0, 7.0);
	p.setBrush(Theme::color(loggedIn ? Theme::ColorRole::Success : Theme::ColorRole::Border));
	p.setPen(QPen(Theme::color(Theme::ColorRole::SurfaceAlt), 1.4));
	p.drawEllipse(dotRect);

	return QIcon(pix);
}

void repolishWidget(QWidget *w) {
	if (!w) return;
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

} // namespace

// ----------------------------------------------------
// WindowControlsWidget 窗口右上角控制按钮
// ----------------------------------------------------

WindowControlsWidget::WindowControlsWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedSize(114, 32);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void WindowControlsWidget::setMaximized(bool maximized) {
	if (_maximized != maximized) {
		_maximized = maximized;
		update();
	}
}

void WindowControlsWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const int h = height();
	const int btnW = 38;

	_minRect = QRect(0, 0, btnW, h);
	_maxRect = QRect(btnW, 0, btnW, h);
	_closeRect = QRect(btnW * 2, 0, btnW, h);

	const auto paintHover = [&p](const QRect &r, const QColor &fill) {
		QPainterPath path;
		path.addRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
		p.fillPath(path, fill);
	};

	// 最小化按钮
	if (_hoverBtn == HoverBtn::Min) {
		paintHover(_minRect, Theme::color(Theme::ColorRole::Hover));
	}
	p.setPen(QPen(Theme::color(Theme::ColorRole::TextSecondary), 1.3, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(_minRect.center().x() - 5, _minRect.center().y(), _minRect.center().x() + 5, _minRect.center().y());

	// 最大化/还原按钮
	if (_hoverBtn == HoverBtn::Max) {
		paintHover(_maxRect, Theme::color(Theme::ColorRole::Hover));
	}
	p.setPen(QPen(Theme::color(Theme::ColorRole::TextSecondary), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	const QPoint mc = _maxRect.center();
	if (_maximized) {
		p.drawRoundedRect(QRectF(mc.x() - 5.0, mc.y() - 3.0, 8.0, 8.0), 1.2, 1.2);
		QPainterPath back;
		back.moveTo(mc.x() - 3.0, mc.y() - 3.0);
		back.lineTo(mc.x() - 3.0, mc.y() - 5.0);
		back.lineTo(mc.x() + 5.0, mc.y() - 5.0);
		back.lineTo(mc.x() + 5.0, mc.y() + 3.0);
		back.lineTo(mc.x() + 3.0, mc.y() + 3.0);
		p.drawPath(back);
	} else {
		p.drawRoundedRect(QRectF(mc.x() - 5.0, mc.y() - 5.0, 10.0, 10.0), 1.5, 1.5);
	}

	// 关闭按钮
	if (_hoverBtn == HoverBtn::Close) {
		paintHover(_closeRect, Theme::color(Theme::ColorRole::Danger));
		p.setPen(QPen(Theme::color(Theme::ColorRole::OnAccent), 1.4, Qt::SolidLine, Qt::RoundCap));
	} else {
		p.setPen(QPen(Theme::color(Theme::ColorRole::TextSecondary), 1.3, Qt::SolidLine, Qt::RoundCap));
	}
	const int ccx = _closeRect.center().x();
	const int ccy = _closeRect.center().y();
	p.drawLine(ccx - 4, ccy - 4, ccx + 4, ccy + 4);
	p.drawLine(ccx + 4, ccy - 4, ccx - 4, ccy + 4);
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

JoinMeetingDialog::JoinMeetingDialog(
		QWidget *parent,
		const QString &initialMeetingId,
		std::optional<OpenMeeting::MeetingSettings> meetingSettings,
		OpenMeeting::SessionManager *session)
	: QDialog(parent)
	, _meetingSettings(std::move(meetingSettings))
	, _session(session ? session : &OpenMeeting::SessionManager::instance()) {
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Join Meeting"));
	setObjectName(QStringLiteral("joinMeetingDialog"));
	resize(460, 560);
	setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground, true);

	MeetingUI::AppTheme::setStyleVariant(*this, "meeting-main-window-this");

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
	auto titleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Join Meeting"), container);
	QFont tf = titleLabel->font();
	tf.setPixelSize(18);
	tf.setBold(true);
	titleLabel->setFont(tf);
	titleLayout->addWidget(titleLabel);
	titleLayout->addStretch();

	_closeBtn = new QPushButton(QString::fromUtf8("✕"), container);
	_closeBtn->setObjectName(QStringLiteral("joinCloseButton"));
	_closeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Close"));
	MeetingUI::AppTheme::setStyleVariant(*_closeBtn, "meeting-main-window-closebtn");
	_closeBtn->setFixedSize(24, 24);
	_closeBtn->setAutoDefault(false);
	connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
	titleLayout->addWidget(_closeBtn);
	mainLayout->addLayout(titleLayout);

	// 会议号输入框
	auto idLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Meeting ID"), container);
	MeetingUI::AppTheme::setStyleVariant(*idLabel, "meeting-main-window-idlabel");
	mainLayout->addWidget(idLabel);

	_meetingIdInput = new QLineEdit(container);
	_meetingIdInput->setObjectName(QStringLiteral("joinMeetingId"));
	_meetingIdInput->setAccessibleName(idLabel->text());
	_meetingIdInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "9-digit meeting ID (e.g. 847-123-456)"));
	_meetingIdInput->setText(initialMeetingId);
	_meetingIdInput->setReadOnly(!initialMeetingId.trimmed().isEmpty());
	mainLayout->addWidget(_meetingIdInput);

	// 入会密码输入框
	auto pwdLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Meeting Password (Optional)"), container);
	MeetingUI::AppTheme::setStyleVariant(*pwdLabel, "meeting-main-window-pwdlabel");
	mainLayout->addWidget(pwdLabel);

	_passwordInput = new QLineEdit(container);
	_passwordInput->setObjectName(QStringLiteral("joinPassword"));
	_passwordInput->setAccessibleName(pwdLabel->text());
	_passwordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter the password if required"));
	_passwordInput->setEchoMode(QLineEdit::Password);
	mainLayout->addWidget(_passwordInput);

	// 参会昵称输入框
	auto nameLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Display Name"), container);
	MeetingUI::AppTheme::setStyleVariant(*nameLabel, "meeting-main-window-namelabel");
	mainLayout->addWidget(nameLabel);

	_displayNameInput = new QLineEdit(container);
	_displayNameInput->setObjectName(QStringLiteral("joinDisplayName"));
	_displayNameInput->setAccessibleName(nameLabel->text());
	_displayNameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Name shown in the meeting"));
	_displayNameInput->setText(_session->nickname());
	mainLayout->addWidget(_displayNameInput);

	// 入会音视频设置
	auto optLayout = new QHBoxLayout();
	_audioMuteBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), container);
	_audioMuteBox->setObjectName(QStringLiteral("joinMicrophone"));
	_audioMuteBox->setChecked(_session->mediaPreferences().enableMicrophone);
	_videoMuteBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), container);
	_videoMuteBox->setObjectName(QStringLiteral("joinCamera"));
	_videoMuteBox->setChecked(_session->mediaPreferences().enableVideo);
	optLayout->addWidget(_audioMuteBox);
	optLayout->addWidget(_videoMuteBox);
	mainLayout->addLayout(optLayout);

	_meetingPolicyLabel = new QLabel(container);
	_meetingPolicyLabel->setWordWrap(true);
	_meetingPolicyLabel->setTextFormat(Qt::PlainText);
	MeetingUI::AppTheme::setStyleVariant(*_meetingPolicyLabel, "meeting-main-window-meetingpolicylabel");
	if (_meetingSettings && _meetingSettings->disableCameraOnJoin &&
		_meetingSettings->disableMicrophoneOnJoin) {
		_meetingPolicyLabel->setText(QCoreApplication::translate("MeetingUI", "This meeting requires your camera and microphone to be off when joining. Your personal preferences above are still saved."));
	} else if (_meetingSettings && _meetingSettings->disableCameraOnJoin) {
		_meetingPolicyLabel->setText(QCoreApplication::translate("MeetingUI", "This meeting requires your camera to be off when joining. Your personal preferences above are still saved."));
	} else if (_meetingSettings && _meetingSettings->disableMicrophoneOnJoin) {
		_meetingPolicyLabel->setText(QCoreApplication::translate("MeetingUI", "This meeting requires your microphone to be off when joining. Your personal preferences above are still saved."));
	} else {
		_meetingPolicyLabel->hide();
	}
	mainLayout->addWidget(_meetingPolicyLabel);

	_encryptionToggleBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "⚙ Encryption / Security Settings ▾"), container);
	_encryptionToggleBtn->setObjectName(QStringLiteral("joinEncryptionToggle"));
	_encryptionToggleBtn->setAutoDefault(false);
	_encryptionToggleBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Encryption / Security Settings"));
	mainLayout->addWidget(_encryptionToggleBtn);
	_encryptionWidget = new QWidget(container);
	_encryptionWidget->setObjectName(QStringLiteral("joinEncryptionWidget"));
	auto encryptionLayout = new QVBoxLayout(_encryptionWidget);
	encryptionLayout->setContentsMargins(0, 2, 0, 2);
	encryptionLayout->setSpacing(8);
	_encryptionRequired = new QCheckBox(QCoreApplication::translate("MeetingUI", "Require end-to-end encryption for this session"), _encryptionWidget);
	_encryptionRequired->setObjectName(QStringLiteral("e2eeRequired"));
	encryptionLayout->addWidget(_encryptionRequired);
	auto encryptionInfo = new QLabel(QCoreApplication::translate("MeetingUI", "Use the same encryption key as the other participants. Share it through a trusted channel. The key is not saved. Participant and connection metadata remain visible to the service."), _encryptionWidget);
	encryptionInfo->setWordWrap(true);
	encryptionLayout->addWidget(encryptionInfo);
	auto encryptionProfile = new QLabel(QCoreApplication::translate("MeetingUI", "Encrypted meetings support VP8 and H264. Auto uses VP8; select a supported codec in Settings for camera and screen sharing."), _encryptionWidget);
	encryptionProfile->setWordWrap(true);
	encryptionLayout->addWidget(encryptionProfile);
	_encryptionKeyInput = new QLineEdit(_encryptionWidget);
	_encryptionKeyInput->setObjectName(QStringLiteral("e2eeKeyInput"));
	_encryptionKeyInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Encryption key"));
	_encryptionKeyInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "ASCII encryption key (not the meeting password)"));
	_encryptionKeyInput->setEchoMode(QLineEdit::Password);
	_encryptionKeyInput->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
	_encryptionKeyInput->setContextMenuPolicy(Qt::NoContextMenu);
	_encryptionKeyInput->setMaxLength(4097);
	_encryptionKeyInput->setEnabled(false);
	encryptionLayout->addWidget(_encryptionKeyInput);
	_encryptionWidget->hide();
	mainLayout->addWidget(_encryptionWidget);
	_encryptionError = new QLabel(container);
	_encryptionError->setObjectName(QStringLiteral("e2eeInputError"));
	_encryptionError->setWordWrap(true);
	AppTheme::setStyleVariant(*_encryptionError, "meeting-main-window-meetingpolicylabel");
	mainLayout->addWidget(_encryptionError);
	connect(_encryptionToggleBtn, &QPushButton::clicked, this, [this] {
		const bool expanded = _encryptionWidget->isHidden();
		_encryptionWidget->setVisible(expanded);
		_encryptionToggleBtn->setText(QCoreApplication::translate("MeetingUI", expanded ? "⚙ Encryption / Security Settings ▴" : "⚙ Encryption / Security Settings ▾"));
		scheduleFormFit();
	});
	connect(_encryptionRequired, &QCheckBox::toggled, this, [this](bool required) {
		if (required) livekit::MarkSensitiveMemoryUsed();
		_encryptionKeyInput->setEnabled(required && !_globalEncryption && !_isLoading);
		if (!required) clearEncryptionEditor();
		revokeEncryptionRequest();
		if (!_globalEncryption) {
			_encryptionError->clear();
			_encryptionError->hide();
		}
		scheduleFormFit();
	});
	connect(_session, &OpenMeeting::SessionManager::meetingSecurityPreferencesChanged, this, [this] {
		// Revalidate pending admission against the current usable defaults.
		revokeEncryptionRequest();
		clearEncryptionEditor();
		updateEncryptionDefaults();
	});
	connect(_session, &OpenMeeting::SessionManager::authenticationReset, this, [this] { reject(); });

	// 手动/高级直连设置折叠栏
	_manualToggleBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "⚙ Advanced LiveKit Connection ▾"), container);
	_manualToggleBtn->setObjectName("linkBtn");
	_manualToggleBtn->setAutoDefault(false);
	_manualToggleBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Advanced LiveKit Connection"));
	connect(_manualToggleBtn, &QPushButton::clicked, this, &JoinMeetingDialog::toggleManualServer);
	mainLayout->addWidget(_manualToggleBtn);
	_manualToggleBtn->setVisible(initialMeetingId.trimmed().isEmpty());

	_manualWidget = new QWidget(container);
	auto manLayout = new QVBoxLayout(_manualWidget);
	manLayout->setContentsMargins(0, 2, 0, 2);
	manLayout->setSpacing(4);

	_serverUrlInput = new QLineEdit(_manualWidget);
	_serverUrlInput->setObjectName(QStringLiteral("joinServerUrl"));
	_serverUrlInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Server URL"));
	_serverUrlInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Server URL (e.g. ws://127.0.0.1:7880)"));
	_serverUrlInput->setText("ws://127.0.0.1:7880");

	_tokenInput = new QLineEdit(_manualWidget);
	_tokenInput->setObjectName(QStringLiteral("joinToken"));
	_tokenInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Custom LiveKit Token"));
	_tokenInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Custom LiveKit Token (Optional)"));

	manLayout->addWidget(_serverUrlInput);
	manLayout->addWidget(_tokenInput);
	_manualWidget->setVisible(false);
	mainLayout->addWidget(_manualWidget);
	updateEncryptionDefaults();

	// 状态/错误提示
	_statusLabel = new QLabel(container);
	_statusLabel->setObjectName(QStringLiteral("joinStatus"));
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-main-window-statuslabel");
	_statusLabel->setAlignment(Qt::AlignCenter);
	_statusLabel->setWordWrap(true);
	_statusLabel->setMinimumHeight(32);
	_statusLabel->setVisible(false);
	mainLayout->addWidget(_statusLabel);

	// 底部按钮栏
	auto btnLayout = new QHBoxLayout();
	btnLayout->addStretch();
	_cancelBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), container);
	_cancelBtn->setObjectName("cancelBtn");
	_cancelBtn->setAutoDefault(false);
	_joinBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Join Meeting"), container);
	_joinBtn->setObjectName("joinBtn");
	_joinBtn->setDefault(true);

	btnLayout->addWidget(_cancelBtn);
	btnLayout->addWidget(_joinBtn);
	mainLayout->addLayout(btnLayout);

	connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
	connect(_joinBtn, &QPushButton::clicked, this, &JoinMeetingDialog::onJoinClicked);
	connect(_meetingIdInput, &QLineEdit::returnPressed, this, &JoinMeetingDialog::onJoinClicked);
	connect(_encryptionKeyInput, &QLineEdit::returnPressed, this, &JoinMeetingDialog::onJoinClicked);
	AppTheme::makeDialogAdaptive(*this, QSize(460, 560));
}

JoinMeetingDialog::~JoinMeetingDialog() {
	clearEncryptionEditor();
	revokeEncryptionRequest();
}

void JoinMeetingDialog::showEvent(QShowEvent *e) {
	QDialog::showEvent(e);
	scheduleFormFit();
}

void JoinMeetingDialog::scheduleFormFit() {
	if (!isVisible() || _formFitPending) return;
	_formFitPending = true;
	// Measure after visibility and wrapped-text changes reach the inner layout.
	QTimer::singleShot(0, this, [this] {
		_formFitPending = false;
		if (isVisible()) fitFormToScreen();
	});
}

void JoinMeetingDialog::fitFormToScreen() {
	auto *scroll = findChild<QScrollArea *>(QStringLiteral("adaptiveDialogScroll"));
	if (!scroll || !scroll->widget()) return;
	auto *content = scroll->widget();
	auto *card = findChild<QWidget *>(QStringLiteral("dialogContainer"));
	if (!content->layout() || !card || !card->layout()) return;
	auto *targetScreen = screen();
	const auto available = targetScreen
		? targetScreen->availableGeometry().adjusted(16, 32, -16, -32)
		: QRect(pos(), QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
	const auto applyDensity = [&](bool compact) {
		content->layout()->setContentsMargins(compact ? 8 : 12, compact ? 8 : 12,
			compact ? 8 : 12, compact ? 8 : 12);
		card->layout()->setContentsMargins(compact ? 20 : 28, compact ? 12 : 24,
			compact ? 20 : 28, compact ? 12 : 24);
		card->layout()->setSpacing(compact ? 8 : 12);
		for (auto *childLayout : content->findChildren<QLayout *>()) {
			childLayout->invalidate();
			childLayout->activate();
		}
		content->layout()->invalidate();
		content->layout()->activate();
	};
	const auto measuredSize = [&](int scrollBarWidth) {
		auto *form = content->layout();
		const auto minimum = form->totalMinimumSize();
		const int width = qMin(available.width(), qMax(460, minimum.width()) + scrollBarWidth);
		const int formWidth = qMax(1, width - scrollBarWidth);
		const int height = form->hasHeightForWidth()
			? form->totalHeightForWidth(formWidth) : form->totalSizeHint().height();
		return QSize(width, qMax(minimum.height(), height));
	};
	applyDensity(false);
	auto desired = measuredSize(0);
	if (desired.height() > available.height()) {
		applyDensity(true);
		desired = measuredSize(0);
	}
	if (desired.height() > available.height()) {
		desired = measuredSize(scroll->verticalScrollBar()->sizeHint().width());
	}
	desired.setHeight(qMin(available.height(), qMax(560, desired.height())));
	if (size() != desired) resize(desired);
	if (targetScreen) {
		move(qBound(available.left(), x(), available.right() - width() + 1),
			qBound(available.top(), y(), available.bottom() - height() + 1));
	}
}

void JoinMeetingDialog::clearEncryptionEditor() {
	if (!_encryptionKeyInput) return;
	_encryptionKeyInput->setText(QString(_encryptionKeyInput->text().size(), QChar(0)));
	_encryptionKeyInput->clear();
}

void JoinMeetingDialog::revokeEncryptionRequest() {
	if (_encryptionRequest) _encryptionRequest->Revoke();
	_encryptionRequest.reset();
}

void JoinMeetingDialog::updateEncryptionDefaults() {
	const bool previousGlobal = _globalEncryption;
	_globalEncryption = _session->meetingSecurityPreferences().allMeetingsE2ee &&
		_session->hasMeetingEncryptionKey();
	_encryptionToggleBtn->setEnabled(!_globalEncryption && !_isLoading);
	_encryptionToggleBtn->setToolTip(_globalEncryption ? QCoreApplication::translate("MeetingUI", "End-to-end encryption for all meetings is enabled in Settings.") : QString());
	if (_globalEncryption || previousGlobal) {
		revokeEncryptionRequest();
		clearEncryptionEditor();
		_encryptionRequired->setChecked(_globalEncryption);
		_encryptionWidget->hide();
		_encryptionToggleBtn->setText(QCoreApplication::translate("MeetingUI", "⚙ Encryption / Security Settings ▾"));
	}
	_encryptionRequired->setEnabled(!_globalEncryption && !_isLoading);
	_encryptionKeyInput->setEnabled(!_globalEncryption && !_isLoading && _encryptionRequired->isChecked());
	_encryptionError->setText(_globalEncryption ? QCoreApplication::translate("MeetingUI",
		"Using the encryption key configured for all meetings in Settings.") : QString());
	_encryptionError->setVisible(_globalEncryption);
	scheduleFormFit();
}

bool JoinMeetingDialog::prepareEncryptionRequest() {
	livekit::MeetingEncryptionRequest request;
	try {
		if (_globalEncryption) {
			request = _session->meetingEncryptionRequest(OpenMeeting::MeetingEncryptionEntry::JoinMeeting);
		} else if (_encryptionRequired->isChecked()) {
			auto text = _encryptionKeyInput->text();
			auto bytes = text.toUtf8();
			text.fill(QChar(0));
			const auto cleanup = qScopeGuard([&] {
				if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
			});
			request.mode = livekit::MeetingEncryptionMode::Required;
			request.secret = livekit::MeetingSecretHandle::Create(std::vector<uint8_t>(bytes.begin(), bytes.end()));
		}
		request.Validate();
	} catch (const livekit::EncryptionRequestException &) {
		if (_joinTiming) _joinTiming->Finish(livekit::diagnostic::Outcome::Failure);
		if (_pageTiming) _pageTiming->Finish(livekit::diagnostic::Outcome::Failure);
		revokeEncryptionRequest();
		_encryptionError->setText(QCoreApplication::translate("MeetingUI",
			_globalEncryption
				? "Set an encryption key in Settings > Security before joining."
				: "Enter 1 to 4096 printable ASCII characters. Unicode keys are not supported."));
		_encryptionError->show();
		if (!_globalEncryption) {
			_encryptionWidget->show();
			_encryptionToggleBtn->setText(QCoreApplication::translate("MeetingUI", "⚙ Encryption / Security Settings ▴"));
			_encryptionKeyInput->setFocus();
		}
		scheduleFormFit();
		return false;
	}
	revokeEncryptionRequest();
	_encryptionRequest = std::move(request);
	if (!_globalEncryption) {
		_encryptionError->clear();
		_encryptionError->hide();
	}
	scheduleFormFit();
	return true;
}

std::optional<livekit::MeetingEncryptionRequest> JoinMeetingDialog::takeEncryptionRequest() {
	return std::exchange(_encryptionRequest, std::nullopt);
}

void JoinMeetingDialog::setMeetingPassword(const QString &password) {
	_passwordInput->setText(password);
}

void JoinMeetingDialog::accept() {
	if (!_encryptionRequest && !prepareEncryptionRequest()) return;
	clearEncryptionEditor();
	QDialog::accept();
}

void JoinMeetingDialog::toggleManualServer() {
	bool isVisible = _manualWidget->isVisible();
	_manualWidget->setVisible(!isVisible);
	_manualToggleBtn->setText(!isVisible ? QCoreApplication::translate("MeetingUI", "⚙ Advanced LiveKit Connection ▴") : QCoreApplication::translate("MeetingUI", "⚙ Advanced LiveKit Connection ▾"));
	scheduleFormFit();
}

void JoinMeetingDialog::reject() {
	if (_pageTiming) _pageTiming->Finish(livekit::diagnostic::Outcome::Cancelled);
	if (_joinTiming) _joinTiming->Finish(livekit::diagnostic::Outcome::Cancelled);
	_isCancelled = true;
	_isLoading = false;
	clearEncryptionEditor();
	revokeEncryptionRequest();
	QDialog::reject();
}

void JoinMeetingDialog::closeEvent(QCloseEvent *e) {
	if (_joinTiming) _joinTiming->Finish(livekit::diagnostic::Outcome::Cancelled);
	if (_pageTiming) _pageTiming->Finish(livekit::diagnostic::Outcome::Cancelled);
	_isCancelled = true;
	_isLoading = false;
	clearEncryptionEditor();
	revokeEncryptionRequest();
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
	if (_manualToggleBtn) _manualToggleBtn->setEnabled(!loading);
	if (_manualWidget) _manualWidget->setEnabled(!loading);
	if (_encryptionToggleBtn) _encryptionToggleBtn->setEnabled(!loading && !_globalEncryption);
	if (_encryptionWidget) _encryptionWidget->setEnabled(!loading && !_globalEncryption);

	if (loading) {
		if (_joinBtn) _joinBtn->setText(QCoreApplication::translate("MeetingUI", "Joining..."));
		if (_statusLabel) {
			MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-main-window-statuslabel-2");
			_statusLabel->setText(statusText.isEmpty() ? QCoreApplication::translate("MeetingUI", "Processing join request...") : statusText);
			_statusLabel->setVisible(true);
		}
	} else {
		if (_joinBtn) _joinBtn->setText(QCoreApplication::translate("MeetingUI", "Join Meeting"));
	}
	scheduleFormFit();
}

void JoinMeetingDialog::showError(const QString &msg) {
	if (_pageTiming) _pageTiming->Finish(livekit::diagnostic::Outcome::Failure);
	if (_joinTiming) _joinTiming->Finish(livekit::diagnostic::Outcome::Failure);
	if (!_statusLabel) return;
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-main-window-statuslabel-3");
	_statusLabel->setText(msg);
	_statusLabel->setVisible(!msg.isEmpty());
	scheduleFormFit();
}

void JoinMeetingDialog::onJoinClicked() {
	if (_isLoading) return;
	_joinTiming = std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
		livekit::diagnostic::TimingMetric::JoinReady, livekit::diagnostic::TimingScene::Meeting);
	_pageTiming = std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
		livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting);

	// 1. 检查是否启用了高级手动直连模式
	_isManualConnection = _manualWidget && _manualWidget->isVisible()
		&& _tokenInput && !_tokenInput->text().trimmed().isEmpty();
	if (_isManualConnection) {
		_resolvedServerUrl = _serverUrlInput ? _serverUrlInput->text().trimmed() : QString();
		_resolvedToken = _tokenInput ? _tokenInput->text().trimmed() : QString();
		_cleanMeetingId = _meetingIdInput ? _meetingIdInput->text().trimmed() : QString();
		if (_cleanMeetingId.isEmpty()) {
			_cleanMeetingId = "livekit_room";
		}
		if (_resolvedServerUrl.isEmpty()) {
			showError(QCoreApplication::translate("MeetingUI", "Enter a LiveKit server URL for a direct connection"));
			return;
		}
		if (!prepareEncryptionRequest()) return;
		persistMediaPreferences();
		accept();
		return;
	}

	// 2. 正常业务入会流程
	QString rawId = _meetingIdInput ? _meetingIdInput->text().trimmed() : QString();
	_cleanMeetingId = rawId;
	_cleanMeetingId.remove('-').remove(' ');

	if (_cleanMeetingId.isEmpty()) {
		showError(QCoreApplication::translate("MeetingUI", "Enter a valid meeting ID"));
		if (_meetingIdInput) _meetingIdInput->setFocus();
		return;
	}
	if (!prepareEncryptionRequest()) return;

	const QString password = _passwordInput ? _passwordInput->text() : QString();
	auto &session = *_session;

	setLoading(true, QCoreApplication::translate("MeetingUI", "Checking meeting access..."));

	QPointer<JoinMeetingDialog> self = this;

	// 第一阶段：加入会议校验
	session.httpClient().joinMeeting(_cleanMeetingId, password, [self](bool ok, bool pass, const OpenMeeting::HttpError &err) {
		if (!self || self->_isCancelled) {
			return;
		}
		if (!ok) {
			self->revokeEncryptionRequest();
			self->setLoading(false);
			if (err.message.contains("user already in meeting", Qt::CaseInsensitive) || err.code == 200001) {
				self->showError(QCoreApplication::translate("MeetingUI", "This account is already in the meeting. Use Guest Access or sign in with another account to join."));
			} else {
				self->showError(QCoreApplication::translate("MeetingUI", "Meeting access check failed: %1").arg(err.message.isEmpty() ? QCoreApplication::translate("MeetingUI", "The meeting does not exist or the network is unreachable") : err.message));
			}
			return;
		}

		self->setLoading(true, QCoreApplication::translate("MeetingUI", "Requesting LiveKit credentials..."));

		// 第二阶段：换取 LiveKit Token 与 URL
		auto &sess = *self->_session;
		sess.httpClient().getMeetingToken(self->_cleanMeetingId, [self](bool tokenOk, const OpenMeeting::LiveKitAuthInfo &auth, const OpenMeeting::HttpError &tokenErr) {
			if (!self || self->_isCancelled) {
				return;
			}
			self->setLoading(false);
			if (!tokenOk || auth.url.isEmpty() || auth.token.isEmpty()) {
				self->revokeEncryptionRequest();
				self->showError(QCoreApplication::translate("MeetingUI", "Unable to obtain credentials: %1").arg(tokenErr.message.isEmpty() ? QCoreApplication::translate("MeetingUI", "Invalid credentials response") : tokenErr.message));
				return;
			}

			self->_resolvedServerUrl = auth.url;
			self->_resolvedToken = auth.token;

			self->persistMediaPreferences();
			self->accept();
		});
	});
}

void JoinMeetingDialog::persistMediaPreferences() {
	auto &session = *_session;
	if (_audioMuteBox) session.setEnableMicrophone(_audioMuteBox->isChecked());
	if (_videoMuteBox) session.setEnableVideo(_videoMuteBox->isChecked());
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
	return name.isEmpty() ? QCoreApplication::translate("MeetingUI", "Participant") : name;
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
	setWindowTitle(AppBranding::displayName());
	resize(1040, 660);
	setMinimumSize(900, 580);
	if (!parent) {
		AppTheme::centerOnScreen(*this);
	}
	setMouseTracking(true);

	// 设置无边框但保留系统窗口特性
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);

	auto &session = OpenMeeting::SessionManager::instance();
	_meetingCatalog = new OpenMeeting::MeetingCatalogController(session, this);
	_agentCoordinator = std::make_shared<OpenMeeting::AgentCoordinator>(session);
	_agentCoordinator->attachCatalog(_meetingCatalog);
	setupContactsAndCallRuntime();
	OpenMeeting::AgentHostSettingsBridge hostBridge;
	hostBridge.getTheme = []() {
		return AppTheme::themeId(AppTheme::selectedTheme());
	};
	hostBridge.setTheme = [](const QString &themeId) {
		AppTheme::setTheme(AppTheme::themeFromId(themeId));
		return true;
	};
	hostBridge.getLanguage = []() {
		return AppTranslation::languageId(AppTranslation::selectedLanguage());
	};
	hostBridge.setLanguage = [](const QString &langId) {
		AppTranslation::setLanguage(AppTranslation::languageFromId(langId));
		return true;
	};
	const auto ensureUpdateService = [this]() -> Cohavora::Update::UpdateService * {
		if (!_updateService) {
			_updateService = new Cohavora::Update::UpdateService(
				Cohavora::Update::UpdateService::BuildConfiguration(),
				QCoreApplication::applicationVersion(),
				this);
		}
		return _updateService;
	};
	const auto describeUpdateState = [ensureUpdateService]() -> QJsonObject {
		auto *svc = ensureUpdateService();
		using State = Cohavora::Update::State;
		using Error = Cohavora::Update::Error;
		auto stateName = [](State s) {
			switch (s) {
			case State::Idle: return QStringLiteral("idle");
			case State::Checking: return QStringLiteral("checking");
			case State::UpToDate: return QStringLiteral("up_to_date");
			case State::Available: return QStringLiteral("available");
			case State::Downloading: return QStringLiteral("downloading");
			case State::Ready: return QStringLiteral("ready");
			case State::Verifying: return QStringLiteral("verifying");
			case State::InstallerStarted: return QStringLiteral("installer_started");
			case State::Failed: return QStringLiteral("failed");
			case State::Exported: return QStringLiteral("exported");
			}
			return QStringLiteral("idle");
		};
		auto errorName = [](Error e) {
			switch (e) {
			case Error::None: return QStringLiteral("none");
			case Error::NotConfigured: return QStringLiteral("not_configured");
			case Error::InvalidConfiguration: return QStringLiteral("invalid_configuration");
			case Error::Network: return QStringLiteral("network");
			case Error::InvalidManifest: return QStringLiteral("invalid_manifest");
			case Error::Disk: return QStringLiteral("disk");
			case Error::Integrity: return QStringLiteral("integrity");
			case Error::Launch: return QStringLiteral("launch");
			}
			return QStringLiteral("none");
		};
		QJsonObject obj;
		obj.insert(QStringLiteral("appName"), QCoreApplication::applicationName().trimmed().isEmpty()
			? AppBranding::name() : QCoreApplication::applicationName());
		const QString ver = QCoreApplication::applicationVersion().trimmed();
		obj.insert(QStringLiteral("currentVersion"), ver.isEmpty() ? QStringLiteral("Development Build") : ver);
		obj.insert(QStringLiteral("checksConfigured"), svc->checksConfigured());
		obj.insert(QStringLiteral("state"), stateName(svc->state()));
		obj.insert(QStringLiteral("error"), errorName(svc->error()));
		if (!svc->release().version.isEmpty()) {
			obj.insert(QStringLiteral("availableVersion"), svc->release().version);
		}
		if (!svc->release().notes.isEmpty()) {
			obj.insert(QStringLiteral("releaseNotes"), svc->release().notes);
		}
		return obj;
	};
	hostBridge.getAppUpdateStatus = describeUpdateState;
	hostBridge.performAppUpdateAction = [ensureUpdateService, describeUpdateState](const QString &action) -> QJsonObject {
		auto *svc = ensureUpdateService();
		using State = Cohavora::Update::State;
		if (action == QStringLiteral("check")) {
			svc->check();
			QJsonObject res = describeUpdateState();
			res.insert(QStringLiteral("ok"), true);
			return res;
		}
		if (action == QStringLiteral("download")) {
			if (svc->state() != State::Available) {
				QJsonObject err;
				err.insert(QStringLiteral("ok"), false);
				err.insert(QStringLiteral("error"), QStringLiteral("No downloadable update is currently available. Check for updates first."));
				return err;
			}
			svc->download();
			QJsonObject res = describeUpdateState();
			res.insert(QStringLiteral("ok"), true);
			return res;
		}
		if (action == QStringLiteral("install")) {
			if (svc->state() != State::Ready) {
				QJsonObject err;
				err.insert(QStringLiteral("ok"), false);
				err.insert(QStringLiteral("error"), QStringLiteral("Update package is not downloaded/verified yet."));
				return err;
			}
			const bool started = svc->launchInstaller();
			QJsonObject res = describeUpdateState();
			res.insert(QStringLiteral("ok"), started);
			if (!started) {
				res.insert(QStringLiteral("error"), QStringLiteral("Failed to start installer."));
			}
			return res;
		}
		if (action == QStringLiteral("cancel")) {
			svc->cancel();
			QJsonObject res = describeUpdateState();
			res.insert(QStringLiteral("ok"), true);
			return res;
		}
		QJsonObject err;
		err.insert(QStringLiteral("ok"), false);
		err.insert(QStringLiteral("error"), QStringLiteral("Unsupported update action."));
		return err;
	};
	const QPointer<MeetingMainWindow> mainWinOwner(this);
	hostBridge.joinMeetingFromLobby = [mainWinOwner](
			const QString &meetingId,
			const QString &password,
			const QString &displayName) -> QJsonObject {
		QJsonObject res;
		if (!mainWinOwner) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Main window is no longer available."));
			return res;
		}
		auto &sess = OpenMeeting::SessionManager::instance();
		if (!sess.isLoggedIn()) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Please sign in before joining a meeting."));
			return res;
		}
		const QString cleanId = meetingId.trimmed();
		if (cleanId.isEmpty()) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Meeting ID is required."));
			return res;
		}
		auto reservation = mainWinOwner->_meetingEntryGuard.tryAcquire();
		if (!reservation) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("A meeting is already open or a request is in progress."));
			return res;
		}
		livekit::MeetingEncryptionRequest encryption;
		try {
			encryption = sess.meetingEncryptionRequest(OpenMeeting::MeetingEncryptionEntry::JoinMeeting);
		} catch (const livekit::EncryptionRequestException &) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Set an encryption key in Settings > Security before joining."));
			return res;
		}
		auto preferences = sess.mediaPreferences();
		if (mainWinOwner->_meetingCatalog) {
			for (const auto &m : mainWinOwner->_meetingCatalog->upcomingState().meetings) {
				if (m.meetingId == cleanId) {
					preferences.enableMicrophone = preferences.enableMicrophone && !m.settings.disableMicrophoneOnJoin;
					preferences.enableVideo = preferences.enableVideo && !m.settings.disableCameraOnJoin;
					break;
				}
			}
		}
		const QString effectiveDisplayName = displayName.trimmed().isEmpty()
			? sess.nickname()
			: displayName.trimmed();
		MeetingRoomWindow::Config config;
		config.meetingId = cleanId;
		config.displayName = effectiveDisplayName;
		config.audioMuted = !preferences.enableMicrophone;
		config.videoEnabled = preferences.enableVideo;
		config.invitationMode = InvitationMode::BusinessMeetingId;
		populateCopilotConfig(config, mainWinOwner.data(), mainWinOwner->_agentCoordinator,
			mainWinOwner->_contactCatalog, mainWinOwner->_presenceWatch, mainWinOwner->_callInvite);

		auto coordinator = OpenMeeting::MeetingCoordinator::create();
		mainWinOwner->bindMeetingCatalog(coordinator.get());
		if (!coordinator->pageTiming()) {
			coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
				livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
		}
		auto *roomWindow = new MeetingRoomWindow(config, coordinator);
		reservation->setParent(roomWindow);
		reservation.release();
		roomWindow->setAttribute(Qt::WA_DeleteOnClose);
		roomWindow->show();
		const auto authGeneration = sess.authGeneration();
		roomWindow->prepareMediaAndJoin([coordinator, cleanId, password, effectiveDisplayName,
			preferences, authGeneration, sessionManager = &sess,
			window = QPointer<MeetingRoomWindow>(roomWindow), encryption = std::move(encryption)]() mutable {
			if (!sessionManager->isLoggedIn() || sessionManager->authGeneration() != authGeneration) {
				encryption.Revoke();
				if (window) window->close();
				return;
			}
			coordinator->joinMeetingAsync(cleanId, password, effectiveDisplayName,
				preferences, std::move(encryption));
		});
		res.insert(QStringLiteral("ok"), true);
		res.insert(QStringLiteral("mode"), QStringLiteral("join"));
		res.insert(QStringLiteral("meetingId"), cleanId);
		res.insert(QStringLiteral("joining"), true);
		return res;
	};
	hostBridge.startQuickMeetingFromLobby = [mainWinOwner](
			const QString &title,
			int durationMinutes) -> QJsonObject {
		QJsonObject res;
		if (!mainWinOwner) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Main window is no longer available."));
			return res;
		}
		auto &sess = OpenMeeting::SessionManager::instance();
		if (!sess.isLoggedIn()) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Please sign in before starting a quick meeting."));
			return res;
		}
		auto reservation = mainWinOwner->_meetingEntryGuard.tryAcquire();
		if (!reservation) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("A meeting is already open or a request is in progress."));
			return res;
		}
		livekit::MeetingEncryptionRequest encryption;
		try {
			encryption = sess.meetingEncryptionRequest(OpenMeeting::MeetingEncryptionEntry::QuickMeeting);
		} catch (const livekit::EncryptionRequestException &) {
			res.insert(QStringLiteral("ok"), false);
			res.insert(QStringLiteral("error"), QStringLiteral("Set an encryption key in Settings > Security before joining."));
			return res;
		}
		const auto preferences = sess.mediaPreferences();
		const QString effectiveTitle = title.trimmed().isEmpty()
			? QCoreApplication::translate("MeetingUI", "%1's Instant Meeting").arg(sess.nickname())
			: title.trimmed().left(128);
		const int clampedMins = qBound(15, durationMinutes, 480);

		MeetingRoomWindow::Config config;
		config.displayName = sess.nickname();
		config.audioMuted = !preferences.enableMicrophone;
		config.videoEnabled = preferences.enableVideo;
		config.invitationMode = InvitationMode::BusinessMeetingId;
		populateCopilotConfig(config, mainWinOwner.data(), mainWinOwner->_agentCoordinator,
			mainWinOwner->_contactCatalog, mainWinOwner->_presenceWatch, mainWinOwner->_callInvite);

		auto coordinator = OpenMeeting::MeetingCoordinator::create();
		mainWinOwner->bindMeetingCatalog(coordinator.get());
		if (!coordinator->pageTiming()) {
			coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
				livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
		}
		auto *roomWindow = new MeetingRoomWindow(config, coordinator);
		reservation->setParent(roomWindow);
		reservation.release();
		roomWindow->setAttribute(Qt::WA_DeleteOnClose);
		roomWindow->show();
		const auto authGeneration = sess.authGeneration();
		roomWindow->prepareMediaAndJoin([coordinator, effectiveTitle, clampedMins, preferences,
			authGeneration, sessionManager = &sess,
			window = QPointer<MeetingRoomWindow>(roomWindow), encryption = std::move(encryption)]() mutable {
			if (!sessionManager->isLoggedIn() || sessionManager->authGeneration() != authGeneration) {
				encryption.Revoke();
				if (window) window->close();
				return;
			}
			coordinator->createAndJoinQuickMeetingAsync(effectiveTitle, clampedMins * 60,
				preferences, std::move(encryption));
		});
		res.insert(QStringLiteral("ok"), true);
		res.insert(QStringLiteral("mode"), QStringLiteral("quick_meeting"));
		res.insert(QStringLiteral("title"), effectiveTitle);
		res.insert(QStringLiteral("durationMinutes"), clampedMins);
		res.insert(QStringLiteral("starting"), true);
		return res;
	};
	_agentCoordinator->setHostSettingsBridge(std::move(hostBridge));
	initLayout();
#ifdef Q_OS_LINUX
	installLinuxWindowFrame(*this, {
		.radius = kWindowCornerRadius,
		.captionHitTest = [this](QPoint position) {
			return isTitleBarDragPosition(position);
		},
	});
#endif
	connect(&session,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingMainWindow::onSessionInvalidated,
	        Qt::QueuedConnection);
	connect(_meetingCatalog, &OpenMeeting::MeetingCatalogController::detailChanged,
		this, [this] { handlePendingMeetingEntryDetail(); });
	connect(&session, &OpenMeeting::SessionManager::authenticationReset,
		this, [this](quint64) {
			clearPendingMeetingEntry();
			if (_eventStream) _eventStream->stop();
			if (_incomingCallWindow) _incomingCallWindow->dismiss();
			if (_outgoingCallOverlay) _outgoingCallOverlay->dismiss();
			_activeMeetingCoordinator = nullptr;
			if (_presenceWatch) _presenceWatch->setLocalInMeeting(false);
			syncAccountBadge();
			syncJoinPresets();
			updateContactsNavBadge();
		});
	connect(&session, &OpenMeeting::SessionManager::loggedIn,
		this, [this](const OpenMeeting::UserInfo &) {
			syncAccountBadge();
			syncJoinPresets();
			startEventStreamForSession();
			if (_contactCatalog) _contactCatalog->refreshAll();
		});
	connect(&session, &OpenMeeting::SessionManager::currentUserProfileChanged,
		this, [this](const OpenMeeting::UserInfo &) {
			syncAccountBadge();
		});
	connect(&session, &OpenMeeting::SessionManager::loggedOut,
		this, [this] {
			if (_eventStream) _eventStream->stop();
			if (_incomingCallWindow) _incomingCallWindow->dismiss();
			if (_outgoingCallOverlay) _outgoingCallOverlay->dismiss();
			_activeMeetingCoordinator = nullptr;
			if (_presenceWatch) _presenceWatch->setLocalInMeeting(false);
			syncAccountBadge();
			syncJoinPresets();
			updateContactsNavBadge();
		});
	connect(&session, &OpenMeeting::SessionManager::preferencesChanged,
		this, [this](const OpenMeeting::MediaPreferences &) {
			syncJoinPresets();
		});
	connect(&session, &OpenMeeting::SessionManager::meetingSecurityPreferencesChanged,
		this, [this] {
			syncJoinPresets();
		});
	Theme::observeChanges(*this, [this] {
		refreshThemeVisuals();
	});
}

MeetingMainWindow::~MeetingMainWindow() {
	clearPendingMeetingEntry();
	MeetingRoomWindow::closeStandaloneNotices();
}

void MeetingMainWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	setupNativeWindow();
	syncHeaderDate();
	syncAccountBadge();
	syncJoinPresets();
}

void MeetingMainWindow::closeEvent(QCloseEvent *e) {
	clearPendingMeetingEntry();
	if (_eventStream) _eventStream->stop();
	if (_incomingCallWindow) _incomingCallWindow->dismiss();
	if (_outgoingCallOverlay) _outgoingCallOverlay->dismiss();
	closeMeetingWindows();
	MeetingRoomWindow::closeStandaloneNotices();
	hideLogConsole();
	Ui::RpWidget::closeEvent(e);
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
	// 1. 顶部一体化标题栏
	_headerBar = new QWidget(this);
	_headerBar->setObjectName(QStringLiteral("mainHeaderBar"));
	auto *headerLayout = new QHBoxLayout(_headerBar);
	headerLayout->setContentsMargins(18, 0, 8, 0);
	headerLayout->setSpacing(8);

	_brandBadge = new BrandBadgeWidget(_headerBar);
	headerLayout->addWidget(_brandBadge);

	auto *brandTitle = new QLabel(AppBranding::name(), _headerBar);
	brandTitle->setObjectName(QStringLiteral("mainBrandTitle"));
	brandTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
	headerLayout->addWidget(brandTitle, 0, Qt::AlignVCenter);

	auto *brandDivider = new QFrame(_headerBar);
	brandDivider->setObjectName(QStringLiteral("mainHeaderDivider"));
	brandDivider->setFixedSize(1, 12);
	headerLayout->addWidget(brandDivider, 0, Qt::AlignVCenter);

	_dateBadge = new QLabel(_headerBar);
	_dateBadge->setObjectName(QStringLiteral("mainDateBadge"));
	_dateBadge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_dateBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
	headerLayout->addWidget(_dateBadge, 0, Qt::AlignVCenter);

	headerLayout->addStretch(1);

	_mainNavCapsule = new QFrame(_headerBar);
	_mainNavCapsule->setObjectName(QStringLiteral("mainNavCapsule"));
	_mainNavCapsule->setFixedHeight(30);
	auto *navLayout = new QHBoxLayout(_mainNavCapsule);
	navLayout->setContentsMargins(3, 3, 3, 3);
	navLayout->setSpacing(2);

	_navMeetingsButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Meetings"), _mainNavCapsule);
	_navMeetingsButton->setObjectName(QStringLiteral("mainNavMeetings"));
	_navMeetingsButton->setCheckable(true);
	_navMeetingsButton->setChecked(true);
	_navMeetingsButton->setCursor(Qt::PointingHandCursor);
	_navMeetingsButton->setFixedHeight(24);
	connect(_navMeetingsButton, &QPushButton::clicked, this, [this] {
		switchMainTab(false);
	});
	navLayout->addWidget(_navMeetingsButton);

	_navContactsButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Contacts"), _mainNavCapsule);
	_navContactsButton->setObjectName(QStringLiteral("mainNavContacts"));
	_navContactsButton->setCheckable(true);
	_navContactsButton->setCursor(Qt::PointingHandCursor);
	_navContactsButton->setFixedHeight(24);
	connect(_navContactsButton, &QPushButton::clicked, this, [this] {
		switchMainTab(true);
	});
	navLayout->addWidget(_navContactsButton);
	headerLayout->addWidget(_mainNavCapsule, 0, Qt::AlignVCenter);

	headerLayout->addStretch(1);

	_copilotButton = new QPushButton(QStringLiteral("Copilot"), _headerBar);
	_copilotButton->setObjectName(QStringLiteral("mainCopilot"));
	_copilotButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "AI Copilot"));
	_copilotButton->setToolTip(QCoreApplication::translate("MeetingUI", "AI Copilot"));
	_copilotButton->setCheckable(true);
	_copilotButton->setCursor(Qt::PointingHandCursor);
	_copilotButton->setFixedHeight(30);
	_copilotButton->setIconSize(QSize(14, 14));
	connect(_copilotButton, &QPushButton::clicked, this, [this] {
		toggleCopilotDrawer();
	});
	headerLayout->addWidget(_copilotButton);

	_settingsButton = new QPushButton(_headerBar);
	_settingsButton->setObjectName(QStringLiteral("mainSettings"));
	_settingsButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Settings"));
	_settingsButton->setToolTip(QCoreApplication::translate("MeetingUI", "Settings"));
	_settingsButton->setCursor(Qt::PointingHandCursor);
	_settingsButton->setFixedSize(30, 30);
	_settingsButton->setIconSize(QSize(16, 16));
	connect(_settingsButton, &QPushButton::clicked, this, [this] {
		openSettingsAtPage(static_cast<int>(SettingsDialog::Page::General), this);
	});
	headerLayout->addWidget(_settingsButton);

	_accountButton = new QPushButton(_headerBar);
	_accountButton->setObjectName(QStringLiteral("mainAccountMenu"));
	_accountButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Account menu"));
	_accountButton->setCursor(Qt::PointingHandCursor);
	_accountButton->setFixedHeight(30);
	_accountButton->setMaximumWidth(168);
	_accountButton->setIconSize(QSize(22, 22));
	connect(_accountButton, &QPushButton::clicked, this, [this] {
		showAccountMenu();
	});
	headerLayout->addWidget(_accountButton);

	auto *headerDivider = new QFrame(_headerBar);
	headerDivider->setObjectName(QStringLiteral("mainHeaderDivider"));
	headerDivider->setFixedSize(1, 16);
	headerLayout->addWidget(headerDivider);

	_windowControls = new WindowControlsWidget(_headerBar);
	headerLayout->addWidget(_windowControls);

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

	// 2. 居中工作台主列（指挥条 + 次动作行 + 宽幅日程卡 + 底部独立预设状态条）
	_centerColumn = new QWidget(this);
	_centerColumn->setObjectName(QStringLiteral("mainCenterColumn"));
	auto *centerLayout = new QVBoxLayout(_centerColumn);
	centerLayout->setContentsMargins(0, 0, 0, 0);
	centerLayout->setSpacing(12);

	// Layer 1: 主指挥条（发起即时会议 + 内联会议号直达）
	auto *commandRow = new QHBoxLayout();
	commandRow->setContentsMargins(0, 0, 0, 0);
	commandRow->setSpacing(10);

	_quickMeetingButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Start Instant Meeting"), _centerColumn);
	_quickMeetingButton->setObjectName(QStringLiteral("mainQuickMeeting"));
	_quickMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Start Meeting"));
	_quickMeetingButton->setCursor(Qt::PointingHandCursor);
	_quickMeetingButton->setFixedHeight(44);
	_quickMeetingButton->setIconSize(QSize(16, 16));
	connect(_quickMeetingButton, &QPushButton::clicked, this, [this] {
		onCardClicked(ActionCardType::QuickMeeting);
	});
	commandRow->addWidget(_quickMeetingButton);

	_joinInputBox = new QFrame(_centerColumn);
	_joinInputBox->setObjectName(QStringLiteral("mainJoinInputBox"));
	_joinInputBox->setFixedHeight(44);
	auto *joinBoxLayout = new QHBoxLayout(_joinInputBox);
	joinBoxLayout->setContentsMargins(14, 5, 6, 5);
	joinBoxLayout->setSpacing(8);

	_meetingIdInput = new QLineEdit(_joinInputBox);
	_meetingIdInput->setObjectName(QStringLiteral("mainMeetingIdInput"));
	_meetingIdInput->setPlaceholderText(
		QCoreApplication::translate("MeetingUI", "Enter 9-digit meeting ID to join, or click Join..."));
	_meetingIdInput->setClearButtonEnabled(true);
	_meetingIdInput->installEventFilter(this);
	connect(_meetingIdInput, &QLineEdit::returnPressed, this, [this] {
		triggerInlineJoin();
	});
	joinBoxLayout->addWidget(_meetingIdInput, 1);

	_joinMeetingButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Join"), _joinInputBox);
	_joinMeetingButton->setObjectName(QStringLiteral("mainJoinMeeting"));
	_joinMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Join Meeting"));
	_joinMeetingButton->setCursor(Qt::PointingHandCursor);
	_joinMeetingButton->setFixedHeight(32);
	connect(_joinMeetingButton, &QPushButton::clicked, this, [this] {
		triggerInlineJoin();
	});
	joinBoxLayout->addWidget(_joinMeetingButton);

	commandRow->addWidget(_joinInputBox, 1);
	centerLayout->addLayout(commandRow);

	// Layer 2: 次级动作行（预定会议 / 共享屏幕 + 全部会议）
	auto *secondaryRow = new QHBoxLayout();
	secondaryRow->setContentsMargins(0, 0, 0, 0);
	secondaryRow->setSpacing(8);

	_scheduleMeetingButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Schedule Meeting"), _centerColumn);
	_scheduleMeetingButton->setObjectName(QStringLiteral("mainScheduleMeeting"));
	_scheduleMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Schedule Meeting"));
	_scheduleMeetingButton->setCursor(Qt::PointingHandCursor);
	_scheduleMeetingButton->setFixedHeight(34);
	_scheduleMeetingButton->setIconSize(QSize(14, 14));
	connect(_scheduleMeetingButton, &QPushButton::clicked, this, [this] {
		onCardClicked(ActionCardType::ScheduleMeeting);
	});
	secondaryRow->addWidget(_scheduleMeetingButton);

	_shareScreenButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Share Screen"), _centerColumn);
	_shareScreenButton->setObjectName(QStringLiteral("mainShareScreen"));
	_shareScreenButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Share Screen"));
	_shareScreenButton->setCursor(Qt::PointingHandCursor);
	_shareScreenButton->setFixedHeight(34);
	_shareScreenButton->setIconSize(QSize(14, 14));
#if defined(Q_OS_LINUX)
	if (!livekit::DesktopCaptureSupported()) {
		const auto reason = QCoreApplication::translate("MeetingUI", "Screen sharing is not available on this platform yet.");
		_shareScreenButton->setToolTip(reason);
		_shareScreenButton->setAccessibleDescription(reason);
		_shareScreenButton->setEnabled(false);
	}
#endif
	connect(_shareScreenButton, &QPushButton::clicked, this, [this] {
		onCardClicked(ActionCardType::ShareScreen);
	});
	secondaryRow->addWidget(_shareScreenButton);

	secondaryRow->addStretch(1);

	_allMeetingsButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "All Meetings"), _centerColumn);
	_allMeetingsButton->setObjectName(QStringLiteral("mainAllMeetings"));
	_allMeetingsButton->setCursor(Qt::PointingHandCursor);
	_allMeetingsButton->setLayoutDirection(Qt::RightToLeft);
	_allMeetingsButton->setFixedHeight(32);
	_allMeetingsButton->setIconSize(QSize(13, 13));
	connect(_allMeetingsButton, &QPushButton::clicked, this, [this] {
		showMeetingListDialog();
	});
	secondaryRow->addWidget(_allMeetingsButton);

	centerLayout->addLayout(secondaryRow);

	// Layer 3: 宽幅日程卡片
	_scheduleWidget = new ScheduleWidget(_centerColumn);
	_scheduleWidget->setShowAllMeetingsAction(false);
	centerLayout->addWidget(_scheduleWidget, 1);

	// Layer 4: 底部独立入会预设状态条（宽屏单行 / 窄屏自动切换双行，避免文字被裁断）
	_presetStrip = new QFrame(_centerColumn);
	_presetStrip->setObjectName(QStringLiteral("mainPresetStrip"));
	_presetStrip->setFixedHeight(44);
	_presetOuterLayout = new QVBoxLayout(_presetStrip);
	_presetOuterLayout->setContentsMargins(14, 7, 10, 7);
	_presetOuterLayout->setSpacing(0);

	_presetTopRow = new QHBoxLayout();
	_presetTopRow->setContentsMargins(0, 0, 0, 0);
	_presetTopRow->setSpacing(8);
	_presetOuterLayout->addLayout(_presetTopRow);

	_presetBottomRow = new QHBoxLayout();
	_presetBottomRow->setContentsMargins(0, 0, 0, 0);
	_presetBottomRow->setSpacing(8);
	_presetOuterLayout->addLayout(_presetBottomRow);

	_presetLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Join Presets"), _presetStrip);
	_presetLabel->setObjectName(QStringLiteral("mainPresetLabel"));
	_presetLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_presetTopRow->addWidget(_presetLabel, 0, Qt::AlignVCenter);

	_presetMicButton = new QPushButton(_presetStrip);
	_presetMicButton->setObjectName(QStringLiteral("mainPresetMic"));
	_presetMicButton->setCheckable(true);
	_presetMicButton->setCursor(Qt::PointingHandCursor);
	_presetMicButton->setFixedHeight(28);
	_presetMicButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_presetMicButton->setIconSize(QSize(13, 13));
	connect(_presetMicButton, &QPushButton::clicked, this, [this] {
		auto &session = OpenMeeting::SessionManager::instance();
		session.setEnableMicrophone(!session.mediaPreferences().enableMicrophone);
	});
	_presetTopRow->addWidget(_presetMicButton, 0, Qt::AlignVCenter);

	_presetCameraButton = new QPushButton(_presetStrip);
	_presetCameraButton->setObjectName(QStringLiteral("mainPresetCamera"));
	_presetCameraButton->setCheckable(true);
	_presetCameraButton->setCursor(Qt::PointingHandCursor);
	_presetCameraButton->setFixedHeight(28);
	_presetCameraButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_presetCameraButton->setIconSize(QSize(13, 13));
	connect(_presetCameraButton, &QPushButton::clicked, this, [this] {
		auto &session = OpenMeeting::SessionManager::instance();
		session.setEnableVideo(!session.mediaPreferences().enableVideo);
	});
	_presetTopRow->addWidget(_presetCameraButton, 0, Qt::AlignVCenter);

	_presetE2eeButton = new QPushButton(_presetStrip);
	_presetE2eeButton->setObjectName(QStringLiteral("mainPresetE2ee"));
	_presetE2eeButton->setCursor(Qt::PointingHandCursor);
	_presetE2eeButton->setFixedHeight(28);
	_presetE2eeButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_presetE2eeButton->setIconSize(QSize(13, 13));
	connect(_presetE2eeButton, &QPushButton::clicked, this, [this] {
		openSettingsAtPage(static_cast<int>(SettingsDialog::Page::Security), this);
	});
	_presetTopRow->addWidget(_presetE2eeButton, 0, Qt::AlignVCenter);

	_presetTopRow->addStretch(1);

	_moreSettingsButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "More Settings"), _presetStrip);
	_moreSettingsButton->setObjectName(QStringLiteral("mainMoreSettings"));
	_moreSettingsButton->setCursor(Qt::PointingHandCursor);
	_moreSettingsButton->setLayoutDirection(Qt::RightToLeft);
	_moreSettingsButton->setFixedHeight(28);
	_moreSettingsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	_moreSettingsButton->setIconSize(QSize(13, 13));
	connect(_moreSettingsButton, &QPushButton::clicked, this, [this] {
		openSettingsAtPage(static_cast<int>(SettingsDialog::Page::General), this);
	});
	_presetTopRow->addWidget(_moreSettingsButton, 0, Qt::AlignVCenter);

	centerLayout->addWidget(_presetStrip);

	// 2b. 通讯录工作区（与居中会议工作台互斥展示，兼容右侧 Copilot 抽屉）
	_contactsPage = new ContactsPageWidget(
		*_contactCatalog, *_presenceWatch, OpenMeeting::SessionManager::instance(), this);
	_contactsPage->hide();
	connect(_contactsPage, &ContactsPageWidget::voiceCallRequested, this, [this](const QString &userId) {
		if (_callInvite) {
			_callInvite->startDirectCall(userId, OpenMeeting::CallMediaType::Audio);
		}
	});
	connect(_contactsPage, &ContactsPageWidget::videoCallRequested, this, [this](const QString &userId) {
		if (_callInvite) {
			_callInvite->startDirectCall(userId, OpenMeeting::CallMediaType::Video);
		}
	});
	connect(_contactsPage, &ContactsPageWidget::scheduleMeetingWithContactRequested, this, [this](const QString &userId) {
		showBookingDialog({userId});
	});

	// 3. 右侧可选 AI Copilot 抽屉
	_copilotSidebar = new OpenMeeting::CopilotSidebarWidget(_agentCoordinator.get(), this);
	_copilotSidebar->setLeftEdgeShadowEnabled(true);
	_copilotSidebar->hide();
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::closeRequested, this, [this] {
		toggleCopilotDrawer(false);
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::expandToggleRequested, this, [this] {
		_copilotExpanded = !_copilotExpanded;
		if (_copilotSidebar) {
			_copilotSidebar->setExpanded(_copilotExpanded);
		}
		if (_copilotVisible) {
			QResizeEvent ev(size(), size());
			resizeEvent(&ev);
			update();
		}
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::openSettingsRequested, this, [this] {
		openCopilotSettings(this);
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::openSettingsPageRequested, this, [this](int pageIndex, int subTabIndex) {
		openSettingsAtPage(pageIndex, this, subTabIndex);
	});

	// 会议目录入口
	_scheduleWidget->allMeetingsClicked() | rpl::on_next([this] {
		showMeetingListDialog();
	}, lifetime());

	_scheduleWidget->addScheduleClicked() | rpl::on_next([this] {
		showBookingDialog();
	}, lifetime());

	_scheduleWidget->refreshClicked() | rpl::on_next([this] {
		if (!_meetingCatalog->upcomingState().refreshing) {
			_meetingCatalog->refreshUpcoming();
		}
	}, lifetime());

	_scheduleWidget->meetingClicked() | rpl::on_next([this](const QString &meetingId) {
		showMeetingDetail(meetingId);
	}, lifetime());

	connect(_meetingCatalog, &OpenMeeting::MeetingCatalogController::upcomingChanged,
		this, [this] { syncSchedule(); });

	syncHeaderDate();
	syncAccountBadge();
	syncJoinPresets();
	refreshThemeVisuals();
	Theme::setStyleVariant(*_headerBar, "main-home-window");
	Theme::setStyleVariant(*_centerColumn, "main-home-window");
	syncSchedule();
	updateContactsNavBadge();
	if (OpenMeeting::SessionManager::instance().isLoggedIn()) {
		_meetingCatalog->refreshUpcoming();
		startEventStreamForSession();
		if (_contactCatalog) _contactCatalog->refreshAll();
	}
}

bool MeetingMainWindow::eventFilter(QObject *watched, QEvent *event) {
	if (watched == _meetingIdInput && _joinInputBox) {
		if (event->type() == QEvent::FocusIn) {
			_joinInputBox->setProperty("focused", true);
			repolishWidget(_joinInputBox);
		} else if (event->type() == QEvent::FocusOut) {
			_joinInputBox->setProperty("focused", false);
			repolishWidget(_joinInputBox);
		}
	}
	return Ui::RpWidget::eventFilter(watched, event);
}

void MeetingMainWindow::syncHeaderDate() {
	if (!_dateBadge) return;
	const QDate today = QDate::currentDate();
	const QString monthDay = QCoreApplication::translate("MeetingUI", "%1/%2")
		.arg(today.month()).arg(today.day());
	const QString weekday = QLocale().dayName(today.dayOfWeek(), QLocale::ShortFormat);
	_dateBadge->setText(QStringLiteral("%1 · %2").arg(monthDay, weekday));
}

void MeetingMainWindow::syncAccountBadge() {
	if (!_accountButton) return;
	const auto &session = OpenMeeting::SessionManager::instance();
	const bool loggedIn = session.isLoggedIn();
	const QString displayName = loggedIn
		? (session.nickname().trimmed().isEmpty() ? session.userId() : session.nickname().trimmed())
		: QCoreApplication::translate("MeetingUI", "Not Signed In");
	const QFontMetrics fm(_accountButton->font());
	_accountButton->setText(fm.elidedText(displayName, Qt::ElideRight, 112));
	_accountButton->setToolTip(loggedIn
		? QCoreApplication::translate("MeetingUI", "Current User: %1 (%2)").arg(session.nickname(), session.userId())
		: QCoreApplication::translate("MeetingUI", "Not Signed In"));
	_accountButton->setIcon(buildAccountAvatarIcon(displayName, loggedIn, devicePixelRatioF()));
}

void MeetingMainWindow::syncJoinPresets() {
	const auto &session = OpenMeeting::SessionManager::instance();
	const auto &media = session.mediaPreferences();
	const auto &sec = session.meetingSecurityPreferences();
	const bool hasKey = session.hasMeetingEncryptionKey();

	if (_presetMicButton) {
		_presetMicButton->setChecked(media.enableMicrophone);
		_presetMicButton->setText(media.enableMicrophone
			? QCoreApplication::translate("MeetingUI", "Mic: On")
			: QCoreApplication::translate("MeetingUI", "Mic: Off"));
		_presetMicButton->setToolTip(QCoreApplication::translate("MeetingUI", "Click to toggle default microphone state on joining"));
		_presetMicButton->setIcon(Theme::icon(
			media.enableMicrophone ? Theme::Icon::Microphone : Theme::Icon::MicrophoneOff,
			media.enableMicrophone ? Theme::ColorRole::Accent : Theme::ColorRole::TextMuted));
	}

	if (_presetCameraButton) {
		_presetCameraButton->setChecked(media.enableVideo);
		_presetCameraButton->setText(media.enableVideo
			? QCoreApplication::translate("MeetingUI", "Camera: On")
			: QCoreApplication::translate("MeetingUI", "Camera: Off"));
		_presetCameraButton->setToolTip(QCoreApplication::translate("MeetingUI", "Click to toggle default camera state on joining"));
		_presetCameraButton->setIcon(Theme::icon(
			media.enableVideo ? Theme::Icon::Video : Theme::Icon::VideoOff,
			media.enableVideo ? Theme::ColorRole::Accent : Theme::ColorRole::TextMuted));
	}

	if (_presetE2eeButton) {
		const bool anyEnabled = sec.allMeetingsE2ee || sec.quickMeetingsE2ee ||
			sec.screenShareE2ee || sec.meetingDetailsE2ee;
		QString tone = QStringLiteral("neutral");
		Theme::ColorRole iconRole = Theme::ColorRole::TextSecondary;
		if (anyEnabled && !hasKey) {
			tone = QStringLiteral("warning");
			iconRole = Theme::ColorRole::Warning;
			_presetE2eeButton->setText(QCoreApplication::translate("MeetingUI", "Encryption: Key Needed"));
		} else if (sec.allMeetingsE2ee) {
			tone = QStringLiteral("active");
			iconRole = Theme::ColorRole::Accent;
			_presetE2eeButton->setText(QCoreApplication::translate("MeetingUI", "Encryption: On"));
		} else if (anyEnabled) {
			tone = QStringLiteral("active");
			iconRole = Theme::ColorRole::Accent;
			_presetE2eeButton->setText(QCoreApplication::translate("MeetingUI", "Encryption: Custom"));
		} else {
			_presetE2eeButton->setText(QCoreApplication::translate("MeetingUI", "Encryption: On Demand"));
		}
		_presetE2eeButton->setToolTip(QCoreApplication::translate("MeetingUI", "Open security and end-to-end encryption settings"));
		_presetE2eeButton->setProperty("presetTone", tone);
		_presetE2eeButton->setIcon(Theme::icon(Theme::Icon::Shield, iconRole));
		repolishWidget(_presetE2eeButton);
	}

	if (_centerColumn && _centerColumn->width() > 0) {
		updatePresetStripLayout(_centerColumn->width());
	}
}

void MeetingMainWindow::updatePresetStripLayout(int availableWidth) {
	if (!_presetStrip || !_presetOuterLayout || !_presetTopRow || !_presetBottomRow ||
		!_presetLabel || !_presetMicButton || !_presetCameraButton ||
		!_presetE2eeButton || !_moreSettingsButton) {
		return;
	}
	const int singleRowNeeded = 24
		+ _presetLabel->sizeHint().width()
		+ _presetMicButton->sizeHint().width()
		+ _presetCameraButton->sizeHint().width()
		+ _presetE2eeButton->sizeHint().width()
		+ _moreSettingsButton->sizeHint().width()
		+ 8 * 5
		+ 12;
	const bool wantTwoRow = availableWidth > 0 && availableWidth < singleRowNeeded;
	if (wantTwoRow == _presetCompactTwoRow) {
		return;
	}
	_presetCompactTwoRow = wantTwoRow;

	auto clearRow = [](QHBoxLayout *row) {
		while (QLayoutItem *item = row->takeAt(0)) {
			delete item;
		}
	};
	clearRow(_presetTopRow);
	clearRow(_presetBottomRow);

	if (_presetCompactTwoRow) {
		_presetOuterLayout->setContentsMargins(14, 8, 10, 10);
		_presetOuterLayout->setSpacing(6);
		_presetTopRow->addWidget(_presetLabel, 0, Qt::AlignVCenter);
		_presetTopRow->addStretch(1);
		_presetTopRow->addWidget(_moreSettingsButton, 0, Qt::AlignVCenter);

		_presetBottomRow->addWidget(_presetMicButton, 0, Qt::AlignVCenter);
		_presetBottomRow->addWidget(_presetCameraButton, 0, Qt::AlignVCenter);
		_presetBottomRow->addWidget(_presetE2eeButton, 0, Qt::AlignVCenter);
		_presetBottomRow->addStretch(1);
		_presetStrip->setFixedHeight(78);
	} else {
		_presetOuterLayout->setContentsMargins(14, 7, 10, 7);
		_presetOuterLayout->setSpacing(0);
		_presetTopRow->addWidget(_presetLabel, 0, Qt::AlignVCenter);
		_presetTopRow->addWidget(_presetMicButton, 0, Qt::AlignVCenter);
		_presetTopRow->addWidget(_presetCameraButton, 0, Qt::AlignVCenter);
		_presetTopRow->addWidget(_presetE2eeButton, 0, Qt::AlignVCenter);
		_presetTopRow->addStretch(1);
		_presetTopRow->addWidget(_moreSettingsButton, 0, Qt::AlignVCenter);
		_presetStrip->setFixedHeight(44);
	}
}

void MeetingMainWindow::refreshThemeVisuals() {
	if (_copilotButton) {
		_copilotButton->setChecked(_copilotVisible);
		_copilotButton->setIcon(Theme::icon(
			Theme::Icon::Sparkle,
			_copilotVisible ? Theme::ColorRole::OnAccent : Theme::ColorRole::Accent));
	}
	if (_settingsButton) {
		_settingsButton->setIcon(Theme::icon(Theme::Icon::Settings, Theme::ColorRole::TextSecondary));
	}
	if (_quickMeetingButton) {
		_quickMeetingButton->setIcon(Theme::icon(Theme::Icon::Video, Theme::ColorRole::OnAccent));
	}
	if (_scheduleMeetingButton) {
		_scheduleMeetingButton->setIcon(Theme::icon(Theme::Icon::Calendar, Theme::ColorRole::TextSecondary));
	}
	if (_shareScreenButton) {
		_shareScreenButton->setIcon(Theme::icon(Theme::Icon::ScreenShare, Theme::ColorRole::TextSecondary));
	}
	if (_allMeetingsButton) {
		_allMeetingsButton->setIcon(Theme::icon(Theme::Icon::ChevronRight, Theme::ColorRole::Accent));
	}
	if (_moreSettingsButton) {
		_moreSettingsButton->setIcon(Theme::icon(Theme::Icon::ChevronRight, Theme::ColorRole::TextSecondary));
	}
	if (_brandBadge) _brandBadge->update();
	if (_windowControls) _windowControls->update();
	if (_contactsPage) _contactsPage->refreshTheme();
	syncAccountBadge();
	syncJoinPresets();
	update();
}

void MeetingMainWindow::showAccountMenu() {
	auto &session = OpenMeeting::SessionManager::instance();
	auto *menu = new QMenu(this);
	menu->setAttribute(Qt::WA_DeleteOnClose);
	AppTheme::styleMenu(
		*menu,
		AppTheme::effectiveTheme() == AppTheme::Theme::Dark
			? AppTheme::Tone::Dark
			: AppTheme::Tone::Light);
	QString statusStr = session.isLoggedIn()
		? QCoreApplication::translate("MeetingUI", "Current User: %1 (%2)").arg(session.nickname(), session.userId())
		: QCoreApplication::translate("MeetingUI", "Not Signed In");
	menu->addAction(statusStr)->setEnabled(false);
	menu->addSeparator();

	if (session.isLoggedIn() && _contactCatalog) {
		auto *profileAction = menu->addAction(QCoreApplication::translate("MeetingUI", "Personal Profile..."));
		profileAction->setObjectName(QStringLiteral("mainPersonalProfile"));
		connect(profileAction, &QAction::triggered, this, [this] {
			if (!_contactCatalog) return;
			MeetingUI::UserProfileDialog dlg(*_contactCatalog, OpenMeeting::SessionManager::instance(), this);
			dlg.exec();
		}, Qt::QueuedConnection);
		menu->addSeparator();
	}

	auto *switchAction = menu->addAction(QCoreApplication::translate("MeetingUI", "Switch Account / Sign In"));
	auto *logoutAction = menu->addAction(QCoreApplication::translate("MeetingUI", "Sign Out"));
	const auto reportCount = livekit::telemetry::InstalledTelemetryHistoryStore()
		? livekit::telemetry::InstalledTelemetryHistoryStore()->Status()->reports.size()
		: 0;
	auto *telemetryAction = menu->addAction(
		AppTheme::icon(AppTheme::Icon::Details),
		QCoreApplication::translate("MeetingUI", "Telemetry reports on this device (%1)")
			.arg(reportCount));
	telemetryAction->setObjectName(QStringLiteral("mainPostMeetingTelemetry"));

	connect(telemetryAction, &QAction::triggered, this, [this] {
		OpenPostMeetingTelemetryDialog(this);
	}, Qt::QueuedConnection);
	connect(switchAction, &QAction::triggered, this,
		[this] { handleUserLogout(); }, Qt::QueuedConnection);
	connect(logoutAction, &QAction::triggered, this,
		[this] { handleUserLogout(); }, Qt::QueuedConnection);

	const QPoint anchor = _accountButton
		? _accountButton->mapToGlobal(QPoint(0, _accountButton->height() + 6))
		: mapToGlobal(QPoint(width() - 220, kHeaderHeight + 4));
	menu->popup(anchor);
}

void MeetingMainWindow::triggerInlineJoin() {
	const QString rawId = _meetingIdInput ? _meetingIdInput->text().trimmed() : QString();
	beginMeetingEntry(rawId);
}

bool MeetingMainWindow::isTitleBarDragPosition(const QPoint &pos) const {
	if (pos.y() < 0 || pos.y() >= kHeaderHeight) {
		return false;
	}
	QWidget *hit = childAt(pos);
	while (hit && hit != this) {
		if (hit == _windowControls ||
			qobject_cast<QAbstractButton *>(hit) ||
			qobject_cast<QLineEdit *>(hit)) {
			return false;
		}
		hit = hit->parentWidget();
	}
	return true;
}

void MeetingMainWindow::handleUserLogout() {
	if (_sessionInvalidationDialogActive) return;
	_sessionInvalidationDialogActive = true;
	const auto reset = qScopeGuard([this] { _sessionInvalidationDialogActive = false; });
	closeMeetingWindows();
	MeetingRoomWindow::closeStandaloneNotices();
	hideLogConsole();
	OpenMeeting::SessionManager::instance().logout();
	showLoginDialog();
}

void MeetingMainWindow::showLoginDialog() {
	// The main window is unavailable while no account is authenticated.  The
	// login dialog has no parent so the hidden main window is never exposed.
	hide();
	for (;;) {
		LoginDialog loginDlg;
		if (loginDlg.exec() != QDialog::Accepted) {
			close();
			return;
		}
		if (OpenMeeting::SessionManager::instance().isLoggedIn()) {
			syncAccountBadge();
			syncJoinPresets();
			if (_meetingCatalog) _meetingCatalog->refreshUpcoming();
			startEventStreamForSession();
			if (_contactCatalog) _contactCatalog->refreshAll();
			show();
			raise();
			activateWindow();
			return;
		}
		QMessageBox::warning(
			nullptr,
			QCoreApplication::translate("MeetingUI", "Sign-In Incomplete"),
			QCoreApplication::translate("MeetingUI", "Your sign-in session could not be established. Please sign in again."));
	}
}

void MeetingMainWindow::closeMeetingWindows() {
	clearPendingMeetingEntry();
	const auto topLevelWidgets = QApplication::topLevelWidgets();
	for (QWidget *widget : topLevelWidgets) {
		if (auto *roomWindow = qobject_cast<MeetingRoomWindow *>(widget)) {
			roomWindow->close();
		}
	}
}

void MeetingMainWindow::hideLogConsole() {
	const auto topLevelWidgets = QApplication::topLevelWidgets();
	for (QWidget *widget : topLevelWidgets) {
		if (auto *console = qobject_cast<MeetingLogConsoleWindow *>(widget)) {
			console->hide();
		}
	}
}

void MeetingMainWindow::onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason) {
	if (_sessionInvalidationDialogActive ||
		OpenMeeting::SessionManager::instance().isLoggedIn()) {
		return;
	}
	_sessionInvalidationDialogActive = true;
	const auto reset = qScopeGuard([this] { _sessionInvalidationDialogActive = false; });
	hide();
	hideLogConsole();

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
	MeetingRoomWindow::closeStandaloneNotices();

	QMessageBox::warning(nullptr,
	                     duplicatedLogin ? QCoreApplication::translate("MeetingUI", "Account Signed Out")
	                                     : QCoreApplication::translate("MeetingUI", "Session Expired"),
	                     duplicatedLogin
	                         ? QCoreApplication::translate("MeetingUI", "Your account signed in on another device. This client has been signed out.")
	                         : QCoreApplication::translate("MeetingUI", "Your session has expired. Please sign in again."));

	// SessionManager 在发射 sessionInvalidated 前已复用 logout(false) 清理 token
	// 和本地 user 设置；这里仅负责让用户回到可重新认证的界面。
	showLoginDialog();
}

void MeetingMainWindow::onCardClicked(ActionCardType type) {
#if defined(Q_OS_LINUX)
	// Reject the action before reserving entry or creating a meeting, including
	// invocations that do not originate from the disabled home-screen card.
	if (type == ActionCardType::ShareScreen && !livekit::DesktopCaptureSupported()) return;
#endif
	if (type == ActionCardType::JoinMeeting) {
		beginMeetingEntry();
		return;
	}

	if (type == ActionCardType::QuickMeeting || type == ActionCardType::ShareScreen) {
		// Reserve before modal dialogs: joining also sends HTTP from its dialog.
		auto meetingReservation = _meetingEntryGuard.tryAcquire();
		if (!meetingReservation) {
			QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Meeting Busy"),
				QCoreApplication::translate("MeetingUI", "A meeting is already open or a request is in progress. Finish or close it before trying again."));
			return;
		}
		openQuickMeeting(std::move(meetingReservation), type == ActionCardType::ShareScreen);
		return;
	}
	if (type == ActionCardType::ScheduleMeeting) {
		showBookingDialog();
	}
}

void MeetingMainWindow::openQuickMeeting(
		std::unique_ptr<QObject> reservation, bool startScreenShare) {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!session.isLoggedIn()) {
		LoginDialog loginDlg(this);
		if (loginDlg.exec() != QDialog::Accepted) return;
		if (!session.isLoggedIn()) {
			QMessageBox::warning(
				this,
				QCoreApplication::translate("MeetingUI", "Sign-In Incomplete"),
				QCoreApplication::translate("MeetingUI", "Your sign-in session could not be established. Please sign in again."));
			return;
		}
	}

	livekit::MeetingEncryptionRequest encryption;
	try {
		encryption = session.meetingEncryptionRequest(startScreenShare
			? OpenMeeting::MeetingEncryptionEntry::ScreenShare
			: OpenMeeting::MeetingEncryptionEntry::QuickMeeting);
	} catch (const livekit::EncryptionRequestException &) {
		QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Meeting Encryption"),
			QCoreApplication::translate("MeetingUI", "Set an encryption key in Settings > Security before joining."));
		return;
	}
	auto coordinator = OpenMeeting::MeetingCoordinator::create();
	bindMeetingCatalog(coordinator.get());
	const auto prefs = session.mediaPreferences();

	MeetingRoomWindow::Config config;
	config.displayName = session.nickname();
	config.audioMuted = !prefs.enableMicrophone;
	config.videoEnabled = prefs.enableVideo;
	config.invitationMode = InvitationMode::BusinessMeetingId;
	populateCopilotConfig(config, this, _agentCoordinator, _contactCatalog, _presenceWatch, _callInvite);

	if (!coordinator->pageTiming()) coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
		livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
	auto *roomWindow = new MeetingRoomWindow(config, coordinator);
	reservation->setParent(roomWindow);
	reservation.release();
	roomWindow->setAttribute(Qt::WA_DeleteOnClose);
	if (startScreenShare) {
        ArmAutomaticScreenShare(coordinator.get(), roomWindow, [roomWindow] {
            roomWindow->requestDefaultScreenShare();
        });
	}
	const auto title = QCoreApplication::translate("MeetingUI", "%1's Instant Meeting").arg(session.nickname());
	roomWindow->show();
	roomWindow->prepareMediaAndJoin([coordinator, title, prefs, encryption = std::move(encryption)]() mutable {
		coordinator->createAndJoinQuickMeetingAsync(title, 3600, prefs, std::move(encryption));
	});
}

void MeetingMainWindow::beginMeetingEntry(
		const QString &meetingId,
		std::optional<OpenMeeting::MeetingSettings> meetingSettings,
		bool requireFreshDetail,
		bool shareScreenAfterJoin) {
	auto reservation = _meetingEntryGuard.tryAcquire();
	if (!reservation) {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Meeting Busy"),
			QCoreApplication::translate("MeetingUI", "A meeting is already open or a request is in progress. Finish or close it before trying again."));
		return;
	}

	if (!requireFreshDetail) {
		openJoinMeetingDialog(
			std::move(reservation), meetingId, std::move(meetingSettings), shareScreenAfterJoin);
		return;
	}

	_pendingMeetingReservation = std::move(reservation);
	_pendingMeetingId = meetingId;
	_pendingShareScreen = shareScreenAfterJoin;
	const auto generation = ++_pendingMeetingEntryGeneration;
	auto *progress = new QProgressDialog(
		QCoreApplication::translate("MeetingUI", "Loading the latest meeting details..."),
		QCoreApplication::translate("MeetingUI", "Cancel"), 0, 0, this);
	progress->setWindowTitle(QCoreApplication::translate("MeetingUI", "Preparing to Join"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setMinimumDuration(0);
	progress->setAttribute(Qt::WA_DeleteOnClose);
	_pendingMeetingProgress = progress;
	connect(progress, &QProgressDialog::canceled, this, [this, generation] {
		if (generation == _pendingMeetingEntryGeneration) clearPendingMeetingEntry();
	});
	progress->show();
	_meetingCatalog->loadMeetingDetail(meetingId);
}

void MeetingMainWindow::openJoinMeetingDialog(
		std::unique_ptr<QObject> reservation,
		const QString &meetingId,
		std::optional<OpenMeeting::MeetingSettings> meetingSettings,
		bool shareScreenAfterJoin,
		const QString &initialPassword) {
	JoinMeetingDialog dialog(this, meetingId, meetingSettings);
	dialog.setMeetingPassword(initialPassword);
	if (dialog.exec() != QDialog::Accepted) return;
	if (dialog.serverUrl().isEmpty() || dialog.token().isEmpty()) {
		QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Unable to Join Meeting"),
			QCoreApplication::translate("MeetingUI", "Meeting credentials are incomplete. Please try again."));
		return;
	}

	auto encryption = dialog.takeEncryptionRequest();
	if (!encryption) {
		QMessageBox::warning(
			this,
			QCoreApplication::translate("MeetingUI", "Unable to Join Meeting"),
			QCoreApplication::translate("MeetingUI", "Meeting encryption settings could not be prepared. Please try again."));
		return;
	}
	auto coordinator = OpenMeeting::MeetingCoordinator::create();
	coordinator->observeJoinTiming(dialog.takeJoinTiming());
	coordinator->observePageTiming(dialog.takePageTiming());
	auto preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
	preferences.enableMicrophone = !dialog.isAudioMuted();
	preferences.enableVideo = !dialog.isVideoMuted();

	MeetingRoomWindow::Config config;
	config.serverUrl = dialog.serverUrl();
	config.token = dialog.token();
	config.meetingId = dialog.meetingId();
	config.displayName = dialog.displayName();
	config.audioMuted = dialog.isAudioMuted() ||
		(meetingSettings && meetingSettings->disableMicrophoneOnJoin);
	config.videoEnabled = !dialog.isVideoMuted() &&
		(!meetingSettings || !meetingSettings->disableCameraOnJoin);
	preferences.enableMicrophone = !config.audioMuted;
	preferences.enableVideo = config.videoEnabled;
	if (auto sample = coordinator->joinTiming())
		sample->Media(!config.audioMuted, config.videoEnabled, false, false);
	if (!dialog.isManualConnection()) bindMeetingCatalog(coordinator.get());
	config.invitationMode = dialog.isManualConnection()
		? InvitationMode::Disabled
		: InvitationMode::BusinessMeetingId;
	populateCopilotConfig(config, this, _agentCoordinator, _contactCatalog, _presenceWatch, _callInvite);

	if (!coordinator->pageTiming()) coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
		livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
	auto *roomWindow = new MeetingRoomWindow(config, coordinator);
	reservation->setParent(roomWindow);
	reservation.release();
	roomWindow->setAttribute(Qt::WA_DeleteOnClose);
	if (shareScreenAfterJoin) {
        ArmAutomaticScreenShare(coordinator.get(), roomWindow, [roomWindow] {
            roomWindow->requestDefaultScreenShare();
        });
	}
	roomWindow->show();
	roomWindow->prepareMediaAndJoin([coordinator, config, preferences, encryption = std::move(*encryption)]() mutable {
		coordinator->connectDirectlyAsync(
			config.serverUrl, config.token, config.meetingId, config.displayName, preferences, std::move(encryption));
	});
}

void MeetingMainWindow::handlePendingMeetingEntryDetail() {
	if (!_pendingMeetingReservation || _pendingMeetingId.isEmpty()) return;
	const auto &state = _meetingCatalog->detailState();
	if (state.meetingId != _pendingMeetingId || state.refreshing) return;

	if (state.state == OpenMeeting::MeetingCatalogLoadState::Ready &&
		state.error.code == 0 && state.detail &&
		state.detail->record.meetingId == _pendingMeetingId) {
		const auto status = state.detail->record.status;
		if (status != OpenMeeting::MeetingStatus::Scheduled &&
			status != OpenMeeting::MeetingStatus::InProgress) {
			clearPendingMeetingEntry();
			QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Unable to Join Meeting"),
				QCoreApplication::translate("MeetingUI", "This meeting cannot be joined in its current state."));
			return;
		}

		auto reservation = std::move(_pendingMeetingReservation);
		const auto meetingId = _pendingMeetingId;
		const auto settings = state.detail->record.settings;
		const bool shareScreen = _pendingShareScreen;
		clearPendingMeetingEntry();
		openJoinMeetingDialog(
			std::move(reservation), meetingId, settings, shareScreen);
		return;
	}

	clearPendingMeetingEntry();
	QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Unable to Read Meeting"),
		QCoreApplication::translate("MeetingUI", "Unable to get the latest meeting details. Check your network and try again."));
}

void MeetingMainWindow::clearPendingMeetingEntry() {
	++_pendingMeetingEntryGeneration;
	_pendingMeetingReservation.reset();
	_pendingMeetingId.clear();
	_pendingShareScreen = false;
	if (_pendingMeetingProgress) {
		disconnect(_pendingMeetingProgress, nullptr, this, nullptr);
		_pendingMeetingProgress->close();
		_pendingMeetingProgress = nullptr;
	}
}

void MeetingMainWindow::showBookingDialog(const QStringList &preselectedInvitees) {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!session.isLoggedIn()) {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Sign-In Required"),
			QCoreApplication::translate("MeetingUI", "Sign in to schedule a meeting."));
		return;
	}
	MeetingBookingDialog dialog(*_meetingCatalog, session, this, _contactCatalog, _presenceWatch);
	if (!preselectedInvitees.isEmpty()) {
		dialog.setPreselectedInvitees(preselectedInvitees);
	}
	dialog.exec();
}

void MeetingMainWindow::showMeetingListDialog() {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!session.isLoggedIn()) {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Sign-In Required"),
			QCoreApplication::translate("MeetingUI", "Sign in to view the meeting list."));
		return;
	}
	MeetingListDialog dialog(*_meetingCatalog, session, this);
	QString requestedJoinMeetingId;
	connect(&dialog, &MeetingListDialog::meetingActivated,
		this, [this](const QString &meetingId) { showMeetingDetail(meetingId); });
	connect(&dialog, &MeetingListDialog::joinRequested,
		this, [&dialog, &requestedJoinMeetingId](const QString &meetingId) {
			requestedJoinMeetingId = meetingId;
			dialog.accept();
		});
	dialog.exec();
	if (!requestedJoinMeetingId.isEmpty()) {
		beginMeetingEntry(requestedJoinMeetingId, std::nullopt, true);
	}
}

void MeetingMainWindow::showMeetingDetail(const QString &meetingId) {
	if (meetingId.isEmpty()) return;
	auto &session = OpenMeeting::SessionManager::instance();
	const auto authGeneration = session.authGeneration();
	MeetingDetailDialog dialog(
		meetingId,
		*_meetingCatalog,
		session,
		this,
		_contactCatalog,
		_presenceWatch);
	if (dialog.exec() != QDialog::Accepted || !session.isLoggedIn() ||
		session.authGeneration() != authGeneration) return;
	if (const auto detail = dialog.detailForJoin()) {
		auto reservation = _meetingEntryGuard.tryAcquire();
		if (!reservation) {
			QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Meeting Busy"),
				QCoreApplication::translate("MeetingUI", "A meeting is already open or a request is in progress. Finish or close it before trying again."));
			return;
		}

		livekit::MeetingEncryptionRequest encryption;
		try {
			encryption = session.meetingEncryptionRequest(OpenMeeting::MeetingEncryptionEntry::MeetingDetails);
		} catch (const livekit::EncryptionRequestException &) {
			QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Meeting Encryption"),
				QCoreApplication::translate("MeetingUI", "Set an encryption key in Settings > Security before joining."));
			return;
		}
		auto preferences = session.mediaPreferences();
		preferences.enableMicrophone = preferences.enableMicrophone &&
			!detail->record.settings.disableMicrophoneOnJoin;
		preferences.enableVideo = preferences.enableVideo &&
			!detail->record.settings.disableCameraOnJoin;
		MeetingRoomWindow::Config config;
		config.meetingId = detail->record.meetingId;
		config.displayName = session.nickname();
		config.audioMuted = !preferences.enableMicrophone;
		config.videoEnabled = preferences.enableVideo;
		config.invitationMode = InvitationMode::BusinessMeetingId;
	populateCopilotConfig(config, this, _agentCoordinator, _contactCatalog, _presenceWatch, _callInvite);

		auto coordinator = OpenMeeting::MeetingCoordinator::create();
		bindMeetingCatalog(coordinator.get());
		if (!coordinator->pageTiming()) coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
			livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
		auto *roomWindow = new MeetingRoomWindow(config, coordinator);
		reservation->setParent(roomWindow);
		reservation.release();
		roomWindow->setAttribute(Qt::WA_DeleteOnClose);
		roomWindow->show();
		roomWindow->prepareMediaAndJoin([coordinator, config, preferences,
			password = detail->password, authGeneration, sessionManager = &session,
			window = QPointer<MeetingRoomWindow>(roomWindow), encryption = std::move(encryption)]() mutable {
			if (!sessionManager->isLoggedIn() || sessionManager->authGeneration() != authGeneration) {
				encryption.Revoke();
				if (window) window->close();
				if (!sessionManager->isSessionInvalidating()) {
					MeetingRoomWindow::showStandaloneNotice(
						QStringLiteral("meetingOperationError"),
						QMessageBox::Warning,
						QCoreApplication::translate("MeetingUI", "Sign-In Required"),
						QCoreApplication::translate("MeetingUI", "Your account session changed before joining the meeting. Please sign in and try again."));
				}
				return;
			}
			coordinator->joinMeetingAsync(config.meetingId, password, config.displayName,
				preferences, std::move(encryption));
		});
	}
}

void MeetingMainWindow::bindMeetingCatalog(OpenMeeting::MeetingCoordinator *coordinator) {
	using namespace OpenMeeting;
	_activeMeetingCoordinator = coordinator;
	auto &session = SessionManager::instance();
	const auto authGeneration = session.authGeneration();
	const auto userId = session.userId();
	const auto service = session.serverBaseUrl();
	const QPointer<MeetingCatalogController> catalog(_meetingCatalog);
	const QPointer<PresenceWatchController> presence(_presenceWatch);
	const QPointer<MeetingMainWindow> mainWin(this);
	const QPointer<MeetingCoordinator> owner(coordinator);
	const auto active = std::make_shared<std::optional<MeetingRecord>>();
	const auto currentAccount = [authGeneration, userId, service] {
		const auto &session = SessionManager::instance();
		return session.isLoggedIn() && session.authGeneration() == authGeneration &&
			session.userId() == userId && session.serverBaseUrl() == service;
	};
	const auto project = [catalog, presence, owner, active, currentAccount] {
		if (!catalog || !owner || !currentAccount() ||
			(owner->state() != MeetingState::InMeeting &&
			 owner->state() != MeetingState::Reconnecting)) return false;
		const auto &detail = owner->meetingDetail();
		const auto meetingId = owner->currentMeetingId();
		if (meetingId.isEmpty()) return false;
		if (presence) {
			presence->setLocalInMeeting(true, meetingId);
		}
		if (*active && (*active)->meetingId == meetingId &&
			(*active)->title == detail.meetingName &&
			(*active)->creatorUserId == detail.creatorUserId &&
			(*active)->hostUserId == detail.effectiveHostUserId() &&
			(*active)->startTimeSeconds == detail.startTime) return false;
		MeetingRecord record;
		for (const auto &existing : catalog->upcomingState().meetings) {
			if (existing.meetingId == meetingId) { record = existing; break; }
		}
		record.meetingId = meetingId;
		record.title = detail.meetingName;
		record.creatorUserId = detail.creatorUserId;
		record.hostUserId = detail.effectiveHostUserId();
		record.status = MeetingStatus::InProgress;
		record.rawStatus = meetingStatusToWire(record.status);
		record.startTimeSeconds = detail.startTime;
		*active = record;
		catalog->setActiveMeeting(record);
		return true;
	};
	const auto refresh = [catalog, currentAccount] {
		if (catalog && currentAccount()) catalog->refreshUpcoming();
	};
	const auto update = [project, refresh] {
		if (project()) refresh();
	};
	connect(coordinator, &MeetingCoordinator::meetingJoinedSuccessfully, this,
		[update](const QString &) { update(); });
	connect(coordinator, &MeetingCoordinator::hostRoleChanged, this,
		[update](const QString &, const QString &) { update(); });
	// Metadata can arrive after the join signal (or be cleared on reconnect).
	connect(coordinator, &MeetingCoordinator::meetingDetailUpdated, this,
		[update](const MeetingDetail &) { update(); });
	const auto finish = [catalog, presence, mainWin, owner, active, currentAccount, refresh] {
		if (mainWin && mainWin->_activeMeetingCoordinator == owner) {
			mainWin->_activeMeetingCoordinator = nullptr;
		}
		if (presence && currentAccount()) {
			presence->setLocalInMeeting(false);
		}
		if (!std::exchange(*active, std::nullopt) || !catalog || !currentAccount()) return;
		catalog->clearActiveMeeting();
		refresh();
	};
	connect(coordinator, &MeetingCoordinator::meetingLeft, this, finish);
	connect(coordinator, &QObject::destroyed, this, finish);
}

void MeetingMainWindow::syncSchedule() {
	if (_scheduleWidget && _meetingCatalog) {
		_scheduleWidget->setCurrentUserId(OpenMeeting::SessionManager::instance().userId());
		_scheduleWidget->setMeetingState(_meetingCatalog->upcomingState());
	}
}

void MeetingMainWindow::setupContactsAndCallRuntime() {
	auto &session = OpenMeeting::SessionManager::instance();
	_eventStream = new OpenMeeting::UserEventStreamClient(session, this);
	_contactCatalog = new OpenMeeting::ContactCatalogController(session, this);
	_presenceWatch = new OpenMeeting::PresenceWatchController(session, *_contactCatalog, *_eventStream, this);
	_callInvite = new OpenMeeting::CallInviteController(session, *_eventStream, this);

	if (_agentCoordinator) {
		_agentCoordinator->attachContacts(_contactCatalog, _presenceWatch, _callInvite);
	}

	connect(_eventStream, &OpenMeeting::UserEventStreamClient::syncSeqAdvanced, this, [this](qint64 seq) {
		if (_contactCatalog) _contactCatalog->advanceSyncSeq(seq);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::contactRelationUpdated, this, [this](const QJsonObject &payload) {
		if (_contactCatalog) _contactCatalog->applyRelationEvent(payload);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::contactRequestReceived, this, [this](const QJsonObject &payload) {
		if (_contactCatalog) _contactCatalog->applyRequestReceivedEvent(payload);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::contactRequestHandled, this, [this](const QJsonObject &payload) {
		if (_contactCatalog) _contactCatalog->applyRequestHandledEvent(payload);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::orgTreeUpdated, this, [this](qint64 treeVersion) {
		if (_contactCatalog) _contactCatalog->applyOrgTreeUpdatedEvent(treeVersion);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::syncResetRequired, this, [this] {
		if (_contactCatalog) _contactCatalog->handleSyncResetRequired();
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::meetingInvited, this, [this](const QJsonObject &) {
		if (_meetingCatalog) _meetingCatalog->refreshUpcoming();
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::sessionKicked, this, [](const QString &reason) {
		const auto invalidation = (reason == QStringLiteral("logout"))
			? OpenMeeting::SessionInvalidationReason::TokenExpired
			: OpenMeeting::SessionInvalidationReason::DuplicatedLogin;
		OpenMeeting::SessionManager::instance().invalidateSession(invalidation);
	});
	connect(_eventStream, &OpenMeeting::UserEventStreamClient::sessionAuthRejected, this, [](const QString &) {
		OpenMeeting::SessionManager::instance().invalidateSession(
			OpenMeeting::SessionInvalidationReason::TokenExpired);
	});

	connect(_contactCatalog, &OpenMeeting::ContactCatalogController::pendingRequestCountChanged, this, [this](qint64) {
		updateContactsNavBadge();
	});

	_callInvite->setEntryArbitrationHooks(
		[this]() { return _meetingEntryGuard.tryAcquire(); },
		[this]() {
			if (!_activeMeetingCoordinator) return false;
			const auto st = _activeMeetingCoordinator->state();
			return st == OpenMeeting::MeetingState::InMeeting ||
				st == OpenMeeting::MeetingState::Reconnecting ||
				st == OpenMeeting::MeetingState::ConnectingRoom ||
				st == OpenMeeting::MeetingState::StartingLocalMedia;
		},
		[this]() {
			return _activeMeetingCoordinator ? _activeMeetingCoordinator->currentMeetingId() : QString();
		},
		[this]() {
			return _activeMeetingCoordinator ? _activeMeetingCoordinator->requiresEncryption() : false;
		},
		[this](OpenMeeting::CallJoinHandoff &&handoff) {
			joinCallMeeting(std::move(handoff));
		});

	connect(_callInvite, &OpenMeeting::CallInviteController::incomingCallRinging,
		this, [this](const OpenMeeting::CallSession &callSession) {
			if (!_incomingCallWindow) {
				_incomingCallWindow = new IncomingCallWindow();
				connect(_incomingCallWindow, &IncomingCallWindow::acceptClicked, this, [this] {
					if (_callInvite) _callInvite->acceptIncomingCall();
				});
				connect(_incomingCallWindow, &IncomingCallWindow::rejectClicked, this, [this] {
					if (_callInvite) _callInvite->rejectIncomingCall();
				});
			}
			_incomingCallWindow->presentCall(callSession, _callInvite->remainingSeconds());
		});

	connect(_callInvite, &OpenMeeting::CallInviteController::outgoingCallRinging,
		this, [this](const OpenMeeting::CallSession &callSession) {
			if (!_outgoingCallOverlay) {
				_outgoingCallOverlay = new OutgoingCallOverlay(this);
				connect(_outgoingCallOverlay, &OutgoingCallOverlay::cancelClicked, this, [this] {
					if (_callInvite) _callInvite->cancelOutgoingCall();
				});
			}
			const QString calleeName = _contactCatalog
				? _contactCatalog->displayNameForUser(callSession.calleeUserId)
				: callSession.calleeUserId;
			_outgoingCallOverlay->presentOutgoing(callSession, calleeName, _callInvite->remainingSeconds());
		});

	connect(_callInvite, &OpenMeeting::CallInviteController::countdownTick, this, [this](int remaining) {
		if (_incomingCallWindow && _incomingCallWindow->isVisible()) {
			_incomingCallWindow->updateCountdown(remaining);
		}
		if (_outgoingCallOverlay && _outgoingCallOverlay->isVisible()) {
			_outgoingCallOverlay->updateCountdown(remaining);
		}
	});

	connect(_callInvite, &OpenMeeting::CallInviteController::stateChanged,
		this, [this](OpenMeeting::CallControllerState state, const std::optional<OpenMeeting::CallSession> &) {
			if (state == OpenMeeting::CallControllerState::Idle ||
				state == OpenMeeting::CallControllerState::HandingOffToMeeting) {
				if (_incomingCallWindow) _incomingCallWindow->dismiss();
				if (_outgoingCallOverlay) _outgoingCallOverlay->dismiss();
			}
		});

	connect(_callInvite, &OpenMeeting::CallInviteController::callEnded,
		this, [this](const QString &, OpenMeeting::CallEndReason reason) {
			if (_incomingCallWindow) _incomingCallWindow->dismiss();
			if (_outgoingCallOverlay) _outgoingCallOverlay->dismiss();
			if (reason == OpenMeeting::CallEndReason::Busy) {
				QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Call Busy"),
					QCoreApplication::translate("MeetingUI", "The callee is currently in another meeting or call."));
			} else if (reason == OpenMeeting::CallEndReason::Rejected) {
				QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Call Declined"),
					QCoreApplication::translate("MeetingUI", "The call was declined."));
			} else if (reason == OpenMeeting::CallEndReason::Timeout) {
				QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "No Answer"),
					QCoreApplication::translate("MeetingUI", "The call was not answered in time."));
			}
		});

	connect(_callInvite, &OpenMeeting::CallInviteController::errorMessage,
		this, [this](const QString &msg) {
			if (!msg.isEmpty()) {
				QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Call"), msg);
			}
		});
}

void MeetingMainWindow::startEventStreamForSession() {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!_eventStream || !session.isLoggedIn()) return;
	if (_contactCatalog) {
		_eventStream->setInitialSinceSeq(_contactCatalog->syncSeq());
	}
	_eventStream->start();
}

void MeetingMainWindow::switchMainTab(bool showContacts) {
	if (_showingContactsTab == showContacts) return;
	_showingContactsTab = showContacts;
	if (_navMeetingsButton) _navMeetingsButton->setChecked(!showContacts);
	if (_navContactsButton) _navContactsButton->setChecked(showContacts);
	if (_centerColumn) _centerColumn->setVisible(!showContacts);
	if (_contactsPage) _contactsPage->setVisible(showContacts);
	if (showContacts && _contactCatalog && OpenMeeting::SessionManager::instance().isLoggedIn()) {
		if (_contactCatalog->contacts().empty() && _contactCatalog->departments().empty()) {
			_contactCatalog->refreshAll();
		}
	}
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
}

void MeetingMainWindow::updateContactsNavBadge() {
	if (!_navContactsButton) return;
	const qint64 count = _contactCatalog ? _contactCatalog->pendingRequestCount() : 0;
	if (count > 0) {
		_navContactsButton->setText(QCoreApplication::translate("MeetingUI", "Contacts (%1)").arg(count));
	} else {
		_navContactsButton->setText(QCoreApplication::translate("MeetingUI", "Contacts"));
	}
}

void MeetingMainWindow::joinCallMeeting(OpenMeeting::CallJoinHandoff &&handoff) {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!session.isLoggedIn() || handoff.liveKit.url.isEmpty() || handoff.liveKit.token.isEmpty()) {
		return;
	}
	if (!handoff.entryReservation) {
		handoff.entryReservation = _meetingEntryGuard.tryAcquire();
		if (!handoff.entryReservation) {
			QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Meeting Busy"),
				QCoreApplication::translate("MeetingUI", "A meeting is already open or a request is in progress."));
			return;
		}
	}

	livekit::MeetingEncryptionRequest encryption;
	try {
		encryption = session.meetingEncryptionRequest(OpenMeeting::MeetingEncryptionEntry::JoinMeeting);
	} catch (const livekit::EncryptionRequestException &) {
		QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Meeting Encryption"),
			QCoreApplication::translate("MeetingUI", "Set an encryption key in Settings > Security before joining."));
		return;
	}

	if (handoff.session.e2eeRequired && encryption.mode != livekit::MeetingEncryptionMode::Required) {
		bool ok = false;
		QString key = QInputDialog::getText(
			this,
			QCoreApplication::translate("MeetingUI", "End-to-End Encryption Key"),
			QCoreApplication::translate("MeetingUI", "This meeting requires end-to-end encryption. Enter the shared ASCII encryption key:"),
			QLineEdit::Password,
			QString(),
			&ok);
		if (!ok || key.isEmpty()) {
			return;
		}
		auto bytes = key.toUtf8();
		key.fill(QChar(0));
		const auto cleanse = qScopeGuard([&] {
			if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
		});
		try {
			encryption.mode = livekit::MeetingEncryptionMode::Required;
			encryption.secret = livekit::MeetingSecretHandle::Create(
				std::vector<uint8_t>(bytes.begin(), bytes.end()));
			encryption.Validate();
		} catch (const livekit::EncryptionRequestException &) {
			QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Invalid Encryption Key"),
				QCoreApplication::translate("MeetingUI", "Enter 1 to 4096 printable ASCII characters."));
			return;
		}
	}

	auto preferences = session.mediaPreferences();
	preferences.enableMicrophone = true;
	preferences.enableVideo = !handoff.audioOnly;

	MeetingRoomWindow::Config config;
	config.serverUrl = handoff.liveKit.url;
	config.token = handoff.liveKit.token;
	config.meetingId = handoff.session.meetingId;
	config.displayName = session.nickname();
	config.audioMuted = !preferences.enableMicrophone;
	config.videoEnabled = preferences.enableVideo;
	config.invitationMode = InvitationMode::BusinessMeetingId;
	populateCopilotConfig(config, this, _agentCoordinator, _contactCatalog, _presenceWatch, _callInvite);

	auto coordinator = OpenMeeting::MeetingCoordinator::create();
	bindMeetingCatalog(coordinator.get());
	if (!coordinator->pageTiming()) {
		coordinator->observePageTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
			livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Meeting));
	}
	auto *roomWindow = new MeetingRoomWindow(config, coordinator);
	handoff.entryReservation->setParent(roomWindow);
	handoff.entryReservation.release();
	roomWindow->setAttribute(Qt::WA_DeleteOnClose);
	roomWindow->show();

	const auto authGeneration = session.authGeneration();
	roomWindow->prepareMediaAndJoin([coordinator, config, preferences, authGeneration,
		sessionManager = &session, window = QPointer<MeetingRoomWindow>(roomWindow),
		encryption = std::move(encryption)]() mutable {
		if (!sessionManager->isLoggedIn() || sessionManager->authGeneration() != authGeneration) {
			encryption.Revoke();
			if (window) window->close();
			return;
		}
		coordinator->connectDirectlyAsync(
			config.serverUrl, config.token, config.meetingId, config.displayName, preferences, std::move(encryption));
	});
}

int MeetingMainWindow::copilotDrawerWidth(int totalWidth) const {
	const int maxAllowed = std::max(280, totalWidth - 480);
	if (_copilotExpanded) {
		return std::min(maxAllowed, std::min(560, std::max(380, totalWidth / 2)));
	}
	return std::min(maxAllowed, std::min(360, std::max(300, totalWidth / 3)));
}

void MeetingMainWindow::toggleCopilotDrawer(std::optional<bool> visible) {
	const bool nextVisible = visible.value_or(!_copilotVisible);
	if (_copilotVisible == nextVisible) {
		return;
	}
	_copilotVisible = nextVisible;
	if (_copilotButton) {
		_copilotButton->setChecked(_copilotVisible);
		_copilotButton->setIcon(Theme::icon(
			Theme::Icon::Sparkle,
			_copilotVisible ? Theme::ColorRole::OnAccent : Theme::ColorRole::Accent));
	}
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
	if (_copilotVisible && _meetingCatalog && OpenMeeting::SessionManager::instance().isLoggedIn() &&
		!_meetingCatalog->upcomingState().hasSnapshot && !_meetingCatalog->upcomingState().refreshing) {
		_meetingCatalog->refreshUpcoming();
	}
	if (_copilotVisible && _copilotSidebar) {
		_copilotSidebar->focusInput();
	}
}

void MeetingMainWindow::openSettingsAtPage(int pageIndex, QWidget *dialogParent, int copilotSubTab) {
	auto currentPage = static_cast<SettingsDialog::Page>(pageIndex);
	int currentSubTab = copilotSubTab;
	QPoint savedPos;
	bool hasSavedPos = false;
	for (;;) {
		const auto clickedAt = CameraDeviceDiscovery::Clock::now();
		QWidget *host = dialogParent ? dialogParent : this;
		SettingsDialog dialog(OpenMeeting::SessionManager::instance(), host, clickedAt);
		dialog.setAgentCoordinator(_agentCoordinator.get());
		dialog.showPage(currentPage);
		if (currentPage == SettingsDialog::Page::Copilot && currentSubTab >= 0) {
			dialog.setCopilotSubTab(currentSubTab);
		}
		if (hasSavedPos) {
			dialog.move(savedPos);
		}
		bool reopenForLanguage = false;
		connect(&dialog, &SettingsDialog::languageChanged, &dialog, [&] {
			currentPage = dialog.currentPage();
			currentSubTab = dialog.currentCopilotSubTab();
			savedPos = dialog.pos();
			hasSavedPos = true;
			reopenForLanguage = true;
			dialog.accept();
		});
		dialog.exec();
		if (!reopenForLanguage) {
			break;
		}
	}
	syncJoinPresets();
	syncAccountBadge();
}

void MeetingMainWindow::openCopilotSettings(QWidget *dialogParent) {
	openSettingsAtPage(static_cast<int>(SettingsDialog::Page::Copilot), dialogParent);
}

void MeetingMainWindow::retranslateUi() {
	if (_copilotButton) {
		_copilotButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "AI Copilot"));
		_copilotButton->setToolTip(QCoreApplication::translate("MeetingUI", "AI Copilot"));
	}
	if (_settingsButton) {
		_settingsButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Settings"));
		_settingsButton->setToolTip(QCoreApplication::translate("MeetingUI", "Settings"));
	}
	if (_accountButton) {
		_accountButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Account menu"));
	}
	if (_quickMeetingButton) {
		_quickMeetingButton->setText(QCoreApplication::translate("MeetingUI", "Start Instant Meeting"));
		_quickMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Start Meeting"));
	}
	if (_meetingIdInput) {
		_meetingIdInput->setPlaceholderText(
			QCoreApplication::translate("MeetingUI", "Enter 9-digit meeting ID to join, or click Join..."));
	}
	if (_joinMeetingButton) {
		_joinMeetingButton->setText(QCoreApplication::translate("MeetingUI", "Join"));
		_joinMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Join Meeting"));
	}
	if (_scheduleMeetingButton) {
		_scheduleMeetingButton->setText(QCoreApplication::translate("MeetingUI", "Schedule Meeting"));
		_scheduleMeetingButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Schedule Meeting"));
	}
	if (_shareScreenButton) {
		_shareScreenButton->setText(QCoreApplication::translate("MeetingUI", "Share Screen"));
		_shareScreenButton->setAccessibleName(QCoreApplication::translate("MeetingUI", "Share Screen"));
	}
	if (_allMeetingsButton) {
		_allMeetingsButton->setText(QCoreApplication::translate("MeetingUI", "All Meetings"));
	}
	if (_presetLabel) {
		_presetLabel->setText(QCoreApplication::translate("MeetingUI", "Join Presets"));
	}
	if (_moreSettingsButton) {
		_moreSettingsButton->setText(QCoreApplication::translate("MeetingUI", "More Settings"));
	}
	if (_navMeetingsButton) {
		_navMeetingsButton->setText(QCoreApplication::translate("MeetingUI", "Meetings"));
	}
	updateContactsNavBadge();
	if (_contactsPage) {
		_contactsPage->retranslateUi();
	}
	syncHeaderDate();
	syncAccountBadge();
	syncJoinPresets();
	syncSchedule();
	update();
}

void MeetingMainWindow::changeEvent(QEvent *e) {
	Ui::RpWidget::changeEvent(e);
	if (e->type() == QEvent::WindowStateChange) {
		if (_windowControls) {
			_windowControls->setMaximized(isMaximized() || isFullScreen());
		}
		update();
	} else if (e->type() == QEvent::LanguageChange) {
		retranslateUi();
	}
}

void MeetingMainWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	// 顶部一体化标题栏
	if (_headerBar) {
		_headerBar->setGeometry(0, 0, w, kHeaderHeight);
	}

	// 右侧可选 Copilot 抽屉 + 左侧居中主工作台
	const int bodyTop = kHeaderHeight;
	const int bodyH = std::max(0, h - bodyTop);
	const int copilotW = (_copilotVisible && _copilotSidebar) ? copilotDrawerWidth(w) : 0;
	const int mainAreaW = std::max(0, w - copilotW);

	if (_centerColumn) {
		const int sidePad = mainAreaW >= 620 ? 32 : 16;
		const int topPad = 24;
		const int bottomPad = 24;
		const int colW = std::min(kMaxColumnWidth, std::max(0, mainAreaW - sidePad * 2));
		const int colX = (mainAreaW - colW) / 2;
		const int colH = std::max(0, bodyH - topPad - bottomPad);
		_centerColumn->setGeometry(colX, bodyTop + topPad, colW, colH);
		_centerColumn->setVisible(!_showingContactsTab);
		updatePresetStripLayout(colW);
	}

	if (_contactsPage) {
		_contactsPage->setGeometry(0, bodyTop, mainAreaW, bodyH);
		_contactsPage->setVisible(_showingContactsTab);
	}

	if (_copilotSidebar) {
		if (_copilotVisible) {
			_copilotSidebar->setGeometry(w - copilotW, bodyTop, copilotW, bodyH);
			_copilotSidebar->show();
			_copilotSidebar->raise();
		} else {
			_copilotSidebar->hide();
		}
	}
}

void MeetingMainWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const int w = width();
	const int h = height();
	const int radius = isMaximized() || isFullScreen() ? 0 : kWindowCornerRadius;

	// 1. 主体圆角窗口底色
	QPainterPath windowPath;
	if (radius > 0) {
		windowPath.addRoundedRect(QRectF(0, 0, w, h), radius, radius);
		p.fillPath(windowPath, Theme::color(Theme::ColorRole::Window));
	} else {
		p.fillRect(rect(), Theme::color(Theme::ColorRole::Window));
	}

	// 2. 顶部一体化标题栏 Surface 顶盖与底部细分割线
	p.save();
	if (radius > 0) {
		p.setClipPath(windowPath);
	}
	p.fillRect(QRect(0, 0, w, kHeaderHeight), Theme::color(Theme::ColorRole::Surface));
	p.setPen(Theme::color(Theme::ColorRole::Border));
	p.drawLine(0, kHeaderHeight - 1, w, kHeaderHeight - 1);
	p.restore();

	// 3. 非最大化时的柔和 1px 窗口轮廓描边
	if (radius > 0) {
		p.setPen(QPen(Theme::color(Theme::ColorRole::Border), 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), radius, radius);
	}
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

		if (isTitleBarDragPosition(QPoint(x, y))) {
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
