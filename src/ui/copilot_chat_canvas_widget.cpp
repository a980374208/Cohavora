#include "src/ui/copilot_chat_canvas_widget.h"

#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "emoji.h"
#include "styles/style_basic.h"
#include "ui/painter.h"
#include "ui/text/text_custom_emoji.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenMeeting {
namespace Theme = MeetingUI::AppTheme;
namespace {

constexpr int kItemSpacing = 12;
constexpr int kUserLeftIndent = 40;
constexpr int kAssistantRightIndent = 6;
constexpr int kBubblePadH = 12;
constexpr int kBubblePadV = 8;
constexpr int kAssistantPadH = 2;
constexpr int kAssistantPadV = 2;
constexpr int kSectionGap = 6;
constexpr int kBubbleRadius = 10;
constexpr int kThumbGap = 6;
constexpr int kThumbRadius = 6;
constexpr int kCardRadius = 10;
constexpr int kCodeHeaderH = 26;
constexpr int kCodePadH = 8;
constexpr int kCodePadV = 6;
constexpr int kTableCellPadH = 7;
constexpr int kTableCellPadV = 5;
constexpr int kActionBtnSize = 24;

constexpr TextParseOptions kCopilotRichTextOptions = {
	TextParseLinks | TextParseMultiline | TextParseMarkdown,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

constexpr TextParseOptions kCopilotErrorTextOptions = {
	TextParseLinks | TextParseMultiline,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

constexpr TextParseOptions kCopilotPlainTextOptions = {
	TextParseMultiline,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

QString formatHeaderText(const AgentChatMessage &msg) {
	const QString who = (msg.role == AgentChatMessage::Role::User)
		? QCoreApplication::translate("MeetingUI", "You")
		: QCoreApplication::translate("MeetingUI", "Copilot");
	const QString timeStr = msg.timestamp.isValid()
		? msg.timestamp.toString(QStringLiteral("HH:mm"))
		: QString();
	return timeStr.isEmpty() ? who : QStringLiteral("%1 · %2").arg(who, timeStr);
}

QString formatStatusText(const AgentChatMessage &msg) {
	switch (msg.status) {
	case AgentChatMessage::Status::Streaming:
		return msg.content.isEmpty()
			? (msg.isThinkingActive
				? QString()
				: QCoreApplication::translate("MeetingUI", "Thinking..."))
			: QCoreApplication::translate("MeetingUI", "Generating...");
	case AgentChatMessage::Status::Cancelled:
		return msg.errorMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Stopped")
			: msg.errorMessage;
	case AgentChatMessage::Status::Failed:
		return msg.errorMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Request failed")
			: msg.errorMessage;
	case AgentChatMessage::Status::Complete:
		return QString();
	}
	return QString();
}

QString formatThinkingHeaderText(bool active, qint64 durationMs, bool expanded) {
	const QString arrow = expanded ? QStringLiteral("▾ ") : QStringLiteral("▸ ");
	if (active) {
		return arrow + QCoreApplication::translate("MeetingUI", "Thinking...");
	}
	if (durationMs > 0) {
		const double secs = std::max(0.1, durationMs / 1000.0);
		return arrow + QCoreApplication::translate("MeetingUI", "Thought for %1s")
			.arg(QString::number(secs, 'f', 1));
	}
	return arrow + QCoreApplication::translate("MeetingUI", "Thinking process");
}

QString toolStateGlyph(AgentResponsePart::ToolState state) {
	switch (state) {
	case AgentResponsePart::ToolState::Running:
		return QStringLiteral("⟳");
	case AgentResponsePart::ToolState::WaitingForConfirmation:
		return QStringLiteral("⏸");
	case AgentResponsePart::ToolState::Completed:
		return QStringLiteral("✓");
	case AgentResponsePart::ToolState::Failed:
		return QStringLiteral("✕");
	case AgentResponsePart::ToolState::Rejected:
		return QStringLiteral("—");
	}
	return QStringLiteral("•");
}

QString contextChipDisplayLabel(const AgentContextChip &chip) {
	if (!chip.label.trimmed().isEmpty()) {
		return chip.label.trimmed();
	}
	switch (chip.kind) {
	case AgentContextChipKind::MeetingChat:
		return QCoreApplication::translate("MeetingUI", "#meeting-chat");
	case AgentContextChipKind::Telemetry:
		return QCoreApplication::translate("MeetingUI", "#telemetry");
	case AgentContextChipKind::Participants:
		return QCoreApplication::translate("MeetingUI", "#participants");
	case AgentContextChipKind::Schedule:
		return QCoreApplication::translate("MeetingUI", "#schedule");
	}
	return QStringLiteral("#context");
}

// Zero-paint transparent proxy so Windows UIA automation sees exact child
// AutomationIds (CopilotAssistantText/CopilotUserText/CopilotMessageStatus)
// and bounding rects without triggering Qt layout or stylesheet overhead.
class TransparentUiaProxy final : public QLabel {
public:
	explicit TransparentUiaProxy(const QString &objectName, QWidget *parent)
		: QLabel(parent) {
		setObjectName(objectName);
		setAttribute(Qt::WA_TransparentForMouseEvents, true);
		setAttribute(Qt::WA_NoSystemBackground, true);
		setFocusPolicy(Qt::NoFocus);
		setTextFormat(Qt::PlainText);
	}

protected:
	void paintEvent(QPaintEvent *) override {
	}
};

struct ColorBundle {
	style::internal::OwnedColor linkFg;
	style::internal::OwnedColor monoFg;
	style::internal::OwnedColor markBg;
	style::internal::OwnedColor spoilerFg;
	style::internal::OwnedColor selectBg;
	style::internal::OwnedColor selectFg;
	style::internal::OwnedColor selectLinkFg;
	style::internal::OwnedColor selectMonoFg;
	style::internal::OwnedColor selectSpoilerFg;
	style::internal::OwnedColor selectOverlay;
	style::TextPalette palette;

	ColorBundle()
		: linkFg(QColor(0, 120, 212))
		, monoFg(QColor(30, 30, 30))
		, markBg(QColor(255, 240, 180))
		, spoilerFg(QColor(130, 130, 130))
		, selectBg(QColor(0, 120, 212, 80))
		, selectFg(QColor(0, 0, 0, 0))
		, selectLinkFg(QColor(0, 120, 212))
		, selectMonoFg(QColor(30, 30, 30))
		, selectSpoilerFg(QColor(130, 130, 130))
		, selectOverlay(QColor(0, 120, 212, 36))
		, palette{
			linkFg.color(),
			monoFg.color(),
			markBg.color(),
			spoilerFg.color(),
			selectBg.color(),
			selectFg.color(),
			selectLinkFg.color(),
			selectMonoFg.color(),
			selectSpoilerFg.color(),
			selectOverlay.color(),
			false,
		} {
	}
};

const QString &systemEmojiPrefix() {
	static const QString prefix = QStringLiteral("copilot-emoji:");
	return prefix;
}

QFont makeSystemEmojiFont(int pixelSize) {
	QFont f;
#if defined(Q_OS_WIN)
	f.setFamily(QStringLiteral("Segoe UI Emoji"));
#elif defined(Q_OS_MAC)
	f.setFamily(QStringLiteral("Apple Color Emoji"));
#else
	f.setFamily(QStringLiteral("Noto Color Emoji"));
#endif
	f.setPixelSize(std::max(12, pixelSize));
	f.setStyleStrategy(QFont::PreferAntialias);
	return f;
}

// Draws Unicode emojis with the OS color emoji font when bundled tdesktop
// webp sprite assets are absent, while keeping plain Unicode text on copy.
class SystemEmoji final : public Ui::Text::CustomEmoji {
public:
	SystemEmoji(QString text, const style::font &baseFont)
		: _text(std::move(text))
		, _entity(systemEmojiPrefix() + _text)
		, _font(makeSystemEmojiFont(baseFont->size()))
		, _ascent(baseFont->ascent)
		, _height(baseFont->height)
		, _descent(std::max(0, baseFont->height - baseFont->ascent))
		, _width(computeWidth(_font, _text)) {
	}

	int width() override { return _width; }
	QString entityData() override { return _entity; }

	std::optional<Ui::Text::CustomEmojiVerticalMetrics> vertical(
			const style::TextStyle &) override {
		return Ui::Text::CustomEmojiVerticalMetrics{_ascent, _descent};
	}

	Ui::Text::CustomEmojiSemantics semantics() override {
		return {
			.isEmoji = true,
			.isRealCustomEmoji = false,
			.exportEntity = false,
			.unloadPersistentAnimation = false,
			.allowCustomEmojiClick = false,
		};
	}

	void paint(QPainter &p, const Context &context) override {
		p.save();
		p.setFont(_font);
		p.setPen(context.textColor);
		p.drawText(
			QRect(context.position, QSize(_width, _height)),
			Qt::AlignCenter | Qt::TextSingleLine,
			_text);
		p.restore();
	}

	void unload() override {}
	bool ready() override { return true; }
	bool readyInDefaultState() override { return true; }

private:
	static int computeWidth(const QFont &font, const QString &text) {
		const QFontMetricsF fm(font);
		const int adv = static_cast<int>(std::ceil(fm.horizontalAdvance(text)));
		return std::clamp(adv + 2, std::max(14, st::emojiSize), 48);
	}

	QString _text;
	QString _entity;
	QFont _font;
	int _ascent = 0;
	int _height = 0;
	int _descent = 0;
	int _width = 0;
};

void appendSystemEmojiEntities(TextWithEntities &richText) {
	if (richText.text.isEmpty() ||
		Ui::Emoji::internal::FullCount() <= 0 ||
		QFile::exists(QStringLiteral(":/gui/emoji/emoji_1.webp"))) {
		return;
	}

	const QChar * const start = richText.text.constData();
	const QChar * const end = start + richText.text.size();
	const QChar *ptr = start;
	bool added = false;

	while (ptr < end) {
		int len = 0;
		const EmojiPtr emoji = Ui::Emoji::internal::Find(ptr, end, &len);
		if (emoji && len > 0 && (ptr + len) <= end) {
			const int offset = static_cast<int>(ptr - start);
			const int endOffset = offset + len;
			bool crossesEntityBoundary = false;
			for (const auto &existing : richText.entities) {
				const int es = existing.offset();
				const int ee = es + existing.length();
				if ((offset < es && endOffset > es && endOffset < ee) ||
					(es < offset && ee > offset && ee < endOffset)) {
					crossesEntityBoundary = true;
					break;
				}
			}
			if (!crossesEntityBoundary) {
				richText.entities.push_back(EntityInText(
					EntityType::CustomEmoji,
					offset,
					len,
					systemEmojiPrefix() + richText.text.mid(offset, len)));
				added = true;
			}
			ptr += len;
			continue;
		}
		if (ptr->isHighSurrogate() && (ptr + 1) < end && (ptr + 1)->isLowSurrogate()) {
			ptr += 2;
		} else {
			++ptr;
		}
	}

	if (added) {
		std::stable_sort(
			richText.entities.begin(),
			richText.entities.end(),
			[](const EntityInText &a, const EntityInText &b) {
				if (a.offset() != b.offset()) {
					return a.offset() < b.offset();
				}
				const int aRank = (a.type() == EntityType::CustomEmoji) ? 1 : 0;
				const int bRank = (b.type() == EntityType::CustomEmoji) ? 1 : 0;
				return aRank < bRank;
			});
	}
}

style::align toStyleAlign(Qt::Alignment align) {
	if (align & Qt::AlignHCenter) {
		return style::al_center;
	}
	if (align & Qt::AlignRight) {
		return style::al_right;
	}
	return style::al_left;
}

bool shouldOfferSettingsPill(const QString &errorMsg) {
	const QString lower = errorMsg.toLower();
	return lower.contains(QStringLiteral("401")) ||
		lower.contains(QStringLiteral("403")) ||
		lower.contains(QStringLiteral("api key")) ||
		lower.contains(QStringLiteral("unauthorized")) ||
		lower.contains(QStringLiteral("settings")) ||
		lower.contains(QStringLiteral("url")) ||
		lower.contains(QStringLiteral("model"));
}

} // namespace

struct CopilotChatCanvasWidget::ThemePaletteHolder {
	style::TextStyle textStyle;
	style::TextStyle codeStyle;
	Ui::Text::MarkedContext markedContext;
	ColorBundle userColors;
	ColorBundle assistantColors;
	Ui::Text::QuotePaintCache assistantPreCache;
	Ui::Text::QuotePaintCache assistantQuoteCache;
	Ui::Text::QuotePaintCache userPreCache;
	Ui::Text::QuotePaintCache userQuoteCache;
};

CopilotChatCanvasWidget::CopilotChatCanvasWidget(QWidget *parent)
	: QWidget(parent)
	, _theme(std::make_unique<ThemePaletteHolder>()) {
	setObjectName(QStringLiteral("CopilotChatCanvas"));
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(0);

	_copyFeedbackTimer.setSingleShot(true);
	_copyFeedbackTimer.setInterval(1500);
	connect(&_copyFeedbackTimer, &QTimer::timeout, this, [this] {
		_copiedCodeBlockKey.clear();
		_copiedMessageId.clear();
		update();
	});

	// Build Copilot TextStyle on top of initialized tdesktop defaultTextStyle.
	_theme->textStyle = st::defaultTextStyle;
	_theme->textStyle.lineHeight = 0;

	_theme->textStyle.blockquote.padding = style::margins(12, 5, 8, 5);
	_theme->textStyle.blockquote.verticalSkip = 6;
	_theme->textStyle.blockquote.header = 0;
	_theme->textStyle.blockquote.outline = 3;
	_theme->textStyle.blockquote.outlineShift = 2;
	_theme->textStyle.blockquote.radius = 6;
	_theme->textStyle.blockquote.scrollable = false;

	_theme->textStyle.pre.padding = style::margins(10, 6, 10, 6);
	_theme->textStyle.pre.verticalSkip = 6;
	_theme->textStyle.pre.header = 22;
	_theme->textStyle.pre.headerPosition = style::point(10, 3);
	_theme->textStyle.pre.outline = 3;
	_theme->textStyle.pre.outlineShift = 2;
	_theme->textStyle.pre.radius = 8;
	_theme->textStyle.pre.scrollable = false;

	_theme->codeStyle = _theme->textStyle;
	_theme->codeStyle.font = _theme->textStyle.font->monospace();

	_theme->markedContext.customEmojiFactory = [this](
			QStringView data,
			const Ui::Text::MarkedContext &) -> std::unique_ptr<Ui::Text::CustomEmoji> {
		const QString &prefix = systemEmojiPrefix();
		if (!data.startsWith(prefix) || data.size() <= prefix.size()) {
			return nullptr;
		}
		return std::make_unique<SystemEmoji>(
			data.mid(prefix.size()).toString(),
			_theme->textStyle.font);
	};

	rebuildThemeResources();
	Theme::observeChanges(*this, [this] {
		rebuildThemeResources();
		update();
	});
}

CopilotChatCanvasWidget::~CopilotChatCanvasWidget() {
	ClickHandler::clearActive(this);
}

void CopilotChatCanvasWidget::rebuildThemeResources() {
	_userBubbleBg = Theme::color(Theme::ColorRole::Accent);
	_userMetaFg = Theme::color(Theme::ColorRole::OnAccent);
	_userTextFg = Theme::color(Theme::ColorRole::OnAccent);

	_assistantBubbleBg = Theme::color(Theme::ColorRole::Surface);
	_assistantBubbleBorder = Theme::color(Theme::ColorRole::Border);
	_assistantMetaFg = Theme::color(Theme::ColorRole::TextSecondary);
	_assistantTextFg = Theme::color(Theme::ColorRole::Text);
	_surfaceAltColor = Theme::color(Theme::ColorRole::SurfaceAlt);
	_hoverColor = Theme::color(Theme::ColorRole::Hover);
	_accentColor = Theme::color(Theme::ColorRole::Accent);
	_accentSoftColor = Theme::color(Theme::ColorRole::AccentSoft);
	_onAccentColor = Theme::color(Theme::ColorRole::OnAccent);

	_statusMutedFg = Theme::color(Theme::ColorRole::TextMuted);
	_statusErrorFg = Theme::color(Theme::ColorRole::Danger);
	_successColor = Theme::color(Theme::ColorRole::Success);

	const QColor &surfaceAlt = _surfaceAltColor;
	const QColor &accent = _accentColor;
	const QColor &onAccent = _userTextFg;

	QColor userSelectBg = onAccent;
	userSelectBg.setAlpha(72);
	QColor userOverlay = onAccent;
	userOverlay.setAlpha(32);

	_theme->userColors.linkFg.update(onAccent);
	_theme->userColors.monoFg.update(onAccent);
	_theme->userColors.markBg.update(QColor(255, 255, 255, 55));
	_theme->userColors.spoilerFg.update(onAccent);
	_theme->userColors.selectBg.update(userSelectBg);
	_theme->userColors.selectFg.update(QColor(0, 0, 0, 0));
	_theme->userColors.selectLinkFg.update(onAccent);
	_theme->userColors.selectMonoFg.update(onAccent);
	_theme->userColors.selectSpoilerFg.update(onAccent);
	_theme->userColors.selectOverlay.update(userOverlay);

	QColor assistantSelectBg = accent;
	assistantSelectBg.setAlpha(72);
	QColor assistantOverlay = accent;
	assistantOverlay.setAlpha(28);
	QColor assistantMono = _assistantTextFg;
	if (Theme::effectiveTheme() == Theme::Theme::Dark ||
		Theme::effectiveTheme() == Theme::Theme::Teal) {
		assistantMono = QColor(0x93, 0xC5, 0xFD);
	} else {
		assistantMono = QColor(0x0F, 0x52, 0x98);
	}

	_theme->assistantColors.linkFg.update(accent);
	_theme->assistantColors.monoFg.update(assistantMono);
	_theme->assistantColors.markBg.update(QColor(accent.red(), accent.green(), accent.blue(), 42));
	_theme->assistantColors.spoilerFg.update(_statusMutedFg);
	_theme->assistantColors.selectBg.update(assistantSelectBg);
	_theme->assistantColors.selectFg.update(QColor(0, 0, 0, 0));
	_theme->assistantColors.selectLinkFg.update(accent);
	_theme->assistantColors.selectMonoFg.update(assistantMono);
	_theme->assistantColors.selectSpoilerFg.update(_statusMutedFg);
	_theme->assistantColors.selectOverlay.update(assistantOverlay);

	QColor preBg = surfaceAlt;
	QColor preHeader = _assistantBubbleBorder;
	preHeader.setAlpha(140);
	_theme->assistantPreCache = Ui::Text::QuotePaintCache{};
	_theme->assistantPreCache.bg = preBg;
	_theme->assistantPreCache.header = preHeader;
	_theme->assistantPreCache.outlines = { accent, QColor(0, 0, 0, 0), QColor(0, 0, 0, 0) };
	_theme->assistantPreCache.icon = accent;

	QColor quoteBg = QColor(accent.red(), accent.green(), accent.blue(), 22);
	_theme->assistantQuoteCache = Ui::Text::QuotePaintCache{};
	_theme->assistantQuoteCache.bg = quoteBg;
	_theme->assistantQuoteCache.header = quoteBg;
	_theme->assistantQuoteCache.outlines = { accent, QColor(0, 0, 0, 0), QColor(0, 0, 0, 0) };
	_theme->assistantQuoteCache.icon = accent;

	QColor userBlockBg = QColor(255, 255, 255, 32);
	_theme->userPreCache = Ui::Text::QuotePaintCache{};
	_theme->userPreCache.bg = userBlockBg;
	_theme->userPreCache.header = QColor(255, 255, 255, 52);
	_theme->userPreCache.outlines = { onAccent, QColor(0, 0, 0, 0), QColor(0, 0, 0, 0) };
	_theme->userPreCache.icon = onAccent;

	_theme->userQuoteCache = Ui::Text::QuotePaintCache{};
	_theme->userQuoteCache.bg = userBlockBg;
	_theme->userQuoteCache.header = userBlockBg;
	_theme->userQuoteCache.outlines = { onAccent, QColor(0, 0, 0, 0), QColor(0, 0, 0, 0) };
	_theme->userQuoteCache.icon = onAccent;
}

QPixmap makeRoundedCopilotThumbnail(
		const QImage &src,
		const QSize &logicalSize,
		qreal dpr,
		int radius) {
	if (src.isNull() || logicalSize.isEmpty()) {
		return {};
	}
	const qreal scale = std::max<qreal>(1.0, dpr);
	const QSize pixelSize(
		std::max(1, qRound(logicalSize.width() * scale)),
		std::max(1, qRound(logicalSize.height() * scale)));
	QImage scaled = src.scaled(
		pixelSize,
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation);
	if (scaled.width() > pixelSize.width() || scaled.height() > pixelSize.height()) {
		const int cropX = std::max(0, (scaled.width() - pixelSize.width()) / 2);
		const int cropY = std::max(0, (scaled.height() - pixelSize.height()) / 2);
		scaled = scaled.copy(cropX, cropY, pixelSize.width(), pixelSize.height());
	}
	QPixmap target(pixelSize);
	target.fill(Qt::transparent);
	target.setDevicePixelRatio(scale);
	QPainter p(&target);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	QPainterPath clipPath;
	clipPath.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(logicalSize)), radius, radius);
	p.setClipPath(clipPath);
	p.drawImage(QRectF(QPointF(0, 0), QSizeF(logicalSize)), scaled);
	return target;
}

void showCopilotImagePreviewDialog(const QImage &img, const QString &title, QWidget *parent) {
	if (img.isNull()) {
		return;
	}
	auto *dlg = new QDialog(parent);
	const bool isDark = parent && parent->window() &&
		parent->window()->palette().color(QPalette::Window).lightness() < 128;
	Theme::setTone(*dlg, isDark ? Theme::Tone::Dark : Theme::Tone::Light);
	dlg->setWindowTitle(title.isEmpty()
		? QCoreApplication::translate("MeetingUI", "Image Preview")
		: title);
	dlg->resize(
		std::min(1000, std::max(400, img.width() + 40)),
		std::min(800, std::max(300, img.height() + 80)));
	Theme::setStyleVariant(*dlg, "meeting-chat-sidebar-widget-dlg");

	auto *layout = new QVBoxLayout(dlg);
	layout->setContentsMargins(16, 16, 16, 16);
	layout->setSpacing(12);

	auto *scroll = new QScrollArea(dlg);
	Theme::setStyleVariant(*scroll, "meeting-chat-sidebar-widget-scroll");
	scroll->setAlignment(Qt::AlignCenter);

	auto *imgLabel = new QLabel(scroll);
	imgLabel->setPixmap(QPixmap::fromImage(img).scaled(
		dlg->size() - QSize(60, 100),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation));
	scroll->setWidget(imgLabel);
	layout->addWidget(scroll, 1);

	auto *bottomRow = new QHBoxLayout();
	bottomRow->addStretch();

	auto *saveBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save As..."), dlg);
	Theme::setStyleVariant(*saveBtn, "meeting-chat-sidebar-widget-savebtn");
	QObject::connect(saveBtn, &QPushButton::clicked, [dlg, img, title] {
		const QString defaultName = title.isEmpty() ? QStringLiteral("image.png") : title;
		const QString savePath = QFileDialog::getSaveFileName(
			dlg,
			QCoreApplication::translate("MeetingUI", "Save Image"),
			defaultName,
			QCoreApplication::translate(
				"MeetingUI",
				"PNG Images (*.png);;JPEG Images (*.jpg *.jpeg);;All Files (*.*)"),
			nullptr,
			QFileDialog::DontUseNativeDialog);
		if (!savePath.isEmpty() && img.save(savePath)) {
			QMessageBox::information(
				dlg,
				QCoreApplication::translate("MeetingUI", "Saved"),
				QCoreApplication::translate("MeetingUI", "Image saved to:\n%1").arg(savePath));
		}
	});
	bottomRow->addWidget(saveBtn);

	auto *closeBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Close"), dlg);
	Theme::setStyleVariant(*closeBtn, "meeting-chat-sidebar-widget-closebtn");
	QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
	bottomRow->addWidget(closeBtn);

	layout->addLayout(bottomRow);

	Theme::makeDialogAdaptive(*dlg, dlg->size());
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->show();
}

void CopilotChatCanvasWidget::setMessages(const QVector<AgentChatMessage> &messages) {
	clearMessages();
	for (const auto &msg : messages) {
		if (msg.role == AgentChatMessage::Role::Tool || msg.role == AgentChatMessage::Role::System) {
			continue;
		}
		CanvasMessageItem item;
		item.id = msg.id;
		applyMessageContent(item, msg);
		_idToIndex.insert(msg.id, static_cast<int>(_items.size()));
		_items.push_back(std::move(item));
	}
	relayoutFromIndex(0);
	for (const auto &item : _items) {
		syncUiaProxy(item);
	}
	update();
}

void CopilotChatCanvasWidget::appendOrUpdateMessage(const AgentChatMessage &message) {
	if (message.role == AgentChatMessage::Role::Tool || message.role == AgentChatMessage::Role::System) {
		return;
	}
	const auto it = _idToIndex.find(message.id);
	if (it == _idToIndex.end()) {
		CanvasMessageItem item;
		item.id = message.id;
		applyMessageContent(item, message);
		const int newIdx = static_cast<int>(_items.size());
		_idToIndex.insert(message.id, newIdx);
		_items.push_back(std::move(item));
		const int prevLast = _lastMessageItemIndex;
		const int relayoutStart = (prevLast >= 0 && prevLast < newIdx) ? prevLast : newIdx;
		relayoutFromIndex(relayoutStart);
		for (int i = relayoutStart; i < static_cast<int>(_items.size()); ++i) {
			syncUiaProxy(_items.at(i));
		}
		update();
		return;
	}

	const int idx = it.value();
	CanvasMessageItem &item = _items[idx];
	const QRect oldBubble = item.bubbleRect;
	const int oldHeight = item.height;
	const int prevLast = _lastMessageItemIndex;
	applyMessageContent(item, message);
	refreshFollowupVisibility();
	if (prevLast != _lastMessageItemIndex) {
		const int relayoutStart = std::min(idx, std::max(0, prevLast));
		relayoutFromIndex(relayoutStart);
		for (int i = relayoutStart; i < static_cast<int>(_items.size()); ++i) {
			syncUiaProxy(_items.at(i));
		}
		update();
		return;
	}
	layoutSingleItem(item, width(), item.y);

	if (item.height == oldHeight) {
		syncUiaProxy(item);
		update(oldBubble.united(item.bubbleRect).adjusted(-2, -2, 2, 2));
		return;
	}

	relayoutFromIndex(idx);
	for (int i = idx; i < static_cast<int>(_items.size()); ++i) {
		syncUiaProxy(_items.at(i));
	}
	update();
}

void CopilotChatCanvasWidget::removeMessage(const QString &messageId) {
	const auto it = _idToIndex.find(messageId);
	if (it == _idToIndex.end()) {
		return;
	}
	const int idx = it.value();
	_thinkingExpandedOverrides.remove(messageId);
	if (auto uiaIt = _uiaProxies.find(messageId); uiaIt != _uiaProxies.end()) {
		if (uiaIt.value().textProxy) {
			uiaIt.value().textProxy->deleteLater();
		}
		if (uiaIt.value().statusProxy) {
			uiaIt.value().statusProxy->deleteLater();
		}
		_uiaProxies.erase(uiaIt);
	}
	_items.erase(_items.begin() + idx);
	_hoveredItemIndex = -1;
	_idToIndex.clear();
	for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
		_idToIndex.insert(_items[i].id, i);
	}
	clearSelection();
	const int relayoutStart = std::max(0, idx - 1);
	relayoutFromIndex(relayoutStart);
	for (int i = relayoutStart; i < static_cast<int>(_items.size()); ++i) {
		syncUiaProxy(_items.at(i));
	}
	update();
}

void CopilotChatCanvasWidget::appendOrUpdateActionCard(const QString &actionId, QWidget *cardWidget) {
	if (actionId.isEmpty() || !cardWidget) {
		return;
	}
	const QString key = QStringLiteral("action:") + actionId;
	const auto it = _idToIndex.find(key);
	if (it == _idToIndex.end()) {
		CanvasMessageItem item;
		item.id = key;
		item.isActionCard = true;
		item.actionCardWidget = cardWidget;
		const int newIdx = static_cast<int>(_items.size());
		_idToIndex.insert(key, newIdx);
		_items.push_back(std::move(item));
		relayoutFromIndex(newIdx);
		update();
		return;
	}

	const int idx = it.value();
	_items[idx].actionCardWidget = cardWidget;
	relayoutFromIndex(idx);
	for (int i = idx + 1; i < static_cast<int>(_items.size()); ++i) {
		syncUiaProxy(_items.at(i));
	}
	update();
}

void CopilotChatCanvasWidget::clearMessages() {
	ClickHandler::clearActive(this);
	_selecting = false;
	_mouseDown = false;
	_hoveredItemIndex = -1;
	_hoverPos = QPoint(-1, -1);
	_selAnchor = {};
	_selCursor = {};
	_thinkingExpandedOverrides.clear();
	_expandedToolRowKeys.clear();
	_copiedCodeBlockKey.clear();
	_copiedMessageId.clear();
	_copyFeedbackTimer.stop();
	_items.clear();
	_idToIndex.clear();
	clearUiaProxies();
	_lastMessageItemIndex = -1;
	_totalHeight = 0;
	setFixedHeight(0);
	update();
}

void CopilotChatCanvasWidget::setActionsEnabled(bool enabled) {
	if (_actionsEnabled == enabled) {
		return;
	}
	_actionsEnabled = enabled;
	update();
}

bool CopilotChatCanvasWidget::isThinkingExpanded(const CanvasMessageItem &item) const {
	if (const auto it = _thinkingExpandedOverrides.constFind(item.id);
		it != _thinkingExpandedOverrides.constEnd()) {
		return it.value();
	}
	return item.isThinkingActive;
}

void CopilotChatCanvasWidget::refreshFollowupVisibility() {
	_lastMessageItemIndex = -1;
	for (int i = static_cast<int>(_items.size()) - 1; i >= 0; --i) {
		if (!_items[i].isActionCard && !_items[i].isHiddenEmptyTurn) {
			_lastMessageItemIndex = i;
			break;
		}
	}
}

void CopilotChatCanvasWidget::applyMessageContent(CanvasMessageItem &item, const AgentChatMessage &src) {
	item.role = src.role;
	item.status = src.status;
	item.rawContent = src.content;
	item.errorMessage = src.errorMessage;
	item.headerText = formatHeaderText(src);
	item.statusText = formatStatusText(src);
	item.cachedForWidth = -1;

	item.chipBadges.clear();
	item.rawContextChips = src.contextChips;
	for (const auto &chip : src.contextChips) {
		CanvasContextChipBadge badge;
		badge.label = contextChipDisplayLabel(chip);
		item.chipBadges.push_back(std::move(badge));
	}

	item.reasoningText = src.reasoningContent;
	item.isThinkingActive = src.isThinkingActive;
	item.thinkingDurationMs = src.thinkingDurationMs;
	item.hasThinking = !item.reasoningText.trimmed().isEmpty();
	if (item.hasThinking) {
		TextWithEntities thinkRich{ item.reasoningText, {} };
		appendSystemEmojiEntities(thinkRich);
		item.thinkingBodyText.setMarkedText(
			_theme->textStyle,
			thinkRich,
			kCopilotPlainTextOptions,
			_theme->markedContext);
	} else {
		item.thinkingBodyText.clear();
	}

	item.toolRows.clear();
	int toolIdx = 0;
	for (const auto &part : src.parts) {
		if (part.type != AgentResponsePart::Type::ToolInvocation) {
			continue;
		}
		CanvasToolRow row;
		row.rowKey = QStringLiteral("%1:tool:%2").arg(src.id).arg(toolIdx++);
		row.toolName = part.toolName;
		row.state = part.toolState;
		if (part.toolState == AgentResponsePart::ToolState::Running ||
			part.toolState == AgentResponsePart::ToolState::WaitingForConfirmation) {
			row.titleText = part.invocationMessage.isEmpty() ? part.toolName : part.invocationMessage;
		} else {
			row.titleText = part.pastTenseMessage.isEmpty() ? part.toolName : part.pastTenseMessage;
		}
		QStringList detailParts;
		if (!part.resultSummary.trimmed().isEmpty()) {
			detailParts.push_back(part.resultSummary.trimmed());
		}
		if (!part.argumentsJson.trimmed().isEmpty() && part.argumentsJson.trimmed() != QStringLiteral("{}")) {
			detailParts.push_back(part.argumentsJson.trimmed());
		}
		row.detailsText = detailParts.join(QStringLiteral("\n"));
		row.expanded = _expandedToolRowKeys.contains(row.rowKey);
		item.toolRows.push_back(std::move(row));
	}

	item.isHiddenEmptyTurn = (src.role == AgentChatMessage::Role::Assistant &&
		src.status == AgentChatMessage::Status::Complete &&
		src.content.trimmed().isEmpty() &&
		!src.toolCalls.isEmpty() &&
		!item.hasThinking &&
		item.toolRows.isEmpty() &&
		src.images.isEmpty());

	item.imageThumbs.clear();
	if (!src.images.isEmpty()) {
		const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
		const bool single = (src.images.size() == 1);
		for (const auto &att : src.images) {
			CanvasImageThumb thumb;
			thumb.fileName = att.fileName.isEmpty() ? QStringLiteral("image") : att.fileName;
			if (!att.encodedBytes.isEmpty()) {
				thumb.fullImage = QImage::fromData(att.encodedBytes);
			}
			const int srcW = std::max(1, !thumb.fullImage.isNull() ? thumb.fullImage.width() : att.width);
			const int srcH = std::max(1, !thumb.fullImage.isNull() ? thumb.fullImage.height() : att.height);
			if (single) {
				constexpr int kMaxSingleW = 180;
				constexpr int kMaxSingleH = 135;
				const double scale = std::min(1.5, std::min(double(kMaxSingleW) / srcW, double(kMaxSingleH) / srcH));
				thumb.logicalSize = QSize(
					std::clamp(static_cast<int>(std::round(srcW * scale)), 64, kMaxSingleW),
					std::clamp(static_cast<int>(std::round(srcH * scale)), 48, kMaxSingleH));
			} else {
				thumb.logicalSize = QSize(104, 78);
			}
			if (!thumb.fullImage.isNull()) {
				thumb.roundedPixmap = makeRoundedCopilotThumbnail(thumb.fullImage, thumb.logicalSize, dpr, kThumbRadius);
			}
			item.imageThumbs.push_back(std::move(thumb));
		}
	}

	item.segments.clear();
	item.totalSelectableSymbols = 0;
	if (!src.content.isEmpty()) {
		if (src.role == AgentChatMessage::Role::Assistant) {
			ParsedMarkdownBlock parsed = CopilotMarkdownParser::parse(src.content);
			int codeIdx = 0;
			for (auto &pseg : parsed.segments) {
				CanvasSegment cseg;
				cseg.type = pseg.type;
				cseg.symbolOffset = item.totalSelectableSymbols;
				if (pseg.type == ParsedMarkdownBlock::Segment::Type::RichText) {
					appendSystemEmojiEntities(pseg.richText);
					cseg.richText.setMarkedText(
						_theme->textStyle,
						pseg.richText,
						kCopilotRichTextOptions,
						_theme->markedContext);
					cseg.symbolCount = static_cast<uint16>(std::min<int>(
						std::numeric_limits<uint16>::max() - item.totalSelectableSymbols,
						cseg.richText.length()));
					item.totalSelectableSymbols += cseg.symbolCount;
				} else if (pseg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock) {
					cseg.codeKey = QStringLiteral("%1:code:%2").arg(src.id).arg(codeIdx++);
					cseg.language = pseg.codeBlock.language;
					cseg.rawCode = pseg.codeBlock.rawCode;
					TextWithEntities codeRich{ cseg.rawCode, {} };
					cseg.richText.setMarkedText(
						_theme->codeStyle,
						codeRich,
						kCopilotPlainTextOptions,
						_theme->markedContext);
					cseg.symbolCount = static_cast<uint16>(std::min<int>(
						std::numeric_limits<uint16>::max() - item.totalSelectableSymbols,
						cseg.richText.length()));
					item.totalSelectableSymbols += cseg.symbolCount;
				} else if (pseg.type == ParsedMarkdownBlock::Segment::Type::Table) {
					for (auto &row : pseg.table.rows) {
						std::vector<CanvasTableCell> cells;
						cells.reserve(static_cast<size_t>(row.size()));
						for (int c = 0; c < row.size(); ++c) {
							CanvasTableCell cell;
							cell.align = (c < pseg.table.alignments.size())
								? pseg.table.alignments.at(c)
								: Qt::AlignLeft;
							appendSystemEmojiEntities(row[c]);
							cell.text.setMarkedText(
								_theme->textStyle,
								row[c],
								kCopilotRichTextOptions,
								_theme->markedContext);
							cell.symbolOffset = item.totalSelectableSymbols;
							cell.symbolCount = static_cast<uint16>(std::min<int>(
								std::numeric_limits<uint16>::max() - item.totalSelectableSymbols,
								cell.text.length()));
							item.totalSelectableSymbols += cell.symbolCount;
							cells.push_back(std::move(cell));
						}
						cseg.tableRows.push_back(std::move(cells));
					}
					cseg.symbolCount = item.totalSelectableSymbols - cseg.symbolOffset;
				}
				item.segments.push_back(std::move(cseg));
			}
		} else {
			CanvasSegment cseg;
			cseg.type = ParsedMarkdownBlock::Segment::Type::RichText;
			TextWithEntities userRich{ src.content, {} };
			appendSystemEmojiEntities(userRich);
			cseg.richText.setMarkedText(
				_theme->textStyle,
				userRich,
				kCopilotPlainTextOptions,
				_theme->markedContext);
			cseg.symbolOffset = 0;
			cseg.symbolCount = static_cast<uint16>(std::min<int>(
				std::numeric_limits<uint16>::max(),
				cseg.richText.length()));
			item.totalSelectableSymbols = cseg.symbolCount;
			item.segments.push_back(std::move(cseg));
		}
	}

	item.hasErrorCard = false;
	item.errorBodyText.clear();
	item.errorSymbolOffset = 0;
	item.errorSymbolCount = 0;
	if (src.role == AgentChatMessage::Role::Assistant &&
		src.status == AgentChatMessage::Status::Failed) {
		item.hasErrorCard = true;
		const QString errDisplay = src.errorMessage.trimmed().isEmpty()
			? QCoreApplication::translate("MeetingUI", "Request failed")
			: src.errorMessage.trimmed();
		TextWithEntities errRich{ errDisplay, {} };
		appendSystemEmojiEntities(errRich);
		item.errorBodyText.setMarkedText(
			_theme->textStyle,
			errRich,
			kCopilotErrorTextOptions,
			_theme->markedContext);
		item.errorSymbolOffset = item.totalSelectableSymbols;
		item.errorSymbolCount = static_cast<uint16>(std::min<int>(
			std::numeric_limits<uint16>::max() - item.totalSelectableSymbols,
			item.errorBodyText.length()));
		item.totalSelectableSymbols += item.errorSymbolCount;
		item.statusText.clear();
	}

	item.actionPills.clear();
	if (src.role == AgentChatMessage::Role::Assistant &&
		(src.status == AgentChatMessage::Status::Failed ||
		 src.status == AgentChatMessage::Status::Cancelled)) {
		item.actionPills.push_back({
			QCoreApplication::translate("MeetingUI", "Retry"),
			QStringLiteral("retry"),
			{},
			true,
		});
		if (src.status == AgentChatMessage::Status::Failed && shouldOfferSettingsPill(src.errorMessage)) {
			item.actionPills.push_back({
				QCoreApplication::translate("MeetingUI", "Open Settings"),
				QStringLiteral("settings"),
				{},
				false,
			});
		}
	}

	item.followupPills.clear();
	if (src.role == AgentChatMessage::Role::Assistant &&
		src.status == AgentChatMessage::Status::Complete) {
		for (const auto &sug : src.followups) {
			if (!sug.label.trimmed().isEmpty() && !sug.prompt.trimmed().isEmpty()) {
				item.followupPills.push_back({ sug.label.trimmed(), sug.prompt.trimmed(), {} });
			}
		}
	}
}

void CopilotChatCanvasWidget::layoutSingleItem(CanvasMessageItem &item, int canvasWidth, int topY) {
	item.y = topY;
	if (canvasWidth <= 0 || item.isHiddenEmptyTurn) {
		item.height = 0;
		item.bubbleRect = {};
		item.headerRect = {};
		item.headerActionRect = {};
		item.chipsAreaRect = {};
		item.thinkingCardRect = {};
		item.thinkingToggleRect = {};
		item.thinkingBodyRect = {};
		item.imagesAreaRect = {};
		item.bodyRect = {};
		item.errorCardRect = {};
		item.errorIconRect = {};
		item.errorBodyRect = {};
		item.statusRect = {};
		item.copyBtnRect = {};
		item.regenerateBtnRect = {};
		item.cachedForWidth = canvasWidth;
		return;
	}

	if (item.isActionCard) {
		if (!item.actionCardWidget) {
			item.height = 0;
			item.bubbleRect = {};
			return;
		}
		const int cardW = std::max(80, canvasWidth - kAssistantRightIndent);
		item.actionCardWidget->setFixedWidth(cardW);
		if (QLayout *lay = item.actionCardWidget->layout()) {
			lay->invalidate();
			lay->activate();
		}
		int cardH = item.actionCardWidget->hasHeightForWidth()
			? item.actionCardWidget->heightForWidth(cardW)
			: (item.actionCardWidget->layout() && item.actionCardWidget->layout()->hasHeightForWidth()
				? item.actionCardWidget->layout()->totalHeightForWidth(cardW)
				: item.actionCardWidget->sizeHint().height());
		cardH = std::max(40, cardH);
		item.bubbleRect = QRect(0, topY, cardW, cardH);
		item.height = cardH;
		item.cachedForWidth = canvasWidth;
		item.actionCardWidget->setGeometry(item.bubbleRect);
		if (!item.actionCardWidget->isVisible()) {
			item.actionCardWidget->show();
		}
		return;
	}

	const bool isUser = (item.role == AgentChatMessage::Role::User);
	const int itemIdx = _idToIndex.value(item.id, -1);
	const bool isLastBubble = (itemIdx >= 0 && itemIdx == _lastMessageItemIndex);

	QFont smallFont = font();
	smallFont.setPixelSize(11);
	const QFontMetrics smallFm(smallFont);

	int leftMargin = 0;
	int bubbleW = 0;
	int innerW = 0;
	int startX = 0;
	const int padH = isUser ? kBubblePadH : kAssistantPadH;
	const int padV = isUser ? kBubblePadV : kAssistantPadV;

	if (isUser) {
		const int minLeft = std::min(kUserLeftIndent, std::max(0, canvasWidth - 100));
		const int maxBubbleW = std::max(80, canvasWidth - minLeft);
		const int maxInnerW = std::max(kMinBodyTextWidth, maxBubbleW - 2 * padH);
		int desiredInnerW = 0;
		if (!item.chipBadges.isEmpty()) {
			int chipsLineW = 0;
			for (const auto &badge : item.chipBadges) {
				chipsLineW += std::min(maxInnerW, smallFm.horizontalAdvance(badge.label) + 14) + 5;
			}
			desiredInnerW = std::max(desiredInnerW, std::min(maxInnerW, std::max(0, chipsLineW - 5)));
		}
		if (!item.imageThumbs.isEmpty()) {
			if (item.imageThumbs.size() == 1) {
				desiredInnerW = std::max(desiredInnerW, std::min(maxInnerW, item.imageThumbs[0].logicalSize.width()));
			} else {
				desiredInnerW = maxInnerW;
			}
		}
		for (const auto &seg : item.segments) {
			if (seg.type == ParsedMarkdownBlock::Segment::Type::RichText && !seg.richText.isNull()) {
				desiredInnerW = std::max(desiredInnerW, std::min(maxInnerW, seg.richText.maxWidth() + 2));
			} else {
				desiredInnerW = maxInnerW;
			}
		}
		innerW = std::clamp(desiredInnerW, std::min(maxInnerW, 24), maxInnerW);
		bubbleW = std::min(maxBubbleW, innerW + 2 * padH);
		leftMargin = std::max(0, canvasWidth - bubbleW);
		startX = leftMargin + padH;
	} else {
		bubbleW = std::max(80, canvasWidth - kAssistantRightIndent);
		innerW = std::max(kMinBodyTextWidth, bubbleW - 2 * padH);
		startX = padH;
	}

	int cursorY = topY + padV;
	bool hasSectionAbove = false;
	auto advanceSection = [&](int gap = kSectionGap) {
		if (hasSectionAbove) {
			cursorY += gap;
		}
		hasSectionAbove = true;
	};

	item.headerRect = {};
	if (isUser) {
		item.headerActionRect = (leftMargin >= 28)
			? QRect(leftMargin - 26, topY + 4, 22, 22)
			: QRect(leftMargin + 2, topY + 2, 22, 22);
	} else {
		item.headerActionRect = {};
	}

	// Context chip badges
	if (!item.chipBadges.isEmpty()) {
		advanceSection();
		int cx = startX;
		int cy = cursorY;
		constexpr int kPillH = 20;
		for (auto &badge : item.chipBadges) {
			const int pw = std::min(innerW, smallFm.horizontalAdvance(badge.label) + 14);
			if (cx > startX && cx + pw > startX + innerW) {
				cx = startX;
				cy += kPillH + 4;
			}
			badge.rect = QRect(cx, cy, pw, kPillH);
			cx += pw + 5;
		}
		item.chipsAreaRect = QRect(startX, cursorY, innerW, (cy + kPillH) - cursorY);
		cursorY = cy + kPillH;
	} else {
		item.chipsAreaRect = {};
	}

	// Collapsible Thinking block
	if (item.hasThinking) {
		advanceSection();
		const bool expanded = isThinkingExpanded(item);
		item.thinkingHeaderText = formatThinkingHeaderText(
			item.isThinkingActive,
			item.thinkingDurationMs,
			expanded);
		const int cardTop = cursorY;
		item.thinkingToggleRect = QRect(startX, cardTop, innerW, 26);
		cursorY += 26;
		if (expanded && !item.thinkingBodyText.isNull() && !item.thinkingBodyText.isEmpty()) {
			const int tInnerW = std::max(kMinBodyTextWidth, innerW - 18);
			const int tBodyH = item.thinkingBodyText.countHeight(tInnerW);
			item.thinkingBodyRect = QRect(startX + 9, cursorY + 4, tInnerW, tBodyH);
			cursorY = item.thinkingBodyRect.bottom() + 8;
		} else {
			item.thinkingBodyRect = {};
		}
		item.thinkingCardRect = QRect(startX, cardTop, innerW, cursorY - cardTop);
	} else {
		item.thinkingCardRect = {};
		item.thinkingToggleRect = {};
		item.thinkingBodyRect = {};
	}

	// Tool Invocation progress rows
	if (!item.toolRows.isEmpty()) {
		for (auto &row : item.toolRows) {
			advanceSection(4);
			const QString lineText = QStringLiteral("%1  %2")
				.arg(toolStateGlyph(row.state), row.titleText);
			const int rowH = std::max(24, smallFm.boundingRect(
				QRect(0, 0, std::max(40, innerW - 16), 200),
				Qt::TextWordWrap,
				lineText).height() + 8);
			row.rowRect = QRect(startX, cursorY, innerW, rowH);
			cursorY += rowH;
			if (row.expanded && !row.detailsText.isEmpty()) {
				const int detH = smallFm.boundingRect(
					QRect(0, 0, std::max(40, innerW - 18), 400),
					Qt::TextWordWrap,
					row.detailsText).height() + 8;
				row.detailsRect = QRect(startX + 4, cursorY + 2, std::max(40, innerW - 4), detH);
				cursorY = row.detailsRect.bottom() + 2;
			} else {
				row.detailsRect = {};
			}
		}
	}

	// Images
	if (!item.imageThumbs.isEmpty()) {
		advanceSection();
		const int areaTop = cursorY;
		if (item.imageThumbs.size() == 1) {
			auto &t = item.imageThumbs[0];
			const int w = std::min(innerW, t.logicalSize.width());
			const int h = (w < t.logicalSize.width() && t.logicalSize.width() > 0)
				? std::max(36, static_cast<int>(std::round(double(t.logicalSize.height()) * w / t.logicalSize.width())))
				: t.logicalSize.height();
			t.rect = QRect(startX, areaTop, w, h);
			item.imagesAreaRect = t.rect;
			cursorY = areaTop + h;
		} else {
			const int colW = std::min(104, std::max(48, (innerW - kThumbGap) / 2));
			constexpr int rowH = 78;
			int maxRight = startX;
			int bottomY = areaTop;
			for (int k = 0; k < item.imageThumbs.size(); ++k) {
				const int row = k / 2;
				const int col = k % 2;
				const int tx = startX + col * (colW + kThumbGap);
				const int ty = areaTop + row * (rowH + kThumbGap);
				item.imageThumbs[k].rect = QRect(tx, ty, colW, rowH);
				maxRight = std::max(maxRight, tx + colW);
				bottomY = std::max(bottomY, ty + rowH);
			}
			item.imagesAreaRect = QRect(startX, areaTop, maxRight - startX, bottomY - areaTop);
			cursorY = bottomY;
		}
	} else {
		item.imagesAreaRect = QRect(startX, cursorY, innerW, 0);
	}

	// Segments (RichText, CodeBlock, Table)
	if (!item.segments.empty()) {
		const int segsTop = cursorY + (hasSectionAbove ? kSectionGap : 0);
		for (auto &seg : item.segments) {
			advanceSection();
			if (seg.type == ParsedMarkdownBlock::Segment::Type::RichText) {
				const int h = seg.richText.isNull() ? 0 : seg.richText.countHeight(innerW);
				seg.rect = QRect(startX, cursorY, innerW, h);
				cursorY += h;
			} else if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock) {
				const int cardTop = cursorY;
				seg.codeHeaderRect = QRect(startX, cardTop, innerW, kCodeHeaderH);
				const int btnW = 68;
				seg.codeCopyBtnRect = QRect(startX + innerW - btnW - 6, cardTop + 3, btnW, kCodeHeaderH - 6);
				const int codeInnerW = std::max(kMinBodyTextWidth, innerW - 2 * kCodePadH);
				const int codeBodyH = seg.richText.isNull() ? 0 : seg.richText.countHeight(codeInnerW);
				seg.codeBodyRect = QRect(startX + kCodePadH, cardTop + kCodeHeaderH + kCodePadV, codeInnerW, codeBodyH);
				const int cardH = kCodeHeaderH + 2 * kCodePadV + codeBodyH;
				seg.rect = QRect(startX, cardTop, innerW, cardH);
				cursorY = cardTop + cardH;
			} else if (seg.type == ParsedMarkdownBlock::Segment::Type::Table && !seg.tableRows.empty()) {
				const int colCount = static_cast<int>(seg.tableRows.front().size());
				if (colCount > 0) {
					seg.tableColWidths.fill(0, colCount);
					int sumDesired = 0;
					QVector<int> desired(colCount, 48);
					for (int c = 0; c < colCount; ++c) {
						int maxCellW = 48;
						for (const auto &row : seg.tableRows) {
							if (c < static_cast<int>(row.size()) && !row[c].text.isNull()) {
								maxCellW = std::max(maxCellW, row[c].text.maxWidth() + 2 * kTableCellPadH);
							}
						}
						desired[c] = std::min<int>(maxCellW, std::max(60, innerW / 2));
						sumDesired += desired[c];
					}
					int assignedSum = 0;
					for (int c = 0; c < colCount; ++c) {
						const int w = (sumDesired > 0)
							? std::max(40, (desired[c] * innerW) / sumDesired)
							: (innerW / colCount);
						seg.tableColWidths[c] = w;
						assignedSum += w;
					}
					if (!seg.tableColWidths.isEmpty()) {
						seg.tableColWidths.last() = std::max(40, seg.tableColWidths.last() + (innerW - assignedSum));
					}

					const int tableTop = cursorY;
					int tableY = tableTop;
					seg.tableRowHeights.clear();
					seg.tableRowHeights.reserve(static_cast<int>(seg.tableRows.size()));
					for (auto &row : seg.tableRows) {
						int rowH = 22;
						for (int c = 0; c < colCount && c < static_cast<int>(row.size()); ++c) {
							const int cellInnerW = std::max(24, seg.tableColWidths[c] - 2 * kTableCellPadH);
							if (!row[c].text.isNull()) {
								rowH = std::max(rowH, row[c].text.countHeight(cellInnerW) + 2 * kTableCellPadV);
							}
						}
						int cellX = startX;
						for (int c = 0; c < colCount && c < static_cast<int>(row.size()); ++c) {
							row[c].rect = QRect(cellX, tableY, seg.tableColWidths[c], rowH);
							cellX += seg.tableColWidths[c];
						}
						seg.tableRowHeights.push_back(rowH);
						tableY += rowH;
					}
					seg.rect = QRect(startX, tableTop, innerW, tableY - tableTop);
					cursorY = tableY;
				}
			}
		}
		item.bodyRect = QRect(startX, segsTop, innerW, std::max(0, cursorY - segsTop));
	} else {
		item.bodyRect = QRect(startX, cursorY, innerW, 0);
	}

	// Dedicated Error Card (with clickable URLs and left icon)
	if (item.hasErrorCard) {
		advanceSection();
		constexpr int kErrPadH = 12;
		constexpr int kErrPadV = 10;
		constexpr int kErrIconSize = 16;
		constexpr int kErrIconGap = 10;
		const int errInnerW = std::max(kMinBodyTextWidth, innerW - 2 * kErrPadH - kErrIconSize - kErrIconGap);
		const int errTextH = item.errorBodyText.isNull() ? 20 : item.errorBodyText.countHeight(errInnerW);
		const int cardH = std::max(36, errTextH + 2 * kErrPadV);
		item.errorCardRect = QRect(startX, cursorY, innerW, cardH);
		item.errorIconRect = QRect(startX + kErrPadH, cursorY + kErrPadV + 1, kErrIconSize, kErrIconSize);
		item.errorBodyRect = QRect(
			startX + kErrPadH + kErrIconSize + kErrIconGap,
			cursorY + kErrPadV,
			errInnerW,
			errTextH);
		cursorY += cardH;
	} else {
		item.errorCardRect = {};
		item.errorIconRect = {};
		item.errorBodyRect = {};
	}

	// Status line (Streaming / Cancelled)
	if (!item.statusText.isEmpty()) {
		advanceSection();
		const int statusH = smallFm.boundingRect(
			QRect(0, 0, innerW, 10000),
			Qt::TextWordWrap,
			item.statusText).height();
		item.statusRect = QRect(startX, cursorY, innerW, statusH);
		cursorY += statusH;
	} else {
		item.statusRect = QRect(startX, cursorY, innerW, 0);
	}

	// Failed/Cancelled action buttons below Error Card (Retry / Open Settings)
	if (!isUser && isLastBubble && !item.actionPills.isEmpty()) {
		advanceSection(8);
		int px = startX;
		int py = cursorY;
		constexpr int kBtnH = 28;
		QFont btnFont = font();
		btnFont.setPixelSize(12);
		btnFont.setBold(true);
		const QFontMetrics btnFm(btnFont);
		for (auto &pill : item.actionPills) {
			const int pw = std::min(innerW, (pill.isPrimary ? btnFm : smallFm).horizontalAdvance(pill.label) + 24);
			if (px > startX && px + pw > startX + innerW) {
				px = startX;
				py += kBtnH + 6;
			}
			pill.rect = QRect(px, py, pw, kBtnH);
			px += pw + 8;
		}
		cursorY = py + kBtnH;
	} else {
		for (auto &pill : item.actionPills) {
			pill.rect = {};
		}
	}

	// Follow-up suggestions on latest complete Assistant turn only
	if (!isUser && isLastBubble && !item.followupPills.isEmpty()) {
		advanceSection(8);
		int fx = startX;
		int fy = cursorY;
		constexpr int kPillH = 24;
		for (auto &pill : item.followupPills) {
			const QString disp = pill.label + QStringLiteral("  ›");
			const int pw = std::min(innerW, smallFm.horizontalAdvance(disp) + 18);
			if (fx > startX && fx + pw > startX + innerW) {
				fx = startX;
				fy += kPillH + 6;
			}
			pill.rect = QRect(fx, fy, pw, kPillH);
			fx += pw + 6;
		}
		cursorY = fy + kPillH;
	} else {
		for (auto &pill : item.followupPills) {
			pill.rect = {};
		}
	}

	// Bottom assistant action bar (Copy + Regenerate)
	const bool showBottomBar = !isUser &&
		item.status != AgentChatMessage::Status::Streaming &&
		(!item.rawContent.isEmpty() || item.hasErrorCard);
	if (showBottomBar) {
		advanceSection(6);
		item.copyBtnRect = QRect(startX, cursorY, kActionBtnSize, kActionBtnSize);
		const bool showRegen = _actionsEnabled &&
			(item.status == AgentChatMessage::Status::Complete || isLastBubble);
		item.regenerateBtnRect = showRegen
			? QRect(startX + kActionBtnSize + 4, cursorY, kActionBtnSize, kActionBtnSize)
			: QRect();
		cursorY += kActionBtnSize;
	} else {
		item.copyBtnRect = {};
		item.regenerateBtnRect = {};
	}

	const int bubbleH = (cursorY + padV) - topY;
	item.bubbleRect = QRect(leftMargin, topY, bubbleW, bubbleH);
	item.height = bubbleH;
	item.cachedForWidth = canvasWidth;
}

void CopilotChatCanvasWidget::relayoutFromIndex(int startIndex) {
	refreshFollowupVisibility();
	if (_items.empty()) {
		_totalHeight = 0;
		setFixedHeight(0);
		return;
	}
	const int w = width();
	int curY = 0;
	const int clampedStart = std::max(0, startIndex);
	if (clampedStart > 0 && clampedStart <= static_cast<int>(_items.size())) {
		const auto &prev = _items[clampedStart - 1];
		curY = prev.y + prev.height + (prev.height > 0 ? kItemSpacing : 0);
	}
	for (int i = clampedStart; i < static_cast<int>(_items.size()); ++i) {
		layoutSingleItem(_items[i], w, curY);
		if (_items[i].height > 0) {
			curY += _items[i].height + kItemSpacing;
		}
	}
	if (curY >= kItemSpacing) {
		curY -= kItemSpacing;
	}
	_totalHeight = curY;
	setFixedHeight(std::max(0, _totalHeight));
}

void CopilotChatCanvasWidget::syncUiaProxy(const CanvasMessageItem &item) {
	if (item.isActionCard) {
		return;
	}
	if (item.isHiddenEmptyTurn) {
		auto it = _uiaProxies.find(item.id);
		if (it != _uiaProxies.end()) {
			if (it->textProxy) it->textProxy->hide();
			if (it->statusProxy) it->statusProxy->hide();
		}
		return;
	}
	UiaProxyPair &pair = _uiaProxies[item.id];
	const QString textObjName = (item.role == AgentChatMessage::Role::User)
		? QStringLiteral("CopilotUserText")
		: QStringLiteral("CopilotAssistantText");
	if (!pair.textProxy) {
		pair.textProxy = new TransparentUiaProxy(textObjName, this);
		pair.textProxy->show();
	} else if (pair.textProxy->objectName() != textObjName) {
		pair.textProxy->setObjectName(textObjName);
	}
	if (item.rawContent.isEmpty() && !item.imageThumbs.isEmpty()) {
		QStringList names;
		for (const auto &t : item.imageThumbs) {
			names.push_back(t.fileName);
		}
		pair.textProxy->setText(QStringLiteral("[Image: %1]").arg(names.join(QStringLiteral(", "))));
		pair.textProxy->setVisible(true);
		pair.textProxy->setGeometry(item.imagesAreaRect);
	} else {
		pair.textProxy->setText(item.rawContent);
		pair.textProxy->setVisible(!item.rawContent.isEmpty());
		pair.textProxy->setGeometry(item.bodyRect);
	}

	if (!pair.statusProxy) {
		pair.statusProxy = new TransparentUiaProxy(QStringLiteral("CopilotMessageStatus"), this);
	}
	const QString uiaStatusText = item.hasErrorCard ? item.errorMessage : item.statusText;
	const QRect uiaStatusRect = item.hasErrorCard ? item.errorBodyRect : item.statusRect;
	pair.statusProxy->setText(uiaStatusText);
	pair.statusProxy->setVisible(!uiaStatusText.isEmpty());
	pair.statusProxy->setGeometry(uiaStatusRect);
}

void CopilotChatCanvasWidget::clearUiaProxies() {
	for (auto it = _uiaProxies.begin(); it != _uiaProxies.end(); ++it) {
		if (it.value().textProxy) {
			it.value().textProxy->deleteLater();
		}
		if (it.value().statusProxy) {
			it.value().statusProxy->deleteLater();
		}
	}
	_uiaProxies.clear();
}

int CopilotChatCanvasWidget::findFirstVisibleIndex(int clipTop) const {
	if (_items.empty()) {
		return 0;
	}
	auto it = std::lower_bound(
		_items.cbegin(),
		_items.cend(),
		clipTop,
		[](const CanvasMessageItem &item, int y) {
			return (item.y + item.height) < y;
		});
	return static_cast<int>(std::distance(_items.cbegin(), it));
}

int CopilotChatCanvasWidget::hitTestItemIndex(int y) const {
	if (_items.empty()) {
		return -1;
	}
	const int idx = findFirstVisibleIndex(y);
	if (idx < static_cast<int>(_items.size()) &&
		y >= _items[idx].y &&
		y <= _items[idx].y + _items[idx].height) {
		return idx;
	}
	return -1;
}

bool CopilotChatCanvasWidget::hitTestImageThumb(
		const QPoint &pos,
		int *outItemIdx,
		int *outThumbIdx) const {
	const int idx = hitTestItemIndex(pos.y());
	if (idx < 0) {
		return false;
	}
	const auto &thumbs = _items[idx].imageThumbs;
	for (int k = 0; k < thumbs.size(); ++k) {
		if (thumbs[k].rect.contains(pos)) {
			if (outItemIdx) *outItemIdx = idx;
			if (outThumbIdx) *outThumbIdx = k;
			return true;
		}
	}
	return false;
}

bool CopilotChatCanvasWidget::hitTestInteractiveElement(const QPoint &pos, int *outItemIdx) const {
	const int idx = hitTestItemIndex(pos.y());
	if (idx < 0) {
		return false;
	}
	const auto &item = _items[idx];
	if (item.isActionCard || item.isHiddenEmptyTurn) {
		return false;
	}
	if (_actionsEnabled && !item.headerActionRect.isEmpty() && item.headerActionRect.contains(pos)) {
		if (outItemIdx) *outItemIdx = idx;
		return true;
	}
	if (!item.copyBtnRect.isEmpty() && item.copyBtnRect.contains(pos)) {
		if (outItemIdx) *outItemIdx = idx;
		return true;
	}
	if (_actionsEnabled && !item.regenerateBtnRect.isEmpty() && item.regenerateBtnRect.contains(pos)) {
		if (outItemIdx) *outItemIdx = idx;
		return true;
	}
	if (item.hasThinking && item.thinkingToggleRect.contains(pos)) {
		if (outItemIdx) *outItemIdx = idx;
		return true;
	}
	for (const auto &row : item.toolRows) {
		if (row.rowRect.contains(pos) && !row.detailsText.isEmpty()) {
			if (outItemIdx) *outItemIdx = idx;
			return true;
		}
	}
	for (const auto &seg : item.segments) {
		if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock &&
			seg.codeCopyBtnRect.contains(pos)) {
			if (outItemIdx) *outItemIdx = idx;
			return true;
		}
	}
	for (const auto &pill : item.actionPills) {
		if (pill.rect.contains(pos)) {
			if (outItemIdx) *outItemIdx = idx;
			return true;
		}
	}
	for (const auto &pill : item.followupPills) {
		if (pill.rect.contains(pos)) {
			if (outItemIdx) *outItemIdx = idx;
			return true;
		}
	}
	return false;
}

void CopilotChatCanvasWidget::showCanvasImagePreview(const QImage &img, const QString &title) {
	showCopilotImagePreviewDialog(img, title, window());
}

CanvasSelectionPoint CopilotChatCanvasWidget::hitTestSymbol(
		const QPoint &pos,
		ClickHandlerPtr *outLink,
		bool *outUpon) const {
	if (outLink) *outLink = nullptr;
	if (outUpon) *outUpon = false;
	if (_items.empty()) {
		return {};
	}

	int idx = hitTestItemIndex(pos.y());
	if (idx < 0) {
		for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
			if (pos.y() < _items[i].y) {
				idx = (i > 0 && (pos.y() - (_items[i - 1].y + _items[i - 1].height)) < (_items[i].y - pos.y()))
					? (i - 1)
					: i;
				break;
			}
		}
		if (idx < 0) {
			idx = static_cast<int>(_items.size()) - 1;
		}
	}

	const auto &item = _items[idx];
	if (item.totalSelectableSymbols == 0 || (item.segments.empty() && !item.hasErrorCard)) {
		return { idx, 0 };
	}

	Ui::Text::StateRequest req;
	req.flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
	req.flags |= Ui::Text::StateRequest::Flag::LookupLink;

	auto probeString = [&](const Ui::Text::String &str, const QRect &rect, uint16 offset, uint16 count) -> CanvasSelectionPoint {
		if (str.isNull() || count == 0) {
			return { idx, offset };
		}
		const auto st = str.getState(pos - rect.topLeft(), std::max(kMinBodyTextWidth, rect.width()), req);
		if (outLink && rect.contains(pos)) {
			*outLink = st.link;
		}
		if (outUpon && rect.contains(pos)) {
			*outUpon = st.uponSymbol;
		}
		uint16 sym = st.symbol;
		if (st.afterSymbol && sym < count) {
			++sym;
		}
		if (pos.y() < rect.top()) {
			sym = 0;
		} else if (pos.y() > rect.bottom()) {
			sym = count;
		}
		return { idx, static_cast<uint16>(offset + sym) };
	};

	if (item.hasErrorCard && !item.errorBodyText.isNull() &&
		(item.segments.empty() || pos.y() >= item.errorCardRect.top())) {
		if (pos.y() < item.errorBodyRect.top() && item.segments.empty()) {
			return { idx, 0 };
		}
		if (pos.y() > item.errorBodyRect.bottom()) {
			return { idx, item.totalSelectableSymbols };
		}
		return probeString(item.errorBodyText, item.errorBodyRect, item.errorSymbolOffset, item.errorSymbolCount);
	}

	if (pos.y() < item.bodyRect.top()) {
		return { idx, 0 };
	}
	if (pos.y() > item.bodyRect.bottom()) {
		return { idx, item.totalSelectableSymbols };
	}

	// Match segment by Y first
	for (const auto &seg : item.segments) {
		if (pos.y() <= seg.rect.bottom() || &seg == &item.segments.back()) {
			if (seg.type == ParsedMarkdownBlock::Segment::Type::RichText) {
				return probeString(seg.richText, seg.rect, seg.symbolOffset, seg.symbolCount);
			}
			if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock) {
				return probeString(seg.richText, seg.codeBodyRect, seg.symbolOffset, seg.symbolCount);
			}
			if (seg.type == ParsedMarkdownBlock::Segment::Type::Table && !seg.tableRows.empty()) {
				const auto *targetRow = &seg.tableRows.back();
				for (const auto &row : seg.tableRows) {
					if (!row.empty() && pos.y() <= row.front().rect.bottom()) {
						targetRow = &row;
						break;
					}
				}
				if (!targetRow->empty()) {
					const CanvasTableCell *targetCell = &targetRow->back();
					for (const auto &cell : *targetRow) {
						if (pos.x() <= cell.rect.right()) {
							targetCell = &cell;
							break;
						}
					}
					const QRect textRect = targetCell->rect.adjusted(
						kTableCellPadH, kTableCellPadV, -kTableCellPadH, -kTableCellPadV);
					return probeString(targetCell->text, textRect, targetCell->symbolOffset, targetCell->symbolCount);
				}
			}
			return { idx, seg.symbolOffset };
		}
	}

	return { idx, item.totalSelectableSymbols };
}

std::pair<CanvasSelectionPoint, CanvasSelectionPoint> CopilotChatCanvasWidget::normalizedSelectionRange() const {
	if (_selAnchor.itemIndex < 0 || _selCursor.itemIndex < 0) {
		return {{}, {}};
	}
	const bool reverse = (_selCursor.itemIndex < _selAnchor.itemIndex) ||
		(_selCursor.itemIndex == _selAnchor.itemIndex && _selCursor.symbol < _selAnchor.symbol);
	return reverse
		? std::make_pair(_selCursor, _selAnchor)
		: std::make_pair(_selAnchor, _selCursor);
}

TextSelection CopilotChatCanvasWidget::itemSelection(int itemIndex) const {
	const auto [start, end] = normalizedSelectionRange();
	if (start.itemIndex < 0 || start == end) {
		return { 0, 0 };
	}
	if (itemIndex < start.itemIndex || itemIndex > end.itemIndex) {
		return { 0, 0 };
	}
	const uint16 maxSym = _items[itemIndex].totalSelectableSymbols;
	if (maxSym == 0) {
		return { 0, 0 };
	}
	const uint16 from = (itemIndex == start.itemIndex) ? std::min(start.symbol, maxSym) : 0;
	const uint16 to = (itemIndex == end.itemIndex) ? std::min(end.symbol, maxSym) : maxSym;
	if (to <= from) {
		return { 0, 0 };
	}
	return { from, to };
}

bool CopilotChatCanvasWidget::hasSelectedText() const {
	const auto [start, end] = normalizedSelectionRange();
	return start.itemIndex >= 0 && start != end;
}

void CopilotChatCanvasWidget::clearSelection() {
	if (!hasSelectedText() && !_selecting) {
		return;
	}
	_selecting = false;
	_selAnchor = {};
	_selCursor = {};
	update();
}

void CopilotChatCanvasWidget::copySelectedTextToClipboard() const {
	const auto [start, end] = normalizedSelectionRange();
	if (start.itemIndex < 0 || start == end) {
		return;
	}
	auto sliceSel = [this](const TextSelection &globalSel, const Ui::Text::String &str, uint16 offset, uint16 count) -> TextSelection {
		if (str.isNull() || count == 0 || globalSel.empty()) {
			return { 0, 0 };
		}
		if (globalSel.to <= offset || globalSel.from >= offset + count) {
			return { 0, 0 };
		}
		const uint16 lf = (globalSel.from > offset) ? (globalSel.from - offset) : 0;
		const uint16 lt = std::min<uint16>(count, globalSel.to - offset);
		if (lt <= lf) {
			return { 0, 0 };
		}
		return str.adjustSelection({ lf, lt }, _selectType);
	};

	TextForMimeData combined;
	bool firstItemAdded = false;
	for (int i = start.itemIndex; i <= end.itemIndex && i < static_cast<int>(_items.size()); ++i) {
		const TextSelection globalSel = itemSelection(i);
		if (globalSel.empty()) {
			continue;
		}
		TextForMimeData itemData;
		bool firstSeg = false;
		for (const auto &seg : _items[i].segments) {
			if (seg.type == ParsedMarkdownBlock::Segment::Type::Table) {
				TextForMimeData tableData;
				bool firstRow = false;
				for (const auto &row : seg.tableRows) {
					TextForMimeData rowData;
					bool firstCell = false;
					for (const auto &cell : row) {
						const TextSelection cSel = sliceSel(globalSel, cell.text, cell.symbolOffset, cell.symbolCount);
						if (!cSel.empty()) {
							TextForMimeData cellMime = cell.text.toTextForMimeData(cSel);
							if (!cellMime.empty()) {
								if (firstCell) {
									rowData.append(QStringLiteral("\t"));
								}
								rowData.append(std::move(cellMime));
								firstCell = true;
							}
						}
					}
					if (!rowData.empty()) {
						if (firstRow) {
							tableData.append(QStringLiteral("\n"));
						}
						tableData.append(std::move(rowData));
						firstRow = true;
					}
				}
				if (!tableData.empty()) {
					if (firstSeg) {
						itemData.append(QStringLiteral("\n\n"));
					}
					itemData.append(std::move(tableData));
					firstSeg = true;
				}
			} else {
				const TextSelection sSel = sliceSel(globalSel, seg.richText, seg.symbolOffset, seg.symbolCount);
				if (!sSel.empty()) {
					TextForMimeData segMime = seg.richText.toTextForMimeData(sSel);
					if (!segMime.empty()) {
						if (firstSeg) {
							itemData.append(QStringLiteral("\n\n"));
						}
						itemData.append(std::move(segMime));
						firstSeg = true;
					}
				}
			}
		}
		if (_items[i].hasErrorCard && !_items[i].errorBodyText.isNull()) {
			const TextSelection eSel = sliceSel(
				globalSel,
				_items[i].errorBodyText,
				_items[i].errorSymbolOffset,
				_items[i].errorSymbolCount);
			if (!eSel.empty()) {
				TextForMimeData errMime = _items[i].errorBodyText.toTextForMimeData(eSel);
				if (!errMime.empty()) {
					if (firstSeg) {
						itemData.append(QStringLiteral("\n\n"));
					}
					itemData.append(std::move(errMime));
					firstSeg = true;
				}
			}
		}
		if (!itemData.empty()) {
			if (firstItemAdded) {
				combined.append(QStringLiteral("\n\n"));
			}
			combined.append(std::move(itemData));
			firstItemAdded = true;
		}
	}
	if (!combined.empty()) {
		TextUtilities::SetClipboardText(combined, QClipboard::Clipboard);
	}
}

void CopilotChatCanvasWidget::copyItemFullTextToClipboard(int itemIndex) const {
	if (itemIndex < 0 || itemIndex >= static_cast<int>(_items.size())) {
		return;
	}
	const auto &item = _items[itemIndex];
	const QString text = !item.rawContent.isEmpty() ? item.rawContent : item.errorMessage;
	if (!text.isEmpty()) {
		QGuiApplication::clipboard()->setText(text);
	}
}

void CopilotChatCanvasWidget::paintEvent(QPaintEvent *event) {
	if (_items.empty()) {
		return;
	}
	const QRect clip = event->rect();
	if (clip.isEmpty()) {
		return;
	}

	Painter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	QFont metaFont = font();
	metaFont.setPixelSize(11);
	metaFont.setBold(true);

	QFont statusFont = font();
	statusFont.setPixelSize(11);

	QFont pillFont = font();
	pillFont.setPixelSize(11);

	const int firstIdx = findFirstVisibleIndex(clip.top());
	const crl::time now = crl::now();

	auto sliceSel = [this](const TextSelection &globalSel, const Ui::Text::String &str, uint16 offset, uint16 count) -> TextSelection {
		if (str.isNull() || count == 0 || globalSel.empty() ||
			globalSel.to <= offset || globalSel.from >= offset + count) {
			return { 0, 0 };
		}
		const uint16 lf = (globalSel.from > offset) ? (globalSel.from - offset) : 0;
		const uint16 lt = std::min<uint16>(count, globalSel.to - offset);
		if (lt <= lf) {
			return { 0, 0 };
		}
		return str.adjustSelection({ lf, lt }, _selectType);
	};

	for (int i = firstIdx; i < static_cast<int>(_items.size()); ++i) {
		const auto &item = _items[i];
		if (item.y > clip.bottom()) {
			break;
		}
		if (item.isActionCard || item.isHiddenEmptyTurn || item.height <= 0) {
			continue;
		}

		const bool isUser = (item.role == AgentChatMessage::Role::User);
		if (isUser) {
			QPainterPath bubblePath;
			bubblePath.addRoundedRect(QRectF(item.bubbleRect), kBubbleRadius, kBubbleRadius);
			p.fillPath(bubblePath, _userBubbleBg);
		}

		if (isUser && _actionsEnabled && i == _hoveredItemIndex && !item.headerActionRect.isEmpty()) {
			p.save();
			p.setPen(Qt::NoPen);
			const bool editHovered = item.headerActionRect.contains(_hoverPos);
			p.setBrush(editHovered ? _hoverColor : _surfaceAltColor);
			p.drawRoundedRect(item.headerActionRect, 5, 5);
			Theme::icon(Theme::Icon::Edit, Theme::ColorRole::TextSecondary)
				.paint(&p, item.headerActionRect.adjusted(4, 4, -4, -4), Qt::AlignCenter);
			p.restore();
		}

		// Context chip badges on bubble
		if (!item.chipBadges.isEmpty()) {
			p.save();
			p.setFont(pillFont);
			const QColor chipBg = isUser ? QColor(255, 255, 255, 42) : _accentSoftColor;
			const QColor chipFg = isUser ? _userTextFg : _accentColor;
			for (const auto &badge : item.chipBadges) {
				if (!badge.rect.intersects(clip)) {
					continue;
				}
				p.setPen(Qt::NoPen);
				p.setBrush(chipBg);
				p.drawRoundedRect(badge.rect, 6, 6);
				p.setPen(chipFg);
				p.drawText(badge.rect, Qt::AlignCenter | Qt::TextSingleLine, badge.label);
			}
			p.restore();
		}

		// Thinking collapsible card
		if (item.hasThinking && item.thinkingCardRect.intersects(clip)) {
			p.save();
			QPainterPath tPath;
			tPath.addRoundedRect(QRectF(item.thinkingCardRect), 6, 6);
			p.fillPath(tPath, _surfaceAltColor);
			p.setPen(QPen(_assistantBubbleBorder, 1.0));
			p.drawPath(tPath);

			p.setFont(pillFont);
			p.setPen(item.isThinkingActive ? _accentColor : _assistantMetaFg);
			p.drawText(
				item.thinkingToggleRect.adjusted(8, 0, -8, 0),
				Qt::AlignLeft | Qt::AlignVCenter,
				item.thinkingHeaderText);

			if (item.thinkingBodyRect.height() > 0 && !item.thinkingBodyText.isNull()) {
				p.setPen(_statusMutedFg);
				p.setTextPalette(_theme->assistantColors.palette);
				item.thinkingBodyText.draw(p, {
					.position = item.thinkingBodyRect.topLeft(),
					.outerWidth = width(),
					.availableWidth = item.thinkingBodyRect.width(),
					.align = style::al_left,
					.clip = clip,
					.palette = &_theme->assistantColors.palette,
					.now = now,
				});
				p.restoreTextPalette();
			}
			p.restore();
		}

		// Tool Invocation progress rows
		if (!item.toolRows.isEmpty()) {
			p.save();
			p.setFont(pillFont);
			for (const auto &row : item.toolRows) {
				if (row.rowRect.intersects(clip)) {
					QPainterPath rPath;
					rPath.addRoundedRect(QRectF(row.rowRect), 6, 6);
					p.fillPath(rPath, _surfaceAltColor);
					p.setPen(QPen(_assistantBubbleBorder, 1.0));
					p.drawPath(rPath);

					QColor fg = _assistantMetaFg;
					if (row.state == AgentResponsePart::ToolState::Completed) {
						fg = _successColor;
					} else if (row.state == AgentResponsePart::ToolState::Failed) {
						fg = _statusErrorFg;
					} else if (row.state == AgentResponsePart::ToolState::Running) {
						fg = _accentColor;
					}
					p.setPen(fg);
					const QString label = QStringLiteral("%1  %2").arg(toolStateGlyph(row.state), row.titleText);
					p.drawText(
						row.rowRect.adjusted(8, 4, -8, -4),
						Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
						label);
				}
				if (row.expanded && row.detailsRect.height() > 0 && row.detailsRect.intersects(clip)) {
					p.setPen(_statusMutedFg);
					p.drawText(
						row.detailsRect.adjusted(6, 2, -6, -2),
						Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
						row.detailsText);
				}
			}
			p.restore();
		}

		// Images
		if (!item.imageThumbs.isEmpty()) {
			for (const auto &thumb : item.imageThumbs) {
				if (!thumb.rect.intersects(clip)) {
					continue;
				}
				if (!thumb.roundedPixmap.isNull()) {
					p.drawPixmap(thumb.rect, thumb.roundedPixmap);
				} else {
					p.save();
					QPainterPath ph;
					ph.addRoundedRect(QRectF(thumb.rect), kThumbRadius, kThumbRadius);
					p.fillPath(ph, QColor(255, 255, 255, 28));
					p.setFont(statusFont);
					p.setPen(isUser ? _userMetaFg : _assistantMetaFg);
					p.drawText(thumb.rect.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap, thumb.fileName);
					p.restore();
				}
				p.save();
				p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
				p.drawRoundedRect(QRectF(thumb.rect).adjusted(0.5, 0.5, -0.5, -0.5), kThumbRadius, kThumbRadius);
				p.restore();
			}
		}

		// Body Segments
		if (!item.segments.empty()) {
			const TextSelection globalSel = itemSelection(i);
			const auto &palette = isUser ? _theme->userColors.palette : _theme->assistantColors.palette;
			auto *preCache = isUser ? &_theme->userPreCache : &_theme->assistantPreCache;
			auto *quoteCache = isUser ? &_theme->userQuoteCache : &_theme->assistantQuoteCache;
			const QColor textFg = isUser ? _userTextFg : _assistantTextFg;

			for (const auto &seg : item.segments) {
				if (!seg.rect.intersects(clip)) {
					continue;
				}
				if (seg.type == ParsedMarkdownBlock::Segment::Type::RichText && !seg.richText.isNull()) {
					p.setPen(textFg);
					p.setTextPalette(palette);
					seg.richText.draw(p, {
						.position = seg.rect.topLeft(),
						.outerWidth = width(),
						.availableWidth = seg.rect.width(),
						.align = style::al_left,
						.clip = clip,
						.palette = &palette,
						.pre = preCache,
						.blockquote = quoteCache,
						.spoiler = Ui::Text::DefaultSpoilerCache(),
						.now = now,
						.fullWidthSelection = true,
						.selection = sliceSel(globalSel, seg.richText, seg.symbolOffset, seg.symbolCount),
					});
					p.restoreTextPalette();
				} else if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock) {
					p.save();
					QPainterPath cardPath;
					cardPath.addRoundedRect(QRectF(seg.rect), kCardRadius, kCardRadius);
					const QColor cardBg = isUser ? QColor(255, 255, 255, 28) : _surfaceAltColor;
					const QColor hdrBg = isUser ? QColor(255, 255, 255, 44) : QColor(_assistantBubbleBorder.red(), _assistantBubbleBorder.green(), _assistantBubbleBorder.blue(), 110);
					p.fillPath(cardPath, cardBg);

					p.save();
					p.setClipPath(cardPath);
					p.fillRect(seg.codeHeaderRect, hdrBg);
					p.restore();

					p.setPen(QPen(isUser ? QColor(255, 255, 255, 65) : _assistantBubbleBorder, 1.0));
					p.drawPath(cardPath);

					p.setFont(pillFont);
					p.setPen(isUser ? _userMetaFg : _assistantMetaFg);
					const QString langLabel = seg.language.isEmpty() ? QStringLiteral("code") : seg.language;
					p.drawText(
						seg.codeHeaderRect.adjusted(10, 0, -(seg.codeCopyBtnRect.width() + 14), 0),
						Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
						langLabel);

					const bool copied = (!_copiedCodeBlockKey.isEmpty() && _copiedCodeBlockKey == seg.codeKey);
					p.setPen(Qt::NoPen);
					p.setBrush(copied
						? (isUser ? QColor(255, 255, 255, 70) : _accentSoftColor)
						: (isUser ? QColor(255, 255, 255, 36) : _assistantBubbleBg));
					p.drawRoundedRect(seg.codeCopyBtnRect, 5, 5);
					p.setPen(copied ? (isUser ? _userTextFg : _accentColor) : (isUser ? _userTextFg : _assistantMetaFg));
					p.drawText(
						seg.codeCopyBtnRect,
						Qt::AlignCenter | Qt::TextSingleLine,
						copied
							? QCoreApplication::translate("MeetingUI", "Copied")
							: QCoreApplication::translate("MeetingUI", "Copy"));
					p.restore();

					if (!seg.richText.isNull() && seg.codeBodyRect.height() > 0) {
						p.setPen(textFg);
						p.setTextPalette(palette);
						seg.richText.draw(p, {
							.position = seg.codeBodyRect.topLeft(),
							.outerWidth = width(),
							.availableWidth = seg.codeBodyRect.width(),
							.align = style::al_left,
							.clip = clip,
							.palette = &palette,
							.now = now,
							.fullWidthSelection = true,
							.selection = sliceSel(globalSel, seg.richText, seg.symbolOffset, seg.symbolCount),
						});
						p.restoreTextPalette();
					}
				} else if (seg.type == ParsedMarkdownBlock::Segment::Type::Table && !seg.tableRows.empty()) {
					p.save();
					QPainterPath tblPath;
					tblPath.addRoundedRect(QRectF(seg.rect), 6, 6);
					p.setClipPath(tblPath);

					// Header background
					if (!seg.tableRowHeights.isEmpty()) {
						const QRect hdrRect(seg.rect.left(), seg.rect.top(), seg.rect.width(), seg.tableRowHeights.first());
						p.fillRect(hdrRect, isUser ? QColor(255, 255, 255, 40) : _surfaceAltColor);
					}

					const QColor gridColor = isUser ? QColor(255, 255, 255, 65) : _assistantBubbleBorder;
					p.setPen(QPen(gridColor, 1.0));
					for (size_t r = 0; r < seg.tableRows.size(); ++r) {
						if (r > 0 && !seg.tableRows[r].empty()) {
							const int ry = seg.tableRows[r].front().rect.top();
							p.drawLine(seg.rect.left(), ry, seg.rect.right(), ry);
						}
					}
					int cx = seg.rect.left();
					for (int c = 0; c + 1 < seg.tableColWidths.size(); ++c) {
						cx += seg.tableColWidths[c];
						p.drawLine(cx, seg.rect.top(), cx, seg.rect.bottom());
					}
					p.restore();

					p.save();
					p.setPen(QPen(isUser ? QColor(255, 255, 255, 65) : _assistantBubbleBorder, 1.0));
					p.drawRoundedRect(QRectF(seg.rect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
					p.restore();

					p.setPen(textFg);
					p.setTextPalette(palette);
					for (const auto &row : seg.tableRows) {
						for (const auto &cell : row) {
							if (!cell.rect.intersects(clip) || cell.text.isNull()) {
								continue;
							}
							const QRect tr = cell.rect.adjusted(
								kTableCellPadH, kTableCellPadV, -kTableCellPadH, -kTableCellPadV);
							cell.text.draw(p, {
								.position = tr.topLeft(),
								.outerWidth = width(),
								.availableWidth = std::max(kMinBodyTextWidth, tr.width()),
								.align = toStyleAlign(cell.align),
								.clip = clip,
								.palette = &palette,
								.now = now,
								.fullWidthSelection = false,
								.selection = sliceSel(globalSel, cell.text, cell.symbolOffset, cell.symbolCount),
							});
						}
					}
					p.restoreTextPalette();
				}
			}
		}

		// Dedicated Error Card
		if (item.hasErrorCard && item.errorCardRect.intersects(clip)) {
			p.save();
			QPainterPath errPath;
			errPath.addRoundedRect(QRectF(item.errorCardRect), kCardRadius, kCardRadius);
			p.fillPath(errPath, _assistantBubbleBg);
			p.setPen(QPen(_assistantBubbleBorder, 1.0));
			p.drawPath(errPath);

			if (!item.errorIconRect.isEmpty()) {
				Theme::icon(Theme::Icon::ErrorCircle, Theme::ColorRole::Danger)
					.paint(&p, item.errorIconRect, Qt::AlignCenter);
			}
			p.restore();

			if (!item.errorBodyText.isNull() && item.errorBodyRect.height() > 0) {
				const TextSelection globalSel = itemSelection(i);
				p.setPen(_assistantTextFg);
				p.setTextPalette(_theme->assistantColors.palette);
				item.errorBodyText.draw(p, {
					.position = item.errorBodyRect.topLeft(),
					.outerWidth = width(),
					.availableWidth = item.errorBodyRect.width(),
					.align = style::al_left,
					.clip = clip,
					.palette = &_theme->assistantColors.palette,
					.now = now,
					.fullWidthSelection = true,
					.selection = sliceSel(globalSel, item.errorBodyText, item.errorSymbolOffset, item.errorSymbolCount),
				});
				p.restoreTextPalette();
			}
		}

		// Status
		if (item.statusRect.height() > 0 && !item.statusText.isEmpty()) {
			p.setFont(statusFont);
			p.setPen(item.status == AgentChatMessage::Status::Failed ? _statusErrorFg : _statusMutedFg);
			p.drawText(item.statusRect, Qt::AlignLeft | Qt::TextWordWrap, item.statusText);
		}

		// Action pills (Retry / Open Settings)
		if (!item.actionPills.isEmpty()) {
			p.save();
			QFont primaryBtnFont = font();
			primaryBtnFont.setPixelSize(12);
			primaryBtnFont.setBold(true);
			for (const auto &pill : item.actionPills) {
				if (pill.rect.isEmpty() || !pill.rect.intersects(clip)) {
					continue;
				}
				const bool hovered = pill.rect.contains(_hoverPos);
				if (pill.isPrimary) {
					p.setPen(Qt::NoPen);
					p.setBrush(hovered ? Theme::color(Theme::ColorRole::AccentHover) : _accentColor);
					p.drawRoundedRect(pill.rect, 7, 7);
					p.setFont(primaryBtnFont);
					p.setPen(_onAccentColor);
				} else {
					p.setPen(QPen(hovered ? _accentColor : _assistantBubbleBorder, 1.0));
					p.setBrush(hovered ? _hoverColor : _assistantBubbleBg);
					p.drawRoundedRect(pill.rect, 7, 7);
					p.setFont(pillFont);
					p.setPen(_assistantTextFg);
				}
				p.drawText(pill.rect, Qt::AlignCenter | Qt::TextSingleLine, pill.label);
			}
			p.restore();
		}

		// Follow-up suggestion pills
		if (!item.followupPills.isEmpty()) {
			p.save();
			p.setFont(pillFont);
			for (const auto &pill : item.followupPills) {
				if (pill.rect.isEmpty() || !pill.rect.intersects(clip)) {
					continue;
				}
				p.setPen(QPen(QColor(_accentColor.red(), _accentColor.green(), _accentColor.blue(), 80), 1.0));
				p.setBrush(_accentSoftColor);
				p.drawRoundedRect(pill.rect, 11, 11);
				p.setPen(_accentColor);
				p.drawText(
					pill.rect,
					Qt::AlignCenter | Qt::TextSingleLine,
					pill.label + QStringLiteral("  ›"));
			}
			p.restore();
		}

		// Bottom Assistant Action Bar (Copy + Regenerate)
		if (!item.copyBtnRect.isEmpty() && item.copyBtnRect.intersects(clip)) {
			p.save();
			const bool copyHovered = item.copyBtnRect.contains(_hoverPos);
			const bool msgCopied = (!_copiedMessageId.isEmpty() && _copiedMessageId == item.id);
			if (copyHovered || msgCopied) {
				p.setPen(Qt::NoPen);
				p.setBrush(msgCopied ? _accentSoftColor : _hoverColor);
				p.drawRoundedRect(item.copyBtnRect, 5, 5);
			}
			Theme::icon(
				msgCopied ? Theme::Icon::Check : Theme::Icon::Copy,
				msgCopied ? Theme::ColorRole::Success : Theme::ColorRole::TextSecondary)
				.paint(&p, item.copyBtnRect.adjusted(4, 4, -4, -4), Qt::AlignCenter);
			p.restore();
		}
		if (_actionsEnabled && !item.regenerateBtnRect.isEmpty() && item.regenerateBtnRect.intersects(clip)) {
			p.save();
			if (item.regenerateBtnRect.contains(_hoverPos)) {
				p.setPen(Qt::NoPen);
				p.setBrush(_hoverColor);
				p.drawRoundedRect(item.regenerateBtnRect, 5, 5);
			}
			Theme::icon(Theme::Icon::Refresh, Theme::ColorRole::TextSecondary)
				.paint(&p, item.regenerateBtnRect.adjusted(4, 4, -4, -4), Qt::AlignCenter);
			p.restore();
		}
	}
}

void CopilotChatCanvasWidget::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	if (event->oldSize().width() != event->size().width()) {
		relayoutFromIndex(0);
		for (const auto &item : _items) {
			syncUiaProxy(item);
		}
		update();
	}
}

void CopilotChatCanvasWidget::updateHoverCursorAndLink(const QPoint &pos) {
	const QPoint prevPos = _hoverPos;
	_hoverPos = pos;
	const int hovered = hitTestItemIndex(pos.y());
	if (hovered != _hoveredItemIndex) {
		const int prevHovered = _hoveredItemIndex;
		_hoveredItemIndex = hovered;
		if (prevHovered >= 0 && prevHovered < static_cast<int>(_items.size())) {
			update(_items[prevHovered].bubbleRect.united(_items[prevHovered].headerActionRect).adjusted(-2, -2, 2, 2));
		}
		if (_hoveredItemIndex >= 0 && _hoveredItemIndex < static_cast<int>(_items.size())) {
			update(_items[_hoveredItemIndex].bubbleRect.united(_items[_hoveredItemIndex].headerActionRect).adjusted(-2, -2, 2, 2));
		}
	} else if (hovered >= 0 && hovered < static_cast<int>(_items.size()) && prevPos != pos) {
		const auto &it = _items[hovered];
		if (!it.copyBtnRect.isEmpty() || !it.regenerateBtnRect.isEmpty() ||
			!it.actionPills.isEmpty() || !it.headerActionRect.isEmpty()) {
			update(it.bubbleRect.united(it.headerActionRect).adjusted(-2, -2, 2, 2));
		}
	}
	if (hitTestImageThumb(pos) || hitTestInteractiveElement(pos)) {
		ClickHandler::setActive(ClickHandlerPtr(), this);
		setCursor(Qt::PointingHandCursor);
		return;
	}
	ClickHandlerPtr link;
	bool upon = false;
	const auto pt = hitTestSymbol(pos, &link, &upon);
	ClickHandler::setActive(link, this);
	if (_selecting && pt.itemIndex >= 0) {
		_selCursor = pt;
		update();
	}
	if (link) {
		setCursor(Qt::PointingHandCursor);
	} else if (upon) {
		setCursor(Qt::IBeamCursor);
	} else {
		setCursor(Qt::ArrowCursor);
	}
}

void CopilotChatCanvasWidget::mousePressEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	setFocus();
	_mouseDown = true;
	_dragMoved = false;
	_pressPos = event->pos();
	_selectType = TextSelectType::Letters;

	if (hitTestImageThumb(event->pos()) || hitTestInteractiveElement(event->pos())) {
		ClickHandler::setActive(ClickHandlerPtr(), this);
		clearSelection();
		update();
		return;
	}

	ClickHandlerPtr link;
	bool upon = false;
	const auto pt = hitTestSymbol(event->pos(), &link, &upon);
	ClickHandler::setActive(link, this);
	if (link) {
		ClickHandler::pressed();
	}
	if (pt.itemIndex >= 0) {
		_selAnchor = pt;
		_selCursor = pt;
		_selecting = true;
	} else {
		clearSelection();
	}
	update();
}

void CopilotChatCanvasWidget::mouseMoveEvent(QMouseEvent *event) {
	if (_mouseDown && (_pressPos - event->pos()).manhattanLength() >= 4) {
		_dragMoved = true;
	}
	updateHoverCursorAndLink(event->pos());
}

void CopilotChatCanvasWidget::mouseReleaseEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QWidget::mouseReleaseEvent(event);
		return;
	}
	const ClickHandlerPtr activated = ClickHandler::unpressed();
	const bool dragMoved = _dragMoved;
	_mouseDown = false;
	_selecting = false;

	if (!dragMoved && !activated) {
		int hitItemIdx = -1;
		int hitThumbIdx = -1;
		if (hitTestImageThumb(event->pos(), &hitItemIdx, &hitThumbIdx)) {
			const auto &thumb = _items[hitItemIdx].imageThumbs[hitThumbIdx];
			if (!thumb.fullImage.isNull()) {
				showCanvasImagePreview(thumb.fullImage, thumb.fileName);
			}
			updateHoverCursorAndLink(event->pos());
			return;
		}

		const int idx = hitTestItemIndex(event->pos().y());
		if (idx >= 0) {
			auto &item = _items[idx];
			if (_actionsEnabled && !item.headerActionRect.isEmpty() && item.headerActionRect.contains(event->pos())) {
				if (item.role == AgentChatMessage::Role::User) {
					emit editMessageRequested(item.id, item.rawContent, item.rawContextChips);
				} else if (item.role == AgentChatMessage::Role::Assistant &&
					item.status != AgentChatMessage::Status::Streaming) {
					emit regenerateMessageRequested(item.id);
				}
				return;
			}
			if (!item.copyBtnRect.isEmpty() && item.copyBtnRect.contains(event->pos())) {
				copyItemFullTextToClipboard(idx);
				_copiedMessageId = item.id;
				_copyFeedbackTimer.start();
				update(item.copyBtnRect.adjusted(-2, -2, 2, 2));
				return;
			}
			if (_actionsEnabled && !item.regenerateBtnRect.isEmpty() && item.regenerateBtnRect.contains(event->pos())) {
				if (item.status == AgentChatMessage::Status::Failed ||
					item.status == AgentChatMessage::Status::Cancelled) {
					emit retryRequested();
				} else {
					emit regenerateMessageRequested(item.id);
				}
				return;
			}
			if (item.hasThinking && item.thinkingToggleRect.contains(event->pos())) {
				_thinkingExpandedOverrides.insert(item.id, !isThinkingExpanded(item));
				relayoutFromIndex(idx);
				for (int i = idx; i < static_cast<int>(_items.size()); ++i) {
					syncUiaProxy(_items[i]);
				}
				update();
				return;
			}
			for (auto &row : item.toolRows) {
				if (row.rowRect.contains(event->pos()) && !row.detailsText.isEmpty()) {
					if (_expandedToolRowKeys.contains(row.rowKey)) {
						_expandedToolRowKeys.remove(row.rowKey);
						row.expanded = false;
					} else {
						_expandedToolRowKeys.insert(row.rowKey);
						row.expanded = true;
					}
					relayoutFromIndex(idx);
					for (int i = idx; i < static_cast<int>(_items.size()); ++i) {
						syncUiaProxy(_items[i]);
					}
					update();
					return;
				}
			}
			for (const auto &seg : item.segments) {
				if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock &&
					seg.codeCopyBtnRect.contains(event->pos())) {
					QGuiApplication::clipboard()->setText(seg.rawCode);
					_copiedCodeBlockKey = seg.codeKey;
					_copyFeedbackTimer.start();
					update(seg.codeHeaderRect);
					return;
				}
			}
			for (const auto &pill : item.actionPills) {
				if (pill.rect.contains(event->pos())) {
					if (pill.payload == QStringLiteral("retry")) {
						emit retryRequested();
					} else if (pill.payload == QStringLiteral("settings")) {
						emit openSettingsRequested();
					}
					return;
				}
			}
			for (const auto &pill : item.followupPills) {
				if (pill.rect.contains(event->pos())) {
					emit followupClicked(pill.payload);
					return;
				}
			}
		}
	}

	if (activated && !dragMoved) {
		const QString urlStr = activated->url();
		if (urlStr.startsWith(QStringLiteral("cohavora://"), Qt::CaseInsensitive)) {
			emit internalActionTriggered(QUrl(urlStr));
		} else if (!urlStr.isEmpty() &&
			(urlStr.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
			 urlStr.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))) {
			QDesktopServices::openUrl(QUrl(urlStr));
		} else {
			ActivateClickHandler(this, activated, event->button());
		}
		clearSelection();
	} else if (_selAnchor == _selCursor) {
		clearSelection();
	}
	updateHoverCursorAndLink(event->pos());
}

void CopilotChatCanvasWidget::mouseDoubleClickEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}
	const auto pt = hitTestSymbol(event->pos());
	if (pt.itemIndex >= 0) {
		_selectType = TextSelectType::Words;
		_selAnchor = pt;
		_selCursor = pt;
		_selecting = false;
		update();
	}
}

void CopilotChatCanvasWidget::keyPressEvent(QKeyEvent *event) {
	if (event->matches(QKeySequence::Copy) && hasSelectedText()) {
		copySelectedTextToClipboard();
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void CopilotChatCanvasWidget::contextMenuEvent(QContextMenuEvent *event) {
	ClickHandlerPtr link;
	const auto pt = hitTestSymbol(event->pos(), &link);

	QMenu menu(this);
	Theme::styleMenu(menu, window() && window()->palette().color(QPalette::Window).lightness() < 128
		? Theme::Tone::Dark
		: Theme::Tone::Light);

	if (hasSelectedText()) {
		menu.addAction(
			QCoreApplication::translate("MeetingUI", "Copy Selected"),
			this,
			&CopilotChatCanvasWidget::copySelectedTextToClipboard);
	}
	if (pt.itemIndex >= 0) {
		const int itemIdx = pt.itemIndex;
		const auto &targetItem = _items[itemIdx];
		for (const auto &seg : targetItem.segments) {
			if (seg.type == ParsedMarkdownBlock::Segment::Type::CodeBlock &&
				seg.rect.contains(event->pos()) &&
				!seg.rawCode.isEmpty()) {
				const QString codeCopy = seg.rawCode;
				menu.addAction(
					QCoreApplication::translate("MeetingUI", "Copy Code Block"),
					this,
					[codeCopy] { QGuiApplication::clipboard()->setText(codeCopy); });
				break;
			}
		}
		menu.addAction(
			QCoreApplication::translate("MeetingUI", "Copy Message"),
			this,
			[this, itemIdx] { copyItemFullTextToClipboard(itemIdx); });
		if (_actionsEnabled && !targetItem.isActionCard) {
			if (targetItem.role == AgentChatMessage::Role::User) {
				const QString msgId = targetItem.id;
				const QString content = targetItem.rawContent;
				const auto chips = targetItem.rawContextChips;
				menu.addAction(
					QCoreApplication::translate("MeetingUI", "Edit & Resend"),
					this,
					[this, msgId, content, chips] {
						emit editMessageRequested(msgId, content, chips);
					});
			} else if (targetItem.role == AgentChatMessage::Role::Assistant &&
				targetItem.status != AgentChatMessage::Status::Streaming) {
				const QString msgId = targetItem.id;
				menu.addAction(
					QCoreApplication::translate("MeetingUI", "Regenerate Response"),
					this,
					[this, msgId] { emit regenerateMessageRequested(msgId); });
			}
		}
	}
	if (link && !link->copyToClipboardText().isEmpty()) {
		const QString copyLinkText = link->copyToClipboardText();
		const QString label = !link->copyToClipboardContextItemText().isEmpty()
			? link->copyToClipboardContextItemText()
			: QCoreApplication::translate("MeetingUI", "Copy Link");
		menu.addAction(label, this, [copyLinkText] {
			QGuiApplication::clipboard()->setText(copyLinkText);
		});
	}
	if (!_items.empty()) {
		if (!menu.isEmpty()) {
			menu.addSeparator();
		}
		menu.addAction(
			QCoreApplication::translate("MeetingUI", "Export Conversation (.md)"),
			this,
			&CopilotChatCanvasWidget::exportConversationRequested);
	}

	if (!menu.isEmpty()) {
		menu.exec(event->globalPos());
		event->accept();
	}
}

void CopilotChatCanvasWidget::leaveEvent(QEvent *event) {
	ClickHandler::clearActive(this);
	_hoverPos = QPoint(-1, -1);
	if (_hoveredItemIndex >= 0) {
		const int prev = _hoveredItemIndex;
		_hoveredItemIndex = -1;
		if (prev < static_cast<int>(_items.size())) {
			update(_items[prev].bubbleRect.united(_items[prev].headerActionRect).adjusted(-2, -2, 2, 2));
		}
	}
	if (!_selecting) {
		setCursor(Qt::ArrowCursor);
	}
	QWidget::leaveEvent(event);
}

} // namespace OpenMeeting
