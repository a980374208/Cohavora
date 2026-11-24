#include "src/ui/whiteboard/accessible_combo_box.h"
#include <QtCore/QCoreApplication>
#include "src/ui/app_branding.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/app_icons.h"
#include "src/ui/settings_dialog.h"
#ifdef Q_OS_LINUX
#include "src/ui/linux_window_frame.h"
#endif
#include "src/ui/screen_share_quality_controls.h"
#include "src/update/update_service.h"
#include <QtWidgets/QFileDialog>

#include "src/core/session_shutdown_service.h"
#include "src/e2ee/meeting_encryption.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/agent/agent_coordinator.h"
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

void wipeQString(QString &text) {
	if (!text.isEmpty()) {
		OPENSSL_cleanse(text.data(), static_cast<size_t>(text.size()) * sizeof(QChar));
	}
	text.clear();
}

void wipeAndClearLineEdit(QLineEdit *edit) {
	if (!edit) {
		return;
	}
	const QSignalBlocker blocker(edit);
	QString text = edit->text();
	// setText resets the undo history. Qt/IME historical copies cannot be guaranteed erased.
	edit->setText(QString(text.size(), QChar(0)));
	edit->clear();
	wipeQString(text);
}

QString keyEndpointMismatchWarningText(
		OpenMeeting::AgentKeyEndpointMismatch mismatch,
		bool forSpeech) {
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Bailian Token/Coding Plan key (sk-sp-...) is not valid for standard DashScope speech endpoints; use a standard sk-... key.")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Bailian Token/Coding Plan key (sk-sp-...) requires a plan endpoint (e.g. coding.dashscope.aliyuncs.com or token-plan.*.maas.aliyuncs.com), not a standard DashScope URL.");
	}
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Standard sk-... key does not match a Bailian Token/Coding Plan host, and speech ASR models generally require the standard DashScope endpoint (dashscope.aliyuncs.com).")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Standard sk-... key does not match a Bailian Token/Coding Plan endpoint (use an sk-sp-... plan key or switch to the standard DashScope URL).");
	}
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::GroqKeyOnNonGroqHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Detected a Groq API key (gsk_...) against a non-Groq STT endpoint. Use a matching provider key (e.g. sk-... for SiliconFlow/OpenAI/DashScope) or set the STT URL to https://api.groq.com/openai/v1.")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Detected a Groq API key (gsk_...) against a non-Groq endpoint. Use a matching provider key or switch the Base URL to https://api.groq.com/openai/v1.");
	}
	return {};
}

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

QScrollArea *makeScrollablePage(QWidget *parent) {
	auto *scroll = new QScrollArea(parent);
	scroll->setObjectName(QStringLiteral("settingsScrollArea"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	auto *content = new QWidget(scroll->viewport());
	content->setObjectName(QStringLiteral("settingsScrollContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(20, 18, 20, 20);
	layout->setSpacing(14);
	scroll->setWidget(content);
	return scroll;
}

QFrame *makeSettingsCard(
		const QString &title,
		QWidget *parent,
		QVBoxLayout **outBody,
		QWidget *headerRight = nullptr,
		QWidget *titleAccessory = nullptr) {
	auto *card = new QFrame(parent);
	card->setObjectName(QStringLiteral("settingsCard"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(0, 0, 0, 0);
	cardLayout->setSpacing(0);

	if (!title.isEmpty() || headerRight || titleAccessory) {
		auto *header = new QWidget(card);
		header->setObjectName(QStringLiteral("settingsCardHeader"));
		auto *headerLayout = new QHBoxLayout(header);
		headerLayout->setContentsMargins(16, 10, 14, 10);
		headerLayout->setSpacing(6);
		if (!title.isEmpty()) {
			auto *titleLabel = new QLabel(title, header);
			titleLabel->setObjectName(QStringLiteral("settingsCardTitle"));
			titleLabel->setWordWrap(false);
			titleLabel->setProperty("keepSingleLine", true);
			headerLayout->addWidget(titleLabel);
		}
		if (titleAccessory) {
			titleAccessory->setParent(header);
			headerLayout->addWidget(titleAccessory, 0, Qt::AlignVCenter);
		}
		headerLayout->addStretch(1);
		if (headerRight) {
			headerLayout->addWidget(headerRight);
		}
		cardLayout->addWidget(header);
	}

	auto *body = new QWidget(card);
	body->setObjectName(QStringLiteral("settingsCardBody"));
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(16, 14, 16, 14);
	bodyLayout->setSpacing(10);
	cardLayout->addWidget(body);

	if (outBody) {
		*outBody = bodyLayout;
	}
	return card;
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

void repolishSettingsWidget(QWidget *w) {
	if (!w) return;
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
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
		const livekit::VideoEncoderBackendCapability *hardware = nullptr) {
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
		add(QCoreApplication::translate("MeetingUI", "Auto (prefer hardware H.264)"),
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

QString formatHintToolTip(const QString &plainText) {
	const QString trimmed = plainText.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}
	QString escaped = trimmed.toHtmlEscaped();
	escaped.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
	return QStringLiteral("<qt>%1</qt>").arg(escaped);
}

QString defaultCopilotModelHint() {
	return QCoreApplication::translate("MeetingUI",
		"Click Refresh Models to fetch available chat models from the endpoint, or enter a model name manually.");
}

class HintIconButton final : public QToolButton {
public:
	explicit HintIconButton(int iconSizePx, QWidget *parent = nullptr)
		: QToolButton(parent) {
		setProperty("copilotHintIcon", true);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::NoFocus);
		setFixedSize(20, 20);
		setIconSize(QSize(iconSizePx, iconSizePx));
		refreshIcon();
		AppTheme::observeChanges(*this, [this] { refreshIcon(); });
	}

	void setHint(const QString &plainText, const QString &tone = QString()) {
		const QString trimmed = plainText.trimmed();
		const QString nextRich = formatHintToolTip(trimmed);
		const bool wasShowingSelf =
			QToolTip::isVisible() && !_richTip.isEmpty() && QToolTip::text() == _richTip;
		_plainText = trimmed;
		_richTip = nextRich;
		setToolTip(_richTip);
		setAccessibleDescription(_plainText);
		if (_tone != tone) {
			_tone = tone;
			setProperty("hintTone", _tone);
			if (auto *st = style()) {
				st->unpolish(this);
				st->polish(this);
			}
		}
		refreshIcon();
		if (_hovered && isVisible()) {
			if (!_richTip.isEmpty()) {
				showHintNow();
			} else if (wasShowingSelf) {
				QToolTip::hideText();
			}
		} else if (wasShowingSelf) {
			QToolTip::hideText();
		}
	}

protected:
	bool event(QEvent *event) override {
		switch (event->type()) {
		case QEvent::Enter:
			_hovered = true;
			refreshIcon();
			showHintNow();
			break;
		case QEvent::Leave:
		case QEvent::Hide:
			_hovered = false;
			refreshIcon();
			if (QToolTip::isVisible() && !_richTip.isEmpty() && QToolTip::text() == _richTip) {
				QToolTip::hideText();
			}
			break;
		case QEvent::ToolTip:
			if (!_richTip.isEmpty()) {
				if (!QToolTip::isVisible() || QToolTip::text() != _richTip) {
					showHintNow();
				}
				event->accept();
				return true;
			}
			break;
		case QEvent::MouseButtonPress:
			if (!_richTip.isEmpty()) {
				showHintNow();
			}
			break;
		default:
			break;
		}
		return QToolButton::event(event);
	}

private:
	void refreshIcon() {
		auto iconKind = AppTheme::Icon::Information;
		auto role = _hovered ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextMuted;
		if (_tone == QStringLiteral("warning")) {
			role = AppTheme::ColorRole::Warning;
		} else if (_tone == QStringLiteral("error")) {
			iconKind = AppTheme::Icon::ErrorCircle;
			role = AppTheme::ColorRole::Danger;
		} else if (_tone == QStringLiteral("pending")) {
			role = AppTheme::ColorRole::Accent;
		}
		setIcon(AppTheme::icon(iconKind, role));
	}

	void showHintNow() {
		if (_richTip.isEmpty() || !isVisible() || QApplication::activePopupWidget() != nullptr) {
			return;
		}
		QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())), _richTip, this, rect());
	}

	QString _plainText;
	QString _richTip;
	QString _tone;
	bool _hovered = false;
};

HintIconButton *makeHintIcon(
		const QString &accessibleTitle,
		const QString &hintText,
		QWidget *parent,
		int iconSizePx = 14) {
	auto *icon = new HintIconButton(iconSizePx, parent);
	icon->setAccessibleName(accessibleTitle);
	if (!hintText.isEmpty()) {
		icon->setHint(hintText);
	}
	return icon;
}

void setHintIcon(QToolButton *button, const QString &plainText, const QString &tone = QString()) {
	if (!button) {
		return;
	}
	static_cast<HintIconButton *>(button)->setHint(plainText, tone);
}

QHBoxLayout *makeTitleWithHintRow(
		QWidget *titleWidget,
		QToolButton *hintIcon,
		bool addTrailingStretch = true) {
	if (auto *label = qobject_cast<QLabel *>(titleWidget)) {
		label->setWordWrap(false);
		label->setProperty("keepSingleLine", true);
	}
	auto *row = new QHBoxLayout();
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);
	row->addWidget(titleWidget, 0, Qt::AlignVCenter);
	row->addWidget(hintIcon, 0, Qt::AlignVCenter);
	if (addTrailingStretch) {
		row->addStretch(1);
	}
	return row;
}

} // namespace

class SettingsCollapsibleSection final : public QWidget {
public:
	explicit SettingsCollapsibleSection(
		const QString &title,
		const QString &toggleObjectName,
		QWidget *parent = nullptr)
		: QWidget(parent) {
		setObjectName(QStringLiteral("settingsCollapsibleSection"));
		auto *rootLayout = new QVBoxLayout(this);
		rootLayout->setContentsMargins(0, 0, 0, 0);
		rootLayout->setSpacing(6);

		_headerBar = new QFrame(this);
		_headerBar->setObjectName(QStringLiteral("settingsCollapsibleBar"));
		_headerBar->setCursor(Qt::PointingHandCursor);
		_headerBar->installEventFilter(this);
		auto *barLayout = new QHBoxLayout(_headerBar);
		barLayout->setContentsMargins(8, 3, 8, 3);
		barLayout->setSpacing(6);

		_toggleBtn = new QPushButton(title, _headerBar);
		_toggleBtn->setObjectName(toggleObjectName);
		_toggleBtn->setCheckable(true);
		_toggleBtn->setChecked(false);
		_toggleBtn->setAutoDefault(false);
		_toggleBtn->setCursor(Qt::PointingHandCursor);
		_toggleBtn->setProperty("collapsibleToggle", true);
		_toggleBtn->setIconSize(QSize(13, 13));
		barLayout->addWidget(_toggleBtn, 0, Qt::AlignVCenter);

		barLayout->addStretch(1);

		_summaryLabel = new QLabel(_headerBar);
		_summaryLabel->setObjectName(QStringLiteral("settingsCollapsibleSummary"));
		_summaryLabel->setWordWrap(false);
		_summaryLabel->setProperty("keepSingleLine", true);
		_summaryLabel->setProperty("customState", QStringLiteral("default"));
		_summaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		barLayout->addWidget(_summaryLabel, 0, Qt::AlignVCenter);

		_resetBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Reset"), _headerBar);
		_resetBtn->setObjectName(QStringLiteral("settingsCollapsibleResetBtn"));
		_resetBtn->setAutoDefault(false);
		_resetBtn->setCursor(Qt::PointingHandCursor);
		_resetBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Restore default values"));
		_resetBtn->hide();
		barLayout->addWidget(_resetBtn, 0, Qt::AlignVCenter);

		rootLayout->addWidget(_headerBar);

		_bodyFrame = new QFrame(this);
		_bodyFrame->setObjectName(QStringLiteral("settingsCollapsibleBody"));
		_bodyLayout = new QVBoxLayout(_bodyFrame);
		_bodyLayout->setContentsMargins(12, 8, 2, 4);
		_bodyLayout->setSpacing(8);
		_bodyFrame->hide();
		rootLayout->addWidget(_bodyFrame);

		connect(_toggleBtn, &QPushButton::toggled, this, [this](bool expanded) {
			_bodyFrame->setVisible(expanded);
			refreshIcons();
			if (_resetBtn) {
				_resetBtn->setVisible(expanded && _isCustomized);
			}
		});
		connect(_resetBtn, &QPushButton::clicked, this, [this] {
			if (_resetHandler) {
				_resetHandler();
			}
		});

		refreshIcons();
		AppTheme::observeChanges(*this, [this] { refreshIcons(); });
	}

	QVBoxLayout *bodyLayout() const { return _bodyLayout; }
	QWidget *bodyWidget() const { return _bodyFrame; }

	bool isExpanded() const { return _toggleBtn && _toggleBtn->isChecked(); }

	void setExpanded(bool expanded) {
		if (_toggleBtn && _toggleBtn->isChecked() != expanded) {
			_toggleBtn->setChecked(expanded);
		}
	}

	void setSummary(const QString &defaultText, const QStringList &customizedItems) {
		_isCustomized = !customizedItems.isEmpty();
		if (_summaryLabel) {
			if (!_isCustomized) {
				_summaryLabel->setText(defaultText);
				_summaryLabel->setToolTip(QString());
				_summaryLabel->setProperty("customState", QStringLiteral("default"));
			} else {
				_summaryLabel->setText(
					QCoreApplication::translate("MeetingUI", "Customized (%1)").arg(customizedItems.size()));
				_summaryLabel->setToolTip(customizedItems.join(QStringLiteral(" · ")));
				_summaryLabel->setProperty("customState", QStringLiteral("custom"));
			}
			if (auto *st = _summaryLabel->style()) {
				st->unpolish(_summaryLabel);
				st->polish(_summaryLabel);
			}
		}
		if (_resetBtn) {
			_resetBtn->setVisible(isExpanded() && _isCustomized);
		}
	}

	void setResetHandler(std::function<void()> handler) {
		_resetHandler = std::move(handler);
	}

	void refreshIcons() {
		if (_toggleBtn) {
			const auto iconKind = _toggleBtn->isChecked()
				? AppTheme::Icon::ChevronDown
				: AppTheme::Icon::ChevronRight;
			_toggleBtn->setIcon(AppTheme::icon(iconKind, AppTheme::ColorRole::TextSecondary));
		}
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == _headerBar && event->type() == QEvent::MouseButtonRelease) {
			const auto *me = static_cast<QMouseEvent *>(event);
			if (me->button() == Qt::LeftButton && _headerBar->rect().contains(me->pos())) {
				QWidget *hit = _headerBar->childAt(me->pos());
				if (!hit || (hit != _toggleBtn && hit != _resetBtn)) {
					if (_toggleBtn) {
						_toggleBtn->toggle();
					}
					return true;
				}
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private:
	QFrame *_headerBar = nullptr;
	QPushButton *_toggleBtn = nullptr;
	QLabel *_summaryLabel = nullptr;
	QPushButton *_resetBtn = nullptr;
	QFrame *_bodyFrame = nullptr;
	QVBoxLayout *_bodyLayout = nullptr;
	bool _isCustomized = false;
	std::function<void()> _resetHandler;
};

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
	syncCopilotControls();
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
		clearCopilotKeyEditor();
		wipeAndClearLineEdit(_copilotSttApiKey);
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
	disconnectCopilotCoordinatorSignals();
	clearSecurityEditor();
	clearCopilotKeyEditor();
	wipeAndClearLineEdit(_copilotSttApiKey);
	cancelDeviceDiscovery();
	if (_audioTestController) {
		_audioTestController->stopAll();
	}
	if (_cameraPreview) {
		_cameraPreview->stopPreview();
	}
}

void SettingsDialog::done(int result) {
	if (_translationPromptCommitTimer && _translationPromptCommitTimer->isActive()) {
		_translationPromptCommitTimer->stop();
		commitPreferences();
	}
	commitCopilotProfile(true);
	if (_updateService) _updateService->cancel();
	clearSecurityEditor();
	clearCopilotKeyEditor();
	wipeAndClearLineEdit(_copilotSttApiKey);
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
		if (row == static_cast<int>(Page::Copilot)) {
			maybeAutoRefreshCopilotModels();
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
	if (!needle.isEmpty() && _pages->currentIndex() == static_cast<int>(Page::Copilot)) {
		focusCopilotSearchMatch(needle);
	}
}

void SettingsDialog::focusCopilotSearchMatch(const QString &needle) {
	if (needle.isEmpty() || !_copilotSubPages) {
		return;
	}
	const auto containsNeedle = [&needle](const QString &text) {
		return text.toLower().contains(needle);
	};
	for (int tab = 0; tab < _copilotSubPages->count(); ++tab) {
		QWidget *subScroll = _copilotSubPages->widget(tab);
		if (!subScroll) {
			continue;
		}
		QWidget *matchedWidget = nullptr;
		// Prefer exact field labels or control buttons inside the subpage content rather than card headers
		const auto checkWidgets = [&](auto list) {
			for (auto *w : list) {
				if (!w) {
					continue;
				}
				const QString objName = w->objectName();
				if (objName == QStringLiteral("settingsCardTitle") ||
					objName == QStringLiteral("settingsScopeBadge") ||
					objName == QStringLiteral("settingsCollapsibleSummary")) {
					continue;
				}
				if (containsNeedle(w->text())) {
					matchedWidget = w;
					return true;
				}
			}
			return false;
		};
		if (!checkWidgets(subScroll->findChildren<QLabel *>())) {
			checkWidgets(subScroll->findChildren<QAbstractButton *>());
		}
		if (!matchedWidget) {
			// Fallback to any label/button match inside subpage
			for (auto *lbl : subScroll->findChildren<QLabel *>()) {
				if (containsNeedle(lbl->text())) {
					matchedWidget = lbl;
					break;
				}
			}
		}
		if (!matchedWidget) {
			continue;
		}

		setCopilotSubTab(tab);
		for (auto *sec : {_copilotChatAdvSection, _copilotSttAdvSection, _copilotTrAdvSection}) {
			if (sec && sec->bodyWidget() && sec->bodyWidget()->isAncestorOf(matchedWidget)) {
				sec->setExpanded(true);
			}
		}
		QWidget *scrollTarget = matchedWidget;
		const bool hiddenInCustomSpeechSection =
			((_copilotSttProfileToolbar && _copilotSttProfileToolbar->isAncestorOf(matchedWidget)) ||
			 (_copilotSttProfileNameBlock && _copilotSttProfileNameBlock->isAncestorOf(matchedWidget)) ||
			 (_copilotSttEndpointContainer && _copilotSttEndpointContainer->isAncestorOf(matchedWidget))) &&
			(selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile);
		if (hiddenInCustomSpeechSection && _copilotSttModeCustomBtn) {
			scrollTarget = _copilotSttModeCustomBtn;
		}
		if (auto *scrollArea = qobject_cast<QScrollArea *>(subScroll)) {
			const QPointer<QScrollArea> guardScroll(scrollArea);
			const QPointer<QWidget> guardTarget(scrollTarget);
			QTimer::singleShot(0, this, [guardScroll, guardTarget] {
				if (guardScroll && guardTarget && guardTarget->isVisible()) {
					guardScroll->ensureWidgetVisible(guardTarget, 0, 24);
				}
			});
		}
		return;
	}
}

void SettingsDialog::refreshDialogThemeVisuals() {
	if (_closeButton) {
		_closeButton->setIcon(AppTheme::icon(AppTheme::Icon::Close, AppTheme::ColorRole::TextSecondary));
	}
	if (_copilotNewProfileBtn) {
		_copilotNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	}
	if (_copilotProfileMoreBtn) {
		_copilotProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	}
	if (_copilotSttNewProfileBtn) {
		_copilotSttNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	}
	if (_copilotSttProfileMoreBtn) {
		_copilotSttProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	}
	for (auto *sec : {_copilotChatAdvSection, _copilotSttAdvSection, _copilotTrAdvSection}) {
		if (sec) {
			sec->refreshIcons();
		}
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
	populateVideoCodecChoices(_screenShareCodecCombo, false);
	codecBody->addWidget(_screenShareCodecCombo);
	codecBody->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Screen share resolution"), codecCard));
	_screenShareResolutionCombo = new QComboBox(codecCard);
	configureAccessibleComboBox(_screenShareResolutionCombo);
	_screenShareResolutionCombo->setObjectName(QStringLiteral("screenShareResolution"));
	_screenShareResolutionCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Screen share resolution"));
	populateScreenShareResolutionChoices(*_screenShareResolutionCombo, screenShareDisplayPixelSize(screen()));
	codecBody->addWidget(_screenShareResolutionCombo);
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

OpenMeeting::AgentCoordinator &SettingsDialog::copilotCoordinator() {
	if (!_agentCoordinator) {
		if (!_ownedAgentCoordinator) {
			_ownedAgentCoordinator = std::make_unique<OpenMeeting::AgentCoordinator>(_session);
		}
		return *_ownedAgentCoordinator;
	}
	return *_agentCoordinator;
}

void SettingsDialog::disconnectCopilotCoordinatorSignals() {
	if (_agentProfileConn) {
		disconnect(_agentProfileConn);
		_agentProfileConn = {};
	}
	if (_agentProfilesConn) {
		disconnect(_agentProfilesConn);
		_agentProfilesConn = {};
	}
	if (_agentSpeechProfilesConn) {
		disconnect(_agentSpeechProfilesConn);
		_agentSpeechProfilesConn = {};
	}
	if (_agentProbeConn) {
		disconnect(_agentProbeConn);
		_agentProbeConn = {};
	}
	if (_agentSpeechProbeConn) {
		disconnect(_agentSpeechProbeConn);
		_agentSpeechProbeConn = {};
	}
	if (_agentModelsConn) {
		disconnect(_agentModelsConn);
		_agentModelsConn = {};
	}
	if (_agentSpeechModelsConn) {
		disconnect(_agentSpeechModelsConn);
		_agentSpeechModelsConn = {};
	}
}

void SettingsDialog::setAgentCoordinator(OpenMeeting::AgentCoordinator *coordinator) {
	if (_agentCoordinator == coordinator) {
		return;
	}
	disconnectCopilotCoordinatorSignals();
	_agentCoordinator = coordinator;
	if (_agentCoordinator) {
		_ownedAgentCoordinator.reset();
	}
	_copilotLastModelFetchBaseUrl.clear();
	_copilotLastSttModelFetchUrl.clear();
	_copilotLastSttEndpointKey.clear();
	_copilotSttModelAutoSelected = false;
	if (_copilotModelCombo) {
		const bool prevUpdating = _updatingUi;
		_updatingUi = true;
		const QSignalBlocker comboBlocker(_copilotModelCombo);
		_copilotModelCombo->clear();
		_updatingUi = prevUpdating;
	}
	auto &activeCoord = copilotCoordinator();
	if (activeCoord.isFetchingModels()) {
		setCopilotModelHint(
			QCoreApplication::translate("MeetingUI", "Fetching models..."),
			QStringLiteral("pending"));
	} else {
		resetCopilotModelHint();
	}
	if (_copilotRefreshModelsBtn) {
		_copilotRefreshModelsBtn->setEnabled(!activeCoord.isFetchingModels());
	}
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(!activeCoord.isProbing());
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(!activeCoord.isSpeechProbing());
	}
	setCopilotTestStatus(
		activeCoord.isProbing() ? QStringLiteral("pending") : QString(),
		activeCoord.isProbing()
			? QCoreApplication::translate("MeetingUI", "Testing connection...")
			: QString());
	setCopilotSttTestStatus(
		activeCoord.isSpeechProbing() ? QStringLiteral("pending") : QString(),
		activeCoord.isSpeechProbing()
			? QCoreApplication::translate("MeetingUI", "Testing connection...")
			: QString());
	syncCopilotControls();
	if (_pages && _pages->currentIndex() == static_cast<int>(Page::Copilot)) {
		maybeAutoRefreshCopilotModels();
	}
}

QWidget *SettingsDialog::buildCopilotPage() {
	auto *page = new QWidget(_pages);
	page->setObjectName(QStringLiteral("settingsCopilotRoot"));
	auto *rootLayout = new QVBoxLayout(page);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	// 顶部固定 3 段式子页签切换栏
	auto *tabBarWrap = new QWidget(page);
	tabBarWrap->setObjectName(QStringLiteral("settingsCopilotTabBarWrap"));
	auto *tabBarWrapLayout = new QVBoxLayout(tabBarWrap);
	tabBarWrapLayout->setContentsMargins(20, 10, 20, 6);
	tabBarWrapLayout->setSpacing(0);

	auto *segmentedFrame = new QFrame(tabBarWrap);
	segmentedFrame->setObjectName(QStringLiteral("settingsSegmentedControl"));
	auto *segLayout = new QHBoxLayout(segmentedFrame);
	segLayout->setContentsMargins(3, 3, 3, 3);
	segLayout->setSpacing(4);

	auto *tabGroup = new QButtonGroup(segmentedFrame);
	tabGroup->setExclusive(true);

	const std::array<std::pair<const char *, QString>, 3> tabDefs = {
		std::pair{"settingsCopilotTabChat", QCoreApplication::translate("MeetingUI", "Models & Chat")},
		std::pair{"settingsCopilotTabStt", QCoreApplication::translate("MeetingUI", "Speech & Dictation (ASR)")},
		std::pair{"settingsCopilotTabTranslation", QCoreApplication::translate("MeetingUI", "Live Translation & Agent")}
	};
	for (size_t i = 0; i < tabDefs.size(); ++i) {
		auto *btn = new QPushButton(tabDefs[i].second, segmentedFrame);
		btn->setObjectName(QString::fromLatin1(tabDefs[i].first));
		btn->setProperty("segmentedTab", true);
		btn->setCheckable(true);
		btn->setAutoDefault(false);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setChecked(i == 0);
		tabGroup->addButton(btn, static_cast<int>(i));
		segLayout->addWidget(btn, 1);
		_copilotSubTabButtons[i] = btn;
	}
	tabBarWrapLayout->addWidget(segmentedFrame);
	rootLayout->addWidget(tabBarWrap);

	_copilotSubPages = new QStackedWidget(page);
	_copilotSubPages->setObjectName(QStringLiteral("settingsCopilotSubPages"));
	rootLayout->addWidget(_copilotSubPages, 1);

	const auto makeSubScrollPage = [this] {
		auto *scroll = makeScrollablePage(_copilotSubPages);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		auto *content = scroll->widget();
		auto *outer = static_cast<QVBoxLayout *>(content->layout());
		outer->setContentsMargins(20, 8, 20, 14);
		outer->setSpacing(10);
		_copilotSubPages->addWidget(scroll);
		return std::pair{content, outer};
	};

	const auto makeScopeAccessory = [](QToolButton *hintIcon, const QString &badgeText, const QString &tone, QWidget *parent, QLabel **outBadge = nullptr) {
		auto *wrap = new QWidget(parent);
		auto *h = new QHBoxLayout(wrap);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(6);
		if (hintIcon) {
			hintIcon->setParent(wrap);
			h->addWidget(hintIcon, 0, Qt::AlignVCenter);
		}
		auto *badge = new QLabel(badgeText, wrap);
		badge->setObjectName(QStringLiteral("settingsScopeBadge"));
		badge->setProperty("badgeTone", tone);
		badge->setWordWrap(false);
		badge->setProperty("keepSingleLine", true);
		h->addWidget(badge, 0, Qt::AlignVCenter);
		if (outBadge) {
			*outBadge = badge;
		}
		return wrap;
	};

	const auto makeFieldBlock = [](QWidget *labelOrRowWidget, QWidget *field, QWidget *parent) {
		auto *block = new QWidget(parent);
		auto *vbox = new QVBoxLayout(block);
		vbox->setContentsMargins(0, 0, 0, 0);
		vbox->setSpacing(4);
		vbox->addWidget(labelOrRowWidget);
		vbox->addWidget(field);
		return block;
	};

	// =========================================================================
	// Sub-Page 0: Models & Chat (Profile-scoped)
	// =========================================================================
	const auto [chatContent, chatOuter] = makeSubScrollPage();

	auto *profileToolbarRow = new QHBoxLayout();
	profileToolbarRow->setSpacing(8);
	auto *profileSelectorLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Active Profile"), chatContent);
	profileSelectorLabel->setWordWrap(false);
	profileSelectorLabel->setProperty("keepSingleLine", true);
	profileToolbarRow->addWidget(profileSelectorLabel);

	_copilotProfileSelector = createElidedAccessibleComboBox(chatContent);
	_copilotProfileSelector->setObjectName(QStringLiteral("settingsCopilotProfileSelector"));
	_copilotProfileSelector->setAccessibleName(profileSelectorLabel->text());
	AppTheme::styleChoiceControls(*_copilotProfileSelector, AppTheme::Tone::Light);
	profileSelectorLabel->setBuddy(_copilotProfileSelector);
	profileToolbarRow->addWidget(_copilotProfileSelector, 1);

	_copilotNewProfileBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "New"), chatContent);
	_copilotNewProfileBtn->setObjectName(QStringLiteral("settingsCopilotNewProfile"));
	_copilotNewProfileBtn->setAutoDefault(false);
	_copilotNewProfileBtn->setIconSize(QSize(13, 13));
	_copilotNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	profileToolbarRow->addWidget(_copilotNewProfileBtn);

	_copilotProfileMoreBtn = new QToolButton(chatContent);
	_copilotProfileMoreBtn->setObjectName(QStringLiteral("settingsCopilotProfileMore"));
	_copilotProfileMoreBtn->setAutoRaise(false);
	_copilotProfileMoreBtn->setCursor(Qt::PointingHandCursor);
	_copilotProfileMoreBtn->setIconSize(QSize(15, 15));
	_copilotProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	_copilotProfileMoreBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Duplicate or delete profile"));
	_copilotProfileMoreBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Profile actions"));
	profileToolbarRow->addWidget(_copilotProfileMoreBtn);
	chatOuter->addLayout(profileToolbarRow);

	auto *introHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "AI Copilot"),
		QCoreApplication::translate("MeetingUI",
			"Configure a custom OpenAI-compatible API, Responses API, Anthropic Messages API, Google Gemini API, or custom REST webhook for AI Copilot."),
		chatContent,
		15);
	introHintIcon->setObjectName(QStringLiteral("settingsCopilotIntroHintIcon"));

	_copilotTestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Connection"), chatContent);
	_copilotTestBtn->setObjectName(QStringLiteral("settingsCopilotTestBtn"));
	_copilotTestBtn->setProperty("headerAction", true);
	_copilotTestBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Test chat model connection"));
	_copilotTestBtn->setAccessibleName(_copilotTestBtn->toolTip());
	_copilotTestBtn->setAutoDefault(false);

	QVBoxLayout *chatLayout = nullptr;
	auto *chatCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Connection & Model"),
		chatContent,
		&chatLayout,
		_copilotTestBtn,
		makeScopeAccessory(
			introHintIcon,
			QCoreApplication::translate("MeetingUI", "Per Profile"),
			QStringLiteral("accent"),
			chatContent));
	chatLayout->setContentsMargins(16, 12, 16, 12);
	chatLayout->setSpacing(7);
	chatOuter->addWidget(chatCard);

	_copilotTestStatus = new QLabel(chatCard);
	_copilotTestStatus->setObjectName(QStringLiteral("settingsCopilotStatus"));
	_copilotTestStatus->setTextFormat(Qt::PlainText);
	_copilotTestStatus->setWordWrap(true);
	_copilotTestStatus->hide();
	chatLayout->addWidget(_copilotTestStatus);

	// 2 列行: Display Name (5) + Protocol Dialect (7)
	auto *nameProtoRow = new QHBoxLayout();
	nameProtoRow->setSpacing(10);

	auto *nameLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Display Name"), chatCard);
	nameLabel->setWordWrap(false);
	nameLabel->setProperty("keepSingleLine", true);
	_copilotProfileName = new QLineEdit(chatCard);
	_copilotProfileName->setObjectName(QStringLiteral("settingsCopilotProfileName"));
	_copilotProfileName->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Custom Agent"));
	nameLabel->setBuddy(_copilotProfileName);
	nameProtoRow->addWidget(makeFieldBlock(nameLabel, _copilotProfileName, chatCard), 5);

	auto *protocolLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Protocol Dialect"), chatCard);
	protocolLabel->setWordWrap(false);
	protocolLabel->setProperty("keepSingleLine", true);
	_copilotProtocolCombo = createElidedAccessibleComboBox(chatCard);
	_copilotProtocolCombo->setObjectName(QStringLiteral("settingsCopilotProtocol"));
	_copilotProtocolCombo->setAccessibleName(protocolLabel->text());
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI ChatCompletions (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::OpenAiChatCompletionsSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Responses (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::OpenAiResponsesSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Anthropic Messages (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Google Gemini (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::GoogleGeminiSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Custom Webhook (JSON / SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::CustomWebhookJson));
	AppTheme::styleChoiceControls(*_copilotProtocolCombo, AppTheme::Tone::Light);
	protocolLabel->setBuddy(_copilotProtocolCombo);
	nameProtoRow->addWidget(makeFieldBlock(protocolLabel, _copilotProtocolCombo, chatCard), 7);
	chatLayout->addLayout(nameProtoRow);

	auto *urlLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Base URL / Endpoint"), chatCard);
	auto *urlHintIcon = makeHintIcon(
		urlLabel->text(),
		QCoreApplication::translate("MeetingUI",
			"HTTPS is required for remote endpoints. HTTP is permitted only for localhost / 127.0.0.1 (e.g. Ollama or vLLM)."),
		chatCard);
	urlHintIcon->setObjectName(QStringLiteral("settingsCopilotUrlHintIcon"));
	_copilotBaseUrl = new QLineEdit(chatCard);
	_copilotBaseUrl->setObjectName(QStringLiteral("settingsCopilotBaseUrl"));
	_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1 or http://127.0.0.1:11434/v1"));
	urlLabel->setBuddy(_copilotBaseUrl);
	auto *urlBlockLayout = new QVBoxLayout();
	urlBlockLayout->setSpacing(4);
	urlBlockLayout->addLayout(makeTitleWithHintRow(urlLabel, urlHintIcon));
	urlBlockLayout->addWidget(_copilotBaseUrl);
	chatLayout->addLayout(urlBlockLayout);

	auto *keyLabel = new QLabel(QCoreApplication::translate("MeetingUI", "API Key / Bearer Token"), chatCard);
	_copilotKeyHintIcon = makeHintIcon(keyLabel->text(), QString(), chatCard);
	_copilotKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotKeyHintIcon"));
	auto *keyRow = new QHBoxLayout();
	keyRow->setSpacing(8);
	_copilotApiKey = new QLineEdit(chatCard);
	_copilotApiKey->setObjectName(QStringLiteral("settingsCopilotApiKey"));
	_copilotApiKey->setEchoMode(QLineEdit::Password);
	_copilotApiKey->setPlaceholderText(
		QCoreApplication::translate("MeetingUI", "Enter API key (leave blank to keep saved key)"));
	keyLabel->setBuddy(_copilotApiKey);
	keyRow->addWidget(_copilotApiKey, 1);

	_copilotSaveKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save Key"), chatCard);
	_copilotSaveKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveKey"));
	_copilotSaveKeyBtn->setAutoDefault(false);
	keyRow->addWidget(_copilotSaveKeyBtn);

	_copilotClearKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear Key"), chatCard);
	_copilotClearKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearKey"));
	_copilotClearKeyBtn->setAutoDefault(false);
	keyRow->addWidget(_copilotClearKeyBtn);

	auto *keyBlockLayout = new QVBoxLayout();
	keyBlockLayout->setSpacing(4);
	keyBlockLayout->addLayout(makeTitleWithHintRow(keyLabel, _copilotKeyHintIcon));
	keyBlockLayout->addLayout(keyRow);
	chatLayout->addLayout(keyBlockLayout);

	auto *modelLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Model Name"), chatCard);
	_copilotModelHintIcon = makeHintIcon(modelLabel->text(), defaultCopilotModelHint(), chatCard);
	_copilotModelHintIcon->setObjectName(QStringLiteral("settingsCopilotModelHintIcon"));
	auto *modelRow = new QHBoxLayout();
	modelRow->setSpacing(8);
	_copilotModelCombo = createElidedAccessibleComboBox(chatCard);
	_copilotModelCombo->setObjectName(QStringLiteral("settingsCopilotModel"));
	_copilotModelCombo->setAccessibleName(modelLabel->text());
	_copilotModelCombo->setEditable(true);
	_copilotModelCombo->setInsertPolicy(QComboBox::NoInsert);
	AppTheme::styleChoiceControls(*_copilotModelCombo, AppTheme::Tone::Light);
	if (auto *edit = _copilotModelCombo->lineEdit()) {
		edit->setPlaceholderText(QStringLiteral("gpt-4o-mini / qwen-plus"));
	}
	modelLabel->setBuddy(_copilotModelCombo);
	modelRow->addWidget(_copilotModelCombo, 1);

	_copilotRefreshModelsBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Refresh Models"), chatCard);
	_copilotRefreshModelsBtn->setObjectName(QStringLiteral("settingsCopilotRefreshModels"));
	_copilotRefreshModelsBtn->setAutoDefault(false);
	modelRow->addWidget(_copilotRefreshModelsBtn);

	auto *modelBlockLayout = new QVBoxLayout();
	modelBlockLayout->setSpacing(4);
	modelBlockLayout->addLayout(makeTitleWithHintRow(modelLabel, _copilotModelHintIcon));
	modelBlockLayout->addLayout(modelRow);
	chatLayout->addLayout(modelBlockLayout);

	auto *reasoningRow = new QHBoxLayout();
	reasoningRow->setSpacing(10);
	auto *reasoningLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Reasoning Effort"), chatCard);
	_copilotReasoningHintIcon = makeHintIcon(reasoningLabel->text(), QString(), chatCard);
	_copilotReasoningHintIcon->setObjectName(QStringLiteral("settingsCopilotReasoningHintIcon"));
	_copilotReasoningCombo = createAccessibleComboBox(chatCard);
	_copilotReasoningCombo->setObjectName(QStringLiteral("settingsCopilotReasoningEffort"));
	_copilotReasoningCombo->setAccessibleName(reasoningLabel->text());
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Default (Provider / Model Default)"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Default));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Low"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Low));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Medium"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Medium));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "High"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::High));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Extra (xhigh)"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Extra));
	AppTheme::styleChoiceControls(*_copilotReasoningCombo, AppTheme::Tone::Light);
	reasoningLabel->setBuddy(_copilotReasoningCombo);
	reasoningRow->addLayout(makeTitleWithHintRow(reasoningLabel, _copilotReasoningHintIcon, false));
	reasoningRow->addWidget(_copilotReasoningCombo, 1);
	chatLayout->addLayout(reasoningRow);

	auto *promptLabel = new QLabel(QCoreApplication::translate("MeetingUI", "System Prompt"), chatCard);
	_copilotSystemPrompt = new QPlainTextEdit(chatCard);
	_copilotSystemPrompt->setObjectName(QStringLiteral("settingsCopilotSystemPrompt"));
	_copilotSystemPrompt->setFixedHeight(56);
	promptLabel->setBuddy(_copilotSystemPrompt);
	chatLayout->addWidget(makeFieldBlock(promptLabel, _copilotSystemPrompt, chatCard));

	// 唯一高级折叠区：Advanced Runtime & Privacy
	_copilotChatAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Runtime & Privacy"),
		QStringLiteral("settingsCopilotChatAdvToggle"),
		chatCard);
	chatLayout->addWidget(_copilotChatAdvSection);
	auto *chatAdvLayout = _copilotChatAdvSection->bodyLayout();

	auto *headersLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Custom Headers"), _copilotChatAdvSection->bodyWidget());
	auto *headersHintIcon = makeHintIcon(
		headersLabel->text(),
		QCoreApplication::translate("MeetingUI", "Optional, one per line: Header-Name: value"),
		_copilotChatAdvSection->bodyWidget());
	headersHintIcon->setObjectName(QStringLiteral("settingsCopilotHeadersHintIcon"));
	_copilotCustomHeaders = new QPlainTextEdit(_copilotChatAdvSection->bodyWidget());
	_copilotCustomHeaders->setObjectName(QStringLiteral("settingsCopilotHeaders"));
	_copilotCustomHeaders->setFixedHeight(56);
	_copilotCustomHeaders->setPlaceholderText(QStringLiteral("X-Custom-Header: value"));
	headersLabel->setBuddy(_copilotCustomHeaders);
	auto *headersBlockLayout = new QVBoxLayout();
	headersBlockLayout->setSpacing(4);
	headersBlockLayout->addLayout(makeTitleWithHintRow(headersLabel, headersHintIcon));
	headersBlockLayout->addWidget(_copilotCustomHeaders);
	chatAdvLayout->addLayout(headersBlockLayout);

	auto *chatAdvGrid = new QGridLayout();
	chatAdvGrid->setHorizontalSpacing(10);
	chatAdvGrid->setVerticalSpacing(8);

	auto *historyLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Max Context Messages"), _copilotChatAdvSection->bodyWidget());
	_copilotMaxHistorySpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotMaxHistorySpin->setObjectName(QStringLiteral("settingsCopilotMaxHistory"));
	_copilotMaxHistorySpin->setRange(2, 64);
	historyLabel->setBuddy(_copilotMaxHistorySpin);
	chatAdvGrid->addWidget(makeFieldBlock(historyLabel, _copilotMaxHistorySpin, _copilotChatAdvSection->bodyWidget()), 0, 0);

	auto *tempLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Temperature"), _copilotChatAdvSection->bodyWidget());
	_copilotTemperatureSpin = new QDoubleSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotTemperatureSpin->setObjectName(QStringLiteral("settingsCopilotTemperature"));
	_copilotTemperatureSpin->setRange(0.0, 2.0);
	_copilotTemperatureSpin->setSingleStep(0.1);
	_copilotTemperatureSpin->setDecimals(2);
	tempLabel->setBuddy(_copilotTemperatureSpin);
	chatAdvGrid->addWidget(makeFieldBlock(tempLabel, _copilotTemperatureSpin, _copilotChatAdvSection->bodyWidget()), 0, 1);

	auto *connectTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Connect Timeout"), _copilotChatAdvSection->bodyWidget());
	_copilotConnectTimeoutSpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotConnectTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotConnectTimeout"));
	_copilotConnectTimeoutSpin->setAccessibleName(connectTimeoutLabel->text());
	_copilotConnectTimeoutSpin->setRange(2000, 60000);
	_copilotConnectTimeoutSpin->setSingleStep(1000);
	_copilotConnectTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotConnectTimeoutSpin->setValue(10000);
	connectTimeoutLabel->setBuddy(_copilotConnectTimeoutSpin);
	chatAdvGrid->addWidget(makeFieldBlock(connectTimeoutLabel, _copilotConnectTimeoutSpin, _copilotChatAdvSection->bodyWidget()), 1, 0);

	auto *idleTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Idle Timeout"), _copilotChatAdvSection->bodyWidget());
	_copilotIdleTimeoutSpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotIdleTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotIdleTimeout"));
	_copilotIdleTimeoutSpin->setAccessibleName(idleTimeoutLabel->text());
	_copilotIdleTimeoutSpin->setRange(3000, 180000);
	_copilotIdleTimeoutSpin->setSingleStep(1000);
	_copilotIdleTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotIdleTimeoutSpin->setValue(30000);
	idleTimeoutLabel->setBuddy(_copilotIdleTimeoutSpin);
	chatAdvGrid->addWidget(makeFieldBlock(idleTimeoutLabel, _copilotIdleTimeoutSpin, _copilotChatAdvSection->bodyWidget()), 1, 1);
	chatAdvLayout->addLayout(chatAdvGrid);

	auto *privacyDivider = new QFrame(_copilotChatAdvSection->bodyWidget());
	privacyDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	privacyDivider->setFixedHeight(1);
	chatAdvLayout->addWidget(privacyDivider);

	auto *privacyGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Privacy & Local Storage"),
		_copilotChatAdvSection->bodyWidget());
	privacyGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	chatAdvLayout->addWidget(privacyGroupTitle);

	_copilotEncryptKeyCheck = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Persist encrypted API key on this device (DPAPI / Keyring)"),
		_copilotChatAdvSection->bodyWidget());
	_copilotEncryptKeyCheck->setObjectName(QStringLiteral("settingsCopilotEncryptKey"));
	_copilotEncryptKeyCheck->setChecked(true);
	auto *encryptHintIcon = makeHintIcon(
		_copilotEncryptKeyCheck->text(),
		QCoreApplication::translate("MeetingUI",
			"Recommended. When unchecked, the API key is kept only in process memory for the current session."),
		_copilotChatAdvSection->bodyWidget());
	encryptHintIcon->setObjectName(QStringLiteral("settingsCopilotEncryptHintIcon"));
	chatAdvLayout->addLayout(makeTitleWithHintRow(_copilotEncryptKeyCheck, encryptHintIcon));

	auto *historyRow = new QHBoxLayout();
	historyRow->setSpacing(10);
	_copilotPersistHistoryCheck = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Save Copilot chat history locally per account"),
		_copilotChatAdvSection->bodyWidget());
	_copilotPersistHistoryCheck->setObjectName(QStringLiteral("settingsCopilotPersistHistory"));
	historyRow->addWidget(_copilotPersistHistoryCheck, 1);

	_copilotClearHistoryBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Clear Saved History"),
		_copilotChatAdvSection->bodyWidget());
	_copilotClearHistoryBtn->setObjectName(QStringLiteral("settingsCopilotClearHistory"));
	_copilotClearHistoryBtn->setAutoDefault(false);
	historyRow->addWidget(_copilotClearHistoryBtn);
	chatAdvLayout->addLayout(historyRow);

	chatOuter->addStretch(1);

	// =========================================================================
	// Sub-Page 1: Speech & Dictation (Follow Chat or Custom Speech Profiles)
	// =========================================================================
	const auto [sttContent, sttOuter] = makeSubScrollPage();

	auto *speechModeRow = new QHBoxLayout();
	speechModeRow->setSpacing(10);
	auto *speechModeLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Source"), sttContent);
	speechModeLabel->setWordWrap(false);
	speechModeLabel->setProperty("keepSingleLine", true);
	speechModeRow->addWidget(speechModeLabel);

	auto *speechModeFrame = new QFrame(sttContent);
	speechModeFrame->setObjectName(QStringLiteral("settingsSpeechModeControl"));
	auto *speechModeLayout = new QHBoxLayout(speechModeFrame);
	speechModeLayout->setContentsMargins(2, 2, 2, 2);
	speechModeLayout->setSpacing(3);
	auto *speechModeGroup = new QButtonGroup(speechModeFrame);
	speechModeGroup->setExclusive(true);

	_copilotSttModeFollowBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Reuse \"Models & Chat\""),
		speechModeFrame);
	_copilotSttModeFollowBtn->setObjectName(QStringLiteral("settingsCopilotSttModeFollow"));
	_copilotSttModeFollowBtn->setProperty("segmentedSubTab", true);
	_copilotSttModeFollowBtn->setCheckable(true);
	_copilotSttModeFollowBtn->setAutoDefault(false);
	_copilotSttModeFollowBtn->setCursor(Qt::PointingHandCursor);
	_copilotSttModeFollowBtn->setChecked(true);
	speechModeGroup->addButton(_copilotSttModeFollowBtn, static_cast<int>(OpenMeeting::AgentSpeechSourceMode::FollowChat));
	speechModeLayout->addWidget(_copilotSttModeFollowBtn, 1);

	_copilotSttModeCustomBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Custom Speech Profiles"),
		speechModeFrame);
	_copilotSttModeCustomBtn->setObjectName(QStringLiteral("settingsCopilotSttModeCustom"));
	_copilotSttModeCustomBtn->setProperty("segmentedSubTab", true);
	_copilotSttModeCustomBtn->setCheckable(true);
	_copilotSttModeCustomBtn->setAutoDefault(false);
	_copilotSttModeCustomBtn->setCursor(Qt::PointingHandCursor);
	speechModeGroup->addButton(_copilotSttModeCustomBtn, static_cast<int>(OpenMeeting::AgentSpeechSourceMode::CustomProfile));
	speechModeLayout->addWidget(_copilotSttModeCustomBtn, 1);

	speechModeRow->addWidget(speechModeFrame, 1);
	sttOuter->addLayout(speechModeRow);

	_copilotSttProfileToolbar = new QWidget(sttContent);
	auto *sttToolbarLayout = new QHBoxLayout(_copilotSttProfileToolbar);
	sttToolbarLayout->setContentsMargins(0, 0, 0, 0);
	sttToolbarLayout->setSpacing(8);

	auto *sttProfileSelectorLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Profile"), _copilotSttProfileToolbar);
	sttProfileSelectorLabel->setWordWrap(false);
	sttProfileSelectorLabel->setProperty("keepSingleLine", true);
	sttToolbarLayout->addWidget(sttProfileSelectorLabel);

	_copilotSttProfileSelector = createElidedAccessibleComboBox(_copilotSttProfileToolbar);
	_copilotSttProfileSelector->setObjectName(QStringLiteral("settingsCopilotSttProfileSelector"));
	_copilotSttProfileSelector->setAccessibleName(sttProfileSelectorLabel->text());
	AppTheme::styleChoiceControls(*_copilotSttProfileSelector, AppTheme::Tone::Light);
	sttProfileSelectorLabel->setBuddy(_copilotSttProfileSelector);
	sttToolbarLayout->addWidget(_copilotSttProfileSelector, 1);

	_copilotSttNewProfileBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "New"), _copilotSttProfileToolbar);
	_copilotSttNewProfileBtn->setObjectName(QStringLiteral("settingsCopilotSttNewProfile"));
	_copilotSttNewProfileBtn->setAutoDefault(false);
	_copilotSttNewProfileBtn->setIconSize(QSize(13, 13));
	_copilotSttNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	sttToolbarLayout->addWidget(_copilotSttNewProfileBtn);

	_copilotSttProfileMoreBtn = new QToolButton(_copilotSttProfileToolbar);
	_copilotSttProfileMoreBtn->setObjectName(QStringLiteral("settingsCopilotSttProfileMore"));
	_copilotSttProfileMoreBtn->setAutoRaise(false);
	_copilotSttProfileMoreBtn->setCursor(Qt::PointingHandCursor);
	_copilotSttProfileMoreBtn->setIconSize(QSize(15, 15));
	_copilotSttProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	_copilotSttProfileMoreBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Duplicate or delete speech profile"));
	_copilotSttProfileMoreBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Speech profile actions"));
	sttToolbarLayout->addWidget(_copilotSttProfileMoreBtn);
	_copilotSttProfileToolbar->hide();
	sttOuter->addWidget(_copilotSttProfileToolbar);

	const QString sttCardTitle = QCoreApplication::translate(
		"MeetingUI", "Speech Recognition & Dictation (ASR / STT)");
	auto *sttHintIcon = makeHintIcon(
		sttCardTitle,
		QCoreApplication::translate(
			"MeetingUI",
			"Shared by Copilot voice dictation and live meeting transcription. Reuse your active \"Models & Chat\" connection, or switch to Custom Speech Profiles to manage multiple dedicated ASR endpoints."),
		sttContent,
		15);
	sttHintIcon->setObjectName(QStringLiteral("settingsCopilotSttHintIcon"));

	_copilotSttTestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Connection"), sttContent);
	_copilotSttTestBtn->setObjectName(QStringLiteral("settingsCopilotSttTestBtn"));
	_copilotSttTestBtn->setProperty("headerAction", true);
	_copilotSttTestBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Test speech-to-text model connection"));
	_copilotSttTestBtn->setAccessibleName(_copilotSttTestBtn->toolTip());
	_copilotSttTestBtn->setAutoDefault(false);

	QVBoxLayout *sttLayout = nullptr;
	auto *sttCard = makeSettingsCard(
		sttCardTitle,
		sttContent,
		&sttLayout,
		_copilotSttTestBtn,
		makeScopeAccessory(
			sttHintIcon,
			QCoreApplication::translate("MeetingUI", "Follows Chat"),
			QStringLiteral("muted"),
			sttContent,
			&_copilotSttScopeBadge));
	sttLayout->setContentsMargins(16, 12, 16, 12);
	sttLayout->setSpacing(8);
	sttOuter->addWidget(sttCard);

	_copilotSttTestStatus = new QLabel(sttCard);
	_copilotSttTestStatus->setObjectName(QStringLiteral("settingsCopilotSttStatus"));
	_copilotSttTestStatus->setTextFormat(Qt::PlainText);
	_copilotSttTestStatus->setWordWrap(true);
	_copilotSttTestStatus->hide();
	sttLayout->addWidget(_copilotSttTestStatus);

	// 复用模式下显示当前对话配置摘要
	_copilotSttFollowBanner = new QFrame(sttCard);
	_copilotSttFollowBanner->setObjectName(QStringLiteral("settingsInlineBanner"));
	auto *followBannerLayout = new QHBoxLayout(_copilotSttFollowBanner);
	followBannerLayout->setContentsMargins(10, 7, 10, 7);
	followBannerLayout->setSpacing(8);
	_copilotSttFollowLabel = new QLabel(_copilotSttFollowBanner);
	_copilotSttFollowLabel->setObjectName(QStringLiteral("hintLabel"));
	_copilotSttFollowLabel->setWordWrap(true);
	followBannerLayout->addWidget(_copilotSttFollowLabel, 1);
	sttLayout->addWidget(_copilotSttFollowBanner);

	_copilotSttKeyWarningBanner = new QLabel(sttCard);
	_copilotSttKeyWarningBanner->setObjectName(QStringLiteral("settingsInlineWarning"));
	_copilotSttKeyWarningBanner->setWordWrap(true);
	_copilotSttKeyWarningBanner->setTextFormat(Qt::PlainText);
	_copilotSttKeyWarningBanner->hide();
	sttLayout->addWidget(_copilotSttKeyWarningBanner);

	// 2 列行: 配置名称 (5, 仅自定义模式显示) + 识别协议 (7, 复用模式下占满整行)
	auto *sttNameProtoRow = new QHBoxLayout();
	sttNameProtoRow->setSpacing(10);

	auto *sttNameLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Profile Name"), sttCard);
	sttNameLabel->setWordWrap(false);
	sttNameLabel->setProperty("keepSingleLine", true);
	_copilotSttProfileName = new QLineEdit(sttCard);
	_copilotSttProfileName->setObjectName(QStringLiteral("settingsCopilotSttProfileName"));
	_copilotSttProfileName->setPlaceholderText(QCoreApplication::translate("MeetingUI", "e.g. ElevenLabs / Deepgram / Groq"));
	sttNameLabel->setBuddy(_copilotSttProfileName);
	_copilotSttProfileNameBlock = makeFieldBlock(sttNameLabel, _copilotSttProfileName, sttCard);
	_copilotSttProfileNameBlock->hide();
	sttNameProtoRow->addWidget(_copilotSttProfileNameBlock, 5);

	auto *sttProtoBlock = new QWidget(sttCard);
	auto *sttProtoBlockLayout = new QVBoxLayout(sttProtoBlock);
	sttProtoBlockLayout->setContentsMargins(0, 0, 0, 0);
	sttProtoBlockLayout->setSpacing(4);
	auto *sttProtoLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Recognition Protocol"), sttProtoBlock);
	sttProtoLabel->setWordWrap(false);
	sttProtoLabel->setProperty("keepSingleLine", true);
	auto *sttProtoHintIcon = makeHintIcon(
		sttProtoLabel->text(),
		QCoreApplication::translate(
			"MeetingUI",
			"Auto mode routes wss:// URLs and streaming models (e.g. gpt-4o-transcribe, paraformer-realtime-v2, qwen3-asr-flash-realtime) to WebSocket streaming ASR, while keeping https:// + whisper-1 / qwen3-asr-flash on HTTP STT. Select a specific streaming protocol in Custom Speech Profiles to use its official wss:// endpoint with a dedicated key."),
		sttProtoBlock);
	sttProtoHintIcon->setObjectName(QStringLiteral("settingsCopilotSttProtoHintIcon"));
	_copilotSttProtocolCombo = createElidedAccessibleComboBox(sttProtoBlock);
	_copilotSttProtocolCombo->setObjectName(QStringLiteral("settingsCopilotSttProtocol"));
	_copilotSttProtocolCombo->setAccessibleName(sttProtoLabel->text());
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Auto (Recommended)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::Auto));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Transcriptions (/audio/transcriptions)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Chat Audio (/chat/completions)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::ChatInputAudio));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Multimodal (multimodal-generation)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeMultimodal));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::OpenAiRealtimeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Inference (WebSocket · Paraformer/Fun-ASR)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Qwen3 Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Deepgram Streaming (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DeepgramWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "AssemblyAI v3 (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Volcengine Doubao SAUC v3 (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "ElevenLabs Scribe Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "xAI Grok STT (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::XaiSttWs));
	AppTheme::styleChoiceControls(*_copilotSttProtocolCombo, AppTheme::Tone::Light);
	sttProtoLabel->setBuddy(_copilotSttProtocolCombo);
	sttProtoBlockLayout->addLayout(makeTitleWithHintRow(sttProtoLabel, sttProtoHintIcon));
	sttProtoBlockLayout->addWidget(_copilotSttProtocolCombo);
	sttNameProtoRow->addWidget(sttProtoBlock, 7);
	sttLayout->addLayout(sttNameProtoRow);

	// 自定义语音端点与密钥容器（仅自定义语音配置模式显示）
	_copilotSttEndpointContainer = new QWidget(sttCard);
	auto *sttEpLayout = new QVBoxLayout(_copilotSttEndpointContainer);
	sttEpLayout->setContentsMargins(0, 0, 0, 0);
	sttEpLayout->setSpacing(8);

	auto *sttUrlLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Speech Endpoint URL (HTTP / WSS)"),
		_copilotSttEndpointContainer);
	sttUrlLabel->setWordWrap(false);
	sttUrlLabel->setProperty("keepSingleLine", true);
	_copilotSttBaseUrl = new QLineEdit(_copilotSttEndpointContainer);
	_copilotSttBaseUrl->setObjectName(QStringLiteral("settingsCopilotSttBaseUrl"));
	_copilotSttBaseUrl->setAccessibleName(sttUrlLabel->text());
	_copilotSttBaseUrl->setClearButtonEnabled(true);
	_copilotSttBaseUrl->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI",
		"Optional — https:// or wss:// (leave blank for protocol default)"));
	sttUrlLabel->setBuddy(_copilotSttBaseUrl);
	sttEpLayout->addWidget(makeFieldBlock(sttUrlLabel, _copilotSttBaseUrl, _copilotSttEndpointContainer));

	_copilotSttKeyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Speech API Key"),
		_copilotSttEndpointContainer);
	_copilotSttKeyLabel->setWordWrap(false);
	_copilotSttKeyLabel->setProperty("keepSingleLine", true);
	_copilotSttKeyHintIcon = makeHintIcon(_copilotSttKeyLabel->text(), QString(), _copilotSttEndpointContainer);
	_copilotSttKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotSttKeyHintIcon"));
	auto *sttKeyRow = new QHBoxLayout();
	sttKeyRow->setSpacing(8);
	_copilotSttApiKey = new QLineEdit(_copilotSttEndpointContainer);
	_copilotSttApiKey->setObjectName(QStringLiteral("settingsCopilotSttApiKey"));
	_copilotSttApiKey->setAccessibleName(_copilotSttKeyLabel->text());
	_copilotSttApiKey->setEchoMode(QLineEdit::Password);
	_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI",
		"Enter speech API key (leave blank to keep saved key)"));
	_copilotSttKeyLabel->setBuddy(_copilotSttApiKey);
	sttKeyRow->addWidget(_copilotSttApiKey, 1);

	_copilotSaveSttKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save Key"), _copilotSttEndpointContainer);
	_copilotSaveSttKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveSttKey"));
	_copilotSaveSttKeyBtn->setAutoDefault(false);
	sttKeyRow->addWidget(_copilotSaveSttKeyBtn);

	_copilotClearSttKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear Key"), _copilotSttEndpointContainer);
	_copilotClearSttKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearSttKey"));
	_copilotClearSttKeyBtn->setAutoDefault(false);
	sttKeyRow->addWidget(_copilotClearSttKeyBtn);

	auto *sttKeyBlockLayout = new QVBoxLayout();
	sttKeyBlockLayout->setSpacing(4);
	sttKeyBlockLayout->addLayout(makeTitleWithHintRow(_copilotSttKeyLabel, _copilotSttKeyHintIcon));
	sttKeyBlockLayout->addLayout(sttKeyRow);
	sttEpLayout->addLayout(sttKeyBlockLayout);

	// Volcengine SAUC v3 auxiliary container (App Key + Resource ID)
	_copilotSttVolcContainer = new QWidget(_copilotSttEndpointContainer);
	_copilotSttVolcContainer->setObjectName(QStringLiteral("settingsCopilotSttVolcContainer"));
	auto *volcLayout = new QVBoxLayout(_copilotSttVolcContainer);
	volcLayout->setContentsMargins(0, 0, 0, 0);
	volcLayout->setSpacing(8);

	auto *volcAppKeyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Volcengine App Key (Optional · Legacy X-Api-App-Key)"),
		_copilotSttVolcContainer);
	_copilotSttAppKeyHintIcon = makeHintIcon(volcAppKeyLabel->text(), QString(), _copilotSttVolcContainer);
	_copilotSttAppKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotSttAppKeyHintIcon"));
	auto *volcAppKeyRow = new QHBoxLayout();
	volcAppKeyRow->setSpacing(8);
	_copilotSttAppKey = new QLineEdit(_copilotSttVolcContainer);
	_copilotSttAppKey->setObjectName(QStringLiteral("settingsCopilotSttAppKey"));
	_copilotSttAppKey->setAccessibleName(volcAppKeyLabel->text());
	_copilotSttAppKey->setEchoMode(QLineEdit::Password);
	_copilotSttAppKey->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — leave blank for single X-Api-Key auth"));
	volcAppKeyLabel->setBuddy(_copilotSttAppKey);
	volcAppKeyRow->addWidget(_copilotSttAppKey, 1);

	_copilotSaveSttAppKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save"), _copilotSttVolcContainer);
	_copilotSaveSttAppKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveSttAppKey"));
	_copilotSaveSttAppKeyBtn->setAutoDefault(false);
	volcAppKeyRow->addWidget(_copilotSaveSttAppKeyBtn);

	_copilotClearSttAppKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear"), _copilotSttVolcContainer);
	_copilotClearSttAppKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearSttAppKey"));
	_copilotClearSttAppKeyBtn->setAutoDefault(false);
	volcAppKeyRow->addWidget(_copilotClearSttAppKeyBtn);

	auto *volcKeyBlock = new QVBoxLayout();
	volcKeyBlock->setSpacing(4);
	volcKeyBlock->addLayout(makeTitleWithHintRow(volcAppKeyLabel, _copilotSttAppKeyHintIcon));
	volcKeyBlock->addLayout(volcAppKeyRow);
	volcLayout->addLayout(volcKeyBlock);

	auto *volcResLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Volcengine Resource ID"),
		_copilotSttVolcContainer);
	_copilotSttResourceIdCombo = createAccessibleComboBox(_copilotSttVolcContainer);
	_copilotSttResourceIdCombo->setObjectName(QStringLiteral("settingsCopilotSttResourceId"));
	_copilotSttResourceIdCombo->setAccessibleName(volcResLabel->text());
	_copilotSttResourceIdCombo->setEditable(true);
	_copilotSttResourceIdCombo->setInsertPolicy(QComboBox::NoInsert);
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.seedasr.sauc.duration"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.seedasr.sauc.concurrent"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.bigasr.sauc.duration"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.bigasr.sauc.concurrent"));
	AppTheme::styleChoiceControls(*_copilotSttResourceIdCombo, AppTheme::Tone::Light);
	volcResLabel->setBuddy(_copilotSttResourceIdCombo);
	volcLayout->addWidget(makeFieldBlock(volcResLabel, _copilotSttResourceIdCombo, _copilotSttVolcContainer));
	_copilotSttVolcContainer->hide();
	sttEpLayout->addWidget(_copilotSttVolcContainer);

	// DashScope Workspace ID auxiliary container
	_copilotSttWorkspaceContainer = new QWidget(_copilotSttEndpointContainer);
	_copilotSttWorkspaceContainer->setObjectName(QStringLiteral("settingsCopilotSttWorkspaceContainer"));
	auto *wsLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "DashScope Workspace ID"),
		_copilotSttWorkspaceContainer);
	_copilotSttWorkspaceEdit = new QLineEdit(_copilotSttWorkspaceContainer);
	_copilotSttWorkspaceEdit->setObjectName(QStringLiteral("settingsCopilotSttWorkspace"));
	_copilotSttWorkspaceEdit->setAccessibleName(wsLabel->text());
	_copilotSttWorkspaceEdit->setClearButtonEnabled(true);
	_copilotSttWorkspaceEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — X-DashScope-WorkSpace"));
	wsLabel->setBuddy(_copilotSttWorkspaceEdit);
	auto *wsLayout = new QVBoxLayout(_copilotSttWorkspaceContainer);
	wsLayout->setContentsMargins(0, 0, 0, 0);
	wsLayout->setSpacing(4);
	wsLayout->addWidget(wsLabel);
	wsLayout->addWidget(_copilotSttWorkspaceEdit);
	_copilotSttWorkspaceContainer->hide();
	sttEpLayout->addWidget(_copilotSttWorkspaceContainer);

	_copilotSttEndpointContainer->hide();
	sttLayout->addWidget(_copilotSttEndpointContainer);

	// 识别模型整行
	auto *sttModelLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Model"), sttCard);
	sttModelLabel->setWordWrap(false);
	sttModelLabel->setProperty("keepSingleLine", true);
	_copilotSttModelCombo = createElidedAccessibleComboBox(sttCard);
	_copilotSttModelCombo->setObjectName(QStringLiteral("settingsCopilotSttModel"));
	_copilotSttModelCombo->setAccessibleName(sttModelLabel->text());
	_copilotSttModelCombo->setEditable(true);
	_copilotSttModelCombo->setInsertPolicy(QComboBox::NoInsert);
	AppTheme::styleChoiceControls(*_copilotSttModelCombo, AppTheme::Tone::Light);
	if (auto *sttEdit = _copilotSttModelCombo->lineEdit()) {
		sttEdit->setPlaceholderText(QCoreApplication::translate(
			"MeetingUI",
			"Auto (e.g. gpt-4o-transcribe / paraformer-realtime-v2 / qwen3-asr-flash / whisper-1)"));
	}
	sttModelLabel->setBuddy(_copilotSttModelCombo);
	sttLayout->addWidget(makeFieldBlock(sttModelLabel, _copilotSttModelCombo, sttCard));

	// 2 列行: Recognition Language + Auto-Send Pause
	auto *dictGrid = new QGridLayout();
	dictGrid->setHorizontalSpacing(10);
	dictGrid->setVerticalSpacing(6);

	auto *dictLangLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Recognition Language"), sttCard);
	dictLangLabel->setWordWrap(false);
	dictLangLabel->setProperty("keepSingleLine", true);
	_copilotDictationLangCombo = createAccessibleComboBox(sttCard);
	_copilotDictationLangCombo->setObjectName(QStringLiteral("settingsCopilotDictationLang"));
	_copilotDictationLangCombo->setAccessibleName(dictLangLabel->text());
	_copilotDictationLangCombo->setEditable(true);
	_copilotDictationLangCombo->setInsertPolicy(QComboBox::NoInsert);
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto-detect"), QString());
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Chinese (zh)"), QStringLiteral("zh"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "English (en)"), QStringLiteral("en"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Japanese (ja)"), QStringLiteral("ja"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Korean (ko)"), QStringLiteral("ko"));
	AppTheme::styleChoiceControls(*_copilotDictationLangCombo, AppTheme::Tone::Light);
	if (auto *langEdit = _copilotDictationLangCombo->lineEdit()) {
		langEdit->setPlaceholderText(QStringLiteral("zh / en / auto"));
	}
	dictLangLabel->setBuddy(_copilotDictationLangCombo);
	dictGrid->addWidget(makeFieldBlock(dictLangLabel, _copilotDictationLangCombo, sttCard), 0, 0);

	auto *speechTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Auto-Send Pause"), sttCard);
	speechTimeoutLabel->setWordWrap(false);
	speechTimeoutLabel->setProperty("keepSingleLine", true);
	_copilotSpeechTimeoutSpin = new QSpinBox(sttCard);
	_copilotSpeechTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotSpeechTimeout"));
	_copilotSpeechTimeoutSpin->setAccessibleName(speechTimeoutLabel->text());
	_copilotSpeechTimeoutSpin->setRange(0, 10000);
	_copilotSpeechTimeoutSpin->setSingleStep(250);
	_copilotSpeechTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotSpeechTimeoutSpin->setSpecialValueText(QCoreApplication::translate("MeetingUI", "Manual (0 ms)"));
	speechTimeoutLabel->setBuddy(_copilotSpeechTimeoutSpin);
	dictGrid->addWidget(makeFieldBlock(speechTimeoutLabel, _copilotSpeechTimeoutSpin, sttCard), 0, 1);
	dictGrid->setColumnStretch(0, 1);
	dictGrid->setColumnStretch(1, 1);
	sttLayout->addLayout(dictGrid);

	// 唯一高级折叠区: Advanced Acoustic & Recognition
	_copilotSttAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Acoustic & Recognition"),
		QStringLiteral("settingsCopilotSttAdvToggle"),
		sttCard);
	sttLayout->addWidget(_copilotSttAdvSection);
	auto *sttAdvLayout = _copilotSttAdvSection->bodyLayout();

	auto *sttPromptLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Recognition Prompt"),
		_copilotSttAdvSection->bodyWidget());
	_copilotSttPromptEdit = new QLineEdit(_copilotSttAdvSection->bodyWidget());
	_copilotSttPromptEdit->setObjectName(QStringLiteral("settingsCopilotSttPrompt"));
	_copilotSttPromptEdit->setAccessibleName(sttPromptLabel->text());
	_copilotSttPromptEdit->setClearButtonEnabled(true);
	_copilotSttPromptEdit->setMaxLength(1000);
	_copilotSttPromptEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — names, acronyms, or domain terms to improve accuracy"));
	sttPromptLabel->setBuddy(_copilotSttPromptEdit);
	sttAdvLayout->addWidget(makeFieldBlock(sttPromptLabel, _copilotSttPromptEdit, _copilotSttAdvSection->bodyWidget()));

	auto *vadGrid = new QGridLayout();
	vadGrid->setHorizontalSpacing(10);
	vadGrid->setVerticalSpacing(6);

	auto *vadThresholdLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "VAD Threshold"),
		_copilotSttAdvSection->bodyWidget());
	auto *vadHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "Voice Activity Detection (VAD)"),
		QCoreApplication::translate(
			"MeetingUI",
			"Threshold controls microphone/speaker wake sensitivity (-60 dBFS more sensitive, -15 dBFS stricter). Silence Split sets pause duration to commit a segment in HTTP STT mode."),
		_copilotSttAdvSection->bodyWidget());
	vadHintIcon->setObjectName(QStringLiteral("settingsCopilotVadHintIcon"));
	_copilotVadThresholdSpin = new QSpinBox(_copilotSttAdvSection->bodyWidget());
	_copilotVadThresholdSpin->setObjectName(QStringLiteral("settingsCopilotVadThreshold"));
	_copilotVadThresholdSpin->setAccessibleName(vadThresholdLabel->text());
	_copilotVadThresholdSpin->setRange(-60, -15);
	_copilotVadThresholdSpin->setSingleStep(1);
	_copilotVadThresholdSpin->setSuffix(QStringLiteral(" dBFS"));
	_copilotVadThresholdSpin->setValue(-42);
	vadThresholdLabel->setBuddy(_copilotVadThresholdSpin);
	auto *vadLeftCol = new QVBoxLayout();
	vadLeftCol->setSpacing(4);
	vadLeftCol->addLayout(makeTitleWithHintRow(vadThresholdLabel, vadHintIcon));
	vadLeftCol->addWidget(_copilotVadThresholdSpin);
	vadGrid->addLayout(vadLeftCol, 0, 0);

	auto *vadSilenceLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "VAD Silence Split"),
		_copilotSttAdvSection->bodyWidget());
	_copilotVadSilenceCommitSpin = new QSpinBox(_copilotSttAdvSection->bodyWidget());
	_copilotVadSilenceCommitSpin->setObjectName(QStringLiteral("settingsCopilotVadSilenceCommit"));
	_copilotVadSilenceCommitSpin->setAccessibleName(vadSilenceLabel->text());
	_copilotVadSilenceCommitSpin->setRange(200, 3000);
	_copilotVadSilenceCommitSpin->setSingleStep(50);
	_copilotVadSilenceCommitSpin->setSuffix(QStringLiteral(" ms"));
	_copilotVadSilenceCommitSpin->setValue(600);
	vadSilenceLabel->setBuddy(_copilotVadSilenceCommitSpin);
	vadGrid->addWidget(makeFieldBlock(vadSilenceLabel, _copilotVadSilenceCommitSpin, _copilotSttAdvSection->bodyWidget()), 0, 1);
	vadGrid->setColumnStretch(0, 1);
	vadGrid->setColumnStretch(1, 1);
	sttAdvLayout->addLayout(vadGrid);

	sttOuter->addStretch(1);

	// =========================================================================
	// Sub-Page 2: Live Translation & Room Agent (Global)
	// =========================================================================
	const auto [trContent, trOuter] = makeSubScrollPage();

	const QString trCardTitle = QCoreApplication::translate("MeetingUI", "Live Translation & Room Agent");
	auto *trHintIcon = makeHintIcon(
		trCardTitle,
		QCoreApplication::translate(
			"MeetingUI",
			"Speech recognition reuses your \"Speech & Dictation (ASR)\" API settings; caption translation reuses \"Models & Chat\" when enabled. You can start/stop translation or claim/release the room Agent role anytime from the top Agent badge in a meeting."),
		trContent,
		15);
	trHintIcon->setObjectName(QStringLiteral("settingsTranslationHintIcon"));
	QVBoxLayout *trLayout = nullptr;
	auto *trCard = makeSettingsCard(
		trCardTitle,
		trContent,
		&trLayout,
		nullptr,
		makeScopeAccessory(
			trHintIcon,
			QCoreApplication::translate("MeetingUI", "Global"),
			QStringLiteral("muted"),
			trContent));
	trLayout->setContentsMargins(16, 12, 16, 12);
	trLayout->setSpacing(8);
	trOuter->addWidget(trCard);

	_translationEnableLocal = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Enable local meeting transcription/translation on join"), trCard);
	_translationEnableLocal->setObjectName(QStringLiteral("settingsTranslationEnableLocal"));
	trLayout->addWidget(_translationEnableLocal);

	auto *trRoleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Working Mode"), trCard);
	_translationRoleModeCombo = createAccessibleComboBox(trCard);
	_translationRoleModeCombo->setObjectName(QStringLiteral("settingsTranslationRoleMode"));
	_translationRoleModeCombo->setAccessibleName(trRoleLabel->text());
	_translationRoleModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Local translation only"),
		static_cast<int>(OpenMeeting::MeetingTranslationRoleMode::LocalOnly));
	_translationRoleModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Also act as room Agent when available"),
		static_cast<int>(OpenMeeting::MeetingTranslationRoleMode::RoomAgentIfAvailable));
	AppTheme::styleChoiceControls(*_translationRoleModeCombo, AppTheme::Tone::Light);
	trRoleLabel->setBuddy(_translationRoleModeCombo);
	trLayout->addWidget(makeFieldBlock(trRoleLabel, _translationRoleModeCombo, trCard));

	// 字幕样式常驻外露（不随翻译开关收起）
	auto *captionHeaderRow = new QHBoxLayout();
	captionHeaderRow->setContentsMargins(0, 0, 0, 0);
	captionHeaderRow->setSpacing(8);
	auto *captionDispLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Caption Style"), trCard);
	captionDispLabel->setWordWrap(false);
	captionDispLabel->setProperty("keepSingleLine", true);
	captionHeaderRow->addWidget(captionDispLabel);
	captionHeaderRow->addStretch(1);
	_captionStyleReuseHint = new QLabel(
		QCoreApplication::translate("MeetingUI", "Applies to reused room captions as well"),
		trCard);
	_captionStyleReuseHint->setObjectName(QStringLiteral("hintLabel"));
	_captionStyleReuseHint->setWordWrap(false);
	_captionStyleReuseHint->setProperty("keepSingleLine", true);
	captionHeaderRow->addWidget(_captionStyleReuseHint);

	_captionDisplayModeCombo = createAccessibleComboBox(trCard);
	_captionDisplayModeCombo->setObjectName(QStringLiteral("settingsCaptionDisplayMode"));
	_captionDisplayModeCombo->setAccessibleName(captionDispLabel->text());
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Bilingual (Original + Translation)"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::Bilingual));
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Translation only"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::TranslatedOnly));
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Original only"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::OriginalOnly));
	AppTheme::styleChoiceControls(*_captionDisplayModeCombo, AppTheme::Tone::Light);
	captionDispLabel->setBuddy(_captionDisplayModeCombo);
	auto *captionBlockLayout = new QVBoxLayout();
	captionBlockLayout->setSpacing(4);
	captionBlockLayout->addLayout(captionHeaderRow);
	captionBlockLayout->addWidget(_captionDisplayModeCombo);
	trLayout->addLayout(captionBlockLayout);

	_translationPreferRoomAgent = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Reuse existing room Agent captions instead of running local API"), trCard);
	_translationPreferRoomAgent->setObjectName(QStringLiteral("settingsTranslationPreferRoomAgent"));
	trLayout->addWidget(_translationPreferRoomAgent);

	_translationIncludeLocalMic = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Include my own microphone speech in local translation"), trCard);
	_translationIncludeLocalMic->setObjectName(QStringLiteral("settingsTranslationIncludeLocalMic"));
	trLayout->addWidget(_translationIncludeLocalMic);

	// 翻译开关动态联动容器
	auto *translatePanel = new QFrame(trCard);
	translatePanel->setObjectName(QStringLiteral("settingsSubPanel"));
	auto *translatePanelLayout = new QVBoxLayout(translatePanel);
	translatePanelLayout->setContentsMargins(12, 8, 12, 8);
	translatePanelLayout->setSpacing(8);

	_translationEnableOutput = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Translate captions to target language (uses Copilot model)"),
		translatePanel);
	_translationEnableOutput->setObjectName(QStringLiteral("settingsTranslationEnableOutput"));
	translatePanelLayout->addWidget(_translationEnableOutput);

	_translationTargetContainer = new QWidget(translatePanel);
	auto *trTargetLayout = new QVBoxLayout(_translationTargetContainer);
	trTargetLayout->setContentsMargins(0, 4, 0, 0);
	trTargetLayout->setSpacing(4);
	auto *trLangLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Target Language"), _translationTargetContainer);
	_translationTargetLangCombo = createAccessibleComboBox(_translationTargetContainer);
	_translationTargetLangCombo->setObjectName(QStringLiteral("settingsTranslationTargetLang"));
	_translationTargetLangCombo->setAccessibleName(trLangLabel->text());
	_translationTargetLangCombo->setEditable(true);
	_translationTargetLangCombo->setInsertPolicy(QComboBox::NoInsert);
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Chinese (zh)"), QStringLiteral("zh"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "English (en)"), QStringLiteral("en"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Japanese (ja)"), QStringLiteral("ja"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Korean (ko)"), QStringLiteral("ko"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "French (fr)"), QStringLiteral("fr"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "German (de)"), QStringLiteral("de"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Spanish (es)"), QStringLiteral("es"));
	AppTheme::styleChoiceControls(*_translationTargetLangCombo, AppTheme::Tone::Light);
	if (auto *trLangEdit = _translationTargetLangCombo->lineEdit()) {
		trLangEdit->setPlaceholderText(QStringLiteral("zh / en / ja / pt-BR"));
	}
	trLangLabel->setBuddy(_translationTargetLangCombo);
	trTargetLayout->addWidget(trLangLabel);
	trTargetLayout->addWidget(_translationTargetLangCombo);
	_translationTargetContainer->hide();
	translatePanelLayout->addWidget(_translationTargetContainer);
	trLayout->addWidget(translatePanel);

	// 唯一平级高级折叠区: Advanced Translation & Streaming
	_copilotTrAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Translation & Streaming"),
		QStringLiteral("settingsTranslationAdvToggle"),
		trCard);
	trLayout->addWidget(_copilotTrAdvSection);
	auto *trAdvLayout = _copilotTrAdvSection->bodyLayout();

	// 组 1: Dedicated Translation Override
	auto *overrideHeaderRow = new QHBoxLayout();
	overrideHeaderRow->setContentsMargins(0, 0, 0, 0);
	overrideHeaderRow->setSpacing(8);
	auto *overrideGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Dedicated Translation Override"),
		_copilotTrAdvSection->bodyWidget());
	overrideGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	overrideGroupTitle->setWordWrap(false);
	overrideGroupTitle->setProperty("keepSingleLine", true);
	overrideHeaderRow->addWidget(overrideGroupTitle);
	overrideHeaderRow->addStretch(1);
	_translationOverrideInactiveHint = new QLabel(
		QCoreApplication::translate("MeetingUI", "Enabled when caption translation is on"),
		_copilotTrAdvSection->bodyWidget());
	_translationOverrideInactiveHint->setObjectName(QStringLiteral("hintLabel"));
	_translationOverrideInactiveHint->setWordWrap(false);
	_translationOverrideInactiveHint->setProperty("keepSingleLine", true);
	overrideHeaderRow->addWidget(_translationOverrideInactiveHint);
	trAdvLayout->addLayout(overrideHeaderRow);

	_translationOverrideGroup = new QWidget(_copilotTrAdvSection->bodyWidget());
	auto *overrideGroupLayout = new QVBoxLayout(_translationOverrideGroup);
	overrideGroupLayout->setContentsMargins(0, 0, 0, 0);
	overrideGroupLayout->setSpacing(8);

	auto *trProfileLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Profile"),
		_translationOverrideGroup);
	_translationProfileCombo = createElidedAccessibleComboBox(_translationOverrideGroup);
	_translationProfileCombo->setObjectName(QStringLiteral("settingsTranslationProfile"));
	_translationProfileCombo->setAccessibleName(trProfileLabel->text());
	_translationProfileCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Follow Active Copilot Profile"), QString());
	AppTheme::styleChoiceControls(*_translationProfileCombo, AppTheme::Tone::Light);
	trProfileLabel->setBuddy(_translationProfileCombo);
	overrideGroupLayout->addWidget(makeFieldBlock(trProfileLabel, _translationProfileCombo, _translationOverrideGroup));

	auto *trModelLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Model Override"),
		_translationOverrideGroup);
	_translationModelEdit = new QLineEdit(_translationOverrideGroup);
	_translationModelEdit->setObjectName(QStringLiteral("settingsTranslationModelOverride"));
	_translationModelEdit->setAccessibleName(trModelLabel->text());
	_translationModelEdit->setClearButtonEnabled(true);
	_translationModelEdit->setMaxLength(256);
	_translationModelEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — e.g. gpt-4o-mini / qwen-turbo (leave blank to use profile model)"));
	trModelLabel->setBuddy(_translationModelEdit);
	overrideGroupLayout->addWidget(makeFieldBlock(trModelLabel, _translationModelEdit, _translationOverrideGroup));

	auto *trPromptLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Prompt"),
		_translationOverrideGroup);
	_translationPromptEdit = new QPlainTextEdit(_translationOverrideGroup);
	_translationPromptEdit->setObjectName(QStringLiteral("settingsTranslationPrompt"));
	_translationPromptEdit->setAccessibleName(trPromptLabel->text());
	_translationPromptEdit->setFixedHeight(58);
	_translationPromptEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — leave blank for default prompt; supports {target_language} placeholder"));
	trPromptLabel->setBuddy(_translationPromptEdit);
	overrideGroupLayout->addWidget(makeFieldBlock(trPromptLabel, _translationPromptEdit, _translationOverrideGroup));

	_translationPromptCommitTimer = new QTimer(this);
	_translationPromptCommitTimer->setSingleShot(true);
	_translationPromptCommitTimer->setInterval(300);

	trAdvLayout->addWidget(_translationOverrideGroup);

	auto *trDivider = new QFrame(_copilotTrAdvSection->bodyWidget());
	trDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	trDivider->setFixedHeight(1);
	trAdvLayout->addWidget(trDivider);

	// 组 2: Streaming Concurrency & Idle Release
	auto *streamGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Streaming Concurrency & Idle Release"),
		_copilotTrAdvSection->bodyWidget());
	streamGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	trAdvLayout->addWidget(streamGroupTitle);

	auto *trConcurrencyGrid = new QGridLayout();
	trConcurrencyGrid->setHorizontalSpacing(10);
	trConcurrencyGrid->setVerticalSpacing(6);

	auto *trConcurrencyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Max Concurrent Streaming Connections"),
		_copilotTrAdvSection->bodyWidget());
	auto *trConcurrencyHint = makeHintIcon(
		trConcurrencyLabel->text(),
		QCoreApplication::translate(
			"MeetingUI",
			"Limits simultaneous per-speaker WebSocket ASR connections during a meeting. Inactive speakers auto-release after the configured idle timeout; speech wakes them with a 250 ms pre-roll buffer."),
		_copilotTrAdvSection->bodyWidget());
	trConcurrencyHint->setObjectName(QStringLiteral("settingsTranslationMaxConcurrentHint"));
	_translationMaxConcurrentStreamsSpin = new QSpinBox(_copilotTrAdvSection->bodyWidget());
	_translationMaxConcurrentStreamsSpin->setObjectName(QStringLiteral("settingsTranslationMaxConcurrentStreams"));
	_translationMaxConcurrentStreamsSpin->setAccessibleName(trConcurrencyLabel->text());
	_translationMaxConcurrentStreamsSpin->setRange(1, 8);
	_translationMaxConcurrentStreamsSpin->setValue(3);
	trConcurrencyLabel->setBuddy(_translationMaxConcurrentStreamsSpin);
	auto *trConcLeftCol = new QVBoxLayout();
	trConcLeftCol->setSpacing(4);
	trConcLeftCol->addLayout(makeTitleWithHintRow(trConcurrencyLabel, trConcurrencyHint));
	trConcLeftCol->addWidget(_translationMaxConcurrentStreamsSpin);
	trConcurrencyGrid->addLayout(trConcLeftCol, 0, 0);

	auto *trIdleLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Idle Stream Release"),
		_copilotTrAdvSection->bodyWidget());
	_translationIdleReleaseSpin = new QSpinBox(_copilotTrAdvSection->bodyWidget());
	_translationIdleReleaseSpin->setObjectName(QStringLiteral("settingsTranslationIdleRelease"));
	_translationIdleReleaseSpin->setAccessibleName(trIdleLabel->text());
	_translationIdleReleaseSpin->setRange(3, 120);
	_translationIdleReleaseSpin->setSingleStep(1);
	_translationIdleReleaseSpin->setSuffix(QStringLiteral(" s"));
	_translationIdleReleaseSpin->setValue(12);
	trIdleLabel->setBuddy(_translationIdleReleaseSpin);
	trConcurrencyGrid->addWidget(makeFieldBlock(trIdleLabel, _translationIdleReleaseSpin, _copilotTrAdvSection->bodyWidget()), 0, 1);
	trConcurrencyGrid->setColumnStretch(0, 1);
	trConcurrencyGrid->setColumnStretch(1, 1);
	trAdvLayout->addLayout(trConcurrencyGrid);

	trOuter->addStretch(1);

	// =========================================================================
	// Signal Connections
	// =========================================================================
	connect(tabGroup, &QButtonGroup::idClicked, this, &SettingsDialog::setCopilotSubTab);

	_copilotChatAdvSection->setResetHandler([this] { resetCopilotChatAdvancedDefaults(); });
	_copilotSttAdvSection->setResetHandler([this] { resetCopilotSttAdvancedDefaults(); });
	_copilotTrAdvSection->setResetHandler([this] { resetCopilotTranslationAdvancedDefaults(); });

	const auto commitLive = [this] {
		if (!_updatingUi) {
			commitCopilotProfile(false);
		}
	};
	const auto onConnectionFieldEdited = [this] {
		if (!_updatingUi) {
			resetCopilotTestStatus();
		}
	};
	const auto onSttFieldEdited = [this] {
		if (!_updatingUi) {
			resetCopilotSttTestStatus();
		}
	};
	connect(_copilotProfileSelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, &SettingsDialog::onCopilotProfileSelected);
	connect(_copilotNewProfileBtn, &QPushButton::clicked, this, [this] {
		createNewCopilotProfile(false);
	});
	connect(_copilotProfileMoreBtn, &QToolButton::clicked, this, &SettingsDialog::showCopilotProfileMenu);
	connect(_copilotProfileName, &QLineEdit::editingFinished, this, [this, commitLive] {
		commitLive();
		updateCopilotSttModeUi();
	});
	connect(_copilotBaseUrl, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
		onConnectionFieldEdited();
		if (!_updatingUi && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotKeyStatus();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
	});
	connect(_copilotBaseUrl, &QLineEdit::editingFinished, this, [this, commitLive] {
		updateCopilotSttProtocolUi();
		commitLive();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
		if (_updatingUi || !_copilotBaseUrl) {
			return;
		}
		const QString currentBase = _copilotBaseUrl->text().trimmed();
		if (currentBase != _copilotLastModelFetchBaseUrl) {
			refreshCopilotModels(true);
		}
	});
	if (auto *modelEdit = _copilotModelCombo->lineEdit()) {
		connect(modelEdit, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
			onConnectionFieldEdited();
			updateCopilotReasoningCapabilityHint();
		});
		connect(modelEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotModelCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [this, commitLive](int) {
			if (!_updatingUi && _copilotModelCombo &&
				_copilotModelCombo->currentText().trimmed() != copilotCoordinator().profile().model) {
				resetCopilotTestStatus();
			}
			updateCopilotReasoningCapabilityHint();
			commitLive();
		});
	connect(_copilotReasoningCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [commitLive](int) { commitLive(); });
	connect(_copilotApiKey, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
		onConnectionFieldEdited();
		if (!_updatingUi && !copilotCoordinator().hasDictationApiKey() && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotKeyStatus();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
	});
	connect(_copilotCustomHeaders, &QPlainTextEdit::textChanged, this, [this, onConnectionFieldEdited] {
		onConnectionFieldEdited();
		if (!_updatingUi && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotAdvSummaries();
	});
	connect(_copilotRefreshModelsBtn, &QPushButton::clicked, this, [this] {
		refreshCopilotModels(false);
	});
	connect(_copilotProtocolCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this, commitLive](int) {
			updateCopilotBaseUrlPlaceholder();
			updateCopilotReasoningCapabilityHint();
			commitLive();
		});
	connect(_copilotEncryptKeyCheck, &QCheckBox::toggled, this, [this, commitLive](bool) {
		commitLive();
		updateCopilotAdvSummaries();
	});
	connect(_copilotPersistHistoryCheck, &QCheckBox::toggled, this, [this, commitLive](bool) {
		commitLive();
		updateCopilotAdvSummaries();
	});
	connect(_copilotClearHistoryBtn, &QPushButton::clicked, this, [this] {
		copilotCoordinator().clearAllSavedSessions();
		setCopilotTestStatus(QStringLiteral("ok"),
			QCoreApplication::translate("MeetingUI", "Saved Copilot chat history cleared for the current account."));
	});
	connect(_copilotMaxHistorySpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotTemperatureSpin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
		this, [this, commitLive](double) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotConnectTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotIdleTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotSttModeFollowBtn, &QPushButton::clicked, this, [this] {
		onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode::FollowChat);
	});
	connect(_copilotSttModeCustomBtn, &QPushButton::clicked, this, [this] {
		onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	});
	connect(_copilotSttProfileSelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, &SettingsDialog::onCopilotSpeechProfileSelected);
	connect(_copilotSttNewProfileBtn, &QPushButton::clicked, this, [this] {
		createNewCopilotSpeechProfile(false);
	});
	connect(_copilotSttProfileMoreBtn, &QToolButton::clicked, this, &SettingsDialog::showCopilotSpeechProfileMenu);
	connect(_copilotSttProfileName, &QLineEdit::editingFinished, this, commitLive);
	connect(_copilotSttBaseUrl, &QLineEdit::textChanged, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
		}
	});
	connect(_copilotSttBaseUrl, &QLineEdit::editingFinished, this, [this, commitLive] {
		updateCopilotSttProtocolUi();
		commitLive();
		updateCopilotSttKeyStatus();
		refreshCopilotSpeechModels(true);
	});
	connect(_copilotSaveSttKeyBtn, &QPushButton::clicked, this, &SettingsDialog::saveCopilotSttKey);
	connect(_copilotClearSttKeyBtn, &QPushButton::clicked, this, &SettingsDialog::clearCopilotSttKey);
	connect(_copilotSaveSttAppKeyBtn, &QPushButton::clicked, this, &SettingsDialog::saveCopilotSttAppKey);
	connect(_copilotClearSttAppKeyBtn, &QPushButton::clicked, this, &SettingsDialog::clearCopilotSttAppKey);
	connect(_copilotSttTestBtn, &QPushButton::clicked, this, &SettingsDialog::testCopilotSttConnection);
	connect(_copilotSttApiKey, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttKeyStatus();
		}
	});
	connect(_copilotSttAppKey, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttAppKeyStatus();
		}
	});
	connect(_copilotSttProtocolCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this, onSttFieldEdited, commitLive](int) {
			if (!_updatingUi &&
				selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile &&
				_copilotSttProtocolCombo) {
				const auto selectedProto = OpenMeeting::agentSpeechProtocolModeFromInt(
					_copilotSttProtocolCombo->currentData().toInt());
				if (selectedProto != OpenMeeting::AgentSpeechProtocolMode::Auto) {
					applyCopilotSpeechProtocolPreset(selectedProto, /*forceOverwrite=*/true);
				}
			}
			onSttFieldEdited();
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitLive();
		});
	if (auto *resEdit = _copilotSttResourceIdCombo->lineEdit()) {
		connect(resEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
			onSttFieldEdited();
		});
		connect(resEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotSttResourceIdCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [onSttFieldEdited, commitLive](int) {
			onSttFieldEdited();
			commitLive();
		});
	connect(_copilotSttWorkspaceEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
		onSttFieldEdited();
	});
	connect(_copilotSttWorkspaceEdit, &QLineEdit::editingFinished, this, commitLive);
	if (auto *sttModelEdit = _copilotSttModelCombo->lineEdit()) {
		connect(sttModelEdit, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
			if (!_updatingUi) {
				_copilotSttModelAutoSelected = false;
				onSttFieldEdited();
				updateCopilotSttProtocolUi(false);
				updateCopilotSttKeyStatus();
			}
		});
		connect(sttModelEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotSttModelCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [this, onSttFieldEdited, commitLive](int) {
			_copilotSttModelAutoSelected = false;
			onSttFieldEdited();
			updateCopilotSttProtocolUi(false);
			updateCopilotSttKeyStatus();
			commitLive();
		});
	if (auto *langEdit = _copilotDictationLangCombo->lineEdit()) {
		connect(langEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
			onSttFieldEdited();
		});
		connect(langEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotDictationLangCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [onSttFieldEdited, commitLive](int) {
			onSttFieldEdited();
			commitLive();
		});
	connect(_copilotSpeechTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [commitLive](int) { commitLive(); });
	connect(_copilotVadThresholdSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotVadSilenceCommitSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotSttPromptEdit, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		onSttFieldEdited();
		updateCopilotAdvSummaries();
	});
	connect(_copilotSttPromptEdit, &QLineEdit::editingFinished, this, commitLive);
	connect(_copilotSaveKeyBtn, &QPushButton::clicked, this, &SettingsDialog::saveCopilotKey);
	connect(_copilotClearKeyBtn, &QPushButton::clicked, this, &SettingsDialog::clearCopilotKey);
	connect(_copilotTestBtn, &QPushButton::clicked, this, &SettingsDialog::testCopilotConnection);

	updateCopilotSttModeUi();
	updateCopilotTranslationLinkedUi();
	updateCopilotAdvSummaries();

	return page;
}

void SettingsDialog::setCopilotModelHint(const QString &text, const QString &tone) {
	if (!_copilotModelHintIcon) {
		return;
	}
	const QString trimmed = text.trimmed();
	setHintIcon(
		_copilotModelHintIcon,
		trimmed.isEmpty() ? defaultCopilotModelHint() : trimmed,
		trimmed.isEmpty() ? QString() : tone);
}

void SettingsDialog::resetCopilotModelHint() {
	setCopilotModelHint(defaultCopilotModelHint());
}

void SettingsDialog::setCopilotTestStatus(const QString &tone, const QString &text) {
	if (!_copilotTestStatus) {
		return;
	}
	_copilotTestStatus->setProperty("statusTone", tone);
	if (auto *st = _copilotTestStatus->style()) {
		st->unpolish(_copilotTestStatus);
		st->polish(_copilotTestStatus);
	}
	_copilotTestStatus->setText(text);
	_copilotTestStatus->setVisible(!text.trimmed().isEmpty());
}

void SettingsDialog::resetCopilotTestStatus() {
	if (_agentCoordinator || _ownedAgentCoordinator) {
		copilotCoordinator().cancelProbe();
	}
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(true);
	}
	setCopilotTestStatus(QString(), QString());
}

void SettingsDialog::setCopilotSttTestStatus(const QString &tone, const QString &text) {
	if (!_copilotSttTestStatus) {
		return;
	}
	_copilotSttTestStatus->setProperty("statusTone", tone);
	if (auto *st = _copilotSttTestStatus->style()) {
		st->unpolish(_copilotSttTestStatus);
		st->polish(_copilotSttTestStatus);
	}
	_copilotSttTestStatus->setText(text);
	_copilotSttTestStatus->setVisible(!text.trimmed().isEmpty());
}

void SettingsDialog::resetCopilotSttTestStatus() {
	if (_agentCoordinator || _ownedAgentCoordinator) {
		copilotCoordinator().cancelSpeechProbe();
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(true);
	}
	setCopilotSttTestStatus(QString(), QString());
}

OpenMeeting::AgentSpeechSourceMode SettingsDialog::selectedCopilotSpeechSourceMode() const {
	if (_copilotSttModeCustomBtn && _copilotSttModeCustomBtn->isChecked()) {
		return OpenMeeting::AgentSpeechSourceMode::CustomProfile;
	}
	if (_copilotSttModeFollowBtn && _copilotSttModeFollowBtn->isChecked()) {
		return OpenMeeting::AgentSpeechSourceMode::FollowChat;
	}
	return const_cast<SettingsDialog *>(this)->copilotCoordinator().speechSourceMode();
}

bool SettingsDialog::copilotSttReusesChatHost() const {
	if (selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat) {
		return true;
	}
	const auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	const QString sttUrl = _copilotSttBaseUrl
		? _copilotSttBaseUrl->text().trimmed()
		: coord.dictationSttBaseUrl();
	const auto sttProto = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();
	const QString modelText = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: coord.dictationSttModel();
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: coord.profile().baseUrl;
	const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
		sttProto, sttUrl, chatUrl, modelText);
	if (sttUrl.isEmpty()) {
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DeepgramWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs) {
			return OpenMeeting::AgentCoordinator::isSameSpeechOrigin(
				OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto), chatUrl);
		}
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			return OpenMeeting::isDashScopeAgentHost(QUrl(chatUrl));
		}
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::XaiSttWs) {
			return OpenMeeting::isXaiAgentHost(QUrl(chatUrl));
		}
		return true;
	}
	return OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, chatUrl) ||
		OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, coord.profile().baseUrl);
}

void SettingsDialog::maybeAutoRefreshCopilotModels() {
	refreshCopilotSpeechModels(true);
	if (!_copilotModelCombo || !_copilotBaseUrl || _copilotModelCombo->count() > 0) {
		return;
	}
	const QString currentBase = _copilotBaseUrl->text().trimmed();
	if (currentBase.isEmpty() || _copilotLastModelFetchBaseUrl == currentBase) {
		return;
	}
	if (!OpenMeeting::evaluateAgentEndpoint(currentBase).allowed) {
		return;
	}
	refreshCopilotModels(true);
}

void SettingsDialog::syncCopilotControls() {
	if (!_copilotProfileName) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (!_agentProfileConn) {
		_agentProfileConn = connect(&coord, &OpenMeeting::AgentCoordinator::profileChanged,
			this, [this] {
				if (!_updatingUi) {
					syncCopilotControls();
				}
			});
	}
	if (!_agentProfilesConn) {
		_agentProfilesConn = connect(&coord, &OpenMeeting::AgentCoordinator::profilesChanged,
			this, [this] {
				refreshCopilotProfileSelectors();
			});
	}
	if (!_agentSpeechProfilesConn) {
		_agentSpeechProfilesConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechProfilesChanged,
			this, [this] {
				refreshCopilotSpeechProfileSelectors();
			});
	}
	if (!_agentProbeConn) {
		_agentProbeConn = connect(&coord, &OpenMeeting::AgentCoordinator::probeFinished,
			this, [this](bool success, const QString &message) {
				if (_copilotTestBtn) {
					_copilotTestBtn->setEnabled(true);
				}
				const QString modelName = _copilotModelCombo
					? _copilotModelCombo->currentText().trimmed()
					: QString();
				const QString okText = modelName.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Connection verified.")
					: QCoreApplication::translate("MeetingUI", "Connection verified (%1).").arg(modelName);
				setCopilotTestStatus(
					success ? QStringLiteral("ok") : QStringLiteral("error"),
					success ? okText : message);
			});
	}
	if (!_agentSpeechProbeConn) {
		_agentSpeechProbeConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechProbeFinished,
			this, [this](bool success, const QString &message) {
				if (_copilotSttTestBtn) {
					_copilotSttTestBtn->setEnabled(true);
				}
				setCopilotSttTestStatus(
					success ? QStringLiteral("ok") : QStringLiteral("error"),
					(success && message.trimmed().isEmpty())
						? QCoreApplication::translate("MeetingUI", "Connection verified.")
						: message);
			});
	}

	if (!_agentSpeechModelsConn) {
		_agentSpeechModelsConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechModelsFetched,
			this, [this](bool success, const QStringList &, const QString &) {
				const QString before = _copilotSttModelCombo
					? _copilotSttModelCombo->currentText().trimmed()
					: QString();
				updateCopilotSttModelChoices(success);
				if (success && !_updatingUi && _copilotSttModelCombo &&
					_copilotSttModelCombo->currentText().trimmed() != before) {
					commitCopilotProfile(false);
				}
			});
	}

	if (!_agentModelsConn) {
		_agentModelsConn = connect(&coord, &OpenMeeting::AgentCoordinator::modelsFetched,
			this, [this](bool success, const QStringList &models, const QString &safeError) {
				if (_copilotRefreshModelsBtn) {
					_copilotRefreshModelsBtn->setEnabled(true);
				}
				if (!_copilotModelCombo) {
					return;
				}
				const bool prevUpdating = _updatingUi;
				_updatingUi = true;
				const QSignalBlocker comboBlocker(_copilotModelCombo);
				const QString currentText = _copilotModelCombo->currentText().trimmed();
				_copilotModelCombo->clear();
				for (const QString &m : models) {
					_copilotModelCombo->addItem(m);
				}
				bool switchedAwayFromMissingModel = false;
				if (!models.isEmpty()) {
					const int idx = currentText.isEmpty() ? -1 : _copilotModelCombo->findText(currentText);
					if (idx >= 0) {
						_copilotModelCombo->setCurrentIndex(idx);
					} else {
						// Previous endpoint's model does not exist on this endpoint -> switch to first available model
						_copilotModelCombo->setCurrentIndex(0);
						switchedAwayFromMissingModel = true;
					}
				} else if (!currentText.isEmpty()) {
					_copilotModelCombo->setEditText(currentText);
				}
				_updatingUi = prevUpdating;

				if (!models.isEmpty() && (currentText.isEmpty() || switchedAwayFromMissingModel)) {
					if (switchedAwayFromMissingModel) {
						resetCopilotTestStatus();
					}
					commitCopilotProfile(false);
				}
				updateCopilotSttModelChoices();
				updateCopilotReasoningCapabilityHint();
				if (success) {
					const int filtered = copilotCoordinator().lastFilteredNonChatModelCount();
					if (models.isEmpty()) {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"No models returned; enter model name manually."));
					} else if (filtered > 0) {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"Loaded %1 chat model(s) (%2 non-chat hidden).").arg(models.size()).arg(filtered));
					} else {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"Loaded %1 model(s).").arg(models.size()));
					}
				} else {
					setCopilotModelHint(safeError, QStringLiteral("error"));
				}
			});
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	refreshCopilotProfileSelectors();
	const auto &p = coord.profile();
	_copilotProfileName->setText(p.name);
	const int protoIdx = _copilotProtocolCombo->findData(static_cast<int>(p.protocol));
	_copilotProtocolCombo->setCurrentIndex(protoIdx >= 0 ? protoIdx : 0);
	_copilotBaseUrl->setText(p.baseUrl);
	{
		const QSignalBlocker comboBlocker(_copilotModelCombo);
		const int modelIdx = _copilotModelCombo->findText(p.model);
		if (modelIdx >= 0) {
			_copilotModelCombo->setCurrentIndex(modelIdx);
		} else {
			_copilotModelCombo->setEditText(p.model);
		}
	}
	if (_copilotReasoningCombo) {
		const QSignalBlocker reasoningBlocker(_copilotReasoningCombo);
		const int rIdx = _copilotReasoningCombo->findData(static_cast<int>(p.reasoningEffort));
		_copilotReasoningCombo->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
	}
	_copilotEncryptKeyCheck->setChecked(p.keyStorage == OpenMeeting::KeyStoragePolicy::EncryptedStore);
	if (_copilotPersistHistoryCheck) {
		_copilotPersistHistoryCheck->setChecked(p.persistChatHistory);
	}
	_copilotSystemPrompt->setPlainText(p.systemPrompt);

	QStringList headerLines;
	for (auto it = p.customHeaders.cbegin(); it != p.customHeaders.cend(); ++it) {
		headerLines.push_back(QStringLiteral("%1: %2").arg(it.key(), it.value()));
	}
	_copilotCustomHeaders->setPlainText(headerLines.join(QLatin1Char('\n')));
	_copilotMaxHistorySpin->setValue(p.maxContextMessages);
	_copilotTemperatureSpin->setValue(p.temperature);
	if (_copilotConnectTimeoutSpin) {
		const QSignalBlocker cToBlocker(_copilotConnectTimeoutSpin);
		_copilotConnectTimeoutSpin->setValue(p.connectTimeoutMs);
	}
	if (_copilotIdleTimeoutSpin) {
		const QSignalBlocker iToBlocker(_copilotIdleTimeoutSpin);
		_copilotIdleTimeoutSpin->setValue(p.idleTimeoutMs);
	}
	if (_copilotRefreshModelsBtn) {
		_copilotRefreshModelsBtn->setEnabled(!coord.isFetchingModels());
	}
	const bool isCustomSpeech = (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	if (_copilotSttModeFollowBtn && _copilotSttModeCustomBtn) {
		const QSignalBlocker followBlocker(_copilotSttModeFollowBtn);
		const QSignalBlocker customBlocker(_copilotSttModeCustomBtn);
		_copilotSttModeFollowBtn->setChecked(!isCustomSpeech);
		_copilotSttModeCustomBtn->setChecked(isCustomSpeech);
	}
	refreshCopilotSpeechProfileSelectors();
	const auto &sp = coord.speechProfile();
	if (_copilotSttProfileName) {
		_copilotSttProfileName->setText(sp.name);
	}
	if (_copilotSttBaseUrl) {
		_copilotSttBaseUrl->setText(sp.baseUrl);
	}
	if (_copilotSttProtocolCombo) {
		const QSignalBlocker sttProtoBlocker(_copilotSttProtocolCombo);
		const int pIdx = _copilotSttProtocolCombo->findData(static_cast<int>(coord.dictationSpeechProtocol()));
		_copilotSttProtocolCombo->setCurrentIndex(pIdx >= 0 ? pIdx : 0);
	}
	const QString syncedEffectiveSttUrl = coord.dictationSttBaseUrl().trimmed().isEmpty()
		? p.baseUrl
		: coord.dictationSttBaseUrl().trimmed();
	_copilotLastSttEndpointKey = OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(syncedEffectiveSttUrl);
	_copilotSttModelAutoSelected = false;
	if (_copilotSttModelCombo) {
		const QSignalBlocker sttBlocker(_copilotSttModelCombo);
		_copilotSttModelCombo->setEditText(coord.dictationSttModel());
	}
	updateCopilotSttModelChoices();
	if (_copilotDictationLangCombo) {
		const QSignalBlocker langBlocker(_copilotDictationLangCombo);
		const QString lang = coord.dictationLanguage().trimmed();
		const int langIdx = _copilotDictationLangCombo->findData(lang);
		if (langIdx >= 0) {
			_copilotDictationLangCombo->setCurrentIndex(langIdx);
		} else {
			_copilotDictationLangCombo->setEditText(lang);
		}
	}
	if (_copilotSpeechTimeoutSpin) {
		const QSignalBlocker timeoutBlocker(_copilotSpeechTimeoutSpin);
		_copilotSpeechTimeoutSpin->setValue(qBound(0, coord.dictationSpeechTimeoutMs(), 10000));
	}
	if (_copilotVadThresholdSpin) {
		const QSignalBlocker vadDbBlocker(_copilotVadThresholdSpin);
		_copilotVadThresholdSpin->setValue(qBound(-60, coord.dictationVadThresholdDb(), -15));
	}
	if (_copilotVadSilenceCommitSpin) {
		const QSignalBlocker vadSilBlocker(_copilotVadSilenceCommitSpin);
		_copilotVadSilenceCommitSpin->setValue(qBound(200, coord.dictationVadSilenceCommitMs(), 3000));
	}
	if (_copilotSttResourceIdCombo) {
		const QSignalBlocker resBlocker(_copilotSttResourceIdCombo);
		const QString resId = sp.resourceId.trimmed().isEmpty()
			? QStringLiteral("volc.seedasr.sauc.duration")
			: sp.resourceId.trimmed();
		const int rIdx = _copilotSttResourceIdCombo->findText(resId);
		if (rIdx >= 0) {
			_copilotSttResourceIdCombo->setCurrentIndex(rIdx);
		} else {
			_copilotSttResourceIdCombo->setEditText(resId);
		}
	}
	if (_copilotSttWorkspaceEdit) {
		_copilotSttWorkspaceEdit->setText(sp.workspaceId);
	}
	if (_copilotSttPromptEdit) {
		_copilotSttPromptEdit->setText(coord.dictationPrompt());
	}
	updateCopilotSttProtocolUi();
	updateCopilotSttModeUi();
	updateCopilotBaseUrlPlaceholder();
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();
	updateCopilotReasoningCapabilityHint();
	updateCopilotAdvSummaries();
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(!coord.isProbing());
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(!coord.isSpeechProbing());
	}
	_updatingUi = oldUpdating;
}

void SettingsDialog::updateCopilotBaseUrlPlaceholder() {
	if (!_copilotBaseUrl) {
		return;
	}
	const auto protocol = OpenMeeting::agentProtocolDialectFromInt(
		_copilotProtocolCombo ? _copilotProtocolCombo->currentData().toInt() : 0);
	if (protocol == OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.anthropic.com/v1"));
	} else if (protocol == OpenMeeting::AgentProtocolDialect::GoogleGeminiSse) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://generativelanguage.googleapis.com/v1beta"));
	} else if (protocol == OpenMeeting::AgentProtocolDialect::CustomWebhookJson) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://your-agent-webhook.example.com/chat"));
	} else {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1 or http://127.0.0.1:11434/v1"));
	}
}

void SettingsDialog::updateCopilotReasoningCapabilityHint() {
	if (!_copilotReasoningHintIcon || !_copilotModelCombo) {
		return;
	}
	const auto protocol = OpenMeeting::agentProtocolDialectFromInt(
		_copilotProtocolCombo ? _copilotProtocolCombo->currentData().toInt() : 0);
	if (protocol == OpenMeeting::AgentProtocolDialect::CustomWebhookJson) {
		setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
			"Reasoning effort is not sent for the Custom Webhook protocol."));
		return;
	}
	const QString modelId = _copilotModelCombo->currentText().trimmed();
	if (modelId.isEmpty()) {
		if (protocol == OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse) {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls extended thinking budget sent to capable models. Keep Default unless overriding."));
		} else if (protocol == OpenMeeting::AgentProtocolDialect::GoogleGeminiSse) {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls thinking budget/level sent to capable Gemini models. Keep Default unless overriding."));
		} else {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls reasoning_effort sent to reasoning-capable models. Keep Default unless overriding."));
		}
		return;
	}
	const auto caps = copilotCoordinator().resolveModelCapabilities(modelId);
	QString hintText;
	if (caps.supportsReasoningEffort) {
		QStringList effortNames;
		for (const auto e : caps.supportedEfforts) {
			const QString label = OpenMeeting::reasoningEffortShortLabel(e);
			if (!label.isEmpty()) {
				effortNames.push_back(label);
			}
		}
		const QString levels = effortNames.join(QStringLiteral(" / "));
		if (caps.source == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
			hintText = QCoreApplication::translate("MeetingUI",
				"Server /models metadata reports reasoning support (%1).").arg(levels);
		} else {
			hintText = QCoreApplication::translate("MeetingUI",
				"Detected as reasoning-capable by model name rule (%1). You can override if needed.").arg(levels);
		}
	} else if (caps.source == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
		hintText = QCoreApplication::translate("MeetingUI",
			"Server /models metadata did not list reasoning parameters for this model; Default is recommended (manual override allowed).");
	} else {
		hintText = QCoreApplication::translate("MeetingUI",
			"No server capability metadata or known reasoning prefix detected; Default is recommended (manual override allowed).");
	}
	if (caps.supportsVision) {
		hintText += QLatin1Char(' ') + QCoreApplication::translate("MeetingUI", "Image input: supported.");
	} else if (caps.visionSource == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
		hintText += QLatin1Char(' ') + QCoreApplication::translate("MeetingUI", "Image input: not listed by server metadata.");
	}
	setHintIcon(_copilotReasoningHintIcon, hintText);
}

OpenMeeting::AgentProfile SettingsDialog::collectCopilotProfileFromUi() const {
	auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	OpenMeeting::AgentProfile p = coord.profile();
	p.name = _copilotProfileName->text().trimmed();
	if (p.name.isEmpty()) {
		p.name = QStringLiteral("Custom Agent");
	}
	p.protocol = static_cast<OpenMeeting::AgentProtocolDialect>(
		_copilotProtocolCombo->currentData().toInt());
	p.baseUrl = _copilotBaseUrl->text().trimmed();
	p.model = _copilotModelCombo ? _copilotModelCombo->currentText().trimmed() : QString();
	if (_copilotReasoningCombo) {
		p.reasoningEffort = OpenMeeting::reasoningEffortFromInt(
			_copilotReasoningCombo->currentData().toInt());
	}
	p.keyStorage = _copilotEncryptKeyCheck->isChecked()
		? OpenMeeting::KeyStoragePolicy::EncryptedStore
		: OpenMeeting::KeyStoragePolicy::MemoryOnly;
	if (_copilotPersistHistoryCheck) {
		p.persistChatHistory = _copilotPersistHistoryCheck->isChecked();
	}
	p.systemPrompt = _copilotSystemPrompt->toPlainText();
	p.maxContextMessages = _copilotMaxHistorySpin->value();
	p.temperature = _copilotTemperatureSpin->value();
	if (_copilotConnectTimeoutSpin) {
		p.connectTimeoutMs = _copilotConnectTimeoutSpin->value();
	}
	if (_copilotIdleTimeoutSpin) {
		p.idleTimeoutMs = _copilotIdleTimeoutSpin->value();
	}

	QMap<QString, QString> headers;
	const auto lines = _copilotCustomHeaders->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	for (const QString &rawLine : lines) {
		const int colon = rawLine.indexOf(QLatin1Char(':'));
		if (colon <= 0) {
			continue;
		}
		const QString key = rawLine.left(colon).trimmed();
		const QString val = rawLine.mid(colon + 1).trimmed();
		if (OpenMeeting::isValidCustomHeaderName(key) && OpenMeeting::isValidCustomHeaderValue(val)) {
			headers.insert(key, val);
		}
	}
	p.customHeaders = std::move(headers);
	return p;
}

OpenMeeting::AgentSpeechProfile SettingsDialog::collectCopilotSpeechProfileFromUi() const {
	const auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	OpenMeeting::AgentSpeechProfile sp = coord.speechProfile();
	if (_copilotSttProfileName) {
		const QString trimmedName = _copilotSttProfileName->text().trimmed();
		sp.name = trimmedName.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Default Speech")
			: trimmedName;
	}
	if (_copilotSttProtocolCombo) {
		sp.protocol = OpenMeeting::agentSpeechProtocolModeFromInt(
			_copilotSttProtocolCombo->currentData().toInt());
	}
	if (_copilotSttBaseUrl) {
		sp.baseUrl = _copilotSttBaseUrl->text().trimmed();
	}
	if (_copilotSttModelCombo) {
		sp.model = _copilotSttModelCombo->currentText().trimmed();
	}
	if (_copilotSttResourceIdCombo) {
		const QString res = _copilotSttResourceIdCombo->currentText().trimmed();
		sp.resourceId = res.isEmpty() ? QStringLiteral("volc.seedasr.sauc.duration") : res;
	}
	if (_copilotSttWorkspaceEdit) {
		sp.workspaceId = _copilotSttWorkspaceEdit->text().trimmed();
	}
	return sp;
}

void SettingsDialog::commitCopilotProfile(bool includeTypedApiKey) {
	if (!_copilotProfileName || _updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	const auto previous = coord.profile();
	const auto next = collectCopilotProfileFromUi();
	const bool connectionChanged = !previous.hasSameConnectionConfig(next);
	const bool isFollowChat = (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const auto nextSttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();
	const QString nextSttModel = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: coord.dictationSttModel();
	QString nextLang = coord.dictationLanguage();
	if (_copilotDictationLangCombo) {
		nextLang = _copilotDictationLangCombo->currentText().trimmed();
		const int langIdx = _copilotDictationLangCombo->currentIndex();
		if (langIdx >= 0 && nextLang == _copilotDictationLangCombo->itemText(langIdx).trimmed()) {
			nextLang = _copilotDictationLangCombo->itemData(langIdx).toString().trimmed();
		}
	}
	const int nextSpeechTimeoutMs = _copilotSpeechTimeoutSpin
		? _copilotSpeechTimeoutSpin->value()
		: coord.dictationSpeechTimeoutMs();
	const int nextVadThresholdDb = _copilotVadThresholdSpin
		? _copilotVadThresholdSpin->value()
		: coord.dictationVadThresholdDb();
	const int nextVadSilenceCommitMs = _copilotVadSilenceCommitSpin
		? _copilotVadSilenceCommitSpin->value()
		: coord.dictationVadSilenceCommitMs();
	const QString nextSttPrompt = _copilotSttPromptEdit
		? _copilotSttPromptEdit->text().trimmed()
		: coord.dictationPrompt();

	bool sttConnectionChanged =
		(coord.dictationSpeechProtocol() != nextSttProtocol) ||
		(coord.dictationSttModel() != nextSttModel) ||
		(coord.dictationLanguage() != nextLang) ||
		(coord.dictationPrompt() != nextSttPrompt);

	QString nextSttBaseUrl;
	if (!isFollowChat) {
		const auto nextSpeechProf = collectCopilotSpeechProfileFromUi();
		nextSttBaseUrl = nextSpeechProf.baseUrl;
		sttConnectionChanged = sttConnectionChanged || !coord.speechProfile().hasSameConnectionConfig(nextSpeechProf);
	}
	const bool sharedHostChanged =
		(previous.baseUrl != next.baseUrl || previous.customHeaders != next.customHeaders) &&
		(isFollowChat ||
			nextSttBaseUrl.isEmpty() ||
			OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, previous.baseUrl) ||
			OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, next.baseUrl));
	QString typedKey = (includeTypedApiKey && _copilotApiKey)
		? _copilotApiKey->text().trimmed()
		: QString();
	const bool keyChanged = !typedKey.isEmpty();

	coord.setProfile(next);
	if (isFollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(nextSttProtocol);
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(nextSttModel);
		}
	} else {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
		refreshCopilotSpeechProfileSelectors();
	}
	if (_copilotDictationLangCombo) {
		coord.setDictationLanguage(nextLang);
	}
	if (_copilotSpeechTimeoutSpin) {
		coord.setDictationSpeechTimeoutMs(nextSpeechTimeoutMs);
	}
	if (_copilotVadThresholdSpin) {
		coord.setDictationVadThresholdDb(nextVadThresholdDb);
	}
	if (_copilotVadSilenceCommitSpin) {
		coord.setDictationVadSilenceCommitMs(nextVadSilenceCommitMs);
	}
	if (_copilotSttPromptEdit) {
		coord.setDictationPrompt(nextSttPrompt);
	}
	if (keyChanged) {
		coord.saveApiKey(typedKey);
		wipeQString(typedKey);
		clearCopilotKeyEditor();
	}
	bool sttKeyChanged = false;
	bool sttAppKeyChanged = false;
	if (!isFollowChat) {
		QString typedSttKey = (includeTypedApiKey && _copilotSttApiKey)
			? _copilotSttApiKey->text().trimmed()
			: QString();
		sttKeyChanged = !typedSttKey.isEmpty();
		if (sttKeyChanged) {
			coord.saveDictationApiKey(typedSttKey);
			wipeQString(typedSttKey);
			wipeAndClearLineEdit(_copilotSttApiKey);
		}
		QString typedAppKey = (includeTypedApiKey && _copilotSttAppKey)
			? _copilotSttAppKey->text().trimmed()
			: QString();
		sttAppKeyChanged = !typedAppKey.isEmpty();
		if (sttAppKeyChanged) {
			coord.saveDictationAppKey(typedAppKey);
			wipeQString(typedAppKey);
			wipeAndClearLineEdit(_copilotSttAppKey);
		}
	}
	if (connectionChanged || keyChanged) {
		resetCopilotTestStatus();
	}
	const bool sharedKeyChanged = keyChanged &&
		(isFollowChat || (!coord.hasDictationApiKey() &&
			(nextSttBaseUrl.isEmpty() || OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, next.baseUrl))));
	if (sttConnectionChanged || sttKeyChanged || sttAppKeyChanged || sharedHostChanged || sharedKeyChanged) {
		resetCopilotSttTestStatus();
	}
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();
	updateCopilotSttModeUi();
	_updatingUi = oldUpdating;
}

void SettingsDialog::refreshCopilotProfileSelectors() {
	const auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	const auto &profiles = coord.profiles();
	if (_copilotProfileSelector) {
		const QSignalBlocker blocker(_copilotProfileSelector);
		_copilotProfileSelector->clear();
		int activeIdx = 0;
		for (int i = 0; i < profiles.size(); ++i) {
			const auto &prof = profiles[i];
			const QString label = prof.name.trimmed().isEmpty() ? prof.id : prof.name.trimmed();
			_copilotProfileSelector->addItem(label, prof.id);
			if (prof.id == coord.activeProfileId()) {
				activeIdx = i;
			}
		}
		_copilotProfileSelector->setCurrentIndex(activeIdx);
	}
	if (_copilotProfileMoreBtn) {
		_copilotProfileMoreBtn->setEnabled(!profiles.isEmpty());
	}
	if (_translationProfileCombo) {
		const QSignalBlocker blocker(_translationProfileCombo);
		const QString desiredId = _translationProfileCombo->count() > 0
			? _translationProfileCombo->currentData().toString().trimmed()
			: _session.mediaPreferences().translationProfileId.trimmed();
		_translationProfileCombo->clear();
		_translationProfileCombo->addItem(
			QCoreApplication::translate("MeetingUI", "Follow Active Copilot Profile"),
			QString());
		for (const auto &prof : profiles) {
			const QString name = prof.name.trimmed().isEmpty() ? prof.id : prof.name.trimmed();
			const QString label = prof.model.trimmed().isEmpty()
				? name
				: QStringLiteral("%1 (%2)").arg(name, prof.model.trimmed());
			_translationProfileCombo->addItem(label, prof.id);
		}
		int idx = desiredId.isEmpty() ? 0 : _translationProfileCombo->findData(desiredId);
		if (idx < 0) {
			idx = 0;
		}
		_translationProfileCombo->setCurrentIndex(idx);
	}
}

void SettingsDialog::onCopilotProfileSelected(int) {
	if (_updatingUi || !_copilotProfileSelector) {
		return;
	}
	auto &coord = copilotCoordinator();
	const QString targetId = _copilotProfileSelector->currentData().toString().trimmed();
	if (targetId.isEmpty() || targetId == coord.activeProfileId()) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	commitCopilotProfile(false);
	clearCopilotKeyEditor();
	const bool switched = coord.switchProfile(targetId);
	_updatingUi = oldUpdating;
	if (switched) {
		_copilotLastModelFetchBaseUrl.clear();
		if (_copilotModelCombo) {
			const bool prev = _updatingUi;
			_updatingUi = true;
			const QSignalBlocker b(_copilotModelCombo);
			_copilotModelCombo->clear();
			_updatingUi = prev;
		}
		resetCopilotTestStatus();
		resetCopilotModelHint();
		syncCopilotControls();
		maybeAutoRefreshCopilotModels();
	}
}

void SettingsDialog::createNewCopilotProfile(bool duplicateCurrent) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	commitCopilotProfile(false);
	clearCopilotKeyEditor();
	const OpenMeeting::AgentProfile currentProfile = coord.profile();
	const QString newName = duplicateCurrent
		? QCoreApplication::translate("MeetingUI", "%1 (Copy)").arg(
			currentProfile.name.trimmed().isEmpty() ? currentProfile.id : currentProfile.name.trimmed())
		: QString();
	coord.createProfile(newName, duplicateCurrent ? &currentProfile : nullptr);
	_updatingUi = oldUpdating;

	_copilotLastModelFetchBaseUrl.clear();
	if (_copilotModelCombo) {
		const bool prev = _updatingUi;
		_updatingUi = true;
		const QSignalBlocker b(_copilotModelCombo);
		_copilotModelCombo->clear();
		_updatingUi = prev;
	}
	resetCopilotTestStatus();
	resetCopilotModelHint();
	syncCopilotControls();
	if (_copilotProfileName) {
		_copilotProfileName->setFocus();
		_copilotProfileName->selectAll();
	}
}

void SettingsDialog::deleteCurrentCopilotProfile() {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.profiles().size() <= 1) {
		return;
	}
	const QString currentName = coord.profile().name.trimmed().isEmpty()
		? coord.activeProfileId()
		: coord.profile().name.trimmed();
	QMessageBox confirmBox(
		QMessageBox::Question,
		QCoreApplication::translate("MeetingUI", "Delete Connection Profile"),
		QCoreApplication::translate(
			"MeetingUI",
			"Delete profile \"%1\" and remove its saved API key?").arg(currentName),
		QMessageBox::Yes | QMessageBox::No,
		this);
	const auto confirmTone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::setTone(confirmBox, confirmTone);
	if (confirmBox.exec() != QMessageBox::Yes) {
		return;
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	clearCopilotKeyEditor();
	const bool deleted = coord.deleteProfile(coord.activeProfileId());
	_updatingUi = oldUpdating;
	if (deleted) {
		_copilotLastModelFetchBaseUrl.clear();
		if (_copilotModelCombo) {
			const bool prev = _updatingUi;
			_updatingUi = true;
			const QSignalBlocker b(_copilotModelCombo);
			_copilotModelCombo->clear();
			_updatingUi = prev;
		}
		resetCopilotTestStatus();
		resetCopilotModelHint();
		syncCopilotControls();
		maybeAutoRefreshCopilotModels();
	}
}

void SettingsDialog::showCopilotProfileMenu() {
	if (!_copilotProfileMoreBtn || _updatingUi) {
		return;
	}
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("settingsCopilotProfileMenu"));
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::styleMenu(menu, tone);

	auto *dupAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Copy, AppTheme::ColorRole::Text),
		QCoreApplication::translate("MeetingUI", "Duplicate Current Profile"));
	dupAct->setObjectName(QStringLiteral("settingsCopilotDuplicateProfile"));
	connect(dupAct, &QAction::triggered, this, [this] {
		createNewCopilotProfile(true);
	});

	const bool canDelete = copilotCoordinator().profiles().size() > 1;
	auto *delAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Trash, AppTheme::ColorRole::Danger),
		QCoreApplication::translate("MeetingUI", "Delete Current Profile"));
	delAct->setObjectName(QStringLiteral("settingsCopilotDeleteProfile"));
	delAct->setEnabled(canDelete);
	connect(delAct, &QAction::triggered, this, [this] {
		deleteCurrentCopilotProfile();
	});

	menu.exec(_copilotProfileMoreBtn->mapToGlobal(QPoint(0, _copilotProfileMoreBtn->height() + 4)));
}

void SettingsDialog::refreshCopilotSpeechProfileSelectors() {
	if (!_copilotSttProfileSelector) {
		return;
	}
	const auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	const auto &profiles = coord.speechProfiles();
	const QSignalBlocker blocker(_copilotSttProfileSelector);
	_copilotSttProfileSelector->clear();
	int activeIdx = 0;
	for (int i = 0; i < profiles.size(); ++i) {
		const auto &sp = profiles[i];
		const QString label = sp.name.trimmed().isEmpty() ? sp.id : sp.name.trimmed();
		_copilotSttProfileSelector->addItem(label, sp.id);
		if (sp.id == coord.activeSpeechProfileId()) {
			activeIdx = i;
		}
	}
	_copilotSttProfileSelector->setCurrentIndex(activeIdx);
	if (_copilotSttProfileMoreBtn) {
		_copilotSttProfileMoreBtn->setEnabled(!profiles.isEmpty());
	}
}

void SettingsDialog::onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode mode) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.speechSourceMode() == mode) {
		updateCopilotSttModeUi();
		return;
	}
	const auto previousFollowProto =
		(coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat && _copilotSttProtocolCombo)
			? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
			: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	// Commit any pending edits in the previous mode before switching cleanly.
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(
				OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt()));
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(_copilotSttModelCombo->currentText().trimmed());
		}
	} else {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
		wipeAndClearLineEdit(_copilotSttApiKey);
		wipeAndClearLineEdit(_copilotSttAppKey);
	}
	coord.setSpeechSourceMode(mode);
	_copilotLastSttEndpointKey.clear();
	_copilotLastSttModelFetchUrl.clear();
	_copilotSttModelAutoSelected = false;
	_updatingUi = oldUpdating;

	resetCopilotSttTestStatus();
	syncCopilotControls();
	if (mode == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		const auto &sp = coord.speechProfile();
		const bool untouchedCustomProfile =
			(sp.protocol == OpenMeeting::AgentSpeechProtocolMode::Auto) &&
			sp.baseUrl.trimmed().isEmpty() &&
			!coord.hasDictationApiKey();
		if (untouchedCustomProfile && previousFollowProto != OpenMeeting::AgentSpeechProtocolMode::Auto) {
			if (_copilotSttProtocolCombo) {
				const QSignalBlocker protoBlocker(_copilotSttProtocolCombo);
				const int pIdx = _copilotSttProtocolCombo->findData(static_cast<int>(previousFollowProto));
				if (pIdx >= 0) {
					_copilotSttProtocolCombo->setCurrentIndex(pIdx);
				}
			}
			applyCopilotSpeechProtocolPreset(previousFollowProto, /*forceOverwrite=*/false);
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitCopilotProfile(false);
		} else if (sp.protocol != OpenMeeting::AgentSpeechProtocolMode::Auto &&
			_copilotSttBaseUrl &&
			_copilotSttBaseUrl->text().trimmed().isEmpty()) {
			applyCopilotSpeechProtocolPreset(sp.protocol, /*forceOverwrite=*/false);
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitCopilotProfile(false);
		}
		refreshCopilotSpeechModels(true);
	}
}

void SettingsDialog::onCopilotSpeechProfileSelected(int) {
	if (_updatingUi || !_copilotSttProfileSelector) {
		return;
	}
	auto &coord = copilotCoordinator();
	const QString targetId = _copilotSttProfileSelector->currentData().toString().trimmed();
	if (targetId.isEmpty() || targetId == coord.activeSpeechProfileId()) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	}
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const bool switched = coord.switchSpeechProfile(targetId);
	_updatingUi = oldUpdating;
	if (switched) {
		_copilotLastSttModelFetchUrl.clear();
		_copilotLastSttEndpointKey.clear();
		_copilotSttModelAutoSelected = false;
		resetCopilotSttTestStatus();
		syncCopilotControls();
		refreshCopilotSpeechModels(true);
	}
}

void SettingsDialog::createNewCopilotSpeechProfile(bool duplicateCurrent) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	}
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const OpenMeeting::AgentSpeechProfile currentSpeechProfile = coord.speechProfile();
	const QString newName = duplicateCurrent
		? QCoreApplication::translate("MeetingUI", "%1 (Copy)").arg(
			currentSpeechProfile.name.trimmed().isEmpty()
				? currentSpeechProfile.id
				: currentSpeechProfile.name.trimmed())
		: QString();
	coord.createSpeechProfile(newName, duplicateCurrent ? &currentSpeechProfile : nullptr);
	_updatingUi = oldUpdating;

	_copilotLastSttModelFetchUrl.clear();
	_copilotLastSttEndpointKey.clear();
	_copilotSttModelAutoSelected = false;
	resetCopilotSttTestStatus();
	syncCopilotControls();
	if (duplicateCurrent) {
		refreshCopilotSpeechModels(true);
	}
	if (_copilotSttProfileName) {
		_copilotSttProfileName->setFocus();
		_copilotSttProfileName->selectAll();
	}
}

void SettingsDialog::deleteCurrentCopilotSpeechProfile() {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.speechProfiles().size() <= 1) {
		return;
	}
	const QString currentName = coord.speechProfile().name.trimmed().isEmpty()
		? coord.activeSpeechProfileId()
		: coord.speechProfile().name.trimmed();
	QMessageBox confirmBox(
		QMessageBox::Question,
		QCoreApplication::translate("MeetingUI", "Delete Speech Profile"),
		QCoreApplication::translate(
			"MeetingUI",
			"Delete speech profile \"%1\" and remove its saved API keys?").arg(currentName),
		QMessageBox::Yes | QMessageBox::No,
		this);
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::setTone(confirmBox, tone);
	if (confirmBox.exec() != QMessageBox::Yes) {
		return;
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const bool deleted = coord.deleteSpeechProfile(coord.activeSpeechProfileId());
	_updatingUi = oldUpdating;
	if (deleted) {
		_copilotLastSttModelFetchUrl.clear();
		_copilotLastSttEndpointKey.clear();
		_copilotSttModelAutoSelected = false;
		resetCopilotSttTestStatus();
		syncCopilotControls();
		refreshCopilotSpeechModels(true);
	}
}

void SettingsDialog::showCopilotSpeechProfileMenu() {
	if (!_copilotSttProfileMoreBtn || _updatingUi) {
		return;
	}
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("settingsCopilotSttProfileMenu"));
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::styleMenu(menu, tone);

	auto *dupAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Copy, AppTheme::ColorRole::Text),
		QCoreApplication::translate("MeetingUI", "Duplicate Speech Profile"));
	dupAct->setObjectName(QStringLiteral("settingsCopilotSttDuplicateProfile"));
	connect(dupAct, &QAction::triggered, this, [this] {
		createNewCopilotSpeechProfile(true);
	});

	const bool canDelete = copilotCoordinator().speechProfiles().size() > 1;
	auto *delAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Trash, AppTheme::ColorRole::Danger),
		QCoreApplication::translate("MeetingUI", "Delete Speech Profile"));
	delAct->setObjectName(QStringLiteral("settingsCopilotSttDeleteProfile"));
	delAct->setEnabled(canDelete);
	connect(delAct, &QAction::triggered, this, [this] {
		deleteCurrentCopilotSpeechProfile();
	});

	menu.exec(_copilotSttProfileMoreBtn->mapToGlobal(QPoint(0, _copilotSttProfileMoreBtn->height() + 4)));
}

void SettingsDialog::setCopilotSubTab(int index) {
	if (!_copilotSubPages || index < 0 || index >= _copilotSubPages->count()) {
		return;
	}
	if (_copilotSubPages->currentIndex() != index) {
		_copilotSubPages->setCurrentIndex(index);
	}
	if (index >= 0 && index < static_cast<int>(_copilotSubTabButtons.size()) && _copilotSubTabButtons[index]) {
		const QSignalBlocker blocker(_copilotSubTabButtons[index]);
		_copilotSubTabButtons[index]->setChecked(true);
	}
}

int SettingsDialog::currentCopilotSubTab() const {
	return _copilotSubPages ? _copilotSubPages->currentIndex() : 0;
}

void SettingsDialog::updateCopilotAdvSummaries() {
	if (_copilotChatAdvSection) {
		QStringList custom;
		if (_copilotCustomHeaders && !_copilotCustomHeaders->toPlainText().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Custom headers"));
		}
		if (_copilotMaxHistorySpin && _copilotMaxHistorySpin->value() != 16) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "%1 turns").arg(_copilotMaxHistorySpin->value()));
		}
		if (_copilotTemperatureSpin && std::abs(_copilotTemperatureSpin->value() - 0.7) > 0.001) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Temp %1").arg(_copilotTemperatureSpin->value(), 0, 'f', 2));
		}
		if (_copilotConnectTimeoutSpin && _copilotConnectTimeoutSpin->value() != 10000) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Connect %1 ms").arg(_copilotConnectTimeoutSpin->value()));
		}
		if (_copilotIdleTimeoutSpin && _copilotIdleTimeoutSpin->value() != 30000) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Idle %1 ms").arg(_copilotIdleTimeoutSpin->value()));
		}
		if (_copilotEncryptKeyCheck && !_copilotEncryptKeyCheck->isChecked()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Session-only key"));
		}
		if (_copilotPersistHistoryCheck && !_copilotPersistHistoryCheck->isChecked()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "History not persisted"));
		}
		_copilotChatAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · 16 turns · 0.70 temp · 10s / 30s"),
			custom);
	}

	if (_copilotSttAdvSection) {
		QStringList custom;
		if (_copilotSttPromptEdit && !_copilotSttPromptEdit->text().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Recognition prompt"));
		}
		if (_copilotVadThresholdSpin && _copilotVadThresholdSpin->value() != -42) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "VAD %1 dBFS").arg(_copilotVadThresholdSpin->value()));
		}
		if (_copilotVadSilenceCommitSpin && _copilotVadSilenceCommitSpin->value() != 600) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Silence %1 ms").arg(_copilotVadSilenceCommitSpin->value()));
		}
		_copilotSttAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · -42 dBFS · 600 ms"),
			custom);
	}

	if (_copilotTrAdvSection) {
		QStringList custom;
		if (_translationProfileCombo && !_translationProfileCombo->currentData().toString().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Dedicated translation profile"));
		}
		if (_translationModelEdit && !_translationModelEdit->text().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Model: %1").arg(_translationModelEdit->text().trimmed()));
		}
		if (_translationPromptEdit && !_translationPromptEdit->toPlainText().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Custom translation prompt"));
		}
		if (_translationMaxConcurrentStreamsSpin && _translationMaxConcurrentStreamsSpin->value() != 3) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "%1 streams").arg(_translationMaxConcurrentStreamsSpin->value()));
		}
		if (_translationIdleReleaseSpin && _translationIdleReleaseSpin->value() != 12) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Idle release %1 s").arg(_translationIdleReleaseSpin->value()));
		}
		_copilotTrAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · Follow active profile · 3 streams · 12s"),
			custom);
	}
}

void SettingsDialog::updateCopilotSttModeUi() {
	const auto &coord = const_cast<SettingsDialog *>(this)->copilotCoordinator();
	const bool isCustomMode =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile);

	if (_copilotSttProfileToolbar) {
		_copilotSttProfileToolbar->setVisible(isCustomMode);
	}
	if (_copilotSttFollowBanner) {
		_copilotSttFollowBanner->setVisible(!isCustomMode);
	}
	if (_copilotSttProfileNameBlock) {
		_copilotSttProfileNameBlock->setVisible(isCustomMode);
	}
	if (_copilotSttEndpointContainer) {
		_copilotSttEndpointContainer->setVisible(isCustomMode);
	}
	if (_copilotSttScopeBadge) {
		_copilotSttScopeBadge->setText(isCustomMode
			? QCoreApplication::translate("MeetingUI", "Per Speech Profile")
			: QCoreApplication::translate("MeetingUI", "Follows Chat"));
		_copilotSttScopeBadge->setProperty("badgeTone",
			isCustomMode ? QStringLiteral("accent") : QStringLiteral("muted"));
		if (auto *st = _copilotSttScopeBadge->style()) {
			st->unpolish(_copilotSttScopeBadge);
			st->polish(_copilotSttScopeBadge);
		}
	}
	if (_copilotSttFollowLabel && !isCustomMode) {
		QString chatName = _copilotProfileName
			? _copilotProfileName->text().trimmed()
			: coord.profile().name.trimmed();
		if (chatName.isEmpty()) {
			chatName = QCoreApplication::translate("MeetingUI", "Active Chat profile");
		}
		const QString chatUrl = _copilotBaseUrl
			? _copilotBaseUrl->text().trimmed()
			: coord.profile().baseUrl.trimmed();
		const QUrl chatParsed = QUrl::fromUserInput(chatUrl);
		const QString hostDisplay = (chatParsed.isValid() && !chatParsed.host().isEmpty())
			? chatParsed.host()
			: QString();
		const bool hasChatKey = coord.hasApiKey() ||
			(_copilotApiKey && !_copilotApiKey->text().trimmed().isEmpty());
		if (hostDisplay.isEmpty()) {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" — configure a Base URL in \"Models & Chat\" first")
					.arg(chatName));
		} else if (hasChatKey) {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" (%2) — endpoint URL & API key synced automatically")
					.arg(chatName, hostDisplay));
		} else {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" (%2) — endpoint synced (no Chat API key saved)")
					.arg(chatName, hostDisplay));
		}
	}
}

void SettingsDialog::updateCopilotTranslationLinkedUi() {
	const bool enabled = _translationEnableOutput && _translationEnableOutput->isChecked();
	if (_translationTargetContainer) {
		_translationTargetContainer->setVisible(enabled);
	}
	if (_captionStyleReuseHint) {
		_captionStyleReuseHint->setVisible(!enabled);
	}
	if (_translationOverrideGroup) {
		_translationOverrideGroup->setEnabled(enabled);
	}
	if (_translationOverrideInactiveHint) {
		_translationOverrideInactiveHint->setVisible(!enabled);
	}
	updateCopilotAdvSummaries();
}

void SettingsDialog::resetCopilotChatAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_copilotCustomHeaders) {
		const QSignalBlocker b(_copilotCustomHeaders);
		_copilotCustomHeaders->clear();
	}
	if (_copilotMaxHistorySpin) {
		const QSignalBlocker b(_copilotMaxHistorySpin);
		_copilotMaxHistorySpin->setValue(16);
	}
	if (_copilotTemperatureSpin) {
		const QSignalBlocker b(_copilotTemperatureSpin);
		_copilotTemperatureSpin->setValue(0.70);
	}
	if (_copilotConnectTimeoutSpin) {
		const QSignalBlocker b(_copilotConnectTimeoutSpin);
		_copilotConnectTimeoutSpin->setValue(10000);
	}
	if (_copilotIdleTimeoutSpin) {
		const QSignalBlocker b(_copilotIdleTimeoutSpin);
		_copilotIdleTimeoutSpin->setValue(30000);
	}
	if (_copilotEncryptKeyCheck) {
		const QSignalBlocker b(_copilotEncryptKeyCheck);
		_copilotEncryptKeyCheck->setChecked(true);
	}
	if (_copilotPersistHistoryCheck) {
		const QSignalBlocker b(_copilotPersistHistoryCheck);
		_copilotPersistHistoryCheck->setChecked(true);
	}
	_updatingUi = oldUpdating;
	resetCopilotTestStatus();
	if (copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	commitCopilotProfile(false);
	updateCopilotAdvSummaries();
}

void SettingsDialog::resetCopilotSttAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_copilotSttPromptEdit) {
		const QSignalBlocker b(_copilotSttPromptEdit);
		_copilotSttPromptEdit->clear();
	}
	if (_copilotVadThresholdSpin) {
		const QSignalBlocker b(_copilotVadThresholdSpin);
		_copilotVadThresholdSpin->setValue(-42);
	}
	if (_copilotVadSilenceCommitSpin) {
		const QSignalBlocker b(_copilotVadSilenceCommitSpin);
		_copilotVadSilenceCommitSpin->setValue(600);
	}
	_updatingUi = oldUpdating;
	resetCopilotSttTestStatus();
	commitCopilotProfile(false);
	updateCopilotAdvSummaries();
}

void SettingsDialog::resetCopilotTranslationAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	if (_translationPromptCommitTimer && _translationPromptCommitTimer->isActive()) {
		_translationPromptCommitTimer->stop();
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_translationProfileCombo) {
		const QSignalBlocker b(_translationProfileCombo);
		_translationProfileCombo->setCurrentIndex(0);
	}
	if (_translationModelEdit) {
		const QSignalBlocker b(_translationModelEdit);
		_translationModelEdit->clear();
	}
	if (_translationPromptEdit) {
		const QSignalBlocker b(_translationPromptEdit);
		_translationPromptEdit->clear();
	}
	if (_translationMaxConcurrentStreamsSpin) {
		const QSignalBlocker b(_translationMaxConcurrentStreamsSpin);
		_translationMaxConcurrentStreamsSpin->setValue(3);
	}
	if (_translationIdleReleaseSpin) {
		const QSignalBlocker b(_translationIdleReleaseSpin);
		_translationIdleReleaseSpin->setValue(12);
	}
	_updatingUi = oldUpdating;
	commitPreferences();
	updateCopilotAdvSummaries();
}

void SettingsDialog::saveCopilotKey() {
	if (!_copilotApiKey) {
		return;
	}
	QString key = _copilotApiKey->text().trimmed();
	if (key.isEmpty()) {
		updateCopilotKeyStatus();
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setProfile(collectCopilotProfileFromUi());
	const auto st = coord.saveApiKey(key);
	wipeQString(key);
	clearCopilotKeyEditor();
	resetCopilotTestStatus();
	if (!coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttModeUi();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotKeyHintIcon) {
		const QString storageNotice = (st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable)
			? QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable on this platform; key kept in session memory only.")
			: QCoreApplication::translate("MeetingUI",
				"Unable to write encrypted API key to settings; kept in session memory only.");
		const QString currentUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
		const QString warn = keyEndpointMismatchWarningText(coord.chatKeyEndpointCompatibility(currentUrl), false);
		setHintIcon(
			_copilotKeyHintIcon,
			warn.isEmpty() ? storageNotice : (storageNotice + QLatin1Char('\n') + warn),
			QStringLiteral("warning"));
	}
	refreshCopilotModels(true);
}

void SettingsDialog::clearCopilotKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	clearCopilotKeyEditor();
	auto &coord = copilotCoordinator();
	coord.clearApiKey();
	resetCopilotTestStatus();
	if (!coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttModeUi();
}

void SettingsDialog::clearCopilotKeyEditor() {
	wipeAndClearLineEdit(_copilotApiKey);
}

void SettingsDialog::updateCopilotKeyStatus() {
	if (!_copilotKeyHintIcon) {
		return;
	}
	auto &coord = copilotCoordinator();
	QString statusText;
	if (!coord.hasApiKey()) {
		statusText = QCoreApplication::translate("MeetingUI",
			"No API key saved (optional for local keyless servers).");
	} else if (coord.isApiKeyPersisted()) {
		statusText = QCoreApplication::translate("MeetingUI",
			"API key saved (encrypted on this device).");
	} else {
		statusText = QCoreApplication::translate("MeetingUI",
			"API key loaded (session memory only).");
	}
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
	QString draftKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	const QString warn = keyEndpointMismatchWarningText(
		coord.chatKeyEndpointCompatibility(chatUrl, draftKey), false);
	wipeQString(draftKey);
	if (!warn.isEmpty()) {
		statusText += QLatin1Char('\n') + warn;
	}
	setHintIcon(_copilotKeyHintIcon, statusText, warn.isEmpty() ? QString() : QStringLiteral("warning"));
}

void SettingsDialog::saveCopilotSttKey() {
	if (!_copilotSttApiKey) {
		return;
	}
	QString key = _copilotSttApiKey->text().trimmed();
	if (key.isEmpty()) {
		updateCopilotSttKeyStatus();
		return;
	}
	updateCopilotSttModelChoices();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	const auto st = coord.saveDictationApiKey(key);
	wipeQString(key);
	wipeAndClearLineEdit(_copilotSttApiKey);
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	refreshCopilotSpeechProfileSelectors();
	updateCopilotSttKeyStatus();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotSttKeyHintIcon) {
		const QString storageNotice = (st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable)
			? QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable on this platform; key kept in session memory only.")
			: QCoreApplication::translate("MeetingUI",
				"Unable to write encrypted API key to settings; kept in session memory only.");
		const QString sttUrl = _copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl();
		const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
		QStringList lines{ storageNotice };
		if (copilotSttReusesChatHost()) {
			lines.push_back(QCoreApplication::translate("MeetingUI",
				"Warning: STT Base URL is blank or shares the Chat host, so this separate STT key overrides your Chat API key for dictation. Click Clear Key to reuse the Chat key."));
		}
		const QString warn = keyEndpointMismatchWarningText(
			coord.speechKeyEndpointCompatibility(sttUrl, chatUrl), true);
		if (!warn.isEmpty()) {
			lines.push_back(warn);
		}
		setHintIcon(_copilotSttKeyHintIcon, lines.join(QLatin1Char('\n')), QStringLiteral("warning"));
	}
	_copilotLastSttModelFetchUrl.clear();
	refreshCopilotSpeechModels(true);
}

void SettingsDialog::clearCopilotSttKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttApiKey);
	copilotCoordinator().clearDictationApiKey();
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	updateCopilotSttKeyStatus();
}

void SettingsDialog::saveCopilotSttAppKey() {
	if (!_copilotSttAppKey) {
		return;
	}
	QString appKey = _copilotSttAppKey->text().trimmed();
	if (appKey.isEmpty()) {
		updateCopilotSttAppKeyStatus();
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	const auto st = coord.saveDictationAppKey(appKey);
	wipeQString(appKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	refreshCopilotSpeechProfileSelectors();
	updateCopilotSttAppKeyStatus();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotSttAppKeyHintIcon) {
		setHintIcon(
			_copilotSttAppKeyHintIcon,
			QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable; Volcengine App Key is kept in session memory only."),
			QStringLiteral("warning"));
	}
}

void SettingsDialog::clearCopilotSttAppKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttAppKey);
	copilotCoordinator().clearDictationAppKey();
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	updateCopilotSttAppKeyStatus();
}

void SettingsDialog::updateCopilotSttAppKeyStatus() {
	if (!_copilotSttAppKeyHintIcon) {
		return;
	}
	const auto &coord = copilotCoordinator();
	QString text;
	if (!coord.hasDictationAppKey()) {
		text = QCoreApplication::translate("MeetingUI",
			"No Volcengine App Key saved (optional — leave blank to use single X-Api-Key auth; fill only for legacy X-Api-App-Key + X-Api-Access-Key).");
	} else if (coord.isDictationAppKeyPersisted()) {
		text = QCoreApplication::translate("MeetingUI",
			"Volcengine App Key saved (encrypted on this device).");
	} else {
		text = QCoreApplication::translate("MeetingUI",
			"Volcengine App Key loaded (session memory only).");
	}
	setHintIcon(_copilotSttAppKeyHintIcon, text);
}

void SettingsDialog::applyCopilotSpeechProtocolPreset(
		OpenMeeting::AgentSpeechProtocolMode protocol,
		bool forceOverwrite) {
	if (selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile ||
		protocol == OpenMeeting::AgentSpeechProtocolMode::Auto) {
		return;
	}
	const QString defaultUrl = OpenMeeting::defaultSpeechEndpoint(protocol);
	const QString defaultName = OpenMeeting::defaultSpeechProfileName(protocol);

	if (_copilotSttBaseUrl && !defaultUrl.isEmpty() &&
		(forceOverwrite || _copilotSttBaseUrl->text().trimmed().isEmpty())) {
		const QSignalBlocker blocker(_copilotSttBaseUrl);
		_copilotSttBaseUrl->setText(defaultUrl);
	}

	if (_copilotSttProfileName && !defaultName.isEmpty()) {
		const QString currentName = _copilotSttProfileName->text().trimmed();
		const auto isIndexedDefaultName = [&currentName](const QString &pattern) {
			const int placeholderIdx = pattern.indexOf(QStringLiteral("%1"));
			if (placeholderIdx < 0) {
				return false;
			}
			const QString prefix = pattern.left(placeholderIdx);
			const QString suffix = pattern.mid(placeholderIdx + 2);
			if (!currentName.startsWith(prefix) || !currentName.endsWith(suffix)) {
				return false;
			}
			const QString numStr = currentName.mid(
				prefix.size(),
				currentName.size() - prefix.size() - suffix.size()).trimmed();
			bool ok = false;
			const int num = numStr.toInt(&ok);
			return ok && num > 0;
		};
		const bool isGenericDefaultName =
			currentName.isEmpty() ||
			currentName == QStringLiteral("Default Speech") ||
			currentName == QCoreApplication::translate("MeetingUI", "Default Speech") ||
			isIndexedDefaultName(QStringLiteral("Speech Profile %1")) ||
			isIndexedDefaultName(QCoreApplication::translate("MeetingUI", "Speech Profile %1"));
		if (forceOverwrite || isGenericDefaultName) {
			const QSignalBlocker blocker(_copilotSttProfileName);
			_copilotSttProfileName->setText(defaultName);
		}
	}
}

void SettingsDialog::updateCopilotSttProtocolUi(bool refreshModelChoices) {
	const auto selectedProto = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString sttUrl = (!followChat && _copilotSttBaseUrl)
		? _copilotSttBaseUrl->text().trimmed()
		: QString();
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : QString();
	const QString modelText = _copilotSttModelCombo ? _copilotSttModelCombo->currentText().trimmed() : QString();
	const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
		selectedProto, sttUrl, chatUrl, modelText);

	if (_copilotSttVolcContainer) {
		_copilotSttVolcContainer->setVisible(
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs);
	}
	if (_copilotSttWorkspaceContainer) {
		_copilotSttWorkspaceContainer->setVisible(
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs);
	}
	if (_copilotSttBaseUrl) {
		const QString defaultWss = OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto);
		_copilotSttBaseUrl->setPlaceholderText(defaultWss.isEmpty()
			? QCoreApplication::translate("MeetingUI",
				"Optional — https:// or wss:// (leave blank for protocol default)")
			: QCoreApplication::translate("MeetingUI",
				"Optional — leave blank to use %1").arg(defaultWss));
	}
	if (_copilotSttKeyLabel && _copilotSttApiKey) {
		const QUrl effectiveEndpointUrl(sttUrl.isEmpty() ? chatUrl : sttUrl, QUrl::StrictMode);
		const bool isElevenLabsEndpoint = effectiveEndpointUrl.isValid() &&
			OpenMeeting::isElevenLabsAgentHost(effectiveEndpointUrl);
		switch (effectiveProto) {
		case OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Volcengine API Key / Access Key (X-Api-Key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter Volcengine API Key (or legacy Access Key)"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::DeepgramWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Deepgram Token)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter Deepgram API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Authorization Key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter AssemblyAI API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::OpenAiRealtimeWs:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		case OpenMeeting::AgentSpeechProtocolMode::XaiSttWs:
		case OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions:
		case OpenMeeting::AgentSpeechProtocolMode::ChatInputAudio:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeMultimodal:
			if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions &&
				isElevenLabsEndpoint) {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			} else {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Bearer Token)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
					"MeetingUI",
					"Enter separate speech API key (optional on same host)"));
			}
			break;
		default:
			if (isElevenLabsEndpoint) {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			} else {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
					"MeetingUI",
					"Enter separate speech API key (optional on same host)"));
			}
			break;
		}
		_copilotSttApiKey->setAccessibleName(_copilotSttKeyLabel->text());
	}
	if (refreshModelChoices) {
		updateCopilotSttModelChoices();
	}
	updateCopilotSttModeUi();
}

void SettingsDialog::updateCopilotSttModelChoices(bool fromServerFetch) {
	if (!_copilotSttModelCombo) {
		return;
	}
	const auto &coord = copilotCoordinator();
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString sttUrl = followChat
		? QString()
		: (_copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl());
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: coord.profile().baseUrl;
	const auto sttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();

	const QStringList externalSpeechModels = coord.cachedExternalSpeechModelsForUrl(sttUrl);
	const auto externalSpeechCaps = coord.cachedExternalSpeechCapabilitiesForUrl(sttUrl);
	const QStringList choices = OpenMeeting::AgentCoordinator::suggestedSpeechModelsForEndpoint(
		sttUrl,
		chatUrl,
		coord.cachedSpeechModels(),
		externalSpeechModels,
		sttProtocol);

	QString effectiveChatOrSttUrl = sttUrl.isEmpty() ? chatUrl : sttUrl;
	if (sttUrl.isEmpty() && OpenMeeting::isStreamingSpeechProtocol(sttProtocol)) {
		const QString defUrl = OpenMeeting::defaultStreamingSpeechEndpoint(sttProtocol);
		if (!defUrl.isEmpty()) {
			effectiveChatOrSttUrl = defUrl;
		}
	}
	const QString currentEndpointKey = OpenMeeting::isStreamingSpeechProtocol(sttProtocol)
		? QStringLiteral("proto:%1:%2").arg(static_cast<int>(sttProtocol)).arg(
			OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(effectiveChatOrSttUrl))
		: OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(effectiveChatOrSttUrl);
	const bool endpointChanged = !currentEndpointKey.isEmpty() &&
		!_copilotLastSttEndpointKey.isEmpty() &&
		currentEndpointKey != _copilotLastSttEndpointKey;
	const bool firstValidEndpoint = !currentEndpointKey.isEmpty() && _copilotLastSttEndpointKey.isEmpty();
	if (!currentEndpointKey.isEmpty()) {
		_copilotLastSttEndpointKey = currentEndpointKey;
	}

	const QSignalBlocker sttBlocker(_copilotSttModelCombo);
	const QString currentModelText = _copilotSttModelCombo->currentText().trimmed();
	_copilotSttModelCombo->clear();
	for (const QString &m : choices) {
		_copilotSttModelCombo->addItem(m);
	}
	const OpenMeeting::AgentEndpointDecision ep = OpenMeeting::evaluateAgentSpeechEndpoint(effectiveChatOrSttUrl);
	const QString epHost = ep.allowed ? ep.normalizedUrl.host().toLower() : QString();
	const bool isExternalGroq = !sttUrl.isEmpty() &&
		!OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, chatUrl) &&
		OpenMeeting::isGroqAgentHost(epHost);

	const QString autoDefaultLabel = OpenMeeting::AgentCoordinator::resolveDefaultSpeechModelForEndpoint(
		sttUrl,
		chatUrl,
		coord.cachedSpeechModels(),
		coord.cachedModelCapabilities(),
		externalSpeechModels,
		externalSpeechCaps,
		sttProtocol);
	if (auto *sttEdit = _copilotSttModelCombo->lineEdit()) {
		sttEdit->setPlaceholderText(isExternalGroq
			? QCoreApplication::translate("MeetingUI", "Auto (Groq default: whisper-large-v3-turbo)")
			: QCoreApplication::translate("MeetingUI", "Auto (default: %1)").arg(autoDefaultLabel));
	}
	const int sttIdx = currentModelText.isEmpty() ? -1 : _copilotSttModelCombo->findText(currentModelText);
	const bool shouldSwitchToEndpointDefault =
		!_updatingUi &&
		!currentModelText.isEmpty() &&
		sttIdx < 0 &&
		(endpointChanged || firstValidEndpoint ||
			(fromServerFetch && !choices.isEmpty() && _copilotSttModelAutoSelected));
	if (sttIdx >= 0) {
		_copilotSttModelCombo->setCurrentIndex(sttIdx);
	} else if (shouldSwitchToEndpointDefault) {
		int defaultIdx = autoDefaultLabel.isEmpty() ? -1 : _copilotSttModelCombo->findText(autoDefaultLabel);
		if (defaultIdx < 0 && !choices.isEmpty()) {
			defaultIdx = 0;
		}
		if (defaultIdx >= 0) {
			_copilotSttModelCombo->setCurrentIndex(defaultIdx);
			_copilotSttModelAutoSelected = true;
		} else {
			_copilotSttModelCombo->setCurrentIndex(-1);
			_copilotSttModelCombo->setEditText(QString());
			_copilotSttModelAutoSelected = false;
		}
	} else if (currentModelText.isEmpty()) {
		_copilotSttModelCombo->setCurrentIndex(-1);
		_copilotSttModelCombo->setEditText(QString());
		_copilotSttModelAutoSelected = false;
	} else {
		_copilotSttModelCombo->setCurrentIndex(-1);
		_copilotSttModelCombo->setEditText(currentModelText);
	}
}

void SettingsDialog::updateCopilotSttKeyStatus() {
	auto &coord = copilotCoordinator();
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;

	if (followChat) {
		const auto selectedProto = _copilotSttProtocolCombo
			? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
			: coord.dictationSpeechProtocol();
		const QString modelText = _copilotSttModelCombo
			? _copilotSttModelCombo->currentText().trimmed()
			: coord.dictationSttModel();
		const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
			selectedProto, QString(), chatUrl, modelText);
		bool followsChatOrigin = true;
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DeepgramWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs) {
			followsChatOrigin = OpenMeeting::AgentCoordinator::isSameSpeechOrigin(
				OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto), chatUrl);
		} else if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
				   effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			followsChatOrigin = OpenMeeting::isDashScopeAgentHost(QUrl(chatUrl)) &&
				!OpenMeeting::isBailianPlanAgentHost(QUrl(chatUrl));
		} else if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::XaiSttWs) {
			followsChatOrigin = OpenMeeting::isXaiAgentHost(QUrl(chatUrl));
		}

		QStringList bannerLines;
		if (!followsChatOrigin) {
			QString chatName = _copilotProfileName
				? _copilotProfileName->text().trimmed()
				: coord.profile().name.trimmed();
			if (chatName.isEmpty()) {
				chatName = QCoreApplication::translate("MeetingUI", "Active Chat profile");
			}
			bannerLines.push_back(QCoreApplication::translate(
				"MeetingUI",
				"The selected protocol or model targets an external streaming endpoint different from \"%1\". Switch to \"Custom Speech Profiles\" above to configure a dedicated endpoint and API key.")
				.arg(chatName));
		}
		const QString mismatchWarn = keyEndpointMismatchWarningText(
			coord.speechKeyEndpointCompatibility(QString(), chatUrl, QString()), true);
		if (!mismatchWarn.isEmpty()) {
			bannerLines.push_back(mismatchWarn);
		}
		if (_copilotSttKeyWarningBanner) {
			const QString bannerText = bannerLines.join(QLatin1Char('\n')).trimmed();
			_copilotSttKeyWarningBanner->setText(bannerText);
			_copilotSttKeyWarningBanner->setVisible(!bannerText.isEmpty());
		}
		return;
	}

	QStringList lines;
	if (!coord.hasDictationApiKey()) {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"No separate speech API key saved (reuses Chat key on the same host; required for third-party HTTP or WebSocket streaming providers such as OpenAI, DashScope, Deepgram, AssemblyAI, Volcengine, ElevenLabs, xAI, or Groq)."));
	} else if (coord.isDictationApiKeyPersisted()) {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"Dedicated speech API key saved (encrypted on this device)."));
	} else {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"Dedicated speech API key loaded (session memory only)."));
	}

	const QString sttUrl = _copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl();
	QString draftSttKey = _copilotSttApiKey ? _copilotSttApiKey->text().trimmed() : QString();
	bool hasWarning = false;
	QStringList bannerLines;
	if (coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		hasWarning = true;
		const QString overrideWarn = QCoreApplication::translate("MeetingUI",
			"Warning: Speech Endpoint URL is blank or shares the Chat host, so this separate speech key overrides your Chat API key for speech recognition. Click Clear Key to reuse the Chat key.");
		lines.push_back(overrideWarn);
		bannerLines.push_back(overrideWarn);
	}
	const QString mismatchWarn = keyEndpointMismatchWarningText(
		coord.speechKeyEndpointCompatibility(sttUrl, chatUrl, draftSttKey), true);
	wipeQString(draftSttKey);
	if (!mismatchWarn.isEmpty()) {
		hasWarning = true;
		lines.push_back(mismatchWarn);
		bannerLines.push_back(mismatchWarn);
	}
	if (_copilotSttKeyHintIcon) {
		setHintIcon(
			_copilotSttKeyHintIcon,
			lines.join(QLatin1Char('\n')),
			hasWarning ? QStringLiteral("warning") : QString());
	}
	if (_copilotSttKeyWarningBanner) {
		const QString bannerText = bannerLines.join(QLatin1Char('\n')).trimmed();
		_copilotSttKeyWarningBanner->setText(bannerText);
		_copilotSttKeyWarningBanner->setVisible(!bannerText.isEmpty());
	}
}

void SettingsDialog::testCopilotConnection() {
	if (!_copilotTestBtn || !_copilotTestStatus) {
		return;
	}
	const auto draft = collectCopilotProfileFromUi();
	QString typedKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	const bool sttProbeWasActive = coord.isSpeechProbing();
	coord.setProfile(draft);
	if (!typedKey.isEmpty()) {
		coord.saveApiKey(typedKey);
		clearCopilotKeyEditor();
	}
	if (sttProbeWasActive && !coord.isSpeechProbing()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();

	_copilotTestBtn->setEnabled(false);
	setCopilotTestStatus(
		QStringLiteral("pending"),
		QCoreApplication::translate("MeetingUI", "Testing connection..."));
	coord.testConnection(draft, typedKey);
	wipeQString(typedKey);
}

void SettingsDialog::testCopilotSttConnection() {
	if (!_copilotSttTestBtn || !_copilotSttTestStatus) {
		return;
	}
	const auto draftProfile = collectCopilotProfileFromUi();
	const auto mode = selectedCopilotSpeechSourceMode();
	const bool isFollowChat = (mode == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const auto sttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const QString sttModel = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: QString();
	QString sttLang;
	if (_copilotDictationLangCombo) {
		sttLang = _copilotDictationLangCombo->currentText().trimmed();
		const int langIdx = _copilotDictationLangCombo->currentIndex();
		if (langIdx >= 0 && sttLang == _copilotDictationLangCombo->itemText(langIdx).trimmed()) {
			sttLang = _copilotDictationLangCombo->itemData(langIdx).toString().trimmed();
		}
	}
	QString typedSttKey = (!isFollowChat && _copilotSttApiKey) ? _copilotSttApiKey->text().trimmed() : QString();
	QString typedChatKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	QString typedAppKey = (!isFollowChat && _copilotSttAppKey) ? _copilotSttAppKey->text().trimmed() : QString();
	const QString sttPrompt = _copilotSttPromptEdit
		? _copilotSttPromptEdit->text().trimmed()
		: QString();

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(mode);
	OpenMeeting::AgentSpeechProfile draftSpeechProf = coord.speechProfile();
	if (isFollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(sttProtocol);
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(sttModel);
		}
	} else {
		draftSpeechProf = collectCopilotSpeechProfileFromUi();
		coord.setSpeechProfile(draftSpeechProf);
		refreshCopilotSpeechProfileSelectors();
		if (!typedSttKey.isEmpty()) {
			coord.saveDictationApiKey(typedSttKey);
			wipeAndClearLineEdit(_copilotSttApiKey);
		}
		if (!typedAppKey.isEmpty()) {
			coord.saveDictationAppKey(typedAppKey);
			wipeAndClearLineEdit(_copilotSttAppKey);
		}
	}
	if (_copilotDictationLangCombo) {
		coord.setDictationLanguage(sttLang);
	}
	if (_copilotSttPromptEdit) {
		coord.setDictationPrompt(sttPrompt);
	}
	_updatingUi = oldUpdating;
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();

	OpenMeeting::AgentSpeechProbeConfig probeCfg;
	probeCfg.chatProfile = draftProfile;
	probeCfg.sourceModeOverride = mode;
	probeCfg.speechProfileId = isFollowChat ? QString() : coord.activeSpeechProfileId();
	probeCfg.sttBaseUrl = isFollowChat ? QString() : draftSpeechProf.baseUrl;
	probeCfg.protocolMode = isFollowChat ? sttProtocol : draftSpeechProf.protocol;
	probeCfg.sttModel = isFollowChat ? sttModel : draftSpeechProf.model;
	probeCfg.language = sttLang;
	probeCfg.prompt = sttPrompt;
	probeCfg.draftSttApiKey = typedSttKey;
	probeCfg.draftChatApiKey = typedChatKey;
	probeCfg.draftAppKey = typedAppKey;
	probeCfg.resourceId = isFollowChat ? QString() : draftSpeechProf.resourceId;
	probeCfg.workspaceId = isFollowChat ? QString() : draftSpeechProf.workspaceId;

	_copilotSttTestBtn->setEnabled(false);
	setCopilotSttTestStatus(
		QStringLiteral("pending"),
		QCoreApplication::translate("MeetingUI", "Testing connection..."));
	coord.testSpeechConnection(probeCfg);
	wipeQString(typedSttKey);
	wipeQString(typedChatKey);
	wipeQString(typedAppKey);
	wipeQString(probeCfg.draftSttApiKey);
	wipeQString(probeCfg.draftChatApiKey);
	wipeQString(probeCfg.draftAppKey);
}

void SettingsDialog::refreshCopilotModels(bool autoTriggered) {
	if (!_copilotBaseUrl || !_copilotRefreshModelsBtn) {
		return;
	}
	const auto draft = collectCopilotProfileFromUi();
	const auto decision = OpenMeeting::evaluateAgentEndpoint(draft.baseUrl);
	if (!decision.allowed) {
		_copilotLastModelFetchBaseUrl.clear();
		if (!autoTriggered) {
			setCopilotModelHint(decision.reason, QStringLiteral("error"));
		}
		return;
	}
	_copilotLastModelFetchBaseUrl = draft.baseUrl.trimmed();
	QString typedKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	_copilotRefreshModelsBtn->setEnabled(false);
	setCopilotModelHint(
		QCoreApplication::translate("MeetingUI", "Fetching models..."),
		QStringLiteral("pending"));
	copilotCoordinator().fetchModels(draft, typedKey);
	wipeQString(typedKey);
	refreshCopilotSpeechModels(autoTriggered);
}

void SettingsDialog::refreshCopilotSpeechModels(bool autoTriggered) {
	if (!_copilotSttBaseUrl ||
		selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const QString sttUrl = _copilotSttBaseUrl->text().trimmed();
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: copilotCoordinator().profile().baseUrl;
	if (sttUrl.isEmpty() || OpenMeeting::AgentCoordinator::isSameAgentOrigin(sttUrl, chatUrl)) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const auto sttDecision = OpenMeeting::evaluateAgentEndpoint(sttUrl);
	if (!sttDecision.allowed || OpenMeeting::isElevenLabsAgentHost(sttDecision.normalizedUrl)) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const QString canonicalKey = OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(sttUrl);
	if (autoTriggered && !canonicalKey.isEmpty() && _copilotLastSttModelFetchUrl == canonicalKey) {
		return;
	}
	auto &coord = copilotCoordinator();
	QString typedSttKey = _copilotSttApiKey ? _copilotSttApiKey->text().trimmed() : QString();
	const bool loopbackStt = OpenMeeting::isAgentLoopbackHost(sttDecision.normalizedUrl.host());
	if (typedSttKey.isEmpty() && !coord.hasDictationApiKey() && !loopbackStt) {
		// 尚未填写或保存独立 STT 密钥时不预先发起无鉴权 /models 请求；保存密钥后再拉取。
		return;
	}
	_copilotLastSttModelFetchUrl = canonicalKey;
	coord.fetchSpeechModels(sttUrl, typedSttKey, chatUrl);
	wipeQString(typedSttKey);
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
		_autoGainControl,
		_translationEnableLocal,
		_translationPreferRoomAgent,
		_translationEnableOutput,
		_translationIncludeLocalMic }) {
		if (!checkBox) continue;
		connect(checkBox, &QCheckBox::toggled, this, [this, checkBox](bool) {
			if (checkBox == _translationEnableOutput) {
				updateCopilotTranslationLinkedUi();
			}
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}
	for (auto *combo : {
		_translationRoleModeCombo,
		_captionDisplayModeCombo,
		_translationProfileCombo }) {
		if (!combo) continue;
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo](int) {
			if (combo == _translationProfileCombo) {
				updateCopilotAdvSummaries();
			}
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}
	if (_translationTargetLangCombo) {
		connect(_translationTargetLangCombo, QOverload<int>::of(&QComboBox::activated),
			this, [this](int) {
				if (!_updatingUi) {
					commitPreferences();
				}
			});
		if (auto *trLangEdit = _translationTargetLangCombo->lineEdit()) {
			connect(trLangEdit, &QLineEdit::editingFinished, this, [this] {
				if (!_updatingUi) {
					commitPreferences();
				}
			});
		}
	}
	if (_translationModelEdit) {
		connect(_translationModelEdit, &QLineEdit::textChanged, this, [this](const QString &) {
			updateCopilotAdvSummaries();
		});
		connect(_translationModelEdit, &QLineEdit::editingFinished, this, [this] {
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}
	if (_translationPromptEdit && _translationPromptCommitTimer) {
		connect(_translationPromptEdit, &QPlainTextEdit::textChanged, this, [this] {
			updateCopilotAdvSummaries();
			if (!_updatingUi && _translationPromptCommitTimer) {
				_translationPromptCommitTimer->start();
			}
		});
		connect(_translationPromptCommitTimer, &QTimer::timeout, this, [this] {
			if (!_updatingUi) {
				commitPreferences();
			}
		});
	}
	if (_translationMaxConcurrentStreamsSpin) {
		connect(_translationMaxConcurrentStreamsSpin,
			static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
			this, [this](int) {
				updateCopilotAdvSummaries();
				if (!_updatingUi) {
					commitPreferences();
				}
			});
	}
	if (_translationIdleReleaseSpin) {
		connect(_translationIdleReleaseSpin,
			static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
			this, [this](int) {
				updateCopilotAdvSummaries();
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
			commitPreferences();
		});
	for (auto *combo : {_cameraCodecCombo, _screenShareCodecCombo, _screenShareResolutionCombo, _screenShareFpsCombo, _receiveResolutionMatchCombo, _participantNameDisplayCombo}) {
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
		populateVideoCodecChoices(_cameraCodecCombo, true, hardware);
		populateVideoCodecChoices(_screenShareCodecCombo, true, hardware);
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
}

OpenMeeting::MediaPreferences SettingsDialog::preferences() const {
	auto value = _session.mediaPreferences();
	value.enableVideo = _generalCamera && _generalCamera->isChecked();
	value.enableMicrophone = _generalMicrophone && _generalMicrophone->isChecked();
	value.enableSpeaker = _generalSpeaker && _generalSpeaker->isChecked();
	value.quitOnMainWindowClose = _quitOnClose && _quitOnClose->isChecked();
	value.showActiveSpeaker = _showActiveSpeaker && _showActiveSpeaker->isChecked();
	value.showLiveCaptions = !_showLiveCaptions || _showLiveCaptions->isChecked();
	value.enableLocalMeetingTranslation = _translationEnableLocal && _translationEnableLocal->isChecked();
	if (_translationRoleModeCombo) {
		value.meetingTranslationRoleMode = static_cast<OpenMeeting::MeetingTranslationRoleMode>(
			_translationRoleModeCombo->currentData().toInt());
	}
	value.preferRoomAgentTranscript = !_translationPreferRoomAgent || _translationPreferRoomAgent->isChecked();
	value.enableTranslationOutput = _translationEnableOutput && _translationEnableOutput->isChecked();
	if (_translationTargetLangCombo) {
		QString lang = _translationTargetLangCombo->currentText().trimmed();
		const int langIdx = _translationTargetLangCombo->currentIndex();
		if (langIdx >= 0 && lang == _translationTargetLangCombo->itemText(langIdx).trimmed()) {
			lang = _translationTargetLangCombo->itemData(langIdx).toString().trimmed();
		}
		lang = lang.left(64);
		value.translationTargetLanguage = lang.isEmpty() ? QStringLiteral("zh") : lang;
	}
	if (_captionDisplayModeCombo) {
		value.liveCaptionDisplayMode = static_cast<OpenMeeting::LiveCaptionDisplayMode>(
			_captionDisplayModeCombo->currentData().toInt());
	}
	if (_translationProfileCombo) {
		value.translationProfileId = _translationProfileCombo->currentData().toString().trimmed();
	}
	if (_translationModelEdit) {
		value.translationModelOverride = _translationModelEdit->text().trimmed().left(256);
	}
	if (_translationPromptEdit) {
		value.translationSystemPrompt = _translationPromptEdit->toPlainText().trimmed().left(4000);
	}
	value.translateLocalMicrophone = !_translationIncludeLocalMic || _translationIncludeLocalMic->isChecked();
	if (_translationMaxConcurrentStreamsSpin) {
		value.maxConcurrentStreamingAsr = std::clamp(_translationMaxConcurrentStreamsSpin->value(), 1, 8);
	}
	if (_translationIdleReleaseSpin) {
		value.streamingIdleReleaseMs = std::clamp(_translationIdleReleaseSpin->value() * 1000, 3000, 120000);
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
	if (_translationEnableLocal) {
		_translationEnableLocal->setChecked(value.enableLocalMeetingTranslation);
	}
	if (_translationRoleModeCombo) {
		const int idx = _translationRoleModeCombo->findData(static_cast<int>(value.meetingTranslationRoleMode));
		_translationRoleModeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationPreferRoomAgent) {
		_translationPreferRoomAgent->setChecked(value.preferRoomAgentTranscript);
	}
	if (_translationEnableOutput) {
		_translationEnableOutput->setChecked(value.enableTranslationOutput);
	}
	if (_translationTargetLangCombo) {
		const QSignalBlocker langBlocker(_translationTargetLangCombo);
		const QString lang = value.translationTargetLanguage.trimmed();
		const int idx = _translationTargetLangCombo->findData(lang);
		if (idx >= 0) {
			_translationTargetLangCombo->setCurrentIndex(idx);
		} else {
			_translationTargetLangCombo->setCurrentIndex(-1);
			_translationTargetLangCombo->setEditText(lang.isEmpty() ? QStringLiteral("zh") : lang);
		}
	}
	if (_captionDisplayModeCombo) {
		const int idx = _captionDisplayModeCombo->findData(static_cast<int>(value.liveCaptionDisplayMode));
		_captionDisplayModeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationProfileCombo) {
		const QSignalBlocker profBlocker(_translationProfileCombo);
		const QString pid = value.translationProfileId.trimmed();
		int idx = pid.isEmpty() ? 0 : _translationProfileCombo->findData(pid);
		if (idx < 0 && !pid.isEmpty()) {
			refreshCopilotProfileSelectors();
			idx = _translationProfileCombo->findData(pid);
		}
		_translationProfileCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationModelEdit) {
		const QSignalBlocker modelBlocker(_translationModelEdit);
		_translationModelEdit->setText(value.translationModelOverride);
	}
	if (_translationPromptEdit) {
		const QSignalBlocker promptBlocker(_translationPromptEdit);
		_translationPromptEdit->setPlainText(value.translationSystemPrompt);
	}
	if (_translationIncludeLocalMic) {
		_translationIncludeLocalMic->setChecked(value.translateLocalMicrophone);
	}
	if (_translationMaxConcurrentStreamsSpin) {
		_translationMaxConcurrentStreamsSpin->setValue(
			std::clamp(value.maxConcurrentStreamingAsr, 1, 8));
	}
	if (_translationIdleReleaseSpin) {
		const QSignalBlocker idleBlocker(_translationIdleReleaseSpin);
		_translationIdleReleaseSpin->setValue(
			std::clamp(value.streamingIdleReleaseMs, 3000, 120000) / 1000);
	}
	updateCopilotTranslationLinkedUi();
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
