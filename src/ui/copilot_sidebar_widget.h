#pragma once

#include "src/agent/agent_coordinator.h"
#include "src/agent/agent_speech_service.h"
#include "src/ui/audio_device_discovery.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QHideEvent>
#include <QtGui/QImage>
#include <QtGui/QMoveEvent>
#include <QtGui/QPixmap>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <memory>

namespace OpenMeeting {

class CopilotChatCanvasWidget;
template <typename T> class QtCallbackGate;

class CopilotSidebarWidget final : public QWidget {
	Q_OBJECT
public:
	explicit CopilotSidebarWidget(AgentCoordinator *coordinator, QWidget *parent = nullptr);
	~CopilotSidebarWidget() override;

	void setCoordinator(AgentCoordinator *coordinator);
	void setLeftEdgeShadowEnabled(bool enabled);
	void setExpanded(bool expanded);
	[[nodiscard]] bool isExpanded() const { return _expanded; }
	void focusInput();

signals:
	void closeRequested();
	void expandToggleRequested();
	void openSettingsRequested();
	void openSettingsPageRequested(int pageIndex, int subTabIndex);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void changeEvent(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void moveEvent(QMoveEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	struct PendingActionUi {
		QWidget *card = nullptr;
		QLabel *titleLabel = nullptr;
		QLabel *summaryLabel = nullptr;
		QPushButton *confirmBtn = nullptr;
		QPushButton *rejectBtn = nullptr;
		QLabel *statusLabel = nullptr;
	};

	void buildUi();
	void bindCoordinator();
	void rebuildMessages();
	void rebuildHistoryList();
	void syncHeaderAndState();
	void syncActionButton();
	void syncThemeVisuals();
	void retranslateUi();
	void updateSessionTitleText();
	void updateModelSwitchLabel();
	void updateModePermLabel();
	void appendPendingActionUi(const AgentPendingToolAction &action);
	void updatePendingActionUi(const AgentPendingToolAction &action);
	void scrollToBottomIfFollowing(bool force = false);
	bool isNearBottom() const;
	void onSendOrStopClicked();
	void onClearClicked();
	void onAttachImageClicked();
	bool addPendingImageFromFile(const QString &filePath, QString *outError = nullptr);
	bool addPendingImageFromQImage(const QImage &image, const QString &sourceName, QString *outError = nullptr);
	void removePendingImageAt(int index);
	void clearPendingImages();
	void rebuildAttachmentPreviewStrip();
	void showRequestError(const QString &safeError);
	void clearRequestError();
	void onMoreMenuClicked();
	void onModelSwitchClicked();
	void onModePermClicked();
	void onDictationToggled();
	void setDictationVisible(bool visible);
	void refreshDictationMicrophones();
	void bindSpeechService();
	void onSpeechEvent(const SpeechToTextEvent &event);
	void onDictationMicActivated();
	void updateDictationStatusHint(const QString &statusText = QString(), bool isError = false);
	void onHistoryViewToggled();
	void setHistoryViewVisible(bool visible);
	void onRenameActiveSession();
	void onDeleteActiveSession();
	void updateChoiceCombosTheme();
	void updateInputEditHeight();
	void updateScrollToBottomButton();
	void onAddContextClicked();
	void addPendingContextChip(AgentContextChipKind kind);
	void removePendingContextChip(AgentContextChipKind kind);
	void clearPendingContextChips();
	void rebuildContextChipsStrip();
	void updateCompletionPopup();
	void hideCompletionPopup();
	void applyCompletionItem(QListWidgetItem *item);
	void handleInternalActionUrl(const QUrl &url);
	void onNewChatClicked();
	void onExportConversationClicked();
	void beginEditingMessage(
		const QString &messageId,
		const QString &content,
		const QVector<AgentContextChip> &chips);
	void cancelEditingMessage();
	void syncLeftEdgeShadow();

	QPointer<AgentCoordinator> _coordinator;
	QVector<QMetaObject::Connection> _coordinatorConnections;
	bool _leftEdgeShadowEnabled = false;
	bool _expanded = false;
	bool _historyVisible = false;
	bool _dictationVisible = false;
	bool _pendingModelsRefreshMenu = false;
	QPointer<QWidget> _leftEdgeShadow;
	std::shared_ptr<QtCallbackGate<CopilotSidebarWidget>> _callbackGate;
	MeetingUI::AudioDeviceDiscovery::Request _micDiscoveryRequest;

	// Layer 1: Primary Tab Header
	QLabel *_chatTabLabel = nullptr;
	QPushButton *_moreActionsBtn = nullptr;
	QPushButton *_newChatBtn = nullptr;
	QPushButton *_settingsBtn = nullptr;
	QPushButton *_expandBtn = nullptr;
	QPushButton *_closeBtn = nullptr;

	// Layer 2: Session Sub-header
	QPushButton *_backSessionsBtn = nullptr;
	QLabel *_sessionTitleLabel = nullptr;
	QPushButton *_sessionsBtn = nullptr;
	QString _fullSessionTitle;

	QWidget *_contextBar = nullptr;
	QWidget *_e2eeBanner = nullptr;
	QLabel *_e2eeBannerLabel = nullptr;
	QPushButton *_e2eeAuthorizeBtn = nullptr;
	QPushButton *_historyRetryBtn = nullptr;

	QStackedWidget *_bodyStack = nullptr;
	QWidget *_chatStreamPage = nullptr;
	QWidget *_historyPage = nullptr;
	QPushButton *_historyNewBtn = nullptr;
	QListWidget *_historyList = nullptr;
	QPushButton *_historyRenameBtn = nullptr;
	QPushButton *_historyDeleteBtn = nullptr;
	QPushButton *_historyExportBtn = nullptr;

	QWidget *_unconfiguredCard = nullptr;
	QLabel *_unconfiguredTitle = nullptr;
	QLabel *_unconfiguredBody = nullptr;
	QPushButton *_configureBtn = nullptr;

	QWidget *_emptyCard = nullptr;
	QLabel *_emptyLabel = nullptr;

	QScrollArea *_scrollArea = nullptr;
	QWidget *_scrollContent = nullptr;
	CopilotChatCanvasWidget *_chatCanvas = nullptr;
	QPushButton *_scrollToBottomBtn = nullptr;
	QHash<QString, PendingActionUi> _actionWidgets;

	QFrame *_completionPopup = nullptr;
	QListWidget *_completionList = nullptr;

	QWidget *_inputPanel = nullptr;
	QWidget *_errorBanner = nullptr;
	QLabel *_errorBannerLabel = nullptr;
	QPushButton *_errorSettingsBtn = nullptr;
	QPushButton *_errorDismissBtn = nullptr;
	QWidget *_editingBanner = nullptr;
	QLabel *_editingBannerLabel = nullptr;
	QPushButton *_editingCancelBtn = nullptr;
	QString _editingMessageId;

	// Dictation voice capture card with real-time energy waveform
	QFrame *_dictationCard = nullptr;
	QPushButton *_dictationCloseBtn = nullptr;
	QLabel *_dictationSubtitle = nullptr;
	QComboBox *_dictationMicCombo = nullptr;
	QWidget *_dictationWaveform = nullptr;
	QLabel *_dictationStatusHint = nullptr;
	AgentSpeechService *_speechService = nullptr;
	DictationAnchorTracker _dictationTracker;
	QTimer *_dictationAutoSendTimer = nullptr;
	bool _applyingDictationEdit = false;
	bool _dictationInfoShown = false;

	// Unified Composer Card
	QFrame *_composerCard = nullptr;
	QWidget *_contextChipsStrip = nullptr;
	QVBoxLayout *_contextChipsRowsLayout = nullptr;
	QWidget *_attachmentStrip = nullptr;
	QHBoxLayout *_attachmentCardsLayout = nullptr;
	QLabel *_attachmentSummaryLabel = nullptr;
	QLabel *_attachmentWarningLabel = nullptr;
	QPlainTextEdit *_inputEdit = nullptr;
	QPushButton *_addContextBtn = nullptr;
	QPushButton *_modelSwitchBtn = nullptr;
	QPushButton *_dictationBtn = nullptr;
	QPushButton *_actionBtn = nullptr;
	QString _fullModelDisplayText;

	// Bottom Status / Mode & Permission Footer
	QPushButton *_modePermBtn = nullptr;
	QPushButton *_statusIndicatorBtn = nullptr;
	QString _fullModePermText;

	QVector<AgentContextChip> _pendingContextChips;
	QVector<AgentImageAttachment> _pendingImages;
	QVector<QPixmap> _pendingThumbnails;
	QString _lastAttachmentError;
};

} // namespace OpenMeeting
