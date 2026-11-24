#pragma once
#include "src/telemetry/ui_timing_measurement.h"

#include "src/net/session_manager.h"
#include "src/ui/camera_device_discovery.h"

#include <QtCore/QHash>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;

namespace OpenMeeting {
template <typename T> class QtCallbackGate;
class AgentCoordinator;
enum class AgentSpeechSourceMode;
struct AgentProfile;
struct AgentSpeechProfile;
}
namespace livekit { struct VideoEncoderCapabilities; }
namespace Cohavora::Update { class UpdateService; }

namespace MeetingUI {

class AudioDeviceTestController;
class CameraPreviewWidget;
class SettingsCollapsibleSection;

class SettingsDialog final : public QDialog {
	Q_OBJECT

public:
	enum class Page {
		General = 0,
		Video,
		Audio,
		Security,
		Copilot,
		About,
	};

	void setAgentCoordinator(OpenMeeting::AgentCoordinator *coordinator);

	explicit SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr,
		CameraDeviceDiscovery::Clock::time_point clickedAt = CameraDeviceDiscovery::Clock::now());
	~SettingsDialog() override;

	OpenMeeting::MediaPreferences preferences() const;
	QString selectedCameraDeviceId() const;
	QString selectedMicrophoneDeviceId() const;
	QString selectedSpeakerDeviceId() const;
	QWidget *videoPreviewHost() const;

	void showPage(Page page);
	Page currentPage() const;
	void setCopilotSubTab(int index);
	int currentCopilotSubTab() const;
	void setPreferences(const OpenMeeting::MediaPreferences &preferences);
	void setVideoPreviewWidget(QWidget *previewWidget);

public slots:
	void done(int result) override;
	void refreshDevices();
	void setMicrophoneLevel(float normalizedLevel);
	void setMicrophoneTestActive(bool active);
	void setSpeakerTestActive(bool active);
	void setVideoPreviewMessage(const QString &message, bool error = false);

signals:
	void preferencesEdited(const OpenMeeting::MediaPreferences &preferences);
	void cameraSelectionChanged(
		const QString &deviceId,
		int captureWidth,
		int captureHeight,
		int captureFps);
	void cameraPreviewRequested(
		const QString &deviceId,
		int captureWidth,
		int captureHeight,
		int captureFps);
	void cameraPreviewStopped();
	void microphoneSelectionChanged(const QString &deviceId);
	void speakerSelectionChanged(const QString &deviceId);
	void microphoneTestStarted(const QString &deviceId);
	void microphoneTestStopped();
	void speakerTestRequested(const QString &deviceId);
	void languageChanged();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	friend class SettingsDialogInspectionAccess;
	struct VideoFormat {
		int width = 0;
		int height = 0;
		int fps = 0;
	};

	void buildUi();
	void applyVideoEncoderCapabilities(const livekit::VideoEncoderCapabilities &capabilities);
	void syncVideoCodecChoices(const QString &cameraCodec, const QString &screenShareCodec);
	QWidget *buildGeneralPage();
	QWidget *buildVideoPage();
	QWidget *buildAudioPage();
	QWidget *buildSecurityPage();
	QWidget *buildCopilotPage();
	QWidget *buildAboutPage();
	void syncUpdateControls();
	void syncThemeControls();
	void syncNavigationHeader();
	void filterNavigation(const QString &query);
	void refreshDialogThemeVisuals();
	void syncSecurityControls();
	void commitSecurityPreferences();
	void updateSecurityKeyControls();
	void saveSecurityKey();
	void clearSecurityEditor();
	void connectPreferenceControls();
	void connectDeviceControllers();
	void prepareDevicePlaceholders();
	void startDeviceDiscovery(bool forceRefresh);
	void applyVideoDevices(const CameraDeviceDiscovery::Devices &devices);
	void cancelDeviceDiscovery();
	void rebuildResolutionChoices(const OpenMeeting::MediaPreferences &preferences);
	int defaultFormatIndex(const QVector<VideoFormat> &formats) const;
	VideoFormat selectedVideoFormat() const;
	void syncJoinPreferenceControls(QCheckBox *source);
	void updateMirrorControls();
	void updateHighDefinitionControls();
	void commitPreferences();
	void emitCameraSelection();
	void requestPreviewIfVisible();

	OpenMeeting::SessionManager &_session;
	CameraDeviceDiscovery *_cameraDiscovery = &CameraDeviceDiscovery::Instance();
	CameraDeviceDiscovery::Request _cameraRequest;
	std::shared_ptr<OpenMeeting::QtCallbackGate<SettingsDialog>> _cameraCallbacks;
	CameraDeviceDiscovery::Clock::time_point _clickedAt;
	std::shared_ptr<livekit::diagnostic::UiTimingMeasurement> _paintTiming;
	bool _firstPaintScheduled = false;
	bool _firstPaintReported = false;
	bool _forceDeviceRefresh = false;
	bool _videoDevicesReady = false;
	bool _videoEncoderCapabilitiesReady = false;
	std::shared_ptr<const livekit::VideoEncoderCapabilities> _videoEncoderCapabilities;
	QString _videoCodecBackend;
	bool _videoCodecChoicesDirty = true;
	bool _closed = false;
	QListWidget *_navigation = nullptr;
	QStackedWidget *_pages = nullptr;
	QLabel *_pageHeaderLabel = nullptr;
	QLineEdit *_searchEdit = nullptr;
	QPushButton *_closeButton = nullptr;
	std::array<QPushButton *, 4> _themeSwatchButtons{};

	QComboBox *_themeCombo = nullptr;
	QComboBox *_languageCombo = nullptr;
	QComboBox *_participantNameDisplayCombo = nullptr;
	QCheckBox *_generalCamera = nullptr;
	QCheckBox *_generalMicrophone = nullptr;
	QCheckBox *_generalSpeaker = nullptr;
	QCheckBox *_quitOnClose = nullptr;
	QCheckBox *_showActiveSpeaker = nullptr;
	QCheckBox *_showLiveCaptions = nullptr;
	QCheckBox *_stayWhenLocked = nullptr;
	QCheckBox *_continuousStatsSampling = nullptr;

	QFrame *_previewHost = nullptr;
	QLabel *_previewMessage = nullptr;
	CameraPreviewWidget *_cameraPreview = nullptr;
	QComboBox *_cameraCombo = nullptr;
	QComboBox *_resolutionCombo = nullptr;
	QComboBox *_cameraCodecCombo = nullptr;
	QComboBox *_screenShareCodecCombo = nullptr;
	QComboBox *_videoEncoderCombo = nullptr;
	QToolButton *_videoEncoderHintIcon = nullptr;
	QComboBox *_screenShareResolutionCombo = nullptr;
	QComboBox *_screenShareFpsCombo = nullptr;
	QComboBox *_receiveResolutionMatchCombo = nullptr;
	QCheckBox *_videoCamera = nullptr;
	QCheckBox *_highDefinition = nullptr;
	QCheckBox *_mirrorEnabled = nullptr;
	QRadioButton *_localMirror = nullptr;
	QRadioButton *_remoteMirror = nullptr;

	QComboBox *_speakerCombo = nullptr;
	QComboBox *_microphoneCombo = nullptr;
	QPushButton *_speakerTestButton = nullptr;
	QPushButton *_microphoneTestButton = nullptr;
	QProgressBar *_microphoneLevel = nullptr;
	QLabel *_audioStatus = nullptr;
	QCheckBox *_audioMicrophone = nullptr;
	QCheckBox *_audioSpeaker = nullptr;
	QCheckBox *_pushToTalk = nullptr;
	QCheckBox *_echoCancellation = nullptr;
	QCheckBox *_noiseSuppression = nullptr;
	QCheckBox *_autoGainControl = nullptr;
	AudioDeviceTestController *_audioTestController = nullptr;

	QCheckBox *_quickE2ee = nullptr;
	QCheckBox *_screenShareE2ee = nullptr;
	QCheckBox *_meetingDetailsE2ee = nullptr;
	QCheckBox *_allE2ee = nullptr;
	QLineEdit *_e2eeKey = nullptr;
	QPushButton *_e2eeSaveKey = nullptr;
	QPushButton *_e2eeClearKey = nullptr;

	void syncCopilotControls();
	void disconnectCopilotCoordinatorSignals();
	void maybeAutoRefreshCopilotModels();
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
	void focusCopilotSearchMatch(const QString &needle);
	OpenMeeting::AgentProfile collectCopilotProfileFromUi() const;
	OpenMeeting::AgentSpeechProfile collectCopilotSpeechProfileFromUi() const;
	OpenMeeting::AgentCoordinator &copilotCoordinator();

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
	QLabel *_e2eeStatus = nullptr;
	Cohavora::Update::UpdateService *_updateService = nullptr;
	QPushButton *_updateCheck = nullptr;
	QPushButton *_updateAction = nullptr;
	QPushButton *_updateCancel = nullptr;
	QLabel *_updateStatus = nullptr;
	QLabel *_updateNotes = nullptr;
	QProgressBar *_updateProgress = nullptr;

	QHash<QString, QVector<VideoFormat>> _cameraFormats;
	bool _usingExternalPreview = false;
	bool _updatingUi = false;
	bool _microphoneTestActive = false;
	bool _speakerTestActive = false;
	bool _dragging = false;
	QPoint _dragOffset;
};

} // namespace MeetingUI
