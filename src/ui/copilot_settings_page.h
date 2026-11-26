#pragma once

#include "src/net/session_manager.h"

#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <array>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace OpenMeeting {
class AgentCoordinator;
enum class AgentSpeechProtocolMode;
enum class AgentSpeechSourceMode;
struct AgentProfile;
struct AgentSpeechProfile;
}

namespace MeetingUI {

class SettingsCollapsibleSection;

class CopilotSettingsPage final : public QWidget {
	Q_OBJECT

public:
	explicit CopilotSettingsPage(
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr);
	~CopilotSettingsPage() override;

	void setAgentCoordinator(OpenMeeting::AgentCoordinator *coordinator, bool pageCurrentlyVisible);
	void setSubTab(int index);
	int currentSubTab() const;
	void maybeAutoRefreshModels();
	void syncControls();
	void connectTranslationControls();
	void readTranslationPreferences(OpenMeeting::MediaPreferences &value) const;
	void applyTranslationPreferences(const OpenMeeting::MediaPreferences &value);
	void focusSearchMatch(const QString &needle);
	void refreshThemeVisuals();
	void clearKeyEditors();
	void flushBeforeClose();
	void shutdown();

signals:
	void preferencesChanged();

private:
	void buildUi();
	void disconnectCopilotCoordinatorSignals();
	void setCopilotModelHint(const QString &text, const QString &tone = QString());
	void resetCopilotModelHint();
	void setCopilotTestStatus(const QString &tone, const QString &text);
	void resetCopilotTestStatus();
	void setCopilotSttTestStatus(const QString &tone, const QString &text);
	void resetCopilotSttTestStatus();
	[[nodiscard]] bool copilotSttReusesChatHost() const;
	[[nodiscard]] OpenMeeting::AgentSpeechSourceMode selectedCopilotSpeechSourceMode() const;
	void commitCopilotProfile(bool includeTypedApiKey = false);
	void saveCopilotKey();
	void clearCopilotKey();
	void clearCopilotKeyEditor();
	void updateCopilotKeyStatus();
	void saveCopilotSttKey();
	void clearCopilotSttKey();
	void updateCopilotSttKeyStatus();
	void saveCopilotSttAppKey();
	void clearCopilotSttAppKey();
	void updateCopilotSttAppKeyStatus();
	void applyCopilotSpeechProtocolPreset(OpenMeeting::AgentSpeechProtocolMode protocol, bool forceOverwrite);
	void updateCopilotSttProtocolUi(bool refreshModelChoices = true);
	void updateCopilotSttModelChoices(bool fromServerFetch = false);
	void testCopilotConnection();
	void testCopilotSttConnection();
	void refreshCopilotModels(bool autoTriggered = false);
	void refreshCopilotSpeechModels(bool autoTriggered = false);
	void updateCopilotBaseUrlPlaceholder();
	void updateCopilotReasoningCapabilityHint();
	void refreshCopilotProfileSelectors();
	void refreshCopilotSpeechProfileSelectors();
	void onCopilotProfileSelected(int index);
	void onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode mode);
	void onCopilotSpeechProfileSelected(int index);
	void createNewCopilotProfile(bool duplicateCurrent);
	void deleteCurrentCopilotProfile();
	void showCopilotProfileMenu();
	void createNewCopilotSpeechProfile(bool duplicateCurrent);
	void deleteCurrentCopilotSpeechProfile();
	void showCopilotSpeechProfileMenu();
	void updateCopilotAdvSummaries();
	void updateCopilotSttModeUi();
	void updateCopilotTranslationLinkedUi();
	void resetCopilotChatAdvancedDefaults();
	void resetCopilotSttAdvancedDefaults();
	void resetCopilotTranslationAdvancedDefaults();
	OpenMeeting::AgentProfile collectCopilotProfileFromUi() const;
	OpenMeeting::AgentSpeechProfile collectCopilotSpeechProfileFromUi() const;
	OpenMeeting::AgentCoordinator &copilotCoordinator();

	OpenMeeting::SessionManager &_session;
	bool _updatingUi = false;
	OpenMeeting::AgentCoordinator *_agentCoordinator = nullptr;
	std::unique_ptr<OpenMeeting::AgentCoordinator> _ownedAgentCoordinator;
	QMetaObject::Connection _agentProfileConn;
	QMetaObject::Connection _agentProfilesConn;
	QMetaObject::Connection _agentSpeechProfilesConn;
	QMetaObject::Connection _agentProbeConn;
	QMetaObject::Connection _agentSpeechProbeConn;
	QMetaObject::Connection _agentModelsConn;
	QMetaObject::Connection _agentSpeechModelsConn;
	QString _copilotLastModelFetchBaseUrl;
	QString _copilotLastSttModelFetchUrl;
	QString _copilotLastSttEndpointKey;
	bool _copilotSttModelAutoSelected = false;
	QStackedWidget *_copilotSubPages = nullptr;
	std::array<QPushButton *, 3> _copilotSubTabButtons{};
	SettingsCollapsibleSection *_copilotChatAdvSection = nullptr;
	SettingsCollapsibleSection *_copilotSttAdvSection = nullptr;
	SettingsCollapsibleSection *_copilotTrAdvSection = nullptr;
	QComboBox *_copilotProfileSelector = nullptr;
	QPushButton *_copilotNewProfileBtn = nullptr;
	QToolButton *_copilotProfileMoreBtn = nullptr;
	QLineEdit *_copilotProfileName = nullptr;
	QComboBox *_copilotProtocolCombo = nullptr;
	QLineEdit *_copilotBaseUrl = nullptr;
	QComboBox *_copilotModelCombo = nullptr;
	QPushButton *_copilotRefreshModelsBtn = nullptr;
	QToolButton *_copilotModelHintIcon = nullptr;
	QComboBox *_copilotReasoningCombo = nullptr;
	QToolButton *_copilotReasoningHintIcon = nullptr;
	QLineEdit *_copilotApiKey = nullptr;
	QPushButton *_copilotSaveKeyBtn = nullptr;
	QPushButton *_copilotClearKeyBtn = nullptr;
	QToolButton *_copilotKeyHintIcon = nullptr;
	QCheckBox *_copilotEncryptKeyCheck = nullptr;
	QCheckBox *_copilotPersistHistoryCheck = nullptr;
	QPushButton *_copilotClearHistoryBtn = nullptr;
	QPlainTextEdit *_copilotSystemPrompt = nullptr;
	QPlainTextEdit *_copilotCustomHeaders = nullptr;
	QSpinBox *_copilotMaxHistorySpin = nullptr;
	QDoubleSpinBox *_copilotTemperatureSpin = nullptr;
	QSpinBox *_copilotConnectTimeoutSpin = nullptr;
	QSpinBox *_copilotIdleTimeoutSpin = nullptr;
	QPushButton *_copilotSttModeFollowBtn = nullptr;
	QPushButton *_copilotSttModeCustomBtn = nullptr;
	QWidget *_copilotSttProfileToolbar = nullptr;
	QComboBox *_copilotSttProfileSelector = nullptr;
	QPushButton *_copilotSttNewProfileBtn = nullptr;
	QToolButton *_copilotSttProfileMoreBtn = nullptr;
	QLabel *_copilotSttScopeBadge = nullptr;
	QFrame *_copilotSttFollowBanner = nullptr;
	QLabel *_copilotSttFollowLabel = nullptr;
	QWidget *_copilotSttProfileNameBlock = nullptr;
	QLineEdit *_copilotSttProfileName = nullptr;
	QWidget *_copilotSttEndpointContainer = nullptr;
	QLabel *_copilotSttKeyWarningBanner = nullptr;
	QLineEdit *_copilotSttBaseUrl = nullptr;
	QLabel *_copilotSttKeyLabel = nullptr;
	QLineEdit *_copilotSttApiKey = nullptr;
	QPushButton *_copilotSaveSttKeyBtn = nullptr;
	QPushButton *_copilotClearSttKeyBtn = nullptr;
	QToolButton *_copilotSttKeyHintIcon = nullptr;
	QComboBox *_copilotSttProtocolCombo = nullptr;
	QComboBox *_copilotSttModelCombo = nullptr;
	QWidget *_copilotSttVolcContainer = nullptr;
	QLineEdit *_copilotSttAppKey = nullptr;
	QPushButton *_copilotSaveSttAppKeyBtn = nullptr;
	QPushButton *_copilotClearSttAppKeyBtn = nullptr;
	QToolButton *_copilotSttAppKeyHintIcon = nullptr;
	QComboBox *_copilotSttResourceIdCombo = nullptr;
	QWidget *_copilotSttWorkspaceContainer = nullptr;
	QLineEdit *_copilotSttWorkspaceEdit = nullptr;
	QComboBox *_copilotDictationLangCombo = nullptr;
	QSpinBox *_copilotSpeechTimeoutSpin = nullptr;
	QSpinBox *_copilotVadThresholdSpin = nullptr;
	QSpinBox *_copilotVadSilenceCommitSpin = nullptr;
	QLineEdit *_copilotSttPromptEdit = nullptr;
	QCheckBox *_translationEnableLocal = nullptr;
	QComboBox *_translationRoleModeCombo = nullptr;
	QCheckBox *_translationPreferRoomAgent = nullptr;
	QCheckBox *_translationEnableOutput = nullptr;
	QWidget *_translationTargetContainer = nullptr;
	QComboBox *_translationTargetLangCombo = nullptr;
	QLabel *_captionStyleReuseHint = nullptr;
	QComboBox *_captionDisplayModeCombo = nullptr;
	QWidget *_translationOverrideGroup = nullptr;
	QLabel *_translationOverrideInactiveHint = nullptr;
	QComboBox *_translationProfileCombo = nullptr;
	QLineEdit *_translationModelEdit = nullptr;
	QPlainTextEdit *_translationPromptEdit = nullptr;
	QTimer *_translationPromptCommitTimer = nullptr;
	QCheckBox *_translationIncludeLocalMic = nullptr;
	QSpinBox *_translationMaxConcurrentStreamsSpin = nullptr;
	QSpinBox *_translationIdleReleaseSpin = nullptr;
	QPushButton *_copilotTestBtn = nullptr;
	QLabel *_copilotTestStatus = nullptr;
	QPushButton *_copilotSttTestBtn = nullptr;
	QLabel *_copilotSttTestStatus = nullptr;
};

} // namespace MeetingUI
