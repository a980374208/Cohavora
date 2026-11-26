#include "src/ui/whiteboard/accessible_combo_box.h"
#include <QtCore/QCoreApplication>
#include "src/ui/app_branding.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/app_icons.h"
#include "src/ui/settings_dialog.h"
#include "src/ui/copilot_settings_page.h"
#include "src/ui/detail/settings_ui_helpers.h"
#ifdef Q_OS_LINUX
#include "src/ui/linux_window_frame.h"
#endif
#include "src/ui/screen_share_quality_controls.h"
#include "src/update/update_service.h"
#include <QtWidgets/QFileDialog>

#include "src/core/room.h"
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
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QRegion>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedLayout>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QStandardItemModel>
#include "src/rtc/webrtc_manager.h"
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
#include "src/rtc/linux/linux_encoder_capability_controller.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>


namespace MeetingUI {
namespace {

#ifdef Q_OS_WIN
constexpr bool kAudioDeviceTestsSupported = true;
#else
constexpr bool kAudioDeviceTestsSupported = false;
#endif

constexpr auto kDeviceIdRole = Qt::UserRole;
constexpr auto kDevicePendingRole = Qt::UserRole + 1;
constexpr auto kWidthRole = Qt::UserRole;
constexpr auto kHeightRole = Qt::UserRole + 1;
constexpr auto kFpsRole = Qt::UserRole + 2;
constexpr auto kSurfaceRadius = 12;
#ifdef Q_OS_LINUX
// LinuxWindowFrame paints outside the content surface without changing its
// geometry. Keep the existing in-buffer shadow only on the other platforms.
constexpr auto kShadowMargin = 0;
#else
constexpr auto kShadowMargin = 18;
#endif
constexpr auto kTitleBarHeight = 49;

#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
QString linuxEncoderCapabilityHint(const livekit::VideoEncoderBackendCapability& capability) {
	using Reason = livekit::LinuxEncoderCapabilityReason;
	const char* text = QT_TRANSLATE_NOOP("MeetingUI", "The encoder could not be initialized.");
	if (capability.probe_ready) text = QT_TRANSLATE_NOOP("MeetingUI", "Initialization succeeded. Encoded output has not been verified.");
	else switch (capability.reason) {
	case Reason::NotCompiled: text = QT_TRANSLATE_NOOP("MeetingUI", "This encoder is not included in this build."); break;
	case Reason::NotProbed: text = QT_TRANSLATE_NOOP("MeetingUI", "This encoder has not been checked yet."); break;
	case Reason::MissingLibrary: text = QT_TRANSLATE_NOOP("MeetingUI", "The required encoder runtime is missing."); break;
	case Reason::RuntimeIncompatible: text = QT_TRANSLATE_NOOP("MeetingUI", "The installed encoder runtime is incompatible with this build."); break;
	case Reason::NoDevice: text = QT_TRANSLATE_NOOP("MeetingUI", "No matching encoding device is accessible."); break;
	case Reason::PermissionDenied: text = QT_TRANSLATE_NOOP("MeetingUI", "Access to the encoding device was denied."); break;
	case Reason::UnsupportedProfile:
	case Reason::UnsupportedConfiguration: text = QT_TRANSLATE_NOOP("MeetingUI", "The encoding configuration is not supported."); break;
	case Reason::TimedOut: text = QT_TRANSLATE_NOOP("MeetingUI", "Encoder detection timed out. Restart the application before retrying."); break;
	case Reason::RetirementFailed: text = QT_TRANSLATE_NOOP("MeetingUI", "Encoder cleanup could not be confirmed. Restart the application before retrying."); break;
	case Reason::DeviceChanged: text = QT_TRANSLATE_NOOP("MeetingUI", "The selected GPU changed. Restart the application to select a different device."); break;
	case Reason::Cancelled: text = QT_TRANSLATE_NOOP("MeetingUI", "Encoder detection was cancelled."); break;
	default: break;
	}
	return QCoreApplication::translate("MeetingUI", text);
}
#endif

class RoundedSurface final : public QFrame {
public:
	explicit RoundedSurface(QWidget *parent)
		: QFrame(parent) {
	}

protected:
	void resizeEvent(QResizeEvent *event) override {
		QFrame::resizeEvent(event);
#ifndef Q_OS_LINUX
		QPainterPath path;
		path.addRoundedRect(QRectF(rect()), kSurfaceRadius, kSurfaceRadius);
		setMask(QRegion(path.toFillPolygon().toPolygon()));
#endif
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

constexpr int kNavGroupHeaderRole = Qt::UserRole + 10;
constexpr int kNavIconKindRole = Qt::UserRole + 11;

class SettingsNavDelegate final : public QStyledItemDelegate {
public:
	explicit SettingsNavDelegate(QObject *parent = nullptr)
		: QStyledItemDelegate(parent) {}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
		const bool hasHeader = !index.data(kNavGroupHeaderRole).toString().isEmpty();
		const int extra = hasHeader ? (index.row() == 0 ? 24 : 30) : 0;
		return QSize(option.rect.width(), 38 + extra);
	}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
		p->save();
		p->setRenderHint(QPainter::Antialiasing);
		p->setRenderHint(QPainter::TextAntialiasing);

		QRect itemRect = option.rect.adjusted(2, 1, -2, -1);
		const QString groupTitle = index.data(kNavGroupHeaderRole).toString();
		if (!groupTitle.isEmpty()) {
			const int headerH = index.row() == 0 ? 22 : 28;
			const QRect headerRect(itemRect.left() + 10, itemRect.top() + (index.row() == 0 ? 2 : 8),
				itemRect.width() - 16, 18);
			QFont hf = option.font;
			hf.setPixelSize(11);
			hf.setBold(true);
			p->setFont(hf);
			p->setPen(AppTheme::color(AppTheme::ColorRole::TextMuted));
			p->drawText(headerRect, Qt::AlignLeft | Qt::AlignVCenter, groupTitle);
			itemRect.setTop(itemRect.top() + headerH + 2);
		}

		const bool selected = option.state.testFlag(QStyle::State_Selected);
		const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
		if (selected) {
			p->setPen(Qt::NoPen);
			p->setBrush(AppTheme::color(AppTheme::ColorRole::AccentSoft));
			p->drawRoundedRect(itemRect, 8, 8);
		} else if (hovered) {
			p->setPen(Qt::NoPen);
			p->setBrush(AppTheme::color(AppTheme::ColorRole::Hover));
			p->drawRoundedRect(itemRect, 8, 8);
		}

		const auto iconKind = static_cast<AppTheme::Icon>(index.data(kNavIconKindRole).toInt());
		const auto role = selected ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextSecondary;
		const QIcon ic = AppTheme::icon(iconKind, role);
		const QRect iconRect(itemRect.left() + 10, itemRect.center().y() - 7, 15, 15);
		ic.paint(p, iconRect);

		QFont tf = option.font;
		tf.setPixelSize(13);
		tf.setBold(selected);
		p->setFont(tf);
		p->setPen(AppTheme::color(selected ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::Text));
		const QRect textRect(iconRect.right() + 10, itemRect.top(), itemRect.right() - iconRect.right() - 16, itemRect.height());
		p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());

		p->restore();
	}
};

QIcon buildThemeSwatchIcon(AppTheme::Theme theme, qreal dpr) {
	const int size = 18;
	const qreal scale = std::max<qreal>(1.0, dpr);
	QPixmap pix(static_cast<int>(size * scale), static_cast<int>(size * scale));
	pix.setDevicePixelRatio(scale);
	pix.fill(Qt::transparent);

	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r(0.5, 0.5, size - 1.0, size - 1.0);
	QPainterPath clip;
	clip.addRoundedRect(r, 5.0, 5.0);
	p.setClipPath(clip);

	if (theme == AppTheme::Theme::System) {
		p.fillRect(QRectF(0, 0, size / 2.0, size), QColor(QStringLiteral("#f5f7fa")));
		p.fillRect(QRectF(size / 2.0, 0, size / 2.0, size), QColor(QStringLiteral("#171a20")));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(QStringLiteral("#0969da")));
		p.drawEllipse(QPointF(size / 2.0, size / 2.0), 3.2, 3.2);
	} else {
		QColor bg(QStringLiteral("#f5f7fa"));
		QColor accent(QStringLiteral("#0969da"));
		if (theme == AppTheme::Theme::Dark) {
			bg = QColor(QStringLiteral("#171a20"));
			accent = QColor(QStringLiteral("#65b5ff"));
		} else if (theme == AppTheme::Theme::Teal) {
			bg = QColor(QStringLiteral("#eaf4f0"));
			accent = QColor(QStringLiteral("#087f65"));
		}
		p.fillRect(r, bg);
		p.setPen(Qt::NoPen);
		p.setBrush(accent);
		p.drawEllipse(QPointF(size / 2.0, size / 2.0), 3.8, 3.8);
	}
	p.setClipping(false);
	p.setPen(QPen(AppTheme::color(AppTheme::ColorRole::Border), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(r, 5.0, 5.0);
	return QIcon(pix);
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

void populateVideoCodecChoices(QComboBox *combo, bool capabilitiesReady,
		const livekit::VideoEncoderBackendCapability *hardware = nullptr,
		bool screenShare = false) {
	combo->clear();
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
	if (!hardware) {
		add(screenShare
				? QCoreApplication::translate("MeetingUI", "Auto (prefer hardware H.264 / VP9)")
				: QCoreApplication::translate("MeetingUI", "Auto (prefer hardware H.264)"),
			QStringLiteral("auto"), true);
	}
	if (!capabilitiesReady) return;
	if (hardware) {
		if (hardware->h264) add(QStringLiteral("H.264"), QStringLiteral("h264"), true);
		if (hardware->h265) add(QStringLiteral("H.265"), QStringLiteral("h265"), true);
		return;
	}
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

QString receiveResolutionMatchHelp(livekit::VideoResolutionMatch mode) {
	switch (mode) {
	case livekit::VideoResolutionMatch::Up:
		return QCoreApplication::translate("MeetingUI",
			"Selects the smallest available layer whose width and height both meet or exceed the display demand, reducing blur from enlarging a lower-resolution image.\n\n"
			"Example: with 180p, 360p and 720p layers, a display demand of about 500p selects 720p.\n\n"
			"Usually uses more bandwidth and decoding resources. If no layer is large enough, uses the highest layer within the allowed limits. The example assumes matching aspect ratios.");
	case livekit::VideoResolutionMatch::Down:
		return QCoreApplication::translate("MeetingUI",
			"Selects the largest available layer whose width and height both stay within the display demand, avoiding unnecessarily high resolution for a small view.\n\n"
			"Example: with 180p, 360p and 720p layers, a display demand of about 500p selects 360p.\n\n"
			"Usually saves bandwidth and decoding resources, but enlargement may look softer. If no layer is small enough, uses the lowest layer within the allowed limits. The example assumes matching aspect ratios.");
	case livekit::VideoResolutionMatch::Nearest:
		return QCoreApplication::translate("MeetingUI",
			"Compares each layer's relative width and height differences from the display demand and selects the closest layer. Equal distances prefer the lower resolution.\n\n"
			"Example: with 180p, 360p and 720p layers of the same aspect ratio:\n"
			"About 500p needed → selects 360p\nAbout 600p needed → selects 720p\n540p needed → equal distances, selects 360p\n\n"
			"Balances clarity and resource usage. The selected layer changes with the display size.");
	default:
		return QCoreApplication::translate("MeetingUI",
			"Chooses the matching direction according to the view's role.\n\n"
			"• Main views and detached screen-share windows match up to prioritize clarity.\n"
			"• Gallery tiles, sidebar thumbnails and picture-in-picture thumbnails match down to save bandwidth and decoding resources.\n\n"
			"This is the default behavior. Changing the layout or display size recalculates the receiving layer.");
	}
}

QString resolutionMatchToolTip(const QString &title, const QString &description) {
	auto body = description.toHtmlEscaped();
	body.replace(QStringLiteral("\n\n"), QStringLiteral("</p><p>"));
	body.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
	return QStringLiteral("<qt><b>%1</b><p>%2</p></qt>").arg(title.toHtmlEscaped(), body);
}

void syncResolutionMatchHelp(QComboBox *combo) {
	combo->setToolTip(combo->currentData(Qt::ToolTipRole).toString());
	combo->setAccessibleDescription(combo->currentData(Qt::AccessibleDescriptionRole).toString());
}

} // namespace

SettingsDialog::SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent,
		CameraDeviceDiscovery::Clock::time_point clickedAt)
	: QDialog(parent)
	, _session(session)
	, _clickedAt(clickedAt)
	, _paintTiming(std::make_shared<livekit::diagnostic::UiTimingMeasurement>(
		livekit::diagnostic::TimingMetric::UiFirstPaint, livekit::diagnostic::TimingScene::Settings, clickedAt)) {
	setObjectName(QStringLiteral("settingsDialog"));
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Settings"));
	setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
	setAttribute(Qt::WA_TranslucentBackground, true);
	setModal(true);
	resize(820 + kShadowMargin * 2, 650 + kShadowMargin * 2);
	setMinimumSize(700 + kShadowMargin * 2, 540 + kShadowMargin * 2);
	buildUi();
#ifdef Q_OS_LINUX
	const QPointer<QWidget> titleBar = findChild<QWidget *>(QStringLiteral("titleBar"));
	const QPointer<QWidget> sidebarTitleBar = findChild<QWidget *>(QStringLiteral("sidebarTitleBar"));
	LinuxWindowFrameOptions frameOptions;
	frameOptions.radius = kSurfaceRadius;
	frameOptions.captionHitTest = [this, titleBar, sidebarTitleBar](QPoint point) {
		const auto checkBar = [this, point](QWidget *bar) {
			if (!bar || !bar->isVisibleTo(this)) return false;
			const auto local = bar->mapFrom(this, point);
			if (!bar->rect().contains(local)) return false;
			for (auto *child = bar->childAt(local); child && child != bar;
					child = child->parentWidget()) {
				if (qobject_cast<QAbstractButton *>(child) || child->focusPolicy() != Qt::NoFocus) return false;
			}
			return true;
		};
		return checkBar(titleBar) || checkBar(sidebarTitleBar);
	};
	installLinuxWindowFrame(*this, std::move(frameOptions));
#endif
	syncThemeControls();
	refreshDialogThemeVisuals();
	AppTheme::observeChanges(*this, [this] {
		syncThemeControls();
		refreshDialogThemeVisuals();
	});
	connect(_themeCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this](int index) {
			if (index >= 0) AppTheme::setTheme(AppTheme::themeFromId(_themeCombo->itemData(index).toString()));
		});
	_languageCombo->setCurrentIndex(_languageCombo->findData(
		AppTranslation::languageId(AppTranslation::selectedLanguage())));
	connect(_languageCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this](int index) {
			if (index < 0) return;
			const auto next = AppTranslation::languageFromId(_languageCombo->itemData(index).toString());
			if (next == AppTranslation::selectedLanguage()) return;
			AppTranslation::setLanguage(next);
			emit languageChanged();
		});
	setPreferences(_session.mediaPreferences());
	syncSecurityControls();
	if (_copilotPage) _copilotPage->syncControls();
	connectPreferenceControls();
	connectDeviceControllers();
	applyVideoEncoderCapabilities(livekit::GetVideoEncoderCapabilities());
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	livekit::detail::RequestLinuxVideoEncoderCapabilityRefresh();
	{
#else
	if (!_videoEncoderCapabilitiesReady) {
#endif
		auto *capabilityTimer = new QTimer(this);
		capabilityTimer->setInterval(200);
		connect(capabilityTimer, &QTimer::timeout, this, [this, capabilityTimer] {
			if (_closed) { capabilityTimer->stop(); return; }
			const auto capabilities = livekit::GetVideoEncoderCapabilities();
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
			if (!capabilities.ready && !capabilities.probing)
				livekit::detail::RequestLinuxVideoEncoderCapabilityRefresh();
			if (_videoEncoderCapabilities && capabilities.revision <= _videoEncoderCapabilities->revision) return;
#else
			if (!capabilities.ready) return;
			capabilityTimer->stop();
#endif
			applyVideoEncoderCapabilities(capabilities);
		});
		capabilityTimer->start();
	}

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
		if (_copilotPage) _copilotPage->clearKeyEditors();
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
	if (_copilotPage) _copilotPage->shutdown();
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
	if (_copilotPage) _copilotPage->flushBeforeClose();
	if (_updateService) _updateService->cancel();
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
	if (event->type() == QEvent::Wheel) {
		auto *watchedWidget = qobject_cast<QWidget *>(watched);
		if (watchedWidget &&
			(qobject_cast<QComboBox *>(watchedWidget) ||
			 qobject_cast<QAbstractSpinBox *>(watchedWidget))) {
			// Scroll the enclosing page instead of changing a setting, including
			// focused combos and spin boxes. At an inner boundary, try the outer scroll area.
			const auto *wheel = static_cast<QWheelEvent *>(event);
			event->ignore();
			for (auto *parent = watchedWidget->parentWidget(); parent; parent = parent->parentWidget()) {
				if (auto *scroll = qobject_cast<QScrollArea *>(parent)) {
					auto *viewport = scroll->viewport();
					QWheelEvent forwarded(viewport->mapFromGlobal(wheel->globalPosition().toPoint()),
						wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
						wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source());
					QCoreApplication::sendEvent(viewport, &forwarded);
					if (forwarded.isAccepted()) {
						event->accept();
						break;
					}
				}
			}
			return true;
		}
	}
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
		if (_closed || !isVisible()) { _firstPaintScheduled = false; return; }
		_paintTiming->Finish(livekit::diagnostic::Outcome::Success);
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
	AppTheme::makeDialogAdaptive(*this, QSize(820 + kShadowMargin * 2, 650 + kShadowMargin * 2));
	auto *content = root->parentWidget();

	auto *layers = new QStackedLayout();
	layers->setContentsMargins(0, 0, 0, 0);
	layers->setStackingMode(QStackedLayout::StackAll);
	root->addLayout(layers);

#ifndef Q_OS_LINUX
	auto *shadowSurface = new QFrame(content);
	shadowSurface->setObjectName(QStringLiteral("settingsShadowSurface"));
	shadowSurface->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	auto *shadow = new QGraphicsDropShadowEffect(shadowSurface);
	shadow->setBlurRadius(30.0);
	shadow->setOffset(0.0, 4.0);
	shadow->setColor(QColor(31, 35, 41, 80));
	shadowSurface->setGraphicsEffect(shadow);
	layers->addWidget(shadowSurface);
#endif

	auto *surface = new RoundedSurface(content);
	surface->setObjectName(QStringLiteral("settingsSurface"));
#ifdef Q_OS_LINUX
	// The shared frame clips normal windows and removes its clipping when
	// maximized/fullscreen. Keep this underlying themed surface rectangular.
	surface->setProperty("linuxWindowFrameSurface", true);
#endif
	auto *surfaceLayout = new QHBoxLayout(surface);
	surfaceLayout->setContentsMargins(0, 0, 0, 0);
	surfaceLayout->setSpacing(0);
	layers->addWidget(surface);
	layers->setCurrentWidget(surface);

	// 1. 左侧分组导航栏（Plan 2：标题 + 快捷搜索 + 偏好/高级分组胶囊导航）
	auto *sidebar = new QFrame(surface);
	sidebar->setObjectName(QStringLiteral("settingsSidebar"));
	sidebar->setFixedWidth(206);
	auto *sidebarLayout = new QVBoxLayout(sidebar);
	sidebarLayout->setContentsMargins(0, 0, 0, 10);
	sidebarLayout->setSpacing(0);

	auto *sidebarTitleBar = new QWidget(sidebar);
	sidebarTitleBar->setObjectName(QStringLiteral("sidebarTitleBar"));
	sidebarTitleBar->setFixedHeight(kTitleBarHeight);
	auto *sidebarTitleLayout = new QHBoxLayout(sidebarTitleBar);
	sidebarTitleLayout->setContentsMargins(16, 0, 14, 0);
	sidebarTitleLayout->setSpacing(8);
	auto *sidebarTitle = new QLabel(QCoreApplication::translate("MeetingUI", "Settings"), sidebarTitleBar);
	sidebarTitle->setObjectName(QStringLiteral("settingsSidebarTitle"));
	sidebarTitleLayout->addWidget(sidebarTitle);
	sidebarTitleLayout->addStretch(1);
	sidebarLayout->addWidget(sidebarTitleBar);

	auto *searchWrap = new QWidget(sidebar);
	searchWrap->setObjectName(QStringLiteral("settingsSearchWrap"));
	auto *searchLayout = new QVBoxLayout(searchWrap);
	searchLayout->setContentsMargins(12, 4, 12, 8);
	searchLayout->setSpacing(0);
	_searchEdit = new QLineEdit(searchWrap);
	_searchEdit->setObjectName(QStringLiteral("settingsSearchInput"));
	_searchEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search settings..."));
	_searchEdit->setClearButtonEnabled(true);
	_searchEdit->setFixedHeight(32);
	searchLayout->addWidget(_searchEdit);
	sidebarLayout->addWidget(searchWrap);

	_navigation = new QListWidget(sidebar);
	_navigation->viewport()->installEventFilter(this);
	_navigation->setObjectName(QStringLiteral("settingsNavigation"));
	_navigation->setAccessibleName(QCoreApplication::translate("MeetingUI", "Settings sections"));
	_navigation->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	_navigation->setWordWrap(false);
	_navigation->setTextElideMode(Qt::ElideRight);
	_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	_navigation->setSelectionMode(QAbstractItemView::SingleSelection);
	_navigation->setMouseTracking(true);
	_navigation->setSpacing(2);
	_navigation->setItemDelegate(new SettingsNavDelegate(_navigation));

	const auto addNav = [this](AppTheme::Icon iconKind, const QString &label, const QString &groupHeader = QString()) {
		auto *item = new QListWidgetItem(label);
		item->setData(kNavIconKindRole, static_cast<int>(iconKind));
		if (!groupHeader.isEmpty()) {
			item->setData(kNavGroupHeaderRole, groupHeader);
		}
		_navigation->addItem(item);
	};
	addNav(AppTheme::Icon::Settings, QCoreApplication::translate("MeetingUI", "General"),
		QCoreApplication::translate("MeetingUI", "Preferences"));
	addNav(AppTheme::Icon::Video, QCoreApplication::translate("MeetingUI", "Video"));
	addNav(AppTheme::Icon::Microphone, QCoreApplication::translate("MeetingUI", "Audio"));
	addNav(AppTheme::Icon::Shield, QCoreApplication::translate("MeetingUI", "Security"),
		QCoreApplication::translate("MeetingUI", "Advanced & Integrations"));
	addNav(AppTheme::Icon::Sparkle, QCoreApplication::translate("MeetingUI", "Copilot"));
	addNav(AppTheme::Icon::Information, QCoreApplication::translate("MeetingUI", "About"));
	sidebarLayout->addWidget(_navigation, 1);
	surfaceLayout->addWidget(sidebar);

	// 2. 右侧主内容区（柔和 Window 底色承载分组圆角卡片）
	auto *mainPane = new QWidget(surface);
	mainPane->setObjectName(QStringLiteral("settingsMainPane"));
	auto *mainPaneLayout = new QVBoxLayout(mainPane);
	mainPaneLayout->setContentsMargins(0, 0, 0, 0);
	mainPaneLayout->setSpacing(0);

	auto *titleBar = new QWidget(mainPane);
	titleBar->setObjectName(QStringLiteral("titleBar"));
	titleBar->setFixedHeight(kTitleBarHeight);
	auto *titleLayout = new QHBoxLayout(titleBar);
	titleLayout->setContentsMargins(20, 0, 12, 0);
	_pageHeaderLabel = new QLabel(QCoreApplication::translate("MeetingUI", "General"), titleBar);
	_pageHeaderLabel->setObjectName(QStringLiteral("dialogTitle"));
	titleLayout->addWidget(_pageHeaderLabel);
	titleLayout->addStretch(1);
	_closeButton = new QPushButton(titleBar);
	_closeButton->setObjectName(QStringLiteral("closeButton"));
	_closeButton->setIcon(AppTheme::icon(AppTheme::Icon::Close, AppTheme::ColorRole::TextSecondary));
	_closeButton->setToolTip(QCoreApplication::translate("MeetingUI", "Close"));
	_closeButton->setAccessibleName(_closeButton->toolTip());
	_closeButton->setCursor(Qt::PointingHandCursor);
	_closeButton->setFixedSize(28, 28);
	titleLayout->addWidget(_closeButton);
	connect(_closeButton, &QPushButton::clicked, this, &QDialog::accept);
	mainPaneLayout->addWidget(titleBar);

	_pages = new QStackedWidget(mainPane);
	_pages->setObjectName(QStringLiteral("settingsPages"));
	_pages->addWidget(buildGeneralPage());
	_pages->addWidget(buildVideoPage());
	_pages->addWidget(buildAudioPage());
	_pages->addWidget(buildSecurityPage());
	_pages->addWidget(buildCopilotPage());
	_pages->addWidget(buildAboutPage());
	mainPaneLayout->addWidget(_pages, 1);
	surfaceLayout->addWidget(mainPane, 1);

	connect(_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
		filterNavigation(text);
	});
	connect(_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row < 0 || row >= _pages->count()) {
			return;
		}
		const auto wasVideo = _pages->currentIndex() == static_cast<int>(Page::Video);
		_pages->setCurrentIndex(row);
		syncNavigationHeader();
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
		if (row == static_cast<int>(Page::Copilot) && _copilotPage) {
			_copilotPage->maybeAutoRefreshModels();
		}
	});
	connect(_navigation, &QListWidget::itemSelectionChanged, this, [this] {
		const auto selected = _navigation->selectedItems();
		if (!selected.isEmpty() && _navigation->currentItem() != selected.front()) {
			_navigation->setCurrentItem(selected.front());
		}
	});
	_navigation->setCurrentRow(static_cast<int>(Page::General));
	syncNavigationHeader();
	for (auto *combo : findChildren<QComboBox *>()) {
		combo->installEventFilter(this);
	}
	for (auto *spin : findChildren<QAbstractSpinBox *>()) {
		spin->installEventFilter(this);
	}
	for (auto *label : findChildren<QLabel *>()) {
		if (label->maximumHeight() == QWIDGETSIZE_MAX && !label->property("keepSingleLine").toBool()) {
			label->setWordWrap(true);
		}
	}
}

void SettingsDialog::syncNavigationHeader() {
	if (!_pageHeaderLabel || !_navigation) {
		return;
	}
	if (auto *item = _navigation->currentItem()) {
		_pageHeaderLabel->setText(item->text());
	}
}

void SettingsDialog::filterNavigation(const QString &query) {
	if (!_navigation || !_pages) {
		return;
	}
	const QString needle = query.trimmed().toLower();
	int firstVisibleRow = -1;
	const auto refreshGroupHeaders = [this] {
		bool seenPref = false;
		bool seenAdv = false;
		for (int i = 0; i < _navigation->count(); ++i) {
			auto *item = _navigation->item(i);
			if (!item) continue;
			if (_navigation->isRowHidden(i)) {
				item->setData(kNavGroupHeaderRole, QString());
				continue;
			}
			if (i < 3) {
				item->setData(kNavGroupHeaderRole, seenPref ? QString()
					: QCoreApplication::translate("MeetingUI", "Preferences"));
				seenPref = true;
			} else {
				item->setData(kNavGroupHeaderRole, seenAdv ? QString()
					: QCoreApplication::translate("MeetingUI", "Advanced & Integrations"));
				seenAdv = true;
			}
		}
	};

	for (int i = 0; i < _navigation->count() && i < _pages->count(); ++i) {
		auto *item = _navigation->item(i);
		bool matched = needle.isEmpty();
		if (!matched && item && item->text().toLower().contains(needle)) {
			matched = true;
		}
		if (!matched) {
			QWidget *page = _pages->widget(i);
			if (page) {
				for (const auto *lbl : page->findChildren<QLabel *>()) {
					if (lbl->text().toLower().contains(needle)) { matched = true; break; }
				}
				if (!matched) {
					for (const auto *chk : page->findChildren<QAbstractButton *>()) {
						if (chk->text().toLower().contains(needle)) { matched = true; break; }
					}
				}
			}
		}
		_navigation->setRowHidden(i, !matched);
		if (matched && firstVisibleRow < 0) {
			firstVisibleRow = i;
		}
	}
	refreshGroupHeaders();
	_navigation->doItemsLayout();
	if (firstVisibleRow >= 0 && _navigation->isRowHidden(_navigation->currentRow())) {
		_navigation->setCurrentRow(firstVisibleRow);
	}
	if (!needle.isEmpty() && _pages->currentIndex() == static_cast<int>(Page::Copilot) && _copilotPage) {
		_copilotPage->focusSearchMatch(needle);
	}
}

void SettingsDialog::refreshDialogThemeVisuals() {
	if (_closeButton) {
		_closeButton->setIcon(AppTheme::icon(AppTheme::Icon::Close, AppTheme::ColorRole::TextSecondary));
	}
	if (_copilotPage) {
		_copilotPage->refreshThemeVisuals();
	}
	const std::array<AppTheme::Theme, 4> themes = {
		AppTheme::Theme::System, AppTheme::Theme::Light, AppTheme::Theme::Dark, AppTheme::Theme::Teal
	};
	const qreal dpr = devicePixelRatioF();
	for (size_t i = 0; i < _themeSwatchButtons.size(); ++i) {
		if (auto *btn = _themeSwatchButtons[i]) {
			btn->setIcon(buildThemeSwatchIcon(themes[i], dpr));
		}
	}
	if (_navigation && _navigation->viewport()) {
		_navigation->viewport()->update();
	}
	update();
}

QWidget *SettingsDialog::buildGeneralPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());

	// 卡片 1：外观与语言（内置可视化主题色卡 + 同步下拉 + 语言）
	_themeCombo = createAccessibleComboBox(content);
	_themeCombo->setObjectName(QStringLiteral("settingsThemeCombo"));
	_themeCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Theme"));
	_themeCombo->addItem(QCoreApplication::translate("MeetingUI", "System"), AppTheme::themeId(AppTheme::Theme::System));
	_themeCombo->addItem(QCoreApplication::translate("MeetingUI", "Light"), AppTheme::themeId(AppTheme::Theme::Light));
	_themeCombo->addItem(QCoreApplication::translate("MeetingUI", "Dark"), AppTheme::themeId(AppTheme::Theme::Dark));
	_themeCombo->addItem(QCoreApplication::translate("MeetingUI", "Teal"), AppTheme::themeId(AppTheme::Theme::Teal));
	_themeCombo->setMinimumWidth(132);
	AppTheme::styleChoiceControls(*_themeCombo, AppTheme::Tone::Light);

	const auto immediateChangeHint = QCoreApplication::translate(
		"MeetingUI", "Changes apply immediately and are saved for the next launch.");
	auto *appearanceHeaderRight = new QWidget(content);
	auto *appearanceHeaderRightLayout = new QHBoxLayout(appearanceHeaderRight);
	appearanceHeaderRightLayout->setContentsMargins(0, 0, 0, 0);
	appearanceHeaderRightLayout->setSpacing(6);
	auto *themeHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "Theme"),
		immediateChangeHint,
		appearanceHeaderRight,
		15);
	themeHintIcon->setObjectName(QStringLiteral("settingsThemeHintIcon"));
	appearanceHeaderRightLayout->addWidget(themeHintIcon, 0, Qt::AlignVCenter);
	appearanceHeaderRightLayout->addWidget(_themeCombo, 0, Qt::AlignVCenter);

	QVBoxLayout *appearanceBody = nullptr;
	auto *appearanceCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Appearance & Language"),
		content, &appearanceBody, appearanceHeaderRight);

	auto *swatchRow = new QHBoxLayout();
	swatchRow->setContentsMargins(0, 0, 0, 0);
	swatchRow->setSpacing(8);
	const std::array<std::pair<AppTheme::Theme, QString>, 4> swatchSpecs = {{
		{ AppTheme::Theme::System, QCoreApplication::translate("MeetingUI", "System") },
		{ AppTheme::Theme::Light, QCoreApplication::translate("MeetingUI", "Light") },
		{ AppTheme::Theme::Dark, QCoreApplication::translate("MeetingUI", "Dark") },
		{ AppTheme::Theme::Teal, QCoreApplication::translate("MeetingUI", "Teal") },
	}};
	for (size_t i = 0; i < swatchSpecs.size(); ++i) {
		auto *btn = new QPushButton(swatchSpecs[i].second, appearanceCard);
		btn->setObjectName(QStringLiteral("settingsThemeSwatch"));
		btn->setCheckable(true);
		btn->setAutoDefault(false);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setFixedHeight(36);
		btn->setIconSize(QSize(18, 18));
		btn->setIcon(buildThemeSwatchIcon(swatchSpecs[i].first, devicePixelRatioF()));
		const auto targetTheme = swatchSpecs[i].first;
		connect(btn, &QPushButton::clicked, this, [targetTheme] {
			AppTheme::setTheme(targetTheme);
		});
		_themeSwatchButtons[i] = btn;
		swatchRow->addWidget(btn, 1);
	}
	appearanceBody->addLayout(swatchRow);

	auto *langDivider = new QFrame(appearanceCard);
	langDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	langDivider->setFixedHeight(1);
	appearanceBody->addWidget(langDivider);

	auto *languageLabel = makeSectionTitle(QCoreApplication::translate("MeetingUI", "Language"), appearanceCard);
	auto *languageHintIcon = makeHintIcon(languageLabel->text(), immediateChangeHint, appearanceCard);
	languageHintIcon->setObjectName(QStringLiteral("settingsLanguageHintIcon"));
	appearanceBody->addLayout(makeTitleWithHintRow(languageLabel, languageHintIcon));
	_languageCombo = createAccessibleComboBox(appearanceCard);
	_languageCombo->setObjectName(QStringLiteral("settingsLanguageCombo"));
	_languageCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Language"));
	_languageCombo->addItem(QCoreApplication::translate("MeetingUI", "Follow system"),
		AppTranslation::languageId(AppTranslation::Language::System));
	_languageCombo->addItem(QCoreApplication::translate("MeetingUI", "Simplified Chinese"),
		AppTranslation::languageId(AppTranslation::Language::Chinese));
	_languageCombo->addItem(QCoreApplication::translate("MeetingUI", "English"),
		AppTranslation::languageId(AppTranslation::Language::English));
	AppTheme::styleChoiceControls(*_languageCombo, AppTheme::Tone::Light);
	languageLabel->setBuddy(_languageCombo);
	appearanceBody->addWidget(_languageCombo);
	layout->addWidget(appearanceCard);

	// 卡片 2：入会默认偏好
	QVBoxLayout *joinBody = nullptr;
	auto *joinCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Default Join Preferences"), content, &joinBody);
	_generalCamera = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), joinCard);
	_generalCamera->setObjectName(QStringLiteral("settingsJoinCamera"));
	_generalMicrophone = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), joinCard);
	_generalMicrophone->setObjectName(QStringLiteral("settingsJoinMicrophone"));
	_generalSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Use computer audio when joining"), joinCard);
	joinBody->addWidget(_generalMicrophone);
	joinBody->addWidget(_generalCamera);
	joinBody->addWidget(_generalSpeaker);
	layout->addWidget(joinCard);

	// 卡片 3：会中显示与窗口行为
	QVBoxLayout *meetingBody = nullptr;
	auto *meetingCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Meeting Display & Window"), content, &meetingBody);
	auto *nameDisplayLabel = makeSectionTitle(
		QCoreApplication::translate("MeetingUI", "Participant names in meetings"), meetingCard);
	const auto nameDisplayHintText = QCoreApplication::translate("MeetingUI",
		"Auto shows names when the participant window's width × height is greater than 250000; otherwise, names appear on hover.");
	auto *nameDisplayHintIcon = makeHintIcon(nameDisplayLabel->text(), nameDisplayHintText, meetingCard);
	nameDisplayHintIcon->setObjectName(QStringLiteral("settingsParticipantNameDisplayHintIcon"));
	meetingBody->addLayout(makeTitleWithHintRow(nameDisplayLabel, nameDisplayHintIcon));
	_participantNameDisplayCombo = createAccessibleComboBox(meetingCard);
	_participantNameDisplayCombo->setObjectName(QStringLiteral("settingsParticipantNameDisplay"));
	_participantNameDisplayCombo->setAccessibleName(nameDisplayLabel->text());
	_participantNameDisplayCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto (default)"),
		static_cast<int>(OpenMeeting::ParticipantNameDisplayMode::Auto));
	_participantNameDisplayCombo->addItem(QCoreApplication::translate("MeetingUI", "Always show"),
		static_cast<int>(OpenMeeting::ParticipantNameDisplayMode::Always));
	_participantNameDisplayCombo->addItem(QCoreApplication::translate("MeetingUI", "Show on hover"),
		static_cast<int>(OpenMeeting::ParticipantNameDisplayMode::OnHover));
	AppTheme::styleChoiceControls(*_participantNameDisplayCombo, AppTheme::Tone::Light);
	nameDisplayLabel->setBuddy(_participantNameDisplayCombo);
	_participantNameDisplayCombo->setAccessibleDescription(nameDisplayHintText);
	meetingBody->addWidget(_participantNameDisplayCombo);

	auto *behaviorDivider = new QFrame(meetingCard);
	behaviorDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	behaviorDivider->setFixedHeight(1);
	meetingBody->addWidget(behaviorDivider);

	_showActiveSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Show the active speaker"), meetingCard);
	_showActiveSpeaker->setObjectName(QStringLiteral("settingsShowActiveSpeaker"));
	_showLiveCaptions = new QCheckBox(QCoreApplication::translate("MeetingUI", "Show live captions"), meetingCard);
	_showLiveCaptions->setObjectName(QStringLiteral("settingsShowLiveCaptions"));
	_stayWhenLocked = new QCheckBox(QCoreApplication::translate("MeetingUI", "Stay in the meeting when the screen is locked"), meetingCard);
	_stayWhenLocked->setObjectName(QStringLiteral("settingsStayWhenLocked"));
	_quitOnClose = new QCheckBox(QCoreApplication::translate("MeetingUI", "Quit when the main window is closed"), meetingCard);
	_quitOnClose->setObjectName(QStringLiteral("settingsQuitOnClose"));
	_continuousStatsSampling = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Continuously collect WebRTC stats every second"),
		meetingCard);
	_continuousStatsSampling->setObjectName(QStringLiteral("settingsContinuousStatsSampling"));
	const auto continuousStatsHintText = QCoreApplication::translate("MeetingUI",
		"Disabled by default to reduce periodic stats polling and track parsing overhead. Enable to populate real-time network and media stats in the telemetry panel.");
	auto *continuousStatsHintIcon = makeHintIcon(
		_continuousStatsSampling->text(), continuousStatsHintText, meetingCard);
	continuousStatsHintIcon->setObjectName(QStringLiteral("settingsContinuousStatsSamplingHintIcon"));
	_continuousStatsSampling->setAccessibleDescription(continuousStatsHintText);
	meetingBody->addWidget(_showActiveSpeaker);
	meetingBody->addWidget(_showLiveCaptions);
	meetingBody->addWidget(_stayWhenLocked);
	meetingBody->addWidget(_quitOnClose);
	meetingBody->addLayout(makeTitleWithHintRow(_continuousStatsSampling, continuousStatsHintIcon));
	layout->addWidget(meetingCard);

	layout->addStretch();
	return scroll;
}

void SettingsDialog::syncThemeControls() {
	const auto selected = AppTheme::selectedTheme();
	if (_themeCombo) {
		const QSignalBlocker blocker(_themeCombo);
		_themeCombo->setCurrentIndex(_themeCombo->findData(AppTheme::themeId(selected)));
	}
	const std::array<AppTheme::Theme, 4> themes = {
		AppTheme::Theme::System, AppTheme::Theme::Light, AppTheme::Theme::Dark, AppTheme::Theme::Teal
	};
	for (size_t i = 0; i < _themeSwatchButtons.size(); ++i) {
		if (auto *btn = _themeSwatchButtons[i]) {
			const QSignalBlocker b(btn);
			btn->setChecked(selected == themes[i]);
			repolishSettingsWidget(btn);
		}
	}
}

QWidget *SettingsDialog::buildVideoPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());

	// 卡片 1：摄像头预览与采集设备
	QVBoxLayout *cameraBody = nullptr;
	auto *cameraCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Camera & Preview"), content, &cameraBody);

	_previewHost = new QFrame(cameraCard);
	_previewHost->setObjectName(QStringLiteral("previewHost"));
	_previewHost->setMinimumHeight(220);
	_previewHost->setMaximumHeight(280);
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
	cameraBody->addWidget(_previewHost);

	cameraBody->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Camera"), cameraCard));
	_cameraCombo = new QComboBox(cameraCard);
	_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_cameraCombo->setMinimumContentsLength(28);
	cameraBody->addWidget(_cameraCombo);

	_videoCamera = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), cameraCard);
	cameraBody->addWidget(_videoCamera);

	_highDefinition = new QCheckBox(QCoreApplication::translate("MeetingUI", "HD Camera Quality"), cameraCard);
	const auto resolutionHintText = QCoreApplication::translate(
		"MeetingUI", "Selects the device mode closest to 1920 × 1080 by default. Devices below 1080p use their highest available resolution.");
	auto *resolutionHintIcon = makeHintIcon(_highDefinition->text(), resolutionHintText, cameraCard);
	resolutionHintIcon->setObjectName(QStringLiteral("settingsResolutionHintIcon"));
	cameraBody->addLayout(makeTitleWithHintRow(_highDefinition, resolutionHintIcon));
	_resolutionCombo = new QComboBox(cameraCard);
	_resolutionCombo->setMinimumWidth(280);
	_resolutionCombo->setAccessibleDescription(resolutionHintText);
	cameraBody->addWidget(_resolutionCombo);
	layout->addWidget(cameraCard);

	// 卡片 2：发布编码与共享屏幕
	QVBoxLayout *codecBody = nullptr;
	auto *codecCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Publishing codec"), content, &codecBody);
	const auto encoderLabel = QCoreApplication::translate("MeetingUI", "Video encoder");
	auto *encoderTitle = new QLabel(encoderLabel, codecCard);
	const auto encoderHintText = QCoreApplication::translate("MeetingUI",
		"Applies to camera and screen sharing when a new encoder is created. Auto prefers available hardware. Software fallback for the same codec depends on the selected encoder and codec; unsupported combinations may fail to start.");
	_videoEncoderHintIcon = makeHintIcon(encoderLabel, encoderHintText, codecCard);
	_videoEncoderHintIcon->setObjectName(QStringLiteral("videoEncoderPreferenceHint"));
	codecBody->addLayout(makeTitleWithHintRow(encoderTitle, _videoEncoderHintIcon));
	_videoEncoderCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_videoEncoderCombo);
	_videoEncoderCombo->setObjectName(QStringLiteral("videoEncoderPreference"));
	_videoEncoderCombo->setAccessibleName(encoderLabel);
	encoderTitle->setBuddy(_videoEncoderCombo);
	_videoEncoderCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto (prefer hardware)"), QStringLiteral("auto"));
	_videoEncoderCombo->addItem(QCoreApplication::translate("MeetingUI", "Software"), QStringLiteral("software"));
	_videoEncoderCombo->setToolTip(encoderHintText);
	_videoEncoderCombo->setAccessibleDescription(encoderHintText);
	codecBody->addWidget(_videoEncoderCombo);
	codecBody->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "Camera"), codecCard));
	_cameraCodecCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_cameraCodecCombo);
	_cameraCodecCombo->setObjectName(QStringLiteral("cameraVideoCodec"));
	_cameraCodecCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Camera codec"));
	populateVideoCodecChoices(_cameraCodecCombo, false);
	codecBody->addWidget(_cameraCodecCombo);
	codecBody->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "Screen share"), codecCard));
	_screenShareCodecCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_screenShareCodecCombo);
	_screenShareCodecCombo->setObjectName(QStringLiteral("screenShareVideoCodec"));
	_screenShareCodecCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Screen share codec"));
	populateVideoCodecChoices(_screenShareCodecCombo, false, nullptr, true);
	codecBody->addWidget(_screenShareCodecCombo);
	codecBody->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Screen share resolution"), codecCard));
	_screenShareResolutionCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_screenShareResolutionCombo);
	_screenShareResolutionCombo->setObjectName(QStringLiteral("screenShareResolution"));
	_screenShareResolutionCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Screen share resolution"));
	populateScreenShareResolutionChoices(*_screenShareResolutionCombo, screenShareDisplayPixelSize(screen()));
	codecBody->addWidget(_screenShareResolutionCombo);
	_screenShareResolutionHint = new QLabel(codecCard);
	_screenShareResolutionHint->setObjectName(QStringLiteral("screenShareResolutionHint"));
	_screenShareResolutionHint->setWordWrap(true);
	_screenShareResolutionHint->setVisible(false);
	codecBody->addWidget(_screenShareResolutionHint);
	codecBody->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Screen share frame rate"), codecCard));
	_screenShareFpsCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_screenShareFpsCombo);
	_screenShareFpsCombo->setObjectName(QStringLiteral("screenShareFps"));
	for (int fps : {15, 20, 30}) _screenShareFpsCombo->addItem(QString::number(fps) + " FPS", fps);
	codecBody->addWidget(_screenShareFpsCombo);
	layout->addWidget(codecCard);

	// 卡片 3：接收质量与镜像
	QVBoxLayout *receiveBody = nullptr;
	auto *receiveCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Receive quality & Mirror"), content, &receiveBody);
	const auto matchLabel = QCoreApplication::translate("MeetingUI", "Simulcast resolution matching");
	auto *matchTitle = new QLabel(matchLabel, receiveCard);
	matchTitle->setObjectName(QStringLiteral("receiveResolutionMatchLabel"));
	matchTitle->setAccessibleName(matchLabel);
	const auto generalHelp = QCoreApplication::translate("MeetingUI",
		"Selects a receiving layer from the resolutions published by the other participant, using the actual display size and screen scaling.\n\n"
		"Applies to remote cameras and screen shares, including detached screen-share windows. Changes are saved and applied to the current meeting immediately; local publishing resolution is unchanged.\n\n"
		"All strategies follow the existing resolution limits. A single valid layer is used as-is. Without layer information, the display dimensions are requested and the server determines the received layer.\n\n"
		"Actual receiving quality also depends on published layers, network bandwidth and server scheduling.");
	const auto matchSummaryHelp = QCoreApplication::translate("MeetingUI",
		"Auto matches up for the main view and down for other views. Applies immediately to received camera and screen-share video. Actual quality depends on published layers and network conditions.");
	const auto combinedMatchHelp = matchSummaryHelp + QStringLiteral("\n\n") + generalHelp;
	auto *matchHintIcon = makeHintIcon(matchLabel, combinedMatchHelp, receiveCard);
	matchHintIcon->setObjectName(QStringLiteral("receiveResolutionMatchHintIcon"));
	matchTitle->setAccessibleDescription(combinedMatchHelp);
	receiveBody->addLayout(makeTitleWithHintRow(matchTitle, matchHintIcon));
	_receiveResolutionMatchCombo = new QComboBox(receiveCard);
	configureAccessibleComboBox(_receiveResolutionMatchCombo);
	_receiveResolutionMatchCombo->setObjectName(QStringLiteral("receiveResolutionMatch"));
	_receiveResolutionMatchCombo->setAccessibleName(matchLabel);
	_receiveResolutionMatchCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto (follow layout)"), QStringLiteral("auto"));
	_receiveResolutionMatchCombo->addItem(QCoreApplication::translate("MeetingUI", "Match up (prefer clarity)"), QStringLiteral("up"));
	_receiveResolutionMatchCombo->addItem(QCoreApplication::translate("MeetingUI", "Match down (save bandwidth)"), QStringLiteral("down"));
	_receiveResolutionMatchCombo->addItem(QCoreApplication::translate("MeetingUI", "Nearest resolution"), QStringLiteral("nearest"));
	matchTitle->setBuddy(_receiveResolutionMatchCombo);
	_receiveResolutionMatchCombo->setToolTipDuration(30000);
	for (int index = 0; index != _receiveResolutionMatchCombo->count(); ++index) {
		const auto mode = livekit::ParseVideoResolutionMatch(
			_receiveResolutionMatchCombo->itemData(index).toString().toStdString());
		const auto description = receiveResolutionMatchHelp(mode);
		_receiveResolutionMatchCombo->setItemData(index,
			resolutionMatchToolTip(_receiveResolutionMatchCombo->itemText(index), description), Qt::ToolTipRole);
		_receiveResolutionMatchCombo->setItemData(index, description, Qt::AccessibleDescriptionRole);
	}
	syncResolutionMatchHelp(_receiveResolutionMatchCombo);
	connect(_receiveResolutionMatchCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
		[this](int) { syncResolutionMatchHelp(_receiveResolutionMatchCombo); });
	receiveBody->addWidget(_receiveResolutionMatchCombo);

	auto *mirrorDivider = new QFrame(receiveCard);
	mirrorDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	mirrorDivider->setFixedHeight(1);
	receiveBody->addWidget(mirrorDivider);

	_mirrorEnabled = new QCheckBox(QCoreApplication::translate("MeetingUI", "Mirror Video"), receiveCard);
	receiveBody->addWidget(_mirrorEnabled);
	_localMirror = new QRadioButton(QCoreApplication::translate("MeetingUI", "Mirror only my preview"), receiveCard);
	_remoteMirror = new QRadioButton(QCoreApplication::translate("MeetingUI", "Mirror for all participants"), receiveCard);
	auto *mirrorGroup = new QButtonGroup(receiveCard);
	mirrorGroup->setExclusive(true);
	mirrorGroup->addButton(_localMirror);
	mirrorGroup->addButton(_remoteMirror);
	auto *localRow = new QHBoxLayout();
	localRow->setContentsMargins(24, 0, 0, 0);
	localRow->addWidget(_localMirror);
	localRow->addStretch();
	receiveBody->addLayout(localRow);
	auto *remoteRow = new QHBoxLayout();
	remoteRow->setContentsMargins(24, 0, 0, 0);
	remoteRow->addWidget(_remoteMirror);
	remoteRow->addStretch();
	receiveBody->addLayout(remoteRow);
	layout->addWidget(receiveCard);

	layout->addStretch();
	return scroll;
}

QWidget *SettingsDialog::buildAudioPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());

	// 卡片 1：扬声器与麦克风设备
	QVBoxLayout *deviceBody = nullptr;
	auto *deviceCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Input & Output Devices"), content, &deviceBody);

	deviceBody->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Speaker"), deviceCard));
	auto *speakerRow = new QHBoxLayout();
	speakerRow->setSpacing(8);
	_speakerCombo = new QComboBox(deviceCard);
	_speakerCombo->setObjectName(QStringLiteral("speakerDeviceCombo"));
	_speakerCombo->setMinimumContentsLength(24);
	_speakerTestButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Speaker"), deviceCard);
	_speakerTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_speakerTestButton->setMinimumWidth(108);
	speakerRow->addWidget(_speakerCombo, 1);
	speakerRow->addWidget(_speakerTestButton);
	deviceBody->addLayout(speakerRow);

	auto *devDivider = new QFrame(deviceCard);
	devDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	devDivider->setFixedHeight(1);
	deviceBody->addWidget(devDivider);

	deviceBody->addWidget(makeSectionTitle(QCoreApplication::translate("MeetingUI", "Microphone"), deviceCard));
	auto *microphoneRow = new QHBoxLayout();
	microphoneRow->setSpacing(8);
	_microphoneCombo = new QComboBox(deviceCard);
	_microphoneCombo->setObjectName(QStringLiteral("microphoneDeviceCombo"));
	_microphoneCombo->setMinimumContentsLength(24);
	_microphoneTestButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Microphone"), deviceCard);
	_microphoneTestButton->setObjectName(QStringLiteral("secondaryButton"));
	_microphoneTestButton->setCheckable(true);
	_microphoneTestButton->setMinimumWidth(108);
	microphoneRow->addWidget(_microphoneCombo, 1);
	microphoneRow->addWidget(_microphoneTestButton);
	deviceBody->addLayout(microphoneRow);
	_microphoneLevel = new QProgressBar(deviceCard);
	_microphoneLevel->setRange(0, 100);
	_microphoneLevel->setValue(0);
	_microphoneLevel->setTextVisible(false);
	_microphoneLevel->setAccessibleName(QCoreApplication::translate("MeetingUI", "Microphone Input Level"));
	deviceBody->addWidget(_microphoneLevel);
	_audioStatus = new QLabel(deviceCard);
	_audioStatus->setObjectName(QStringLiteral("hintLabel"));
	_audioStatus->setWordWrap(true);
	_audioStatus->hide();
	deviceBody->addWidget(_audioStatus);
	layout->addWidget(deviceCard);

	// 卡片 2：入会音频行为
	QVBoxLayout *joinAudioBody = nullptr;
	auto *joinAudioCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Audio Behavior"), content, &joinAudioBody);
	_audioMicrophone = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), joinAudioCard);
	_audioSpeaker = new QCheckBox(QCoreApplication::translate("MeetingUI", "Use computer audio when joining"), joinAudioCard);
	_pushToTalk = new QCheckBox(QCoreApplication::translate("MeetingUI", "Hold Space to temporarily unmute"), joinAudioCard);
	joinAudioBody->addWidget(_audioMicrophone);
	joinAudioBody->addWidget(_audioSpeaker);
	joinAudioBody->addWidget(_pushToTalk);
	layout->addWidget(joinAudioCard);

	// 卡片 3：降噪与音频增强
	QVBoxLayout *enhanceBody = nullptr;
	QWidget *enhanceHeaderHint = nullptr;
#ifndef Q_OS_WIN
	auto *processingHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "Noise Reduction and Audio Enhancement"),
		QCoreApplication::translate("MeetingUI",
			"Audio processing changes take effect the next time you join a meeting."),
		content,
		15);
	processingHintIcon->setObjectName(QStringLiteral("settingsAudioProcessingHintIcon"));
	enhanceHeaderHint = processingHintIcon;
#endif
	auto *enhanceCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Noise Reduction and Audio Enhancement"),
		content, &enhanceBody, enhanceHeaderHint);
	_echoCancellation = new QCheckBox(QCoreApplication::translate("MeetingUI", "Echo cancellation (AEC)"), enhanceCard);
	_echoCancellation->setObjectName(QStringLiteral("echoCancellationCheckBox"));
	_noiseSuppression = new QCheckBox(QCoreApplication::translate("MeetingUI", "Noise suppression (ANS)"), enhanceCard);
	_noiseSuppression->setObjectName(QStringLiteral("noiseSuppressionCheckBox"));
	_autoGainControl = new QCheckBox(QCoreApplication::translate("MeetingUI", "Automatic gain control (AGC)"), enhanceCard);
	_autoGainControl->setObjectName(QStringLiteral("autoGainControlCheckBox"));
	enhanceBody->addWidget(_echoCancellation);
	enhanceBody->addWidget(_noiseSuppression);
	enhanceBody->addWidget(_autoGainControl);
#ifndef Q_OS_WIN
	const auto testHint = QCoreApplication::translate("MeetingUI",
		"Audio device testing is not available on this platform yet.");
	_microphoneTestButton->setToolTip(testHint);
	_speakerTestButton->setToolTip(testHint);
	_microphoneLevel->hide();
#endif
	layout->addWidget(enhanceCard);
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
				if (device.id.isEmpty()) continue; // System Default is already listed above.
				auto name = device.name.trimmed().isEmpty()
					? QCoreApplication::translate("MeetingUI", "Unnamed Device")
					: device.name;
				if (device.isDefault) name += QCoreApplication::translate("MeetingUI", "(System Default)");
				_microphoneCombo->addItem(name, device.id);
			}
			for (const auto &device : speakers) {
				if (device.id.isEmpty()) continue; // System Default is already listed above.
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
				_microphoneTestButton->setEnabled(kAudioDeviceTestsSupported);
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
				_speakerTestButton->setEnabled(kAudioDeviceTestsSupported);
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

	// 卡片 1：端到端加密范围
	QVBoxLayout *scopeBody = nullptr;
	auto *scopeCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "End-to-end encryption (E2EE)"), content, &scopeBody);

	_quickE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt quick meetings"), scopeCard);
	_quickE2ee->setObjectName(QStringLiteral("settingsQuickE2ee"));
	scopeBody->addWidget(_quickE2ee);
	_screenShareE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt screen sharing"), scopeCard);
	_screenShareE2ee->setObjectName(QStringLiteral("settingsScreenShareE2ee"));
	scopeBody->addWidget(_screenShareE2ee);
	_meetingDetailsE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt meetings joined from meeting details"), scopeCard);
	_meetingDetailsE2ee->setObjectName(QStringLiteral("settingsMeetingDetailsE2ee"));
	scopeBody->addWidget(_meetingDetailsE2ee);
	_allE2ee = new QCheckBox(QCoreApplication::translate(
		"MeetingUI", "Encrypt all meetings"), scopeCard);
	_allE2ee->setObjectName(QStringLiteral("settingsAllE2ee"));
	const auto scopeHintText = QCoreApplication::translate(
		"MeetingUI", "All meetings also includes quick meetings, screen sharing, and meeting details.");
	auto *scopeHintIcon = makeHintIcon(_allE2ee->text(), scopeHintText, scopeCard);
	scopeHintIcon->setObjectName(QStringLiteral("settingsAllE2eeHintIcon"));
	_allE2ee->setAccessibleDescription(scopeHintText);
	scopeBody->addLayout(makeTitleWithHintRow(_allE2ee, scopeHintIcon));
	layout->addWidget(scopeCard);

	// 卡片 2：会话加密密钥
	const QStringList keyHintLines = {
		QCoreApplication::translate("MeetingUI", "Use the same encryption key as the other participants and share it through a trusted channel. Participant and connection metadata remain visible to the service."),
		QCoreApplication::translate("MeetingUI", "The encryption key is kept only in memory for this sign-in and cleared on sign-out."),
		QCoreApplication::translate("MeetingUI", "Changes apply the next time you create or join a meeting."),
		QCoreApplication::translate("MeetingUI", "Encrypted meetings support VP8 and H264. Auto uses VP8; select a supported codec in Settings for camera and screen sharing."),
	};
	auto *keyHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "Encryption key"),
		keyHintLines.join(QLatin1Char('\n')),
		content,
		15);
	keyHintIcon->setObjectName(QStringLiteral("settingsE2eeKeyHintIcon"));
	QVBoxLayout *keyBody = nullptr;
	auto *keyCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Encryption key"), content, &keyBody, keyHintIcon);
	_e2eeKey = new QLineEdit(keyCard);
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
	keyBody->addWidget(_e2eeKey);

	auto *keyButtons = new QHBoxLayout();
	keyButtons->setSpacing(8);
	_e2eeSaveKey = new QPushButton(QCoreApplication::translate("MeetingUI", "Save key"), keyCard);
	_e2eeSaveKey->setObjectName(QStringLiteral("settingsE2eeSaveKey"));
	_e2eeSaveKey->setAutoDefault(false);
	_e2eeClearKey = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear key"), keyCard);
	_e2eeClearKey->setObjectName(QStringLiteral("settingsE2eeClearKey"));
	_e2eeClearKey->setAutoDefault(false);
	keyButtons->addWidget(_e2eeSaveKey);
	keyButtons->addWidget(_e2eeClearKey);
	keyButtons->addStretch();
	keyBody->addLayout(keyButtons);

	_e2eeStatus = new QLabel(keyCard);
	_e2eeStatus->setObjectName(QStringLiteral("settingsE2eeStatus"));
	_e2eeStatus->setWordWrap(true);
	keyBody->addWidget(_e2eeStatus);
	layout->addWidget(keyCard);
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
#ifdef Q_OS_LINUX
    // Imported requirements stay visible and fail closed. The user may turn
    // them off explicitly, but this build cannot enable a new E2EE policy.
    _quickE2ee->setEnabled(!value.allMeetingsE2ee && value.quickMeetingsE2ee);
    _screenShareE2ee->setEnabled(!value.allMeetingsE2ee && value.screenShareE2ee);
    _meetingDetailsE2ee->setEnabled(!value.allMeetingsE2ee && value.meetingDetailsE2ee);
    _allE2ee->setEnabled(value.allMeetingsE2ee);
#endif
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
#ifdef Q_OS_LINUX
    _e2eeKey->setEnabled(false);
    _e2eeSaveKey->setEnabled(false);
    _e2eeClearKey->setEnabled(_session.hasMeetingEncryptionKey());
    _e2eeStatus->setText(QCoreApplication::translate("MeetingUI",
        "End-to-end encryption is not available on Linux yet. Meetings that require it cannot be joined."));
    _e2eeStatus->show();
    return;
#endif
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
	wipeAndClearLineEdit(_e2eeKey);
}

void SettingsDialog::setAgentCoordinator(OpenMeeting::AgentCoordinator *coordinator) {
	if (_copilotPage) {
		_copilotPage->setAgentCoordinator(
			coordinator,
			_pages && _pages->currentIndex() == static_cast<int>(Page::Copilot));
	}
}

void SettingsDialog::setCopilotSubTab(int index) {
	if (_copilotPage) {
		_copilotPage->setSubTab(index);
	}
}

int SettingsDialog::currentCopilotSubTab() const {
	return _copilotPage ? _copilotPage->currentSubTab() : 0;
}

QWidget *SettingsDialog::buildCopilotPage() {
	_copilotPage = new CopilotSettingsPage(_session, _pages);
	return _copilotPage;
}

QWidget *SettingsDialog::buildAboutPage() {
	auto *scroll = makeScrollablePage(_pages);
	auto *content = scroll->widget();
	auto *layout = static_cast<QVBoxLayout *>(content->layout());

	QVBoxLayout *aboutBody = nullptr;
	auto *aboutCard = makeSettingsCard(QString(), content, &aboutBody);
	aboutBody->setContentsMargins(24, 28, 24, 28);
	aboutBody->setSpacing(12);

	auto *productName = new QLabel(QCoreApplication::applicationName(), aboutCard);
	if (productName->text().trimmed().isEmpty()) {
		productName->setText(AppBranding::name());
	}
	productName->setAlignment(Qt::AlignCenter);
	auto nameFont = productName->font();
	nameFont.setPixelSize(22);
	nameFont.setBold(true);
	productName->setFont(nameFont);
	aboutBody->addWidget(productName);

	auto version = QCoreApplication::applicationVersion().trimmed();
	if (version.isEmpty()) {
		version = QCoreApplication::translate("MeetingUI", "Development Build");
	}
	auto *versionLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Version %1").arg(version), aboutCard);
	versionLabel->setObjectName(QStringLiteral("hintLabel"));
	versionLabel->setAlignment(Qt::AlignCenter);
	aboutBody->addWidget(versionLabel);

	_updateService = new Cohavora::Update::UpdateService(
		Cohavora::Update::UpdateService::BuildConfiguration(),
		QCoreApplication::applicationVersion(), this);
	_updateCheck = new QPushButton(QCoreApplication::translate("MeetingUI", "Check for Updates"), aboutCard);
	_updateCheck->setObjectName(QStringLiteral("secondaryButton"));
	_updateCheck->setAccessibleName(QCoreApplication::translate("MeetingUI", "Check for Updates"));
	_updateCheck->setMinimumWidth(220);
	aboutBody->addWidget(_updateCheck, 0, Qt::AlignHCenter);
	_updateStatus = new QLabel(aboutCard);
	_updateStatus->setObjectName(QStringLiteral("hintLabel"));
	_updateStatus->setAlignment(Qt::AlignCenter);
	_updateStatus->setWordWrap(true);
	_updateStatus->setTextFormat(Qt::PlainText);
	aboutBody->addWidget(_updateStatus);
	_updateNotes = new QLabel(aboutCard);
	_updateNotes->setObjectName(QStringLiteral("hintLabel"));
	_updateNotes->setWordWrap(true);
	_updateNotes->setTextFormat(Qt::PlainText);
	_updateNotes->setTextInteractionFlags(Qt::TextSelectableByMouse);
	aboutBody->addWidget(_updateNotes);
	_updateProgress = new QProgressBar(aboutCard);
	_updateProgress->setRange(0, 100);
	_updateProgress->setTextVisible(false);
	aboutBody->addWidget(_updateProgress);
	auto *actions = new QHBoxLayout();
	_updateAction = new QPushButton(aboutCard);
	_updateAction->setObjectName(QStringLiteral("secondaryButton"));
	_updateCancel = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), aboutCard);
	_updateCancel->setObjectName(QStringLiteral("secondaryButton"));
	actions->addStretch();
	actions->addWidget(_updateAction);
	actions->addWidget(_updateCancel);
	actions->addStretch();
	aboutBody->addLayout(actions);
	layout->addWidget(aboutCard);
	layout->addStretch(1);
	connect(_updateCheck, &QPushButton::clicked, _updateService, &Cohavora::Update::UpdateService::check);
	connect(_updateCancel, &QPushButton::clicked, _updateService, &Cohavora::Update::UpdateService::cancel);
	connect(_updateAction, &QPushButton::clicked, this, [this] {
		using State = Cohavora::Update::State;
		if (_updateService->state() == State::Available) {
			_updateService->download();
		} else if (_updateService->state() == State::Ready) {
#ifdef Q_OS_LINUX
            auto* directory = new QFileDialog(this,QCoreApplication::translate("MeetingUI", "Save installation package"));
            directory->setAttribute(Qt::WA_DeleteOnClose);
            directory->setOption(QFileDialog::DontUseNativeDialog);
            directory->setOption(QFileDialog::ShowDirsOnly);
            directory->setFileMode(QFileDialog::Directory);
            AppTheme::styleChoiceControls(*directory,
                AppTheme::effectiveTheme() == AppTheme::Theme::Light
                    ? AppTheme::Tone::Light : AppTheme::Tone::Dark);
            connect(directory,&QFileDialog::accepted,this,[this,directory] {
                const auto selected = directory->selectedFiles();
                if (selected.size() == 1) _updateService->exportPackage(selected.front());
            });
            directory->open();
#else
			_updateService->launchInstaller();
#endif
		}
	});
	connect(_updateService, &Cohavora::Update::UpdateService::changed, this, &SettingsDialog::syncUpdateControls);
	connect(_updateService, &Cohavora::Update::UpdateService::downloadProgress,
		this, [this](qint64 received, qint64 total) {
			_updateProgress->setValue(total > 0 ? int(received * 100 / total) : 0);
		});
	connect(_updateService, &Cohavora::Update::UpdateService::installerStarted, this, [this] {
		_updateStatus->setText(QCoreApplication::translate("MeetingUI",
			"Installer started. Finish your meeting and close Cohavora to complete the update."));
	});
	syncUpdateControls();
	return scroll;
}

void SettingsDialog::syncUpdateControls() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
    _updateCheck->setEnabled(false);
    _updateCancel->hide();
    _updateProgress->hide();
    _updateAction->hide();
    _updateNotes->hide();
    _updateStatus->setText(QCoreApplication::translate("MeetingUI",
        "Updates are not available on this platform yet."));
    return;
#endif
#ifdef Q_OS_LINUX
    if (!_updateService->checksConfigured()) {
        _updateCheck->setEnabled(false);
        _updateCancel->hide(); _updateProgress->hide(); _updateAction->hide(); _updateNotes->hide();
        _updateStatus->setText(QCoreApplication::translate("MeetingUI",
            "No trusted Linux update source is configured for this platform. Use your signed package repository."));
        return;
    }
#endif
	using State = Cohavora::Update::State;
	using Error = Cohavora::Update::Error;
	const auto state = _updateService->state();
	const auto busy = state == State::Checking || state == State::Downloading || state == State::Verifying;
	_updateCheck->setEnabled(!busy);
	_updateCancel->setVisible(busy);
	_updateProgress->setVisible(state == State::Downloading || state == State::Verifying);
	_updateProgress->setRange(0, state == State::Verifying ? 0 : 100);
	if (state != State::Downloading) _updateProgress->setValue(0);
	_updateAction->setVisible(state == State::Available || state == State::Ready);
	_updateAction->setText(state == State::Ready
#ifdef Q_OS_LINUX
        ? QCoreApplication::translate("MeetingUI", "Save installation package")
#else
		? QCoreApplication::translate("MeetingUI", "Start Installer")
#endif
		: QCoreApplication::translate("MeetingUI", "Download Update"));
	const auto hasRelease = state == State::Available || state == State::Downloading ||
		state == State::Ready || state == State::Verifying;
	_updateNotes->setVisible(hasRelease && !_updateService->release().notes.isEmpty());
	_updateNotes->setText(hasRelease ? _updateService->release().notes : QString());
	QString message;
	switch (state) {
	case State::Idle:
		message = QCoreApplication::translate("MeetingUI", "Check for a new version of Cohavora."); break;
	case State::Checking:
		message = QCoreApplication::translate("MeetingUI", "Checking for updates..."); break;
	case State::UpToDate:
		message = QCoreApplication::translate("MeetingUI", "You are using the latest version."); break;
	case State::Available:
		message = QCoreApplication::translate("MeetingUI", "Version %1 is available.")
			.arg(_updateService->release().version); break;
	case State::Downloading:
		message = QCoreApplication::translate("MeetingUI", "Downloading the update..."); break;
	case State::Ready:
#ifdef Q_OS_LINUX
        message = QCoreApplication::translate("MeetingUI", "Package verified. Save it, then leave your meeting and close Cohavora before installing with the system package manager."); break;
#else
		message = QCoreApplication::translate("MeetingUI", "Update verified. Start the installer when you are ready to close Cohavora."); break;
#endif
	case State::Verifying:
#ifdef Q_OS_LINUX
        message = QCoreApplication::translate("MeetingUI", "Verifying and saving the installation package..."); break;
#else
		message = QCoreApplication::translate("MeetingUI", "Verifying the installer..."); break;
#endif
	case State::Exported:
        message = QCoreApplication::translate("MeetingUI", "Package saved to %1. Leave your meeting and close Cohavora before installing with the system package manager.")
            .arg(_updateService->exportedPackagePath()); break;
	case State::InstallerStarted:
		message = QCoreApplication::translate("MeetingUI",
			"Installer started. Finish your meeting and close Cohavora to complete the update."); break;
	case State::Failed:
		switch (_updateService->error()) {
		case Error::NotConfigured:
			message = QCoreApplication::translate("MeetingUI", "Updates are not configured for this build."); break;
		case Error::InvalidConfiguration:
			message = QCoreApplication::translate("MeetingUI", "The update configuration is invalid."); break;
		case Error::InvalidManifest:
			message = QCoreApplication::translate("MeetingUI", "The update information could not be verified. Try again later."); break;
		case Error::Disk:
#ifdef Q_OS_LINUX
            message = QCoreApplication::translate("MeetingUI", "The package could not be saved. Choose a personal folder that other users cannot modify and check available disk space."); break;
#else
			message = QCoreApplication::translate("MeetingUI", "The update could not be saved. Check available disk space and try again."); break;
#endif
		case Error::Integrity:
#ifdef Q_OS_LINUX
            message = QCoreApplication::translate("MeetingUI", "The package could not be verified. Check for updates to try again."); break;
#else
			message = QCoreApplication::translate("MeetingUI", "The installer could not be verified. Check for updates to try again."); break;
#endif
		case Error::Launch:
			message = QCoreApplication::translate("MeetingUI", "The installer could not be started. Check for updates to try again."); break;
		default:
			message = QCoreApplication::translate("MeetingUI", "The update server could not be reached. Try again later."); break;
		}
		break;
	}
	_updateStatus->setText(message);
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
		_showLiveCaptions,
		_stayWhenLocked,
		_continuousStatsSampling,
		_pushToTalk,
		_echoCancellation,
		_noiseSuppression,
		_autoGainControl }) {
		if (!checkBox) continue;
		connect(checkBox, &QCheckBox::toggled, this, [this](bool) {
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}
	if (_copilotPage) {
		_copilotPage->connectTranslationControls();
		connect(_copilotPage, &CopilotSettingsPage::preferencesChanged, this, [this] {
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
	connect(_videoEncoderCombo,
		static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this](int) {
			if (_updatingUi) return;
			// Normalize both formats before emitting the new backend preference.
			// No observer sees a hardware backend paired with a software-only codec.
			syncVideoCodecChoices(_cameraCodecCombo->currentData().toString(),
				_screenShareCodecCombo->currentData().toString());
			updateScreenShareResolutionCeilingUi();
			commitPreferences();
		});
	for (auto *combo : {_screenShareCodecCombo, _screenShareResolutionCombo, _screenShareFpsCombo}) {
		connect(combo,
			static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
			this, [this](int) {
				if (_updatingUi) return;
				updateScreenShareResolutionCeilingUi();
				commitPreferences();
			});
	}
	for (auto *combo : {_cameraCodecCombo, _receiveResolutionMatchCombo, _participantNameDisplayCombo}) {
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

void SettingsDialog::applyVideoEncoderCapabilities(
		const livekit::VideoEncoderCapabilities &capabilities) {
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	if (_closed || (_videoEncoderCapabilities &&
		capabilities.revision <= _videoEncoderCapabilities->revision)) return;
	_videoEncoderCapabilitiesReady = capabilities.ready;
	_videoEncoderCapabilities = std::make_shared<const livekit::VideoEncoderCapabilities>(capabilities);
#endif
	for (auto *combo : {_videoEncoderCombo, _cameraCodecCombo, _screenShareCodecCombo}) {
		combo->setEnabled(capabilities.ready);
	}
	auto description = QCoreApplication::translate("MeetingUI",
		"Applies to camera and screen sharing when a new encoder is created. Auto prefers available hardware. Software fallback for the same codec depends on the selected encoder and codec; unsupported combinations may fail to start.");
	const auto preferenceReason = _session.mediaPreferences().videoEncoderPreferenceReason;
	if (preferenceReason == OpenMeeting::VideoEncoderPreferenceReason::UnsupportedOnPlatform) {
		description += QLatin1Char('\n') + QCoreApplication::translate("MeetingUI",
			"The saved encoder is unsupported on this platform. Auto is used; settings for other platforms are preserved.");
	} else if (preferenceReason == OpenMeeting::VideoEncoderPreferenceReason::InvalidValue) {
		description += QLatin1Char('\n') + QCoreApplication::translate("MeetingUI", "The saved encoder setting is invalid. Auto is used.");
	}
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	description += QLatin1Char('\n') + QCoreApplication::translate("MeetingUI",
		"Unavailable hardware choices are preserved. Availability confirms initialization only; meeting statistics show whether hardware has produced encoded frames.");
#endif
	const auto text = capabilities.ready ? description :
		QCoreApplication::translate("MeetingUI", "Detecting available video encoders...")
			+ QLatin1Char('\n') + description;
	const auto tone = preferenceReason != OpenMeeting::VideoEncoderPreferenceReason::None
		? QStringLiteral("warning")
		: QString();
	setHintIcon(_videoEncoderHintIcon, text, tone);
	_videoEncoderCombo->setToolTip(text);
	_videoEncoderCombo->setAccessibleDescription(text);
	if (!capabilities.ready) return;
	_videoEncoderCapabilities = std::make_shared<const livekit::VideoEncoderCapabilities>(capabilities);
	_videoCodecChoicesDirty = true;
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	auto preferences = _session.mediaPreferences();
	_videoEncoderCombo->clear();
	_videoEncoderCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto (prefer hardware)"), QStringLiteral("auto"));
	_videoEncoderCombo->addItem(QCoreApplication::translate("MeetingUI", "Software"), QStringLiteral("software"));
#if !defined(Q_OS_LINUX)
	if (capabilities.amf.h264 || capabilities.amf.h265) {
		_videoEncoderCombo->addItem(QStringLiteral("AMD AMF"), QStringLiteral("amf"));
	}
#endif
	if (capabilities.nvenc.h264 || capabilities.nvenc.h265
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		|| capabilities.nvenc.compiled
#endif
	) {
		auto label = QStringLiteral("NVIDIA NVENC");
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		if (!capabilities.nvenc.h264 && !capabilities.nvenc.h265) {
			label += QCoreApplication::translate("MeetingUI", " (unavailable)");
		}
#endif
		_videoEncoderCombo->addItem(label, QStringLiteral("nvenc"));
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		_videoEncoderCombo->setItemData(_videoEncoderCombo->count() - 1,
			linuxEncoderCapabilityHint(capabilities.nvenc), Qt::ToolTipRole);
#endif
	}
	if (capabilities.qsv.h264 || capabilities.qsv.h265
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		|| capabilities.qsv.compiled
#endif
	) {
		auto label = QStringLiteral("Intel QSV");
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		if (!capabilities.qsv.h264 && !capabilities.qsv.h265) {
			label += QCoreApplication::translate("MeetingUI", " (unavailable)");
		}
#endif
		_videoEncoderCombo->addItem(label, QStringLiteral("qsv"));
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		_videoEncoderCombo->setItemData(_videoEncoderCombo->count() - 1,
			linuxEncoderCapabilityHint(capabilities.qsv), Qt::ToolTipRole);
#endif
	}
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	if (capabilities.vaapi.compiled || capabilities.vaapi.h264 || capabilities.vaapi.h265) {
		auto label = QStringLiteral("VAAPI");
		if (!capabilities.vaapi.h264 && !capabilities.vaapi.h265) {
			label += QCoreApplication::translate("MeetingUI", " (unavailable)");
		}
		_videoEncoderCombo->addItem(label, QStringLiteral("vaapi"));
		_videoEncoderCombo->setItemData(_videoEncoderCombo->count() - 1,
			linuxEncoderCapabilityHint(capabilities.vaapi), Qt::ToolTipRole);
	}
#endif
	const auto savedBackend = OpenMeeting::normalizeVideoEncoderPreference(preferences.videoEncoderPreference);
#if defined(Q_OS_LINUX)
	// A missing driver/device is transient and must not rewrite saved intent.
	if (_videoEncoderCombo->findData(savedBackend) < 0) {
		const auto label = savedBackend == QStringLiteral("qsv") ? QStringLiteral("Intel QSV") :
				(savedBackend == QStringLiteral("nvenc") ? QStringLiteral("NVIDIA NVENC") :
					(savedBackend == QStringLiteral("vaapi") ? QStringLiteral("VAAPI") : savedBackend));
		_videoEncoderCombo->addItem(label + QCoreApplication::translate("MeetingUI", " (unavailable)"), savedBackend);
	}
#endif
	const bool missingBackend = _videoEncoderCombo->findData(savedBackend) < 0;
	if (missingBackend) preferences.videoEncoderPreference = QStringLiteral("auto");
	_videoEncoderCapabilitiesReady = true;
	setPreferences(preferences);
	const auto cameraCodec = _cameraCodecCombo->currentData().toString();
	const auto screenShareCodec = _screenShareCodecCombo->currentData().toString();
	const bool changedCodecs = cameraCodec != preferences.cameraVideoCodec ||
		screenShareCodec != preferences.screenShareVideoCodec;
	preferences.cameraVideoCodec = cameraCodec;
	preferences.screenShareVideoCodec = screenShareCodec;
	_updatingUi = oldUpdating;
	// A preference saved on another PC must agree with the visible selection.
	if (missingBackend || changedCodecs) _session.setMediaPreferences(preferences);
}

void SettingsDialog::syncVideoCodecChoices(
		const QString &cameraCodec, const QString &screenShareCodec) {
	if (!_videoEncoderCapabilitiesReady || !_videoEncoderCapabilities) return;
	const auto preference = _videoEncoderCombo->currentData().toString();
	const livekit::VideoEncoderBackendCapability *hardware = nullptr;
	if (preference == QStringLiteral("amf")) hardware = &_videoEncoderCapabilities->amf;
	else if (preference == QStringLiteral("nvenc")) hardware = &_videoEncoderCapabilities->nvenc;
	else if (preference == QStringLiteral("qsv")) hardware = &_videoEncoderCapabilities->qsv;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	else if (preference == QStringLiteral("vaapi")) hardware = &_videoEncoderCapabilities->vaapi;
#endif
	const QSignalBlocker cameraBlocker(_cameraCodecCombo);
	const QSignalBlocker screenShareBlocker(_screenShareCodecCombo);
	if (_videoCodecChoicesDirty || _videoCodecBackend != preference) {
		populateVideoCodecChoices(_cameraCodecCombo, true, hardware, false);
		populateVideoCodecChoices(_screenShareCodecCombo, true, hardware, true);
		_videoCodecBackend = preference;
		_videoCodecChoicesDirty = false;
	}
	const auto select = [&](QComboBox *combo, const QString &codec) {
		const auto requested = OpenMeeting::normalizeVideoCodecPreference(codec);
		auto selected = findData(combo, requested);
#if defined(Q_OS_LINUX)
		if (selected < 0) {
			combo->addItem(requested.toUpper() + QCoreApplication::translate("MeetingUI", " (unavailable)"), requested);
			selected = combo->count() - 1;
			if (auto *model = qobject_cast<QStandardItemModel*>(combo->model())) {
				if (auto *item = model->item(selected)) item->setEnabled(false);
			}
		}
#endif
		// Hardware lists are ordered H264, then H265, and contain only probed formats.
		combo->setCurrentIndex(selected >= 0 ? selected : 0);
	};
	select(_cameraCodecCombo, cameraCodec);
	select(_screenShareCodecCombo, screenShareCodec);
	updateScreenShareResolutionCeilingUi();
}

void SettingsDialog::updateScreenShareResolutionCeilingUi() {
	if (!_screenShareResolutionCombo || !_screenShareResolutionHint) return;
	const auto saved = _session.mediaPreferences();
	const auto codec = OpenMeeting::normalizeVideoCodecPreference(
		_videoEncoderCapabilitiesReady && _screenShareCodecCombo && _screenShareCodecCombo->currentData().isValid()
			? _screenShareCodecCombo->currentData().toString()
			: saved.screenShareVideoCodec).toStdString();
	const auto encoder = OpenMeeting::normalizeVideoEncoderPreference(
		_videoEncoderCapabilitiesReady && _videoEncoderCombo && _videoEncoderCombo->currentData().isValid()
			? _videoEncoderCombo->currentData().toString()
			: saved.videoEncoderPreference).toStdString();
	const int fps = _screenShareFpsCombo && _screenShareFpsCombo->currentData().isValid()
		? _screenShareFpsCombo->currentData().toInt()
		: saved.screenShareFps;
	QSize pixels = screenShareDisplayPixelSize(screen());
	if (_screenShareResolutionCombo->findData(static_cast<int>(livekit::ScreenShareResolution::P2160)) >= 0) {
		pixels = QSize(std::max(pixels.width(), 3840), std::max(pixels.height(), 2160));
	} else if (_screenShareResolutionCombo->findData(static_cast<int>(livekit::ScreenShareResolution::P1440)) >= 0) {
		pixels = QSize(std::max(pixels.width(), 2560), std::max(pixels.height(), 1440));
	} else if (_screenShareResolutionCombo->findData(static_cast<int>(livekit::ScreenShareResolution::P1080)) >= 0) {
		pixels = QSize(std::max(pixels.width(), 1920), std::max(pixels.height(), 1080));
	}
	const auto ceiling = livekit::EvaluateScreenShareCodecCeiling(codec, encoder, fps);
	annotateScreenShareResolutionChoices(*_screenShareResolutionCombo, pixels, ceiling);
	const auto selected = _screenShareResolutionCombo->currentData().isValid()
		? static_cast<livekit::ScreenShareResolution>(_screenShareResolutionCombo->currentData().toInt())
		: livekit::ScreenShareResolution::Auto;
	const auto hint = screenShareConstraintHintText(pixels, selected, ceiling);
	_screenShareResolutionHint->setText(hint);
	_screenShareResolutionHint->setVisible(!hint.isEmpty());
}

OpenMeeting::MediaPreferences SettingsDialog::preferences() const {
	auto value = _session.mediaPreferences();
	value.enableVideo = _generalCamera && _generalCamera->isChecked();
	value.enableMicrophone = _generalMicrophone && _generalMicrophone->isChecked();
	value.enableSpeaker = _generalSpeaker && _generalSpeaker->isChecked();
	value.quitOnMainWindowClose = _quitOnClose && _quitOnClose->isChecked();
	value.showActiveSpeaker = _showActiveSpeaker && _showActiveSpeaker->isChecked();
	value.showLiveCaptions = !_showLiveCaptions || _showLiveCaptions->isChecked();
	if (_copilotPage) {
		_copilotPage->readTranslationPreferences(value);
	}
	value.participantNameDisplay = static_cast<OpenMeeting::ParticipantNameDisplayMode>(
		_participantNameDisplayCombo->currentData().toInt());
	value.stayInMeetingWhenLocked = _stayWhenLocked && _stayWhenLocked->isChecked();
	value.enableContinuousStatsSampling =
		_continuousStatsSampling && _continuousStatsSampling->isChecked();
	value.pushToTalkWhenMuted = _pushToTalk && _pushToTalk->isChecked();
	value.echoCancellation = _echoCancellation && _echoCancellation->isChecked();
	value.noiseSuppression = _noiseSuppression && _noiseSuppression->isChecked();
	value.autoGainControl = _autoGainControl && _autoGainControl->isChecked();
	if (_videoDevicesReady) value.cameraDeviceId = selectedCameraDeviceId();
	value.microphoneDeviceId = selectedMicrophoneDeviceId();
	value.speakerDeviceId = selectedSpeakerDeviceId();
	if (_videoEncoderCapabilitiesReady) {
		value.cameraVideoCodec = _cameraCodecCombo->currentData().toString();
		value.videoEncoderPreference = _videoEncoderCombo->currentData().toString();
		value.screenShareVideoCodec = _screenShareCodecCombo->currentData().toString();
	}
	value.screenShareResolution = _screenShareResolutionCombo->currentData().toInt();
	value.screenShareFps = _screenShareFpsCombo->currentData().toInt();
	value.receiveResolutionMatch = livekit::ParseVideoResolutionMatch(
		_receiveResolutionMatchCombo->currentData().toString().toStdString());

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
	_showLiveCaptions->setChecked(value.showLiveCaptions);
	if (_copilotPage) {
		_copilotPage->applyTranslationPreferences(value);
	}
	const auto nameDisplayIndex = _participantNameDisplayCombo->findData(static_cast<int>(value.participantNameDisplay));
	_participantNameDisplayCombo->setCurrentIndex(nameDisplayIndex >= 0 ? nameDisplayIndex : 0);
	_stayWhenLocked->setChecked(value.stayInMeetingWhenLocked);
	if (_continuousStatsSampling) {
		_continuousStatsSampling->setChecked(value.enableContinuousStatsSampling);
	}
	_pushToTalk->setChecked(value.pushToTalkWhenMuted);
	_echoCancellation->setChecked(value.echoCancellation);
	_noiseSuppression->setChecked(value.noiseSuppression);
	_autoGainControl->setChecked(value.autoGainControl);
	if (_videoEncoderCombo) {
		const auto selected = findData(_videoEncoderCombo,
			OpenMeeting::normalizeVideoEncoderPreference(value.videoEncoderPreference));
		_videoEncoderCombo->setCurrentIndex(selected >= 0 ? selected : 0);
	}
	syncVideoCodecChoices(value.cameraVideoCodec, value.screenShareVideoCodec);
	selectScreenShareResolution(*_screenShareResolutionCombo, value.screenShareResolution);
	_screenShareFpsCombo->setCurrentIndex(_screenShareFpsCombo->findData(value.screenShareFps));
	updateScreenShareResolutionCeilingUi();
	_receiveResolutionMatchCombo->setCurrentIndex(_receiveResolutionMatchCombo->findData(
		QString::fromLatin1(livekit::VideoResolutionMatchName(value.receiveResolutionMatch))));
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

SettingsDialog::Page SettingsDialog::currentPage() const {
	if (_navigation && _navigation->currentRow() >= 0) {
		return static_cast<Page>(_navigation->currentRow());
	}
	return Page::General;
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
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
	livekit::detail::RequestLinuxVideoEncoderCapabilityRefresh(true);
	applyVideoEncoderCapabilities(livekit::GetVideoEncoderCapabilities());
#endif
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
#ifndef Q_OS_LINUX
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
#endif
	QDialog::mousePressEvent(event);
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *event) {
#ifndef Q_OS_LINUX
	if (_dragging && (event->buttons() & Qt::LeftButton)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		move(event->globalPosition().toPoint() - _dragOffset);
#else
		move(event->globalPos() - _dragOffset);
#endif
		event->accept();
		return;
	}
#endif
	QDialog::mouseMoveEvent(event);
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *event) {
#ifndef Q_OS_LINUX
	if (event->button() == Qt::LeftButton) {
		_dragging = false;
	}
#endif
	QDialog::mouseReleaseEvent(event);
}

} // namespace MeetingUI
