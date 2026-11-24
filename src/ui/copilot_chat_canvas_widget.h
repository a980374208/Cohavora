#pragma once

#include "src/agent/agent_types.h"
#include "src/ui/copilot_markdown_parser.h"
#include "ui/click_handler.h"
#include "ui/text/text.h"

#include <QtCore/QHash>
#include <QtCore/QPoint>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QSet>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtWidgets/QLabel>
#include <QtWidgets/QWidget>

#include <memory>
#include <vector>

namespace OpenMeeting {

[[nodiscard]] QPixmap makeRoundedCopilotThumbnail(
	const QImage &src,
	const QSize &logicalSize,
	qreal dpr,
	int radius = 6);
void showCopilotImagePreviewDialog(const QImage &img, const QString &title, QWidget *parent = nullptr);

struct CanvasSelectionPoint {
	int itemIndex = -1;
	uint16 symbol = 0;

	bool operator==(const CanvasSelectionPoint &other) const = default;
};

// Single-canvas virtualized message list backed by tdesktop Ui::Text::String.
// Eliminates per-bubble QWidget/QLayout cascades during 40ms SSE streaming updates,
// performs O(log N) viewport culling on paint, and supports cross-bubble selection.
class CopilotChatCanvasWidget final : public QWidget, public ClickHandlerHost {
	Q_OBJECT
public:
	explicit CopilotChatCanvasWidget(QWidget *parent = nullptr);
	~CopilotChatCanvasWidget() override;

	void setMessages(const QVector<AgentChatMessage> &messages);
	void appendOrUpdateMessage(const AgentChatMessage &message);
	void removeMessage(const QString &messageId);
	void appendOrUpdateActionCard(const QString &actionId, QWidget *cardWidget);
	void clearMessages();
	void setActionsEnabled(bool enabled);

	[[nodiscard]] int totalContentHeight() const { return _totalHeight; }
	[[nodiscard]] bool hasSelectedText() const;
	void copySelectedTextToClipboard() const;
	void clearSelection();

signals:
	void retryRequested();
	void openSettingsRequested();
	void followupClicked(const QString &prompt);
	void internalActionTriggered(const QUrl &url);
	void editMessageRequested(
		const QString &messageId,
		const QString &content,
		const QVector<OpenMeeting::AgentContextChip> &chips);
	void regenerateMessageRequested(const QString &assistantMessageId);
	void exportConversationRequested();

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	static constexpr int kMinBodyTextWidth = 40;

	struct CanvasImageThumb {
		QString fileName;
		QPixmap roundedPixmap;
		QImage fullImage;
		QRect rect;
		QSize logicalSize;
	};

	struct CanvasContextChipBadge {
		QString label;
		QRect rect;
	};

	struct CanvasToolRow {
		QString rowKey;
		QString toolName;
		QString titleText;
		QString detailsText;
		AgentResponsePart::ToolState state = AgentResponsePart::ToolState::Running;
		bool expanded = false;
		QRect rowRect;
		QRect detailsRect;
	};

	struct CanvasTableCell {
		Ui::Text::String text{kMinBodyTextWidth};
		Qt::Alignment align = Qt::AlignLeft;
		QRect rect;
		uint16 symbolOffset = 0;
		uint16 symbolCount = 0;
	};

	struct CanvasSegment {
		ParsedMarkdownBlock::Segment::Type type = ParsedMarkdownBlock::Segment::Type::RichText;
		Ui::Text::String richText{kMinBodyTextWidth};
		QString codeKey;
		QString language;
		QString rawCode;
		QRect rect;
		QRect codeHeaderRect;
		QRect codeCopyBtnRect;
		QRect codeBodyRect;
		std::vector<std::vector<CanvasTableCell>> tableRows;
		QVector<int> tableColWidths;
		QVector<int> tableRowHeights;
		uint16 symbolOffset = 0;
		uint16 symbolCount = 0;
	};

	struct CanvasButtonPill {
		QString label;
		QString payload;
		QRect rect;
		bool isPrimary = false;
	};

	struct CanvasMessageItem {
		QString id;
		bool isActionCard = false;
		bool isHiddenEmptyTurn = false;
		QPointer<QWidget> actionCardWidget;
		AgentChatMessage::Role role = AgentChatMessage::Role::Assistant;
		AgentChatMessage::Status status = AgentChatMessage::Status::Complete;
		QString rawContent;
		QString errorMessage;
		QString headerText;
		QString statusText;

		bool hasErrorCard = false;
		Ui::Text::String errorBodyText{kMinBodyTextWidth};
		uint16 errorSymbolOffset = 0;
		uint16 errorSymbolCount = 0;
		QRect errorCardRect;
		QRect errorIconRect;
		QRect errorBodyRect;

		bool hasThinking = false;
		bool isThinkingActive = false;
		qint64 thinkingDurationMs = 0;
		QString reasoningText;
		QString thinkingHeaderText;
		Ui::Text::String thinkingBodyText{kMinBodyTextWidth};
		QRect thinkingCardRect;
		QRect thinkingToggleRect;
		QRect thinkingBodyRect;

		QVector<CanvasContextChipBadge> chipBadges;
		QVector<AgentContextChip> rawContextChips;
		QVector<CanvasToolRow> toolRows;
		std::vector<CanvasSegment> segments;
		QVector<CanvasImageThumb> imageThumbs;
		QVector<CanvasButtonPill> actionPills;
		QVector<CanvasButtonPill> followupPills;
		uint16 totalSelectableSymbols = 0;

		int y = 0;
		int height = 0;
		QRect bubbleRect;
		QRect headerRect;
		QRect headerActionRect;
		QRect chipsAreaRect;
		QRect imagesAreaRect;
		QRect bodyRect;
		QRect statusRect;
		QRect copyBtnRect;
		QRect regenerateBtnRect;
		int cachedForWidth = -1;
	};

	struct ThemePaletteHolder;
	struct UiaProxyPair {
		QLabel *textProxy = nullptr;
		QLabel *statusProxy = nullptr;
	};

	void rebuildThemeResources();
	void applyMessageContent(CanvasMessageItem &item, const AgentChatMessage &src);
	[[nodiscard]] bool isThinkingExpanded(const CanvasMessageItem &item) const;
	void relayoutFromIndex(int startIndex);
	void layoutSingleItem(CanvasMessageItem &item, int canvasWidth, int topY);
	void refreshFollowupVisibility();
	void syncUiaProxy(const CanvasMessageItem &item);
	void clearUiaProxies();

	[[nodiscard]] int findFirstVisibleIndex(int clipTop) const;
	[[nodiscard]] int hitTestItemIndex(int y) const;
	[[nodiscard]] bool hitTestImageThumb(const QPoint &pos, int *outItemIdx = nullptr, int *outThumbIdx = nullptr) const;
	[[nodiscard]] bool hitTestInteractiveElement(const QPoint &pos, int *outItemIdx = nullptr) const;
	[[nodiscard]] CanvasSelectionPoint hitTestSymbol(const QPoint &pos, ClickHandlerPtr *outLink = nullptr, bool *outUpon = nullptr) const;
	[[nodiscard]] std::pair<CanvasSelectionPoint, CanvasSelectionPoint> normalizedSelectionRange() const;
	[[nodiscard]] TextSelection itemSelection(int itemIndex) const;
	void updateHoverCursorAndLink(const QPoint &pos);
	void showCanvasImagePreview(const QImage &img, const QString &title);
	void copyItemFullTextToClipboard(int itemIndex) const;

	std::vector<CanvasMessageItem> _items;
	QHash<QString, int> _idToIndex;
	int _totalHeight = 0;
	int _lastMessageItemIndex = -1;
	int _hoveredItemIndex = -1;
	bool _actionsEnabled = true;

	std::unique_ptr<ThemePaletteHolder> _theme;
	QColor _userBubbleBg;
	QColor _userMetaFg;
	QColor _userTextFg;
	QColor _assistantBubbleBg;
	QColor _assistantBubbleBorder;
	QColor _assistantMetaFg;
	QColor _assistantTextFg;
	QColor _surfaceAltColor;
	QColor _hoverColor;
	QColor _accentColor;
	QColor _accentSoftColor;
	QColor _onAccentColor;
	QColor _statusMutedFg;
	QColor _statusErrorFg;
	QColor _successColor;

	QHash<QString, bool> _thinkingExpandedOverrides;
	QSet<QString> _expandedToolRowKeys;
	QString _copiedCodeBlockKey;
	QString _copiedMessageId;
	QTimer _copyFeedbackTimer;
	QPoint _hoverPos{-1, -1};

	bool _selecting = false;
	bool _mouseDown = false;
	bool _dragMoved = false;
	QPoint _pressPos;
	TextSelectType _selectType = TextSelectType::Letters;
	CanvasSelectionPoint _selAnchor;
	CanvasSelectionPoint _selCursor;

	QHash<QString, UiaProxyPair> _uiaProxies;
};

} // namespace OpenMeeting
