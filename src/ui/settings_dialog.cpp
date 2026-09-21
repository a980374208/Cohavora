#include "src/ui/settings_dialog.h"

#include "src/media/dshow_enumerator.h"
#include "src/ui/audio_device_test_controller.h"
#include "src/ui/camera_preview_widget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSignalBlocker>
#include <QtCore/QVariant>
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
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedLayout>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace MeetingUI {
namespace {

constexpr auto kDeviceIdRole = Qt::UserRole;
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

QWidget *makeScrollablePage(QWidget *content, QWidget *parent) {
	auto *scroll = new QScrollArea(parent);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(content);
	return scroll;
}

QString displayDeviceName(const std::string &name, bool isDefault) {
	auto result = QString::fromStdString(name);
	if (result.trimmed().isEmpty()) {
		result = QString::fromUtf8("未命名设备");
	}
	if (isDefault) {
		result += QString::fromUtf8("（系统默认）");
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

} // namespace

SettingsDialog::SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent)
	: QDialog(parent)
	, _session(session) {
	setObjectName(QStringLiteral("settingsDialog"));
	setWindowTitle(QString::fromUtf8("设置"));
	setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
	setAttribute(Qt::WA_TranslucentBackground, true);
	setModal(true);
	resize(760 + kShadowMargin * 2, 640 + kShadowMargin * 2);
	setMinimumSize(680 + kShadowMargin * 2, 520 + kShadowMargin * 2);
	buildUi();
	setPreferences(_session.mediaPreferences());
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
		_audioTestController->stopAll();
		setMicrophoneTestActive(false);
		_cameraPreview->stopPreview();
		emit cameraPreviewStopped();
	});

	refreshDevices();
}

SettingsDialog::~SettingsDialog() {
	if (_audioTestController) {
		_audioTestController->stopAll();
	}
	if (_cameraPreview) {
		_cameraPreview->stopPreview();
	}
}

void SettingsDialog::buildUi() {
	setStyleSheet(R"(
		QDialog#settingsDialog {
			background: transparent;
		}
		QFrame#settingsShadowSurface {
			background: #ffffff;
			border-radius: 12px;
		}
		QFrame#settingsSurface {
			background: #ffffff;
			border: 1px solid #dfe3e8;
			border-radius: 12px;
		}
		QWidget {
			font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
			font-size: 13px;
			color: #1f2329;
		}
		QWidget#titleBar {
			background: #ffffff;
			border-bottom: 1px solid #e5e8ec;
		}
		QLabel#dialogTitle {
			font-size: 15px;
			font-weight: 500;
			color: #303133;
		}
		QPushButton#closeButton {
			border: none;
			background: transparent;
			border-radius: 4px;
		}
		QPushButton#closeButton:hover { background: #f2f3f5; }
		QListWidget#settingsNavigation {
			background: #ffffff;
			border: none;
			border-right: 1px solid #e5e8ec;
			outline: none;
			padding: 8px 7px;
		}
		QListWidget#settingsNavigation::item {
			height: 40px;
			padding-left: 12px;
			border-radius: 6px;
			color: #303133;
		}
		QListWidget#settingsNavigation::item:hover { background: #f5f7fa; }
		QListWidget#settingsNavigation::item:selected {
			background: #1677ff;
			color: #ffffff;
		}
		QLabel#pageTitle { color: #1f2329; }
		QLabel#sectionTitle { color: #1f2329; }
		QLabel#hintLabel { color: #8f959e; font-size: 12px; }
		QComboBox {
			min-height: 34px;
			border: 1px solid #dcdfe6;
			border-radius: 5px;
			background: #ffffff;
			padding: 0 10px;
		}
		QComboBox:hover, QComboBox:focus { border-color: #1677ff; }
		QComboBox:disabled { background: #f5f6f7; color: #a8abb2; }
		QCheckBox, QRadioButton { spacing: 8px; min-height: 26px; }
		QCheckBox::indicator, QRadioButton::indicator { width: 16px; height: 16px; }
		QPushButton#secondaryButton {
			min-height: 34px;
			padding: 0 16px;
			border: 1px solid #dcdfe6;
			border-radius: 5px;
			background: #ffffff;
			color: #303133;
		}
		QPushButton#secondaryButton:hover { border-color: #1677ff; color: #1677ff; }
		QPushButton#secondaryButton:pressed { background: #eef5ff; }
		QPushButton#secondaryButton:disabled { color: #a8abb2; background: #f5f6f7; }
		QFrame#previewHost {
			background: #172033;
			border: 1px solid #26334d;
			border-radius: 7px;
		}
		QLabel#previewMessage { color: #c8d1df; font-size: 14px; }
		QProgressBar {
			min-height: 8px;
			max-height: 8px;
			border: none;
			border-radius: 4px;
			background: #e9edf2;
		}
		QProgressBar::chunk { border-radius: 4px; background: #20b26b; }
	)" );

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(
		kShadowMargin,
		kShadowMargin,
		kShadowMargin,
		kShadowMargin);
	root->setSpacing(0);

	auto *layers = new QStackedLayout();
	layers->setContentsMargins(0, 0, 0, 0);
	layers->setStackingMode(QStackedLayout::StackAll);
	root->addLayout(layers);

	auto *shadowSurface = new QFrame(this);
	shadowSurface->setObjectName(QStringLiteral("settingsShadowSurface"));
	shadowSurface->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	auto *shadow = new QGraphicsDropShadowEffect(shadowSurface);
	shadow->setBlurRadius(30.0);
	shadow->setOffset(0.0, 4.0);
	shadow->setColor(QColor(31, 35, 41, 80));
	shadowSurface->setGraphicsEffect(shadow);
	layers->addWidget(shadowSurface);

	auto *surface = new RoundedSurface(this);
	surface->setObjectName(QStringLiteral("settingsSurface"));
	auto *surfaceLayout = new QVBoxLayout(surface);
	surfaceLayout->setContentsMargins(0, 0, 0, 0);
	surfaceLayout->setSpacing(0);
	layers->addWidget(surface);
	layers->setCurrentWidget(surface);

	auto *titleBar = new QWidget(surface);
	titleBar->setObjectName(QStringLiteral("titleBar"));
	titleBar->setFixedHeight(kTitleBarHeight);
	auto *titleLayout = new QHBoxLayout(titleBar);
	titleLayout->setContentsMargins(18, 0, 12, 0);
	titleLayout->addStretch();
	auto *title = new QLabel(QString::fromUtf8("设置"), titleBar);
	title->setObjectName(QStringLiteral("dialogTitle"));
	titleLayout->addWidget(title);
	titleLayout->addStretch();
	auto *closeButton = new QPushButton(titleBar);
	closeButton->setObjectName(QStringLiteral("closeButton"));
	closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
	closeButton->setToolTip(QString::fromUtf8("关闭"));
	closeButton->setFixedSize(28, 28);
	titleLayout->addWidget(closeButton);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	surfaceLayout->addWidget(titleBar);

	auto *body = new QHBoxLayout();
	body->setContentsMargins(0, 0, 0, 0);
	body->setSpacing(0);

	_navigation = new QListWidget(surface);
	_navigation->setObjectName(QStringLiteral("settingsNavigation"));
	_navigation->setFixedWidth(190);
	_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_navigation->setSelectionMode(QAbstractItemView::SingleSelection);
	_navigation->addItem(new QListWidgetItem(
		style()->standardIcon(QStyle::SP_FileDialogDetailedView),
		QString::fromUtf8("常规设置")));
	_navigation->addItem(new QListWidgetItem(
		style()->standardIcon(QStyle::SP_MediaPlay),
		QString::fromUtf8("视频")));
	_navigation->addItem(new QListWidgetItem(
		style()->standardIcon(QStyle::SP_MediaVolume),
		QString::fromUtf8("音频")));
	_navigation->addItem(new QListWidgetItem(
		style()->standardIcon(QStyle::SP_MessageBoxInformation),
		QString::fromUtf8("关于我们")));
	body->addWidget(_navigation);

	_pages = new QStackedWidget(surface);
	_pages->addWidget(buildGeneralPage());
	_pages->addWidget(buildVideoPage());
	_pages->addWidget(buildAudioPage());
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
	_navigation->setCurrentRow(static_cast<int>(Page::General));
}

QWidget *SettingsDialog::buildGeneralPage() {
	auto *content = new QWidget(this);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QString::fromUtf8("常规设置"), content));
	layout->addSpacing(6);

	_generalCamera = new QCheckBox(QString::fromUtf8("入会开启摄像头"), content);
	_generalMicrophone = new QCheckBox(QString::fromUtf8("入会开启麦克风"), content);
	_generalSpeaker = new QCheckBox(QString::fromUtf8("入会时使用电脑音频"), content);
	_quitOnClose = new QCheckBox(QString::fromUtf8("关闭主面板时，退出程序"), content);
	_showActiveSpeaker = new QCheckBox(QString::fromUtf8("显示当前说话者"), content);
	_stayWhenLocked = new QCheckBox(QString::fromUtf8("锁屏时不退出会议"), content);

	layout->addWidget(_generalCamera);
	layout->addWidget(_generalMicrophone);
	layout->addWidget(_generalSpeaker);
	layout->addSpacing(5);
	layout->addWidget(_quitOnClose);
	layout->addWidget(_showActiveSpeaker);
	layout->addWidget(_stayWhenLocked);
	layout->addStretch();
	return makeScrollablePage(content, this);
}

QWidget *SettingsDialog::buildVideoPage() {
	auto *content = new QWidget(this);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QString::fromUtf8("视频"), content));
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
	_previewMessage = new QLabel(QString::fromUtf8("摄像头预览"), _previewHost);
	_previewMessage->setObjectName(QStringLiteral("previewMessage"));
	_previewMessage->setAlignment(Qt::AlignCenter);
	_previewMessage->setWordWrap(true);
	_previewMessage->hide();
	previewLayout->addWidget(_previewMessage, 1);
	layout->addWidget(_previewHost);

	layout->addWidget(makeSectionTitle(QString::fromUtf8("摄像头"), content));
	_cameraCombo = new QComboBox(content);
	_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_cameraCombo->setMinimumContentsLength(28);
	layout->addWidget(_cameraCombo);

	_videoCamera = new QCheckBox(QString::fromUtf8("入会开启摄像头"), content);
	layout->addWidget(_videoCamera);

	_highDefinition = new QCheckBox(QString::fromUtf8("高清摄像头画质"), content);
	layout->addWidget(_highDefinition);
	_resolutionCombo = new QComboBox(content);
	_resolutionCombo->setMinimumWidth(280);
	layout->addWidget(_resolutionCombo);
	auto *resolutionHint = new QLabel(
		QString::fromUtf8("默认选择最接近 1920 × 1080 的设备档位；低于 1080p 的设备使用其最高档位。"),
		content);
	resolutionHint->setObjectName(QStringLiteral("hintLabel"));
	resolutionHint->setWordWrap(true);
	layout->addWidget(resolutionHint);

	_mirrorEnabled = new QCheckBox(QString::fromUtf8("视频镜像"), content);
	layout->addWidget(_mirrorEnabled);
	_localMirror = new QRadioButton(QString::fromUtf8("仅自己看到镜像"), content);
	_remoteMirror = new QRadioButton(QString::fromUtf8("所有参会者看到镜像"), content);
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
	return makeScrollablePage(content, this);
}

QWidget *SettingsDialog::buildAudioPage() {
	auto *content = new QWidget(this);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(8);
	layout->addWidget(makePageTitle(QString::fromUtf8("音频"), content));
	layout->addSpacing(4);

	layout->addWidget(makeSectionTitle(QString::fromUtf8("扬声器"), content));
	auto *speakerRow = new QHBoxLayout();
	_speakerCombo = new QComboBox(content);
	_speakerCombo->setMinimumContentsLength(24);
	_speakerTestButton = new QPushButton(QString::fromUtf8("检测扬声器"), content);
	_speakerTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_speakerTestButton->setFixedWidth(108);
	speakerRow->addWidget(_speakerCombo, 1);
	speakerRow->addWidget(_speakerTestButton);
	layout->addLayout(speakerRow);
	layout->addSpacing(12);

	layout->addWidget(makeSectionTitle(QString::fromUtf8("麦克风"), content));
	auto *microphoneRow = new QHBoxLayout();
	_microphoneCombo = new QComboBox(content);
	_microphoneCombo->setMinimumContentsLength(24);
	_microphoneTestButton = new QPushButton(QString::fromUtf8("检测麦克风"), content);
	_microphoneTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_microphoneTestButton->setCheckable(true);
	_microphoneTestButton->setFixedWidth(108);
	microphoneRow->addWidget(_microphoneCombo, 1);
	microphoneRow->addWidget(_microphoneTestButton);
	layout->addLayout(microphoneRow);
	_microphoneLevel = new QProgressBar(content);
	_microphoneLevel->setRange(0, 100);
	_microphoneLevel->setValue(0);
	_microphoneLevel->setTextVisible(false);
	_microphoneLevel->setAccessibleName(QString::fromUtf8("麦克风输入电平"));
	layout->addWidget(_microphoneLevel);
	_audioStatus = new QLabel(content);
	_audioStatus->setObjectName(QStringLiteral("hintLabel"));
	_audioStatus->setWordWrap(true);
	_audioStatus->hide();
	layout->addWidget(_audioStatus);
	layout->addSpacing(8);

	_audioMicrophone = new QCheckBox(QString::fromUtf8("入会开启麦克风"), content);
	_audioSpeaker = new QCheckBox(QString::fromUtf8("入会时使用电脑音频"), content);
	_pushToTalk = new QCheckBox(QString::fromUtf8("静音状态下长按空格键临时开启麦克风"), content);
	_noiseSuppression = new QCheckBox(QString::fromUtf8("抑制背景噪声"), content);
	layout->addWidget(_audioMicrophone);
	layout->addWidget(_audioSpeaker);
	layout->addWidget(_pushToTalk);
	layout->addSpacing(8);
	layout->addWidget(makeSectionTitle(QString::fromUtf8("音频降噪与增强"), content));
	layout->addWidget(_noiseSuppression);
	layout->addStretch();
	return makeScrollablePage(content, this);
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
			_audioStatus->setStyleSheet(QStringLiteral("color: #f53f3f;"));
			_audioStatus->setText(message);
			_audioStatus->show();
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::speakerTestFinished,
		this,
		[this](bool success, const QString &message) {
			_audioStatus->setStyleSheet(success
				? QStringLiteral("color: #20a162;")
				: QStringLiteral("color: #f53f3f;"));
			_audioStatus->setText(success
				? QString::fromUtf8("扬声器检测完成")
				: message);
			_audioStatus->setVisible(success || !message.isEmpty());
		});
	connect(
		_audioTestController,
		&AudioDeviceTestController::deviceEnumerationFailed,
		this,
		[this](const QString &message) {
			_audioStatus->setStyleSheet(QStringLiteral("color: #f53f3f;"));
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
			auto defaultMicrophone = -1;
			auto defaultSpeaker = -1;
			for (const auto &device : microphones) {
				auto name = device.name.trimmed().isEmpty()
					? QString::fromUtf8("未命名设备")
					: device.name;
				if (device.isDefault) name += QString::fromUtf8("（系统默认）");
				_microphoneCombo->addItem(name, device.id);
				if (device.isDefault) defaultMicrophone = _microphoneCombo->count() - 1;
			}
			for (const auto &device : speakers) {
				auto name = device.name.trimmed().isEmpty()
					? QString::fromUtf8("未命名设备")
					: device.name;
				if (device.isDefault) name += QString::fromUtf8("（系统默认）");
				_speakerCombo->addItem(name, device.id);
				if (device.isDefault) defaultSpeaker = _speakerCombo->count() - 1;
			}

			if (_microphoneCombo->count() == 0) {
				_microphoneCombo->addItem(QString::fromUtf8("未检测到可用麦克风"), QString());
				_microphoneCombo->setEnabled(false);
				_microphoneTestButton->setEnabled(false);
			} else {
				_microphoneCombo->setEnabled(true);
				_microphoneTestButton->setEnabled(true);
				const auto selected = findData(_microphoneCombo, saved.microphoneDeviceId);
				_microphoneCombo->setCurrentIndex(
					selected >= 0 ? selected : (defaultMicrophone >= 0 ? defaultMicrophone : 0));
			}
			if (_speakerCombo->count() == 0) {
				_speakerCombo->addItem(QString::fromUtf8("未检测到可用扬声器"), QString());
				_speakerCombo->setEnabled(false);
				_speakerTestButton->setEnabled(false);
			} else {
				_speakerCombo->setEnabled(true);
				_speakerTestButton->setEnabled(true);
				const auto selected = findData(_speakerCombo, saved.speakerDeviceId);
				_speakerCombo->setCurrentIndex(
					selected >= 0 ? selected : (defaultSpeaker >= 0 ? defaultSpeaker : 0));
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

QWidget *SettingsDialog::buildAboutPage() {
	auto *content = new QWidget(this);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(24, 20, 24, 24);
	layout->setSpacing(12);
	layout->addWidget(makePageTitle(QString::fromUtf8("关于我们"), content));
	layout->addStretch();

	auto *productName = new QLabel(QCoreApplication::applicationName(), content);
	if (productName->text().trimmed().isEmpty()) {
		productName->setText(QStringLiteral("LiveKit Meeting"));
	}
	productName->setAlignment(Qt::AlignCenter);
	auto nameFont = productName->font();
	nameFont.setPixelSize(24);
	nameFont.setBold(true);
	productName->setFont(nameFont);
	layout->addWidget(productName);

	auto version = QCoreApplication::applicationVersion().trimmed();
	if (version.isEmpty()) {
		version = QString::fromUtf8("开发版本");
	}
	auto *versionLabel = new QLabel(QString::fromUtf8("版本 %1").arg(version), content);
	versionLabel->setObjectName(QStringLiteral("hintLabel"));
	versionLabel->setAlignment(Qt::AlignCenter);
	layout->addWidget(versionLabel);

	auto *updateButton = new QPushButton(QString::fromUtf8("检查更新"), content);
	updateButton->setObjectName(QStringLiteral("secondaryButton"));
	updateButton->setFixedWidth(240);
	layout->addWidget(updateButton, 0, Qt::AlignHCenter);
	layout->addStretch(2);
	return makeScrollablePage(content, this);
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
		_noiseSuppression }) {
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
	value.noiseSuppression = _noiseSuppression && _noiseSuppression->isChecked();
	value.cameraDeviceId = selectedCameraDeviceId();
	value.microphoneDeviceId = selectedMicrophoneDeviceId();
	value.speakerDeviceId = selectedSpeakerDeviceId();

	if (!_mirrorEnabled || !_mirrorEnabled->isChecked()) {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::Off;
	} else if (_remoteMirror && _remoteMirror->isChecked()) {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::LocalAndRemote;
	} else {
		value.mirrorMode = OpenMeeting::VideoMirrorMode::LocalOnly;
	}

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
	return value;
}

void SettingsDialog::setPreferences(const OpenMeeting::MediaPreferences &value) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_generalCamera->setChecked(value.enableVideo);
	_videoCamera->setChecked(value.enableVideo);
	_generalMicrophone->setChecked(value.enableMicrophone);
	_audioMicrophone->setChecked(value.enableMicrophone);
	_generalSpeaker->setChecked(value.enableSpeaker);
	_audioSpeaker->setChecked(value.enableSpeaker);
	_quitOnClose->setChecked(value.quitOnMainWindowClose);
	_showActiveSpeaker->setChecked(value.showActiveSpeaker);
	_stayWhenLocked->setChecked(value.stayInMeetingWhenLocked);
	_pushToTalk->setChecked(value.pushToTalkWhenMuted);
	_noiseSuppression->setChecked(value.noiseSuppression);

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
	const auto value = _session.mediaPreferences();
	refreshVideoDevices(value);
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_microphoneCombo->clear();
	_microphoneCombo->addItem(QString::fromUtf8("正在检测麦克风…"), QString());
	_microphoneCombo->setEnabled(false);
	_speakerCombo->clear();
	_speakerCombo->addItem(QString::fromUtf8("正在检测扬声器…"), QString());
	_speakerCombo->setEnabled(false);
	_microphoneTestButton->setEnabled(false);
	_speakerTestButton->setEnabled(false);
	_updatingUi = oldUpdating;
	_audioTestController->refreshDevices();
}

void SettingsDialog::refreshVideoDevices(const OpenMeeting::MediaPreferences &value) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	_cameraCombo->clear();
	_cameraFormats.clear();
	int defaultIndex = -1;
	try {
		const auto devices = livekit::DShowEnumerator::EnumerateVideoDevices();
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
	} catch (...) {
		// Device discovery failure is represented as an empty selector.
	}

	if (_cameraCombo->count() == 0) {
		_cameraCombo->addItem(QString::fromUtf8("未检测到可用摄像头"), QString());
		_cameraCombo->setEnabled(false);
		setVideoPreviewMessage(QString::fromUtf8("未检测到可用摄像头"), true);
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
			QString::fromUtf8("%1 × %2  ·  %3 fps")
				.arg(format.width)
				.arg(format.height)
				.arg(format.fps));
		const auto index = _resolutionCombo->count() - 1;
		_resolutionCombo->setItemData(index, format.width, kWidthRole);
		_resolutionCombo->setItemData(index, format.height, kHeightRole);
		_resolutionCombo->setItemData(index, format.fps, kFpsRole);
	}

	if (formats.isEmpty()) {
		_resolutionCombo->addItem(QString::fromUtf8("未获取到分辨率档位"));
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
	if (!_pages || _pages->currentIndex() != static_cast<int>(Page::Video)) {
		return;
	}
	const auto value = preferences();
	if (value.cameraDeviceId.isEmpty()) {
		_cameraPreview->stopPreview();
		setVideoPreviewMessage(QString::fromUtf8("未检测到可用摄像头"), true);
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
		active ? QString::fromUtf8("停止检测") : QString::fromUtf8("检测麦克风"));
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
		active ? QString::fromUtf8("正在播放") : QString::fromUtf8("检测扬声器"));
}

void SettingsDialog::setVideoPreviewMessage(const QString &message, bool error) {
	if (!_previewMessage) {
		return;
	}
	_previewMessage->setText(message);
	_previewMessage->setStyleSheet(error ? QStringLiteral("color: #ff7875;") : QString());
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
