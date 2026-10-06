#include "src/ui/meeting_auto_share.h"
#include <QtCore/QCoreApplication>
#include "src/ui/app_branding.h"
#include "src/ui/meeting_main_window.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_icons.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_booking_dialog.h"
#include "src/ui/meeting_detail_dialog.h"
#include "src/ui/meeting_list_dialog.h"
#include "src/ui/login_dialog.h"
#include "src/ui/settings_dialog.h"
#include "src/ui/shadow_helper.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/core/meeting_coordinator.h"
#include "src/net/session_manager.h"
#include "src/telemetry/telemetry_report.h"
#include "styles/style_widgets.h"
#include <QtCore/QPointer>
#include <QtCore/qscopeguard.h>
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
		layout()->activate();
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
		return false;
	}
	revokeEncryptionRequest();
	_encryptionRequest = std::move(request);
	if (!_globalEncryption) {
		_encryptionError->clear();
		_encryptionError->hide();
	}
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
	layout()->activate();
}

void JoinMeetingDialog::reject() {
	_isCancelled = true;
	_isLoading = false;
	clearEncryptionEditor();
	revokeEncryptionRequest();
	QDialog::reject();
}

void JoinMeetingDialog::closeEvent(QCloseEvent *e) {
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
}

void JoinMeetingDialog::showError(const QString &msg) {
	if (!_statusLabel) return;
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-main-window-statuslabel-3");
	_statusLabel->setText(msg);
	_statusLabel->setVisible(!msg.isEmpty());
}

void JoinMeetingDialog::onJoinClicked() {
	if (_isLoading) return;

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
	initLayout();
	connect(&session,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingMainWindow::onSessionInvalidated,
	        Qt::QueuedConnection);
	connect(_meetingCatalog, &OpenMeeting::MeetingCatalogController::detailChanged,
		this, [this] { handlePendingMeetingEntryDetail(); });
	connect(&session, &OpenMeeting::SessionManager::authenticationReset,
		this, [this](quint64) { clearPendingMeetingEntry(); });
}

void MeetingMainWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	setupNativeWindow();
}

void MeetingMainWindow::closeEvent(QCloseEvent *e) {
	clearPendingMeetingEntry();
	closeMeetingWindows();
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

	_sidebar->navChanged() | rpl::on_next([this](NavItemType type) {
		if (type != NavItemType::Contacts) return;
		_sidebar->setActiveNav(NavItemType::Meeting);
		QMessageBox::information(
			this,
			QCoreApplication::translate("MeetingUI", "Notice"),
			QCoreApplication::translate("MeetingUI", "This feature is not yet available"));
	}, lifetime());

	_sidebar->bottomItemClicked() | rpl::on_next([this](BottomItemType type) {
		if (type == BottomItemType::Settings) {
			const auto clickedAt = CameraDeviceDiscovery::Clock::now();
			SettingsDialog dialog(OpenMeeting::SessionManager::instance(), this, clickedAt);
			dialog.exec();
			return;
		}
		QMessageBox::information(
			this,
			QCoreApplication::translate("MeetingUI", "Notice"),
			QCoreApplication::translate("MeetingUI", "This feature is not yet available"));
	}, lifetime());

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
	syncSchedule();
	if (OpenMeeting::SessionManager::instance().isLoggedIn()) {
		_meetingCatalog->refreshUpcoming();
	}

	// 侧边栏用户头像点击菜单
	_sidebar->avatarClicked() | rpl::on_next([this] {
		auto &session = OpenMeeting::SessionManager::instance();
		auto *menu = new QMenu(this);
		menu->setAttribute(Qt::WA_DeleteOnClose);
		AppTheme::styleMenu(*menu, AppTheme::Tone::Light);
		QString statusStr = session.isLoggedIn()
			? QCoreApplication::translate("MeetingUI", "Current User: %1 (%2)").arg(session.nickname(), session.userId())
			: QCoreApplication::translate("MeetingUI", "Not Signed In");
		menu->addAction(statusStr)->setEnabled(false);
		menu->addSeparator();

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
		menu->popup(_sidebar->mapToGlobal(QPoint(_sidebar->width(), 24)));
	}, lifetime());
}

void MeetingMainWindow::handleUserLogout() {
	closeMeetingWindows();
	hideLogConsole();
	OpenMeeting::SessionManager::instance().logout();

	// The main window is unavailable while no account is authenticated.  The
	// login dialog has no parent so the hidden main window is never exposed.
	hide();
	LoginDialog loginDlg;
	if (loginDlg.exec() == QDialog::Accepted) {
		if (_sidebar) _sidebar->update();
		if (_meetingCatalog) _meetingCatalog->refreshUpcoming();
		show();
		raise();
		activateWindow();
		return;
	}

	close();
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
	                     duplicatedLogin ? QCoreApplication::translate("MeetingUI", "Account Signed Out")
	                                     : QCoreApplication::translate("MeetingUI", "Session Expired"),
	                     duplicatedLogin
	                         ? QCoreApplication::translate("MeetingUI", "Your account signed in on another device. This client has been signed out.")
	                         : QCoreApplication::translate("MeetingUI", "Your session has expired. Please sign in again."));

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
	const auto prefs = session.mediaPreferences();

	MeetingRoomWindow::Config config;
	config.displayName = session.nickname();
	config.audioMuted = !prefs.enableMicrophone;
	config.videoEnabled = prefs.enableVideo;
	config.invitationMode = InvitationMode::BusinessMeetingId;

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
	if (!encryption) return;
	auto coordinator = OpenMeeting::MeetingCoordinator::create();
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
	config.invitationMode = dialog.isManualConnection()
		? InvitationMode::Disabled
		: InvitationMode::BusinessMeetingId;

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

void MeetingMainWindow::showBookingDialog() {
	auto &session = OpenMeeting::SessionManager::instance();
	if (!session.isLoggedIn()) {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Sign-In Required"),
			QCoreApplication::translate("MeetingUI", "Sign in to schedule a meeting."));
		return;
	}
	MeetingBookingDialog dialog(*_meetingCatalog, session, this);
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
		this);
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

		auto coordinator = OpenMeeting::MeetingCoordinator::create();
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
				return;
			}
			coordinator->joinMeetingAsync(config.meetingId, password, config.displayName,
				preferences, std::move(encryption));
		});
	}
}

void MeetingMainWindow::syncSchedule() {
	if (_scheduleWidget && _meetingCatalog) {
		_scheduleWidget->setMeetingState(_meetingCatalog->upcomingState());
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
