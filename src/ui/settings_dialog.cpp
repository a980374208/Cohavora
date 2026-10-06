#include "src/ui/whiteboard/accessible_combo_box.h"
#include <QtCore/QCoreApplication>
#include "src/ui/app_branding.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_icons.h"
#include "src/ui/settings_dialog.h"

#include "src/core/session_shutdown_service.h"
#include "src/e2ee/meeting_encryption.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/ui/audio_device_test_controller.h"
#include "src/ui/camera_preview_widget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSignalBlocker>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtCore/qscopeguard.h>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QRegion>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedLayout>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QStandardItemModel>
#include "src/rtc/webrtc_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace MeetingUI {
namespace {

constexpr auto kDeviceIdRole = Qt::UserRole;
constexpr auto kDevicePendingRole = Qt::UserRole + 1;
constexpr auto kWidthRole = Qt::UserRole;
constexpr auto kHeightRole = Qt::UserRole + 1;
constexpr auto kFpsRole = Qt::UserRole + 2;
constexpr auto kSurfaceRadius = 12;
constexpr auto kShadowMargin = 18;
constexpr auto kTitleBarHeight = 49;

class RoundedSurface final : public QFrame {
public:
	explicit RoundedSurface(QWidget *parent)
		: QFrame(parent) {
	}

protected:
	void resizeEvent(QResizeEvent *event) override {
		QFrame::resizeEvent(event);
		QPainterPath path;
		path.addRoundedRect(QRectF(rect()), kSurfaceRadius, kSurfaceRadius);
		setMask(QRegion(path.toFillPolygon().toPolygon()));
	}
};

QLabel *makePageTitle(const QString &text, QWidget *parent) {
	auto *label = new QLabel(text, parent);
	auto font = label->font();
	font.setPixelSize(18);
	font.setBold(true);
	label->setFont(font);
	label->setObjectName(QStringLiteral("pageTitle"));
	return label;
}

QLabel *makeSectionTitle(const QString &text, QWidget *parent) {
	auto *label = new QLabel(text, parent);
	auto font = label->font();
	font.setPixelSize(14);
	font.setBold(true);
	label->setFont(font);
	label->setObjectName(QStringLiteral("sectionTitle"));
	return label;
}

QScrollArea *makeScrollablePage(QWidget *parent) {
	auto *scroll = new QScrollArea(parent);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	auto *content = new QWidget(scroll->viewport());
	new QVBoxLayout(content);
	scroll->setWidget(content);
	return scroll;
}

QString displayDeviceName(const std::string &name, bool isDefault) {
	auto result = QString::fromStdString(name);
	if (result.trimmed().isEmpty()) {
		result = QCoreApplication::translate("MeetingUI", "Unnamed Device");
	}
	if (isDefault) {
		result += QCoreApplication::translate("MeetingUI", "(System Default)");
	}
	return result;
}

int findData(const QComboBox *combo, const QString &value) {
	if (!combo || value.isEmpty()) {
		return -1;
	}
	for (auto i = 0; i != combo->count(); ++i) {
		if (combo->itemData(i, kDeviceIdRole).toString() == value) {
			return i;
		}
	}
	return -1;
}

void populateVideoCodecChoices(QComboBox *combo) {
	const auto add = [combo](const QString &label, const QString &value,
			bool available, const QString &reason = {}) {
		combo->addItem(label, value);
		const auto index = combo->count() - 1;
		if (!available) {
			combo->setItemData(index, reason, Qt::ToolTipRole);
			if (auto *model = qobject_cast<QStandardItemModel*>(combo->model())) {
				if (auto *item = model->item(index)) item->setEnabled(false);
			}
		}
	};
	add(QCoreApplication::translate("MeetingUI", "Auto (VP8 preferred)"),
		QStringLiteral("auto"), true);
	for (const auto &entry : std::array<std::pair<const char*, const char*>, 4>{
			std::pair{"VP8", "vp8"}, std::pair{"H.264", "h264"},
			std::pair{"VP9", "vp9"}, std::pair{"AV1", "av1"}}) {
		const auto available = livekit::IsVideoEncoderFormatSupported(entry.second);
		add(QString::fromLatin1(entry.first), QString::fromLatin1(entry.second), available,
			QCoreApplication::translate(
				"MeetingUI", "This codec is unavailable in the current WebRTC package."));
	}
	if (livekit::IsVideoEncoderFormatSupported("h265")) {
		add(QStringLiteral("H.265"), QStringLiteral("h265"), true);
	}
}

} // namespace

SettingsDialog::SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent,
		CameraDeviceDiscovery::Clock::time_point clickedAt)
	: QDialog(parent)
	, _session(session)
	, _clickedAt(clickedAt) {
	setObjectName(QStringLiteral("settingsDialog"));
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Settings"));
	setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
	setAttribute(Qt::WA_TranslucentBackground, true);
	setModal(true);
	resize(760 + kShadowMargin * 2, 640 + kShadowMargin * 2);
	setMinimumSize(680 + kShadowMargin * 2, 520 + kShadowMargin * 2);
	buildUi();
	setPreferences(_session.mediaPreferences());
	syncSecurityControls();
	connectPreferenceControls();
	connectDeviceControllers();

	connect(
		&_session,
		&OpenMeeting::SessionManager::preferencesChanged,
		this,
		[this](const OpenMeeting::MediaPreferences &value) {
			if (!_updatingUi) {
				setPreferences(value);
			}
		});
	connect(this, &QDialog::finished, this, [this] {
		clearSecurityEditor();
		_audioTestController->stopAll();
		setMicrophoneTestActive(false);
		_cameraPreview->stopPreview();
		emit cameraPreviewStopped();
	});
	connect(
		&_session,
		&OpenMeeting::SessionManager::meetingSecurityPreferencesChanged,
		this,
		&SettingsDialog::syncSecurityControls);
	connect(
		&_session,
		&OpenMeeting::SessionManager::authenticationReset,
		this,
		[this] {
			// A draft key belongs to the same authentication generation as the
			// in-memory saved key, even when no key has been saved yet.
			clearSecurityEditor();
			updateSecurityKeyControls();
		});

	prepareDevicePlaceholders();
}

SettingsDialog::~SettingsDialog() {
	clearSecurityEditor();
	cancelDeviceDiscovery();
	if (_audioTestController) {
		_audioTestController->stopAll();
	}
	if (_cameraPreview) {
		_cameraPreview->stopPreview();
	}
}

void SettingsDialog::done(int result) {
	clearSecurityEditor();
	cancelDeviceDiscovery();
	QDialog::done(result);
}

void SettingsDialog::cancelDeviceDiscovery() {
	_closed = true;
	if (_cameraCallbacks) _cameraCallbacks->Revoke();
	_cameraRequest.reset();
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event) {
	if (watched == _e2eeKey && event->type() == QEvent::KeyPress) {
		const auto *keyEvent = static_cast<QKeyEvent *>(event);
		if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
			saveSecurityKey();
			return true;
		}
	}
	if (event->type() != QEvent::Paint || _firstPaintScheduled || _closed || !isVisible()) {
		return QDialog::eventFilter(watched, event);
	}
	_firstPaintScheduled = true;
	// The translucent top-level itself can be completely covered and never
	// receive Paint. Observe the real navigation viewport instead, then return
	// to the event loop after this paint pass. This is not compositor presentation.
	QTimer::singleShot(0, this, [this] {
		if (_closed) return;
		_firstPaintReported = true;
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			CameraDeviceDiscovery::Clock::now() - _clickedAt).count();
		livekit::diagnostic::EmitBusinessEvent(
			livekit::diagnostic::Event::SettingsPaint(elapsed));
		startDeviceDiscovery(_forceDeviceRefresh);
	});
	return QDialog::eventFilter(watched, event);
}

void SettingsDialog::buildUi() {
	MeetingUI::AppTheme::setStyleVariant(*this, "settings-dialog-this");

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(
		kShadowMargin,
		kShadowMargin,
		kShadowMargin,
		kShadowMargin);
	root->setSpacing(0);
	// Establish the final scroll/stylesheet ancestry before adding controls.
	// Moving populated pages through temporary parents repeatedly resolves
	// inherited QSS fonts and palettes for every descendant.
	AppTheme::makeDialogAdaptive(*this, QSize(796, 676));
	auto *content = root->parentWidget();

	auto *layers = new QStackedLayout();
	layers->setContentsMargins(0, 0, 0, 0);
	layers->setStackingMode(QStackedLayout::StackAll);
	root->addLayout(layers);

	auto *shadowSurface = new QFrame(content);
	shadowSurface->setObjectName(QStringLiteral("settingsShadowSurface"));
	shadowSurface->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	auto *shadow = new QGraphicsDropShadowEffect(shadowSurface);
	shadow->setBlurRadius(30.0);
	shadow->setOffset(0.0, 4.0);
	shadow->setColor(QColor(31, 35, 41, 80));
	shadowSurface->setGraphicsEffect(shadow);
	layers->addWidget(shadowSurface);

	auto *surface = new RoundedSurface(content);
	surface->setObjectName(QStringLiteral("settingsSurface"));
	auto *surfaceLayout = new QVBoxLayout(surface);
	surfaceLayout->setContentsMargins(0, 0, 0, 0);
	surfaceLayout->setSpacing(0);
	layers->addWidget(surface);
	layers->setCurrentWidget(surface);

	auto *titleBar = new QWidget(surface);
	titleBar->setObjectName(QStringLiteral("titleBar"));
	titleBar->setMinimumHeight(kTitleBarHeight);
	auto *titleLayout = new QHBoxLayout(titleBar);
	titleLayout->setContentsMargins(18, 0, 12, 0);
	titleLayout->addStretch();
	auto *title = new QLabel(QCoreApplication::translate("MeetingUI", "Settings"), titleBar);
	title->setObjectName(QStringLiteral("dialogTitle"));
	titleLayout->addWidget(title);
	titleLayout->addStretch();
	auto *closeButton = new QPushButton(titleBar);
	closeButton->setObjectName(QStringLiteral("closeButton"));
	closeButton->setIcon(AppTheme::icon(AppTheme::Icon::Close));
	closeButton->setToolTip(QCoreApplication::translate("MeetingUI", "Close"));
	closeButton->setAccessibleName(closeButton->toolTip());
	closeButton->setFixedSize(28, 28);
	titleLayout->addWidget(closeButton);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	surfaceLayout->addWidget(titleBar);

	auto *body = new QHBoxLayout();
	body->setContentsMargins(0, 0, 0, 0);
	body->setSpacing(0);

	_navigation = new QListWidget(surface);
	_navigation->viewport()->installEventFilter(this);
	_navigation->setObjectName(QStringLiteral("settingsNavigation"));
	_navigation->setAccessibleName(QCoreApplication::translate("MeetingUI", "Settings sections"));
	_navigation->setMinimumWidth(150);
	_navigation->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
	_navigation->setWordWrap(true);
	_navigation->setTextElideMode(Qt::ElideNone);
	_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	_navigation->setSelectionMode(QAbstractItemView::SingleSelection);
	_navigation->addItem(new QListWidgetItem(
		AppTheme::icon(AppTheme::Icon::Details),
		QCoreApplication::translate("MeetingUI", "General")));
	_navigation->addItem(new QListWidgetItem(
		AppTheme::icon(AppTheme::Icon::Video),
		QCoreApplication::translate("MeetingUI", "Video")));
	_navigation->addItem(new QListWidgetItem(
		AppTheme::icon(AppTheme::Icon::Audio),
		QCoreApplication::translate("MeetingUI", "Audio")));
	_navigation->addItem(new QListWidgetItem(
		AppTheme::icon(AppTheme::Icon::Details),
		QCoreApplication::translate("MeetingUI", "Security")));
	_navigation->addItem(new QListWidgetItem(
		AppTheme::icon(AppTheme::Icon::Information),
		QCoreApplication::translate("MeetingUI", "About")));
	body->addWidget(_navigation);

	_pages = new QStackedWidget(surface);
	_pages->addWidget(buildGeneralPage());
	_pages->addWidget(buildVideoPage());
	_pages->addWidget(buildAudioPage());
	_pages->addWidget(buildSecurityPage());
	_pages->addWidget(buildAboutPage());
	body->addWidget(_pages, 1);
	surfaceLayout->addLayout(body, 1);

	connect(_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row < 0 || row >= _pages->count()) {
			return;
		}
		const auto wasVideo = _pages->currentIndex() == static_cast<int>(Page::Video);
		_pages->setCurrentIndex(row);
		const auto isVideo = row == static_cast<int>(Page::Video);
		if (isVideo) {
			requestPreviewIfVisible();
		} else if (wasVideo) {
			_cameraPreview->stopPreview();
			emit cameraPreviewStopped();
		}
		if (row != static_cast<int>(Page::Audio) && _microphoneTestActive) {
			setMicrophoneTestActive(false);
			emit microphoneTestStopped();
		}
	});
	connect(_navigation, &QListWidget::itemSelectionChanged, this, [this] {
		const auto selected = _navigation->selectedItems();
		if (!selected.isEmpty() && _navigation->currentItem() != selected.front()) {
			_navigation->setCurrentItem(selected.front());
		}
	});
	_navigation->setCurrentRow(static_cast<int>(Page::General));
	for (auto *label : findChildren<QLabel *>()) {
		if (label->maximumHeight() == QWIDGETSIZE_MAX) label->setWordWrap(true);
	}
}

QWidget *SettingsDialog::buildGeneralPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QCoreApplication::translate("MeetingUI", "General"), content));
	layout->addSpacing(6);

	_generalCamera = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), content);
	_generalCamera->setObjectName(QStringLiteral("settingsJoinCamera"));
	_generalMicrophone = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), content);
	_generalMicrophone->setObjectName(QStringLiteral("settingsJoinMicrophone"));
	_generalSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Use computer audio when joining"), content);
	_quitOnClose = new QCheckBox(QCoreApplication::translate("MeetingUI", "Quit when the main window is closed"), content);
	_quitOnClose->setObjectName(QStringLiteral("settingsQuitOnClose"));
	_showActiveSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Show the active speaker"), content);
	_showActiveSpeaker->setObjectName(QStringLiteral("settingsShowActiveSpeaker"));
	_stayWhenLocked = new QCheckBox(QCoreApplication::translate("MeetingUI", "Stay in the meeting when the screen is locked"), content);
	_stayWhenLocked->setObjectName(QStringLiteral("settingsStayWhenLocked"));

	layout->addWidget(_generalCamera);
	layout->addWidget(_generalMicrophone);
	layout->addWidget(_generalSpeaker);
	layout->addSpacing(5);
	layout->addWidget(_quitOnClose);
	layout->addWidget(_showActiveSpeaker);
	layout->addWidget(_stayWhenLocked);
	layout->addStretch();
	return scroll;
}

QWidget *SettingsDialog::buildVideoPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QCoreApplication::translate("MeetingUI", "Video"), content));
	layout->addSpacing(4);

	_previewHost = new QFrame(content);
	_previewHost->setObjectName(QStringLiteral("previewHost"));
	_previewHost->setMinimumHeight(230);
	_previewHost->setMaximumHeight(290);
	_previewHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	auto *previewLayout = new QVBoxLayout(_previewHost);
	previewLayout->setContentsMargins(0, 0, 0, 0);
	_cameraPreview = new CameraPreviewWidget(_previewHost);
	previewLayout->addWidget(_cameraPreview, 1);
	_previewMessage = new QLabel(QCoreApplication::translate("MeetingUI", "Camera Preview"), _previewHost);
	_previewMessage->setObjectName(QStringLiteral("previewMessage"));
	_previewMessage->setAlignment(Qt::AlignCenter);
	_previewMessage->setWordWrap(true);
	_previewMessage->hide();
	previewLayout->addWidget(_previewMessage, 1);
	layout->addWidget(_previewHost);

	layout->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Camera"), content));
	_cameraCombo = new QComboBox(content);
	_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_cameraCombo->setMinimumContentsLength(28);
	layout->addWidget(_cameraCombo);

	_videoCamera = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), content);
	layout->addWidget(_videoCamera);

	_highDefinition = new QCheckBox(QCoreApplication::translate("MeetingUI", "HD Camera Quality"), content);
	layout->addWidget(_highDefinition);
	_resolutionCombo = new QComboBox(content);
	_resolutionCombo->setMinimumWidth(280);
	layout->addWidget(_resolutionCombo);
	auto *resolutionHint = new QLabel(
		QCoreApplication::translate("MeetingUI", "Selects the device mode closest to 1920 × 1080 by default. Devices below 1080p use their highest available resolution."),
		content);
	resolutionHint->setObjectName(QStringLiteral("hintLabel"));
	resolutionHint->setWordWrap(true);
	layout->addWidget(resolutionHint);

	layout->addWidget(makeSectionTitle(
		QCoreApplication::translate("MeetingUI", "Publishing codec"), content));
	layout->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "Camera"), content));
	_cameraCodecCombo = new QComboBox(content);
	populateVideoCodecChoices(_cameraCodecCombo);
	layout->addWidget(_cameraCodecCombo);
	layout->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "Screen share"), content));
	_screenShareCodecCombo = new QComboBox(content);
	populateVideoCodecChoices(_screenShareCodecCombo);
	layout->addWidget(_screenShareCodecCombo);
    layout->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Screen share resolution"), content));
    _screenShareResolutionCombo = new QComboBox(content);
    configureAccessibleComboBox(_screenShareResolutionCombo);
    _screenShareResolutionCombo->setObjectName(QStringLiteral("screenShareResolution"));
    _screenShareResolutionCombo->addItems({QCoreApplication::translate("MeetingUI", "Auto (up to 2K)"), QCoreApplication::translate("MeetingUI", "720p"), QCoreApplication::translate("MeetingUI", "1080p"), QCoreApplication::translate("MeetingUI", "1440p (2K)"), QCoreApplication::translate("MeetingUI", "Native (up to 4K)")});
    layout->addWidget(_screenShareResolutionCombo);
    layout->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Screen share frame rate"), content));
    _screenShareFpsCombo = new QComboBox(content);
    configureAccessibleComboBox(_screenShareFpsCombo);
    _screenShareFpsCombo->setObjectName(QStringLiteral("screenShareFps"));
    for (int fps : {15, 20, 30}) _screenShareFpsCombo->addItem(QString::number(fps) + " FPS", fps);
    layout->addWidget(_screenShareFpsCombo);

	_mirrorEnabled = new QCheckBox(QCoreApplication::translate("MeetingUI", "Mirror Video"), content);
	layout->addWidget(_mirrorEnabled);
	_localMirror = new QRadioButton(QCoreApplication::translate("MeetingUI", "Mirror only my preview"), content);
	_remoteMirror = new QRadioButton(QCoreApplication::translate("MeetingUI", "Mirror for all participants"), content);
	auto *mirrorGroup = new QButtonGroup(content);
	mirrorGroup->setExclusive(true);
	mirrorGroup->addButton(_localMirror);
	mirrorGroup->addButton(_remoteMirror);
	auto *localRow = new QHBoxLayout();
	localRow->setContentsMargins(24, 0, 0, 0);
	localRow->addWidget(_localMirror);
	localRow->addStretch();
	layout->addLayout(localRow);
	auto *remoteRow = new QHBoxLayout();
	remoteRow->setContentsMargins(24, 0, 0, 0);
	remoteRow->addWidget(_remoteMirror);
	remoteRow->addStretch();
	layout->addLayout(remoteRow);
	layout->addStretch();
	return scroll;
}

QWidget *SettingsDialog::buildAudioPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QCoreApplication::translate("MeetingUI", "Audio"), content));
	layout->addSpacing(4);

	layout->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Speaker"), content));
	auto *speakerRow = new QHBoxLayout();
	_speakerCombo = new QComboBox(content);
	_speakerCombo->setObjectName(QStringLiteral("speakerDeviceCombo"));
	_speakerCombo->setMinimumContentsLength(24);
	_speakerTestButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Speaker"), content);
	_speakerTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_speakerTestButton->setMinimumWidth(108);
	speakerRow->addWidget(_speakerCombo, 1);
	speakerRow->addWidget(_speakerTestButton);
	layout->addLayout(speakerRow);
	layout->addSpacing(12);

	layout->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Microphone"), content));
	auto *microphoneRow = new QHBoxLayout();
	_microphoneCombo = new QComboBox(content);
	_microphoneCombo->setObjectName(QStringLiteral("microphoneDeviceCombo"));
	_microphoneCombo->setMinimumContentsLength(24);
	_microphoneTestButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Microphone"), content);
	_microphoneTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_microphoneTestButton->setCheckable(true);
	_microphoneTestButton->setMinimumWidth(108);
	microphoneRow->addWidget(_microphoneCombo, 1);
	microphoneRow->addWidget(_microphoneTestButton);
	layout->addLayout(microphoneRow);
	_microphoneLevel = new QProgressBar(content);
	_microphoneLevel->setRange(0, 100);
	_microphoneLevel->setValue(0);
	_microphoneLevel->setTextVisible(false);
	_microphoneLevel->setAccessibleName(QCoreApplication::translate("MeetingUI", "Microphone Input Level"));
	layout->addWidget(_microphoneLevel);
	_audioStatus = new QLabel(content);
	_audioStatus->setObjectName(QStringLiteral("hintLabel"));
	_audioStatus->setWordWrap(true);
	_audioStatus->hide();
	layout->addWidget(_audioStatus);
	layout->addSpacing(8);

	_audioMicrophone = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), content);
	_audioSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Use computer audio when joining"), content);
	_pushToTalk = new QCheckBox(QCoreApplication::translate("MeetingUI", "Hold Space to temporarily unmute"), content);
	_echoCancellation = new QCheckBox(QCoreApplication::translate("MeetingUI", "Echo cancellation (AEC)"), content);
	_echoCancellation->setObjectName(QStringLiteral("echoCancellationCheckBox"));
	_noiseSuppression = new QCheckBox(QCoreApplication::translate("MeetingUI", "Noise suppression (ANS)"), content);
	_noiseSuppression->setObjectName(QStringLiteral("noiseSuppressionCheckBox"));
	_autoGainControl = new QCheckBox(QCoreApplication::translate("MeetingUI", "Automatic gain control (AGC)"), content);
	_autoGainControl->setObjectName(QStringLiteral("autoGainControlCheckBox"));
	layout->addWidget(_audioMicrophone);
	layout->addWidget(_audioSpeaker);
	layout->addWidget(_pushToTalk);
	layout->addSpacing(8);
	layout->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Noise Reduction and Audio Enhancement"), content));
	layout->addWidget(_echoCancellation);
	layout->addWidget(_noiseSuppression);
	layout->addWidget(_autoGainControl);
	layout->addStretch();
	return scroll;
}

void SettingsDialog::connectDeviceControllers() {
	_audioTestController = new AudioDeviceTestController(this);
	connect(
		this,
		&SettingsDialog::microphoneTestStarted,
		_audioTestController,
		&AudioDeviceTestController::startMicrophoneTest);
	connect(
		this,
		&SettingsDialog::microphoneTestStopped,
		_audioTestController,
		&AudioDeviceTestController::stopMicrophoneTest);
	connect(
		this,
		&SettingsDialog::speakerTestRequested,
		_audioTestController,
		[this](const QString &deviceId) {
			_audioStatus->hide();
			_audioTestController->playSpeakerTestTone(deviceId);
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::microphoneLevelChanged,
		this,
		&SettingsDialog::setMicrophoneLevel);
	connect(
		_audioTestController,
		&AudioDeviceTestController::microphoneTestStateChanged,
		this,
		&SettingsDialog::setMicrophoneTestActive);
	connect(
		_audioTestController,
		&AudioDeviceTestController::speakerTestStateChanged,
		this,
		&SettingsDialog::setSpeakerTestActive);
	connect(
		_audioTestController,
		&AudioDeviceTestController::microphoneTestFailed,
		this,
		[this](const QString &message) {
			setMicrophoneTestActive(false);
			MeetingUI::AppTheme::setStyleVariant(*_audioStatus, "settings-dialog-audiostatus");
			_audioStatus->setText(message);
			_audioStatus->show();
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::speakerTestFinished,
		this,
		[this](bool success, const QString &message) {
			MeetingUI::AppTheme::setStyleVariant(*_audioStatus, success ? "settings-dialog-audiostatus-2-active" : "settings-dialog-audiostatus-2-normal");
			_audioStatus->setText(success
				? QCoreApplication::translate("MeetingUI", "Speaker test complete")
				: message);
			_audioStatus->setVisible(success || !message.isEmpty());
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::deviceEnumerationFailed,
		this,
		[this](const QString &message) {
			// Keep the pending IDs so editing other settings cannot reset a
			// saved endpoint after a transient enumeration failure.
			for (auto *combo : { _microphoneCombo, _speakerCombo }) {
				if (combo->count() > 0 && combo->itemData(0, kDevicePendingRole).toBool()) {
					combo->setItemText(0, QCoreApplication::translate("MeetingUI", "Failed to list audio devices"));
				}
			}
			MeetingUI::AppTheme::setStyleVariant(*_audioStatus, "settings-dialog-audiostatus-3");
			_audioStatus->setText(message);
			_audioStatus->show();
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::deviceEnumerationStateChanged,
		this,
		[this](bool active) {
			if (active) {
				_microphoneTestButton->setEnabled(false);
				_speakerTestButton->setEnabled(false);
			}
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::devicesChanged,
		this,
		[this](
				const QVector<AudioDeviceDescriptor> &microphones,
				const QVector<AudioDeviceDescriptor> &speakers) {
			const auto saved = _session.mediaPreferences();
			const auto oldUpdating = _updatingUi;
			_updatingUi = true;
			_microphoneCombo->clear();
			_speakerCombo->clear();
			// An empty ID represents the system default, not a pinned endpoint.
			_microphoneCombo->addItem(QCoreApplication::translate("MeetingUI", "System Default"), QString());
			_speakerCombo->addItem(QCoreApplication::translate("MeetingUI", "System Default"), QString());
			for (const auto &device : microphones) {
				auto name = device.name.trimmed().isEmpty()
					? QCoreApplication::translate("MeetingUI", "Unnamed Device")
					: device.name;
				if (device.isDefault) name += QCoreApplication::translate("MeetingUI", "(System Default)");
				_microphoneCombo->addItem(name, device.id);
			}
			for (const auto &device : speakers) {
				auto name = device.name.trimmed().isEmpty()
					? QCoreApplication::translate("MeetingUI", "Unnamed Device")
					: device.name;
				if (device.isDefault) name += QCoreApplication::translate("MeetingUI", "(System Default)");
				_speakerCombo->addItem(name, device.id);
			}

			if (microphones.isEmpty()) {
				_microphoneCombo->setItemText(0, QCoreApplication::translate("MeetingUI", "No microphone detected"));
				_microphoneCombo->setEnabled(false);
				_microphoneTestButton->setEnabled(false);
			} else {
				_microphoneCombo->setEnabled(true);
				_microphoneTestButton->setEnabled(true);
				const auto selected = findData(_microphoneCombo, saved.microphoneDeviceId);
				_microphoneCombo->setCurrentIndex(
					selected >= 0 ? selected : 0);
			}
			if (speakers.isEmpty()) {
				_speakerCombo->setItemText(0, QCoreApplication::translate("MeetingUI", "No speaker detected"));
				_speakerCombo->setEnabled(false);
				_speakerTestButton->setEnabled(false);
			} else {
				_speakerCombo->setEnabled(true);
				_speakerTestButton->setEnabled(true);
				const auto selected = findData(_speakerCombo, saved.speakerDeviceId);
				_speakerCombo->setCurrentIndex(
					selected >= 0 ? selected : 0);
			}
			_updatingUi = oldUpdating;
			_audioStatus->hide();

			const auto resolved = preferences();
			if (resolved.microphoneDeviceId != saved.microphoneDeviceId
				|| resolved.speakerDeviceId != saved.speakerDeviceId) {
				commitPreferences();
			}
		});
}

QWidget *SettingsDialog::buildSecurityPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(12);
	layout->addWidget(makePageTitle(
		QCoreApplication::translate("MeetingUI", "Security"), content));
	layout->addWidget(makeSectionTitle(
		QCoreApplication::translate("MeetingUI", "End-to-end encryption (E2EE)"), content));

	_quickE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt quick meetings"), content);
	_quickE2ee->setObjectName(QStringLiteral("settingsQuickE2ee"));
	layout->addWidget(_quickE2ee);
	_screenShareE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt screen sharing"), content);
	_screenShareE2ee->setObjectName(QStringLiteral("settingsScreenShareE2ee"));
	layout->addWidget(_screenShareE2ee);
	_meetingDetailsE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt meetings joined from meeting details"), content);
	_meetingDetailsE2ee->setObjectName(QStringLiteral("settingsMeetingDetailsE2ee"));
	layout->addWidget(_meetingDetailsE2ee);
	_allE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt all meetings"), content);
	_allE2ee->setObjectName(QStringLiteral("settingsAllE2ee"));
	layout->addWidget(_allE2ee);
	auto *scopeHint = new QLabel(QCoreApplication::translate(
		"MeetingUI", "All meetings also includes quick meetings, screen sharing, and meeting details."), content);
	scopeHint->setObjectName(QStringLiteral("hintLabel"));
	scopeHint->setWordWrap(true);
	layout->addWidget(scopeHint);

	layout->addWidget(makeSectionTitle(
		QCoreApplication::translate("MeetingUI", "Encryption key"), content));
	_e2eeKey = new QLineEdit(content);
	_e2eeKey->setObjectName(QStringLiteral("settingsE2eeKey"));
	_e2eeKey->setAccessibleName(QCoreApplication::translate("MeetingUI", "Encryption key"));
	_e2eeKey->setEchoMode(QLineEdit::Password);
	_e2eeKey->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
	_e2eeKey->setContextMenuPolicy(Qt::NoContextMenu);
	_e2eeKey->installEventFilter(this);
	// Retain one overflow character so an overlong paste is rejected rather
	// than silently accepted as a different encryption key.
	_e2eeKey->setMaxLength(4097);
	AppTheme::setStyleVariant(*_e2eeKey, "meeting-main-window-this");
	layout->addWidget(_e2eeKey);

	auto *keyButtons = new QHBoxLayout();
	_e2eeSaveKey = new QPushButton(QCoreApplication::translate("MeetingUI", "Save key"), content);
	_e2eeSaveKey->setObjectName(QStringLiteral("settingsE2eeSaveKey"));
	_e2eeSaveKey->setAutoDefault(false);
	_e2eeClearKey = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear key"), content);
	_e2eeClearKey->setObjectName(QStringLiteral("settingsE2eeClearKey"));
	_e2eeClearKey->setAutoDefault(false);
	keyButtons->addWidget(_e2eeSaveKey);
	keyButtons->addWidget(_e2eeClearKey);
	keyButtons->addStretch();
	layout->addLayout(keyButtons);

	_e2eeStatus = new QLabel(content);
	_e2eeStatus->setObjectName(QStringLiteral("settingsE2eeStatus"));
	_e2eeStatus->setWordWrap(true);
	layout->addWidget(_e2eeStatus);
	for (const auto &text : {
			QCoreApplication::translate("MeetingUI", "Use the same encryption key as the other participants and share it through a trusted channel. Participant and connection metadata remain visible to the service."),
			QCoreApplication::translate("MeetingUI", "The encryption key is kept only in memory for this sign-in and cleared on sign-out."),
			QCoreApplication::translate("MeetingUI", "Changes apply the next time you create or join a meeting."),
			QCoreApplication::translate("MeetingUI", "Encrypted meetings support VP8 and H264. Auto uses VP8; select a supported codec in Settings for camera and screen sharing.") }) {
		auto *hint = new QLabel(text, content);
		hint->setObjectName(QStringLiteral("hintLabel"));
		hint->setWordWrap(true);
		layout->addWidget(hint);
	}
	layout->addStretch();

	connect(_quickE2ee, &QCheckBox::toggled, this, [this] {
		commitSecurityPreferences();
	});
	connect(_screenShareE2ee, &QCheckBox::toggled, this, [this] {
		commitSecurityPreferences();
	});
	connect(_meetingDetailsE2ee, &QCheckBox::toggled, this, [this] {
		commitSecurityPreferences();
	});
	connect(_allE2ee, &QCheckBox::toggled, this, [this] {
		commitSecurityPreferences();
	});
	connect(_e2eeKey, &QLineEdit::textChanged, this, [this] {
		updateSecurityKeyControls();
	});
	connect(_e2eeSaveKey, &QPushButton::clicked, this, &SettingsDialog::saveSecurityKey);
	connect(_e2eeClearKey, &QPushButton::clicked, this, [this] {
		clearSecurityEditor();
		_session.clearMeetingEncryptionKey();
		updateSecurityKeyControls();
	});
	return scroll;
}

void SettingsDialog::syncSecurityControls() {
	const auto value = _session.meetingSecurityPreferences();
	const QSignalBlocker quickBlocker(_quickE2ee);
	const QSignalBlocker screenShareBlocker(_screenShareE2ee);
	const QSignalBlocker meetingDetailsBlocker(_meetingDetailsE2ee);
	const QSignalBlocker allBlocker(_allE2ee);
	_quickE2ee->setChecked(value.quickMeetingsE2ee || value.allMeetingsE2ee);
	_screenShareE2ee->setChecked(value.screenShareE2ee || value.allMeetingsE2ee);
	_meetingDetailsE2ee->setChecked(value.meetingDetailsE2ee || value.allMeetingsE2ee);
	_allE2ee->setChecked(value.allMeetingsE2ee);
	_quickE2ee->setEnabled(!value.allMeetingsE2ee);
	_screenShareE2ee->setEnabled(!value.allMeetingsE2ee);
	_meetingDetailsE2ee->setEnabled(!value.allMeetingsE2ee);
	updateSecurityKeyControls();
}

void SettingsDialog::commitSecurityPreferences() {
	auto value = _session.meetingSecurityPreferences();
	// While All is active the checked entry controls are only a projection of
	// its override. Never save that projection over the independent choices.
	if (!value.allMeetingsE2ee && !_allE2ee->isChecked()) {
		value.quickMeetingsE2ee = _quickE2ee->isChecked();
		value.screenShareE2ee = _screenShareE2ee->isChecked();
		value.meetingDetailsE2ee = _meetingDetailsE2ee->isChecked();
	}
	value.allMeetingsE2ee = _allE2ee->isChecked();
	_session.setMeetingSecurityPreferences(value);
	syncSecurityControls();
}

void SettingsDialog::updateSecurityKeyControls() {
	const auto enabled = _quickE2ee->isChecked() || _screenShareE2ee->isChecked() ||
		_meetingDetailsE2ee->isChecked() || _allE2ee->isChecked();
	if (enabled) livekit::MarkSensitiveMemoryUsed();
	_e2eeKey->setEnabled(enabled);
	if (!enabled && !_e2eeKey->text().isEmpty()) clearSecurityEditor();
	const auto hasKey = _session.hasMeetingEncryptionKey();
	_e2eeKey->setPlaceholderText(hasKey
		? QCoreApplication::translate("MeetingUI", "Key configured. Enter a new key to replace it.")
		: QCoreApplication::translate("MeetingUI", "ASCII encryption key (not the meeting password)"));
	_e2eeSaveKey->setEnabled(enabled && !_e2eeKey->text().isEmpty());
	_e2eeClearKey->setEnabled(hasKey);
	AppTheme::setStyleVariant(*_e2eeStatus, hasKey
		? "settings-dialog-audiostatus-2-active" : "");
	_e2eeStatus->setText(hasKey
		? QCoreApplication::translate("MeetingUI", "Encryption key configured.")
		: QCoreApplication::translate("MeetingUI", "Without a saved encryption key, E2EE is off. You can create or join meetings normally."));
	_e2eeStatus->setVisible(enabled || hasKey);
}

void SettingsDialog::saveSecurityKey() {
	if (!_e2eeKey->isEnabled() || _e2eeKey->text().isEmpty()) return;
	auto text = _e2eeKey->text();
	auto bytes = text.toUtf8();
	const auto cleanup = qScopeGuard([&] {
		if (!text.isEmpty()) OPENSSL_cleanse(text.data(), static_cast<size_t>(text.size()) * sizeof(QChar));
		if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
	});
	clearSecurityEditor();
	updateSecurityKeyControls();
	try {
		_session.setMeetingEncryptionKey(std::vector<uint8_t>(bytes.begin(), bytes.end()));
		updateSecurityKeyControls();
	} catch (const livekit::EncryptionRequestException&) {
		AppTheme::setStyleVariant(*_e2eeStatus, "settings-dialog-audiostatus-3");
		_e2eeStatus->setText(QCoreApplication::translate("MeetingUI", "Enter 1 to 4096 printable ASCII characters. Unicode keys are not supported."));
		_e2eeStatus->show();
	}
}

void SettingsDialog::clearSecurityEditor() {
	if (!_e2eeKey) return;
	const QSignalBlocker blocker(_e2eeKey);
	auto text = _e2eeKey->text();
	// setText resets the undo history. Qt/IME historical copies cannot be guaranteed erased.
	_e2eeKey->setText(QString(text.size(), QChar(0)));
	_e2eeKey->clear();
	if (!text.isEmpty()) OPENSSL_cleanse(text.data(), static_cast<size_t>(text.size()) * sizeof(QChar));
}

QWidget *SettingsDialog::buildAboutPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(12);
	layout->addWidget(makePageTitle(QCoreApplication::translate("MeetingUI", "About"), content));
	layout->addStretch();

	auto *productName = new QLabel(QCoreApplication::applicationName(), content);
	if (productName->text().trimmed().isEmpty()) {
		productName->setText(AppBranding::name());
	}
	productName->setAlignment(Qt::AlignCenter);
	auto nameFont = productName->font();
	nameFont.setPixelSize(24);
	nameFont.setBold(true);
	productName->setFont(nameFont);
	layout->addWidget(productName);

	auto version = QCoreApplication::applicationVersion().trimmed();
	if (version.isEmpty()) {
		version = QCoreApplication::translate("MeetingUI", "Development Build");
	}
	auto *versionLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Version %1").arg(version), content);
	versionLabel->setObjectName(QStringLiteral("hintLabel"));
	versionLabel->setAlignment(Qt::AlignCenter);
	layout->addWidget(versionLabel);

	auto *updateButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Check for Updates"), content);
	updateButton->setObjectName(QStringLiteral("secondaryButton"));
	updateButton->setMinimumWidth(240);
	layout->addWidget(updateButton, 0, Qt::AlignHCenter);
	layout->addStretch(2);
	return scroll;
}

void SettingsDialog::connectPreferenceControls() {
	auto syncAndCommit = [this](QCheckBox *source) {
		if (_updatingUi) {
			return;
		}
		syncJoinPreferenceControls(source);
		commitPreferences();
	};
	for (auto *checkBox : {
		_generalCamera,
		_videoCamera,
		_generalMicrophone,
		_audioMicrophone,
		_generalSpeaker,
		_audioSpeaker }) {
		connect(checkBox, &QCheckBox::toggled, this, [syncAndCommit, checkBox](bool) {
			syncAndCommit(checkBox);
		});
	}
	for (auto *checkBox : {
		_quitOnClose,
		_showActiveSpeaker,
		_stayWhenLocked,
		_pushToTalk,
		_echoCancellation,
		_noiseSuppression,
		_autoGainControl }) {
		connect(checkBox, &QCheckBox::toggled, this, [this](bool) {
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}

	connect(_mirrorEnabled, &QCheckBox::toggled, this, [this](bool) {
		if (_updatingUi) {
			return;
		}
		updateMirrorControls();
		commitPreferences();
		requestPreviewIfVisible();
	});
	connect(_localMirror, &QRadioButton::toggled, this, [this](bool checked) {
		if (checked && !_updatingUi) {
			commitPreferences();
			requestPreviewIfVisible();
		}
	});
	connect(_remoteMirror, &QRadioButton::toggled, this, [this](bool checked) {
		if (checked && !_updatingUi) {
			commitPreferences();
			requestPreviewIfVisible();
		}
	});

	connect(_highDefinition, &QCheckBox::toggled, this, [this](bool) {
		if (_updatingUi) {
			return;
		}
		updateHighDefinitionControls();
		commitPreferences();
		emitCameraSelection();
		requestPreviewIfVisible();
	});
	connect(
		_resolutionCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this,
		[this](int) {
			if (_updatingUi) {
				return;
			}
			commitPreferences();
			emitCameraSelection();
			requestPreviewIfVisible();
		});
	for (auto *combo : {_cameraCodecCombo, _screenShareCodecCombo, _screenShareResolutionCombo, _screenShareFpsCombo}) {
		connect(combo,
			static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
			this, [this](int) {
				if (!_updatingUi) commitPreferences();
			});
	}
	connect(
		_cameraCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this,
		[this](int) {
			if (_updatingUi) {
				return;
			}
			auto automatic = _session.mediaPreferences();
			automatic.videoCaptureWidth = 0;
			automatic.videoCaptureHeight = 0;
			automatic.videoCaptureFps = 0;
			rebuildResolutionChoices(automatic);
			commitPreferences();
			emitCameraSelection();
			requestPreviewIfVisible();
		});

	connect(
		_microphoneCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this,
		[this](int) {
			if (_updatingUi) {
				return;
			}
			if (_microphoneTestActive) {
				setMicrophoneTestActive(false);
				emit microphoneTestStopped();
			}
			commitPreferences();
			emit microphoneSelectionChanged(selectedMicrophoneDeviceId());
		});
	connect(
		_speakerCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this,
		[this](int) {
			if (_updatingUi) {
				return;
			}
			commitPreferences();
			emit speakerSelectionChanged(selectedSpeakerDeviceId());
		});
	connect(_microphoneTestButton, &QPushButton::clicked, this, [this](bool checked) {
		setMicrophoneTestActive(checked);
		if (checked) {
			emit microphoneTestStarted(selectedMicrophoneDeviceId());
		} else {
			emit microphoneTestStopped();
		}
	});
	connect(_speakerTestButton, &QPushButton::clicked, this, [this] {
		emit speakerTestRequested(selectedSpeakerDeviceId());
	});
}

OpenMeeting::MediaPreferences SettingsDialog::preferences() const {
	auto value = _session.mediaPreferences();
	value.enableVideo = _generalCamera && _generalCamera->isChecked();
	value.enableMicrophone = _generalMicrophone && _generalMicrophone->isChecked();
	value.enableSpeaker = _generalSpeaker && _generalSpeaker->isChecked();
	value.quitOnMainWindowClose = _quitOnClose && _quitOnClose->isChecked();
	value.showActiveSpeaker = _showActiveSpeaker && _showActiveSpeaker->isChecked();
	value.stayInMeetingWhenLocked = _stayWhenLocked && _stayWhenLocked->isChecked();
	value.pushToTalkWhenMuted = _pushToTalk && _pushToTalk->isChecked();
	value.echoCancellation = _echoCancellation && _echoCancellation->isChecked();
	value.noiseSuppression = _noiseSuppression && _noiseSuppression->isChecked();
	value.autoGainControl = _autoGainControl && _autoGainControl->isChecked();
	if (_videoDevicesReady) value.cameraDeviceId = selectedCameraDeviceId();
	value.microphoneDeviceId = selectedMicrophoneDeviceId();
	value.speakerDeviceId = selectedSpeakerDeviceId();
	value.cameraVideoCodec = _cameraCodecCombo
		? _cameraCodecCombo->currentData().toString() : QStringLiteral("auto");
	value.screenShareResolution = _screenShareResolutionCombo->currentIndex();
	value.screenShareFps = _screenShareFpsCombo->currentData().toInt();
	value.screenShareVideoCodec = _screenShareCodecCombo
		? _screenShareCodecCombo->currentData().toString() : QStringLiteral("auto");

	if (!_mirrorEnabled || !_mirrorEnabled->isChecked()) {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::Off;
	} else if (_remoteMirror && _remoteMirror->isChecked()) {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::LocalAndRemote;
	} else {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::LocalOnly;
	}

	// General settings remain editable while discovery is pending or failed.
	// Do not persist an empty selector over the user's saved device/format.
	if (_videoDevicesReady) {
		const auto format = selectedVideoFormat();
		if (_highDefinition && _highDefinition->isChecked() && format.width > 0 && format.height > 0) {
			value.videoCaptureWidth = format.width;
			value.videoCaptureHeight = format.height;
			value.videoCaptureFps = format.fps;
		} else {
			value.videoCaptureWidth = 0;
			value.videoCaptureHeight = 0;
			value.videoCaptureFps = 0;
		}
	}
	return value;
}

void SettingsDialog::setPreferences(const OpenMeeting::MediaPreferences &value) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_generalCamera->setChecked(value.enableVideo);
	if (!_videoDevicesReady && _cameraCombo->count() > 0) {
		_cameraCombo->setItemData(0, value.cameraDeviceId, kDeviceIdRole);
	}
	_videoCamera->setChecked(value.enableVideo);
	_generalMicrophone->setChecked(value.enableMicrophone);
	_audioMicrophone->setChecked(value.enableMicrophone);
	_generalSpeaker->setChecked(value.enableSpeaker);
	_audioSpeaker->setChecked(value.enableSpeaker);
	_quitOnClose->setChecked(value.quitOnMainWindowClose);
	_showActiveSpeaker->setChecked(value.showActiveSpeaker);
	_stayWhenLocked->setChecked(value.stayInMeetingWhenLocked);
	_pushToTalk->setChecked(value.pushToTalkWhenMuted);
	_echoCancellation->setChecked(value.echoCancellation);
	_noiseSuppression->setChecked(value.noiseSuppression);
	_autoGainControl->setChecked(value.autoGainControl);
	if (_cameraCodecCombo) {
		const auto selected = findData(
			_cameraCodecCombo, OpenMeeting::normalizeVideoCodecPreference(value.cameraVideoCodec));
		_cameraCodecCombo->setCurrentIndex(selected >= 0 ? selected : 0);
	}
	_screenShareResolutionCombo->setCurrentIndex(value.screenShareResolution);
	_screenShareFpsCombo->setCurrentIndex(_screenShareFpsCombo->findData(value.screenShareFps));
	if (_screenShareCodecCombo) {
		const auto selected = findData(_screenShareCodecCombo,
			OpenMeeting::normalizeVideoCodecPreference(value.screenShareVideoCodec));
		_screenShareCodecCombo->setCurrentIndex(selected >= 0 ? selected : 0);
	}
	if (_microphoneCombo && _microphoneCombo->count() > 0) {
		if (_microphoneCombo->itemData(0, kDevicePendingRole).toBool()) {
			_microphoneCombo->setItemData(0, value.microphoneDeviceId, kDeviceIdRole);
		} else {
			const auto selected = _microphoneCombo->findData(value.microphoneDeviceId, kDeviceIdRole);
			_microphoneCombo->setCurrentIndex(selected >= 0 ? selected : 0);
		}
	}
	if (_speakerCombo && _speakerCombo->count() > 0) {
		if (_speakerCombo->itemData(0, kDevicePendingRole).toBool()) {
			// Keep the requested endpoint while asynchronous enumeration is pending.
			_speakerCombo->setItemData(0, value.speakerDeviceId, kDeviceIdRole);
		} else {
			const auto selected = _speakerCombo->findData(value.speakerDeviceId, kDeviceIdRole);
			_speakerCombo->setCurrentIndex(selected >= 0 ? selected : 0);
		}
	}

	const auto mirrorEnabled = value.mirrorMode != OpenMeeting::VideoMirrorMode::Off;
	_mirrorEnabled->setChecked(mirrorEnabled);
	_remoteMirror->setChecked(value.mirrorMode == OpenMeeting::VideoMirrorMode::LocalAndRemote);
	_localMirror->setChecked(value.mirrorMode != OpenMeeting::VideoMirrorMode::LocalAndRemote);
	_highDefinition->setChecked(
		value.videoCaptureWidth > 0 && value.videoCaptureHeight > 0);
	updateMirrorControls();
	updateHighDefinitionControls();
	_updatingUi = oldUpdating;
}

QString SettingsDialog::selectedCameraDeviceId() const {
	if (!_videoDevicesReady) return _session.mediaPreferences().cameraDeviceId;
	return _cameraCombo && _cameraCombo->currentIndex() >= 0
		? _cameraCombo->currentData(kDeviceIdRole).toString()
		: QString();
}

QString SettingsDialog::selectedMicrophoneDeviceId() const {
	return _microphoneCombo && _microphoneCombo->currentIndex() >= 0
		? _microphoneCombo->currentData(kDeviceIdRole).toString()
		: QString();
}

QString SettingsDialog::selectedSpeakerDeviceId() const {
	return _speakerCombo && _speakerCombo->currentIndex() >= 0
		? _speakerCombo->currentData(kDeviceIdRole).toString()
		: QString();
}

QWidget *SettingsDialog::videoPreviewHost() const {
	return _previewHost;
}

void SettingsDialog::showPage(Page page) {
	if (_navigation) {
		_navigation->setCurrentRow(static_cast<int>(page));
	}
}

void SettingsDialog::setVideoPreviewWidget(QWidget *previewWidget) {
	if (!_previewHost || !previewWidget) {
		return;
	}
	_usingExternalPreview = true;
	_cameraPreview->stopPreview();
	_cameraPreview->hide();
	previewWidget->setParent(_previewHost);
	previewWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	_previewHost->layout()->addWidget(previewWidget);
	_previewMessage->hide();
}

void SettingsDialog::refreshDevices() {
	if (_closed) return;
	if (!_firstPaintReported) {
		_forceDeviceRefresh = true;
		return;
	}
	startDeviceDiscovery(true);
}

void SettingsDialog::prepareDevicePlaceholders() {
	const auto value = _session.mediaPreferences();
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_videoDevicesReady = false;
	_cameraCombo->clear();
	_cameraCombo->addItem(QCoreApplication::translate("MeetingUI", "Loading cameras..."), value.cameraDeviceId);
	_cameraCombo->setEnabled(false);
	_cameraFormats.clear();
	_resolutionCombo->clear();
	_resolutionCombo->setEnabled(false);
	_highDefinition->setEnabled(false);
	_microphoneCombo->clear();
	_microphoneCombo->addItem(QCoreApplication::translate("MeetingUI", "Testing microphone..."), value.microphoneDeviceId);
	_microphoneCombo->setItemData(0, true, kDevicePendingRole);
	_microphoneCombo->setEnabled(false);
	_speakerCombo->clear();
	_speakerCombo->addItem(QCoreApplication::translate("MeetingUI", "Testing speaker..."), value.speakerDeviceId);
	_speakerCombo->setItemData(0, true, kDevicePendingRole);
	_speakerCombo->setEnabled(false);
	_microphoneTestButton->setEnabled(false);
	_speakerTestButton->setEnabled(false);
	_updatingUi = oldUpdating;
}

void SettingsDialog::startDeviceDiscovery(bool forceRefresh) {
	if (_closed) return;
	if (_cameraCallbacks) _cameraCallbacks->Revoke();
	_cameraRequest.reset();
	prepareDevicePlaceholders();
	_cameraCallbacks = OpenMeeting::QtCallbackGate<SettingsDialog>::Create(this);
	_cameraRequest = _cameraDiscovery->request(
		[gate = _cameraCallbacks](const CameraDeviceDiscovery::Result &result) {
			gate->Post([result](SettingsDialog *dialog) {
				if (!result.succeeded) {
					dialog->_cameraCombo->setItemText(0,
						QCoreApplication::translate("MeetingUI", "Failed to list cameras"));
					return;
				}
				dialog->applyVideoDevices(*result.devices);
				dialog->requestPreviewIfVisible();
			});
		}, forceRefresh);
	_audioTestController->refreshDevices();
}

void SettingsDialog::applyVideoDevices(const CameraDeviceDiscovery::Devices &devices) {
	// Read preferences at delivery time, not when the worker was started.
	const auto value = _session.mediaPreferences();
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_videoDevicesReady = true;
	_cameraCombo->clear();
	_cameraFormats.clear();
	int defaultIndex = -1;
	{
		for (const auto &device : devices) {
			auto deviceId = QString::fromStdString(device.path);
			if (deviceId.isEmpty()) {
				deviceId = QString::fromStdString(device.name);
			}
			_cameraCombo->addItem(displayDeviceName(device.name, device.is_default), deviceId);
			if (device.is_default) {
				defaultIndex = _cameraCombo->count() - 1;
			}

			QVector<VideoFormat> formats;
			for (const auto &capability : device.capabilities) {
				if (capability.width <= 0 || capability.height <= 0) {
					continue;
				}
				auto existing = std::find_if(
					formats.begin(),
					formats.end(),
					[&](const VideoFormat &format) {
						return format.width == capability.width && format.height == capability.height;
					});
				const auto fps = capability.max_fps > 0
					? std::min(capability.max_fps, 30)
					: 30;
				if (existing == formats.end()) {
					formats.push_back({ capability.width, capability.height, fps });
				} else {
					existing->fps = std::max(existing->fps, fps);
				}
			}
			std::sort(formats.begin(), formats.end(), [](const VideoFormat &a, const VideoFormat &b) {
				const auto aPixels = static_cast<qint64>(a.width) * a.height;
				const auto bPixels = static_cast<qint64>(b.width) * b.height;
				if (aPixels != bPixels) {
					return aPixels < bPixels;
				}
				if (a.width != b.width) {
					return a.width < b.width;
				}
				return a.height < b.height;
			});
			_cameraFormats.insert(deviceId, formats);
		}
	}

	if (_cameraCombo->count() == 0) {
		_cameraCombo->addItem(QCoreApplication::translate("MeetingUI", "No camera available"), QString());
		_cameraCombo->setEnabled(false);
		setVideoPreviewMessage(QCoreApplication::translate("MeetingUI", "No camera available"), true);
	} else {
		_cameraCombo->setEnabled(true);
		auto selectedIndex = findData(_cameraCombo, value.cameraDeviceId);
		if (selectedIndex < 0) {
			selectedIndex = defaultIndex >= 0 ? defaultIndex : 0;
		}
		_cameraCombo->setCurrentIndex(selectedIndex);
	}
	rebuildResolutionChoices(value);
	_updatingUi = oldUpdating;

	if (_cameraCombo->isEnabled()) {
		const auto resolved = preferences();
		if (resolved.cameraDeviceId != value.cameraDeviceId
			|| resolved.videoCaptureWidth != value.videoCaptureWidth
			|| resolved.videoCaptureHeight != value.videoCaptureHeight
			|| resolved.videoCaptureFps != value.videoCaptureFps) {
			commitPreferences();
		}
	}
}

void SettingsDialog::rebuildResolutionChoices(
		const OpenMeeting::MediaPreferences &value) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_resolutionCombo->clear();
	const auto formats = _cameraFormats.value(selectedCameraDeviceId());
	for (const auto &format : formats) {
		_resolutionCombo->addItem(
			QCoreApplication::translate("MeetingUI", "%1 × %2  ·  %3 fps")
				.arg(format.width)
				.arg(format.height)
				.arg(format.fps));
		const auto index = _resolutionCombo->count() - 1;
		_resolutionCombo->setItemData(index, format.width, kWidthRole);
		_resolutionCombo->setItemData(index, format.height, kHeightRole);
		_resolutionCombo->setItemData(index, format.fps, kFpsRole);
	}

	if (formats.isEmpty()) {
		_resolutionCombo->addItem(QCoreApplication::translate("MeetingUI", "No resolution modes available"));
		_resolutionCombo->setEnabled(false);
		_highDefinition->setEnabled(false);
		_highDefinition->setChecked(false);
	} else {
		_highDefinition->setEnabled(true);
		if (!_highDefinition->isChecked()) {
			_highDefinition->setChecked(true);
		}
		auto selected = -1;
		if (value.videoCaptureWidth > 0 && value.videoCaptureHeight > 0) {
			for (auto i = 0; i != formats.size(); ++i) {
				if (formats[i].width == value.videoCaptureWidth
					&& formats[i].height == value.videoCaptureHeight) {
					selected = i;
					break;
				}
			}
		}
		_resolutionCombo->setCurrentIndex(selected >= 0 ? selected : defaultFormatIndex(formats));
		_resolutionCombo->setEnabled(_highDefinition->isChecked());
	}
	_updatingUi = oldUpdating;
}

int SettingsDialog::defaultFormatIndex(const QVector<VideoFormat> &formats) const {
	if (formats.isEmpty()) {
		return -1;
	}
	constexpr auto targetWidth = 1920;
	constexpr auto targetHeight = 1080;
	const auto targetPixels = static_cast<qint64>(targetWidth) * targetHeight;
	auto maximumPixels = qint64(0);
	auto maximumIndex = 0;
	for (auto i = 0; i != formats.size(); ++i) {
		const auto pixels = static_cast<qint64>(formats[i].width) * formats[i].height;
		if (pixels > maximumPixels) {
			maximumPixels = pixels;
			maximumIndex = i;
		}
		if (formats[i].width == targetWidth && formats[i].height == targetHeight) {
			return i;
		}
	}
	if (maximumPixels < targetPixels) {
		return maximumIndex;
	}

	auto bestIndex = 0;
	auto bestDistance = std::numeric_limits<double>::max();
	for (auto i = 0; i != formats.size(); ++i) {
		const auto widthDelta = (formats[i].width - targetWidth) / static_cast<double>(targetWidth);
		const auto heightDelta = (formats[i].height - targetHeight) / static_cast<double>(targetHeight);
		const auto distance = widthDelta * widthDelta + heightDelta * heightDelta;
		if (distance < bestDistance) {
			bestDistance = distance;
			bestIndex = i;
		}
	}
	return bestIndex;
}

SettingsDialog::VideoFormat SettingsDialog::selectedVideoFormat() const {
	if (!_resolutionCombo || _resolutionCombo->currentIndex() < 0) {
		return {};
	}
	return {
		_resolutionCombo->currentData(kWidthRole).toInt(),
		_resolutionCombo->currentData(kHeightRole).toInt(),
		_resolutionCombo->currentData(kFpsRole).toInt(),
	};
}

void SettingsDialog::syncJoinPreferenceControls(QCheckBox *source) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	if (source == _generalCamera || source == _videoCamera) {
		_generalCamera->setChecked(source->isChecked());
		_videoCamera->setChecked(source->isChecked());
	} else if (source == _generalMicrophone || source == _audioMicrophone) {
		_generalMicrophone->setChecked(source->isChecked());
		_audioMicrophone->setChecked(source->isChecked());
	} else if (source == _generalSpeaker || source == _audioSpeaker) {
		_generalSpeaker->setChecked(source->isChecked());
		_audioSpeaker->setChecked(source->isChecked());
	}
	_updatingUi = oldUpdating;
}

void SettingsDialog::updateMirrorControls() {
	const auto enabled = _mirrorEnabled && _mirrorEnabled->isChecked();
	_localMirror->setEnabled(enabled);
	_remoteMirror->setEnabled(enabled);
	if (enabled && !_localMirror->isChecked() && !_remoteMirror->isChecked()) {
		_localMirror->setChecked(true);
	}
	if (_cameraPreview) {
		_cameraPreview->setMirrored(enabled);
	}
}

void SettingsDialog::updateHighDefinitionControls() {
	_resolutionCombo->setEnabled(
		_highDefinition->isEnabled() && _highDefinition->isChecked()
		&& !_cameraFormats.value(selectedCameraDeviceId()).isEmpty());
}

void SettingsDialog::commitPreferences() {
	if (_updatingUi) {
		return;
	}
	const auto value = preferences();
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_session.setMediaPreferences(value);
	_updatingUi = oldUpdating;
	emit preferencesEdited(value);
}

void SettingsDialog::emitCameraSelection() {
	const auto value = preferences();
	emit cameraSelectionChanged(
		value.cameraDeviceId,
		value.videoCaptureWidth,
		value.videoCaptureHeight,
		value.videoCaptureFps);
}

void SettingsDialog::requestPreviewIfVisible() {
	if (_closed || !_videoDevicesReady || !isVisible()
		|| !_pages || _pages->currentIndex() != static_cast<int>(Page::Video)) {
		return;
	}
	const auto value = preferences();
	if (value.cameraDeviceId.isEmpty()) {
		_cameraPreview->stopPreview();
		setVideoPreviewMessage(QCoreApplication::translate("MeetingUI", "No camera available"), true);
		return;
	}
	const auto format = selectedVideoFormat();
	if (!_usingExternalPreview) {
		_previewMessage->hide();
		_cameraPreview->show();
		_cameraPreview->startPreview(
			value.cameraDeviceId,
			format.width,
			format.height,
			format.fps);
	}
	emit cameraPreviewRequested(
		value.cameraDeviceId,
		format.width,
		format.height,
		format.fps);
}

void SettingsDialog::setMicrophoneLevel(float normalizedLevel) {
	if (!_microphoneLevel) {
		return;
	}
	const auto level = std::clamp(normalizedLevel, 0.0f, 1.0f);
	_microphoneLevel->setValue(static_cast<int>(std::lround(level * 100.0f)));
}

void SettingsDialog::setMicrophoneTestActive(bool active) {
	_microphoneTestActive = active;
	if (!_microphoneTestButton) {
		return;
	}
	const QSignalBlocker blocker(_microphoneTestButton);
	_microphoneTestButton->setChecked(active);
	_microphoneTestButton->setText(
		active ? QCoreApplication::translate("MeetingUI", "Stop Test") : QCoreApplication::translate("MeetingUI", "Test Microphone"));
	if (!active) {
		setMicrophoneLevel(0.0f);
	}
}

void SettingsDialog::setSpeakerTestActive(bool active) {
	_speakerTestActive = active;
	if (!_speakerTestButton) {
		return;
	}
	_speakerTestButton->setEnabled(!active && _speakerCombo->isEnabled());
	_speakerTestButton->setText(
		active ? QCoreApplication::translate("MeetingUI", "Playing") : QCoreApplication::translate("MeetingUI", "Test Speaker"));
}

void SettingsDialog::setVideoPreviewMessage(const QString &message, bool error) {
	if (!_previewMessage) {
		return;
	}
	_previewMessage->setText(message);
	MeetingUI::AppTheme::setStyleVariant(*_previewMessage, error ? "settings-dialog-previewmessage-active" : "settings-dialog-previewmessage-normal");
	_previewMessage->setVisible(true);
	if (!_usingExternalPreview) {
		_cameraPreview->setVisible(false);
	}
}

void SettingsDialog::mousePressEvent(QMouseEvent *event) {
	const QRect titleBarRect(
		kShadowMargin,
		kShadowMargin,
		width() - kShadowMargin * 2,
		kTitleBarHeight);
	if (event->button() == Qt::LeftButton && titleBarRect.contains(event->pos())) {
		_dragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
#else
		_dragOffset = event->globalPos() - frameGeometry().topLeft();
#endif
		event->accept();
		return;
	}
	QDialog::mousePressEvent(event);
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *event) {
	if (_dragging && (event->buttons() & Qt::LeftButton)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		move(event->globalPosition().toPoint() - _dragOffset);
#else
		move(event->globalPos() - _dragOffset);
#endif
		event->accept();
		return;
	}
	QDialog::mouseMoveEvent(event);
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton) {
		_dragging = false;
	}
	QDialog::mouseReleaseEvent(event);
}

} // namespace MeetingUI
