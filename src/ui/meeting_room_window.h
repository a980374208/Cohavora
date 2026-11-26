#pragma once

#include <QtCore/QCoreApplication>

#include "base/basic_types.h"
#include "ui/widgets/rp_window.h"
#include "src/rtc/video_frame.h"
#include "src/core/room.h"
#include "src/core/local_audio_track.h"
#include "src/core/local_video_track.h"
#include "src/media/camera_source_manager.h"
#include "src/media/camera_enumerator.h"
#include "src/media/audio_device_enumerator.h"
#include "src/media/microphone_capture.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/session_shutdown_service.h"
#include "src/render/video_render_session.h"
#include "src/ui/participants_sidebar_widget.h"
#include "src/ui/meeting_chat_sidebar_widget.h"
#include "src/agent/agent_coordinator.h"
#include "src/agent/meeting_translator_service.h"
#include "src/agent/room_agent_publisher.h"
#include "src/ui/copilot_sidebar_widget.h"
#include "src/ui/render/video_canvas.h"
#include "src/ui/camera_switch_completion_owner.h"
#include "src/ui/camera_device_discovery.h"
#include "src/ui/audio_device_discovery.h"
#include <array>
#include "src/ui/video_tile_widget.h"
#include "src/ui/room_top_bar_widget.h"
#include "src/ui/room_bottom_bar_widget.h"

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtCore/QPointer>
#include <QtCore/QAbstractNativeEventFilter>
#include "media/camera_source_manager.h"
#include <QtWidgets/QSlider>
#include "src/ui/audio_visualizer_widget.h"
#include <QtCore/QTimer>
#include <QtCore/QTime>
#include <QtGui/QImage>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <functional>
#include <optional>
#include <set>


class CameraOwnerInspectionAccess;
class ParticipantWindowInspectionAccess;

namespace OpenMeeting {
class CallInviteController;
struct CallSession;
class ContactCatalogController;
class PresenceWatchController;
}

namespace MeetingUI {
class MeetingEncryptionPanel;
class RemoteControlUi;
class MeetingLayoutPopup;
class MeetingVideoPager;
class SharedScreenWindow;
class FloatingMemberListWindow;

class WhiteboardPanel;
class AnnotationOverlayWindow;


enum class ActiveSidebar {
	None,          // 侧边栏折叠，视频舞台全宽
	Participants,  // 仅显示参会人列表
	Chat,          // 仅显示会议聊天记录
	Copilot        // AI Copilot 侧边栏
};

enum class InvitationMode {
	Disabled,
	BusinessMeetingId,
};


// ----------------------------------------------------
// MeetingRoomWindow: 现代化会议室主视窗
// ----------------------------------------------------
class MeetingRoomWindow : public Ui::RpWidget, private QAbstractNativeEventFilter {
	Q_OBJECT
public:
	struct Config {
		QString serverUrl;
		QString token;
		QString meetingId;
		QString displayName = QCoreApplication::translate("MeetingUI", "Cohavora User");
		bool audioMuted = false;
		bool videoEnabled = true;
		QString videoCodec = "vp8"; // "vp8", "h264", "vp9", "av1"
		QString backupCodec = "vp8";
		livekit::BackupCodecPolicy backupCodecPolicy = livekit::BackupCodecPolicy::PreferRegression;
		InvitationMode invitationMode = InvitationMode::Disabled;
		std::shared_ptr<OpenMeeting::AgentCoordinator> agentCoordinator;
		OpenMeeting::ContactCatalogController *contacts = nullptr;
		OpenMeeting::PresenceWatchController *presence = nullptr;
		OpenMeeting::CallInviteController *callInvite = nullptr;
		std::function<void(QWidget *parent)> openCopilotSettings;
		std::function<void(int pageIndex, int subTabIndex, QWidget *parent)> openSettingsPage;
	};

	// Constructs presentation only; no device startup waits on the Qt thread.
	explicit MeetingRoomWindow(const Config &config,
	                           std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator = nullptr,
	                           QWidget *parent = nullptr);
	// Call after show(). Admission is invoked once, only after device preparation.
	void prepareMediaAndJoin(std::function<void()> admission);
	void requestScreenShare();
	void requestDefaultScreenShare();
	~MeetingRoomWindow() override;

	void receiveRemoteVideoFrame(
		const QImage &frame,
		const QString &user,
		livekit::render::VideoRenderFrame::Ptr renderFrame = {});
	void receiveLocalVideoFrame(
		const QImage &frame,
		livekit::render::VideoRenderFrame::Ptr renderFrame = {});

	void onRemoteParticipantJoined(const QString &identity, const QString &name = QString());
	void onRemoteParticipantLeft(const QString &identity);
	void onRemoteTrackMuted(const QString &identity, bool isVideo, bool muted);
	void updateActiveSpeakers(const std::vector<livekit::ActiveSpeakerInfo> &speakers);

	void onKickedOff(const QString &reason, int reasonCode);
	void onMeetingKickOff(livekit::RoomDisconnectReason reason);
	void onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason);
	void onRemoteMuteRequested(bool isVideo, bool mute, const QString &operatorId);
	void onMeetingDetailUpdated(const OpenMeeting::MeetingDetail &detail);
	void onHostRoleChanged(const QString &newHostId, const QString &operatorName);
	void handleEndMeetingClicked();
	void beginLeaveTimingObservation();
	livekit::render::RenderDiagnostics renderDiagnostics() const { return _renderDiagnostics; }

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private slots:
	void onTimerTick();
	void onLocalVideoGenerated();
	void onRemoteRenderTick();

private:
	friend class ::CameraOwnerInspectionAccess;
	friend class ::ParticipantWindowInspectionAccess;
	struct ParticipantWindowInjectionTag final {};
	MeetingRoomWindow(
		ParticipantWindowInjectionTag,
		const Config &config,
		std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
		bool fullLayoutForChatPrivacy = false,
		QWidget *parent = nullptr);

	struct CameraOwnerInjectionTag final {};
	using CameraLogEffect = std::function<void(bool error, const QString &tag, const QString &message)>;
	using CameraWarningEffect = std::function<void(QWidget *parent, const QString &title, const QString &message)>;
	using InvitationNoticeEffect = std::function<void(bool success, const QString &title, const QString &message)>;

	MeetingRoomWindow(
		CameraOwnerInjectionTag,
		const Config &config,
		std::shared_ptr<livekit::CameraSourceManager> cameraManager,
		OpenMeeting::SessionManager &sessionManager,
		QWidget *parent = nullptr);

	void setupNativeWindow();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
#else
	bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override;
#endif
#if defined(Q_OS_WIN)
	int nativeResizeHitTest(LPARAM position) const;
#endif
	bool _nativeResizeFilterInstalled = false;
	void initLayout();
	void updateVideoLayout();
	void setupWhiteboardBinding();
	void setupRemoteControl();
	std::unique_ptr<RemoteControlUi> _remoteControlUi;
    void showVideoTileMenu(const QString& renderKey, const QPoint& globalAnchor = {});
    void detachSharedScreen(std::optional<livekit::TrackKey> key, bool local);
    void returnSharedScreen();
    void syncSharedScreenPlan();
    void syncFloatingListWindowVisibility();
    void dockFloatingMemberList();
    bool isFloatingStripModeActive() const;
    bool isFloatingListVisible() const;
    void updateFloatingListLayout(VideoTileWidget* pinnedTile);
    void removeUserFromCanvases(const std::string& key);

    bool isDetachedShareFrame(const QString& key) const;
    bool isSharedScreenVisible() const;
    std::unique_ptr<SharedScreenWindow> _sharedWindow;
    std::unique_ptr<FloatingMemberListWindow> _floatingListWindow;
    std::set<QString> _floatingStripRenderKeys;
    uint64_t _floatingSurfaceId = 0;
    bool _lastFloatingCollapsed = false;
    bool _lastFloatingMinimized = false;
    std::optional<livekit::DetachedShareViewport> _detachedShareIntent;
    uint64_t _nextSharedSurfaceId = 0;
    uint64_t _displayedSharedSurfaceId = 0;
    bool _sharedReturnPending = false;
    bool _receiveRemoteVideo = true;
	void setupVideoPagingControls();
	void setupLiveCaptionBanner();
	void onMeetingTranscriptionReceived(const OpenMeeting::MeetingTranscriptItem &item);
	void clearLiveCaption();
	void saveVideoLayoutPreferences();
	void updateStageGeometry();
	void setWhiteboardVisible(bool visible);
	void scheduleViewportIntent(bool immediate = false);
	void submitViewportIntent();
	void applyAcceptedVideoDemandPlan(livekit::VideoDemandPlan plan);
	void syncVisibleRemoteTiles();
	void reconcileRemoteRenderSelection();
	bool isVideoStageVisible() const;
    bool localScreenPreviewVisible() const;
    void updateLocalScreenPreviewDemand();
	bool isTrackVisible(const livekit::TrackKey &key) const;
	std::optional<livekit::TrackKey> trackKeyForRenderKey(const QString &renderKey) const;
	VideoTileWidget *ensureRemoteCameraTile(const QString &identity, const QString &name);
	void recycleRemoteCameraTile(std::unique_ptr<VideoTileWidget> tile);
	VideoTileWidget *ensureRemoteScreenTile(const livekit::VideoSeat &seat, const QString &name);
	VideoTileWidget *remoteVideoTile(const livekit::TrackKey &key) const;
	void openAnnotationOverlay();
	void closeAnnotationOverlay();
	void setAnnotationInteractionEnabled(bool enabled);
	void tryActivateGpuBackend();
	void receiveRenderedVideoFrame(
		const QImage&,
		const QString&,
		livekit::render::VideoRenderFrame::Ptr);
	void receiveGpuVideoFrame(const std::string&, livekit::render::VideoRenderFrame::Ptr);
	void fallBackToQtCpuBackend();
	void syncVideoCanvasLayout(const std::vector<VideoTileWidget*> &tiles);
	void setupVideoCanvasInteractions();
	void bindTileInteractions(VideoTileWidget *tile);
	void setPinnedTile(const QString &renderKey, bool pinned);
	VideoTileWidget *focusedVideoTile() const;
	void togglePinForRenderKey(const QString &renderKey);
	void setupCoordinatorBindings();
	void showDepartureNotice(const QString &title, const QString &message,
		QMessageBox::Icon icon = QMessageBox::Warning);
	void dismissDepartureNotice();
	void setupInvitationBinding();
	void handleInviteClicked();
	void copyMeetingIdInvitation();
	void openInMeetingContactInviteDialog();
	void showSilentMissedCallBanner(const OpenMeeting::CallSession &session);
	void showInvitationNotice(bool success, const QString &title, const QString &message);
	void restoreParticipantPresentations();
	void applyParticipantPresentation(const OpenMeeting::ParticipantPresentation &presentation);
	bool attachRemoteVideo(const OpenMeeting::ParticipantPresentation &presentation,
		const OpenMeeting::RemoteVideoTrackPresentation &track, bool reconcile = true, bool relayout = true);
	void removeRemoteVideo(const QString &trackSid, bool reconcile = true);
	VideoTileWidget *remoteVideoTile(const QString &trackSid) const;
	bool canRenderRemoteVideo(const QString &trackSid) const;
	livekit::TrackPublication::SubscriptionError remoteVideoSubscriptionError(
		const livekit::TrackKey &key) const;
	void refreshRemoteVideoPresentations();
	void applyScreenShareSnapshot(livekit::ScreenShareSnapshot snapshot);
    void updateRemoteControlStatus(const QString& status);
    void syncScreenShareControls();
	void handleScreenShareSources(const std::vector<livekit::DesktopSource> &sources);
    void openScreenShareAudioPicker(uint64_t run, uint64_t audioEpoch);
	void applyRemoteParticipantJoined(const QString &identity, const QString &name,
		const OpenMeeting::ParticipantPresentation *presentation, bool relayout = true);
	void setupCameraCompletionOwner(OpenMeeting::SessionManager &sessionManager);
	void applyParticipantNameDisplayPreferences(const OpenMeeting::MediaPreferences &preferences);
	void setupAudioPreferencesBinding(OpenMeeting::SessionManager &sessionManager);
	void applyAudioProcessingPreferences(const OpenMeeting::MediaPreferences &preferences);
#ifdef Q_OS_LINUX
	void requestSpeakerDevice(const QString &deviceId, bool fallbackToDefault = false,
		std::function<void(bool)> completed = {});
#else
	bool selectSpeakerDevice(const QString &deviceId);
#endif
	void requestMicrophoneSwitch(const QString &deviceId);
	void queueMicrophoneMute(bool muted, bool commitRequestedState = false);
	void applyMicrophoneAvailability(bool available);
	void bindMicrophoneCaptureState();
	void setSpeakerOutputMuted(bool muted);
	void applyEffectiveSpeakerMute();
	void bindCameraDeviceChanges();
	void requestCameraSwitch(const QString &devicePath);
	void handleCameraSwitchResult(
		const CameraSwitchCompletionOwner::Ticket &ticket,
		const QString &devicePath,
		bool success,
		const std::string &error);
	void invalidateCameraCompletion();
	struct DeviceSwitchTelemetry {
		std::weak_ptr<livekit::telemetry::SessionTelemetry> telemetry;
		std::string operationId;
		livekit::telemetry::OperationKind kind =
			livekit::telemetry::OperationKind::Unknown;
		std::uint64_t serial = 0;
	};
	std::uint64_t beginDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		livekit::telemetry::OperationKind kind);
	void finishDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		std::uint64_t serial,
		livekit::telemetry::OperationOutcome outcome);
	void cancelDeviceSwitchTelemetry();
	struct MediaPreparation;
	void beginMediaPreparation(bool microphone, bool camera);
	void requestLocalMicrophone(bool muted);
	void requestLocalCamera(bool enabled);
	void finishMediaPreparation();
	void cancelMediaPreparation();
	void updateMediaPreparationFeedback();
	void setMediaPreparationError(const QString &error);
	std::shared_ptr<MediaPreparation> _mediaPreparation;
	std::shared_ptr<OpenMeeting::QtCallbackGate<MeetingRoomWindow>> _preparationCallbacks;
	std::function<void()> _pendingAdmission;
	std::function<void(bool, bool)> _mediaPreparationOverride;
	bool _mediaPreparationStarted = false;
	bool _preparingMedia = false;
	// Pending intent survives availability signals; _config remains effective state.
	std::optional<bool> _pendingAudioMuted;
	std::optional<bool> _pendingVideoEnabled;
	QString _mediaPreparationError;
	QTimer *_mediaPreparationErrorTimer = nullptr;
	bool _audioPreferencesBound = false;
	void retireLocalCapture();
	void bindLocalMediaSources();
	void attachCoordinatorSession();
	void stopLiveKitSession(bool requestLeave = true);

	std::shared_ptr<OpenMeeting::MeetingCoordinator> _coordinator;

	Config _config;
	int _elapsedSeconds = 0;
	QTimer *_meetingTimer = nullptr;
	QTimer *_remoteRenderTimer = nullptr;
	std::unique_ptr<livekit::render::VideoRenderSession> _remoteRenderSession;
	std::atomic<bool> _usingGpuBackend{false};
	bool _gpuBackendActivationAttempted = false;
	livekit::render::RenderDiagnostics _renderDiagnostics;
	qint64 _lastRenderTelemetrySampleMs = 0;
	bool _closeRequested = false;
	bool _closingForSessionInvalidation = false;
	QPointer<QMessageBox> _departureNotice;
	QString _pendingDepartureTitle;
	QString _pendingDepartureMessage;
	VideoViewMode _viewMode = VideoViewMode::Grid;
	livekit::VideoDemandPlan _acceptedVideoPlan;
	std::optional<livekit::ViewportIntent> _lastViewportIntent;
	uint64_t _viewportRevision = 0;
	uint32_t _videoPage = 0;
	uint32_t _videoPageSize = 9;
	livekit::SpeakerLayout _speakerLayout = livekit::SpeakerLayout::Right;
	bool _speakingOnly = false;
	bool _persistVideoLayoutPreferences = false;
	bool _logicalWindowVisible = true;
	QTimer *_viewportIntentTimer = nullptr;
	MeetingVideoPager *_videoPagingControls = nullptr;
	MeetingLayoutPopup *_layoutPopup = nullptr;

	// 参会状态
	int _participantCount = 1;
	QString _pinnedRenderKey;
	std::optional<livekit::TrackKey> _pinnedTrackKey;
	std::optional<livekit::LocalVideoTarget> _pinnedLocalTarget;

	// UI 组件
	QPointer<QDialog> _telemetryDialog;
	RoomTopBarWidget *_topBar = nullptr;
	QWidget *_stageContainer = nullptr;
	livekit::render::VideoCanvas *_videoCanvas = nullptr;
	VideoTileWidget *_localTile = nullptr;
	std::map<QString, std::unique_ptr<VideoTileWidget>> _remoteTiles;
	std::vector<std::unique_ptr<VideoTileWidget>> _remoteTilePool;
	std::map<QString, QString> _remoteParticipantNames;
	struct RemoteVideoBinding {
		QString identity;
		bool screen = false;
		std::weak_ptr<livekit::Track> track;
		livekit::TrackKey key;
		livekit::TrackTicket ticket;
		livekit::MediaBindingKey mediaBindingKey;
		livekit::MediaBindingTicket mediaBindingTicket;
		bool muted = false;
		bool paused = false;
	};
	std::map<QString, RemoteVideoBinding> _remoteVideoBindings;
	std::map<QString, livekit::MediaBindingKey> _activeRemoteRenderLeases;
	uint64_t _lastSyncedVideoTracksRevision = ~uint64_t{0};
	std::map<QString, std::unique_ptr<VideoTileWidget>> _remoteScreenTiles;
	std::unique_ptr<VideoTileWidget> _localScreenTile;
	std::shared_ptr<livekit::render::VideoRenderRouter> _localScreenPreview;
	MeetingEncryptionPanel *_encryptionPanel = nullptr;
	QLabel *_screenShareBanner = nullptr;
    QString _screenShareText;
    QString _screenShareDetails;
    QString _screenShareAudioStatus;
    bool _screenShareAudioControlNeeded = false;
    bool _screenShareActive = false;
    bool _screenShareBannerNeeded = false;
    QString _remoteControlStatus;
    QPushButton *_remoteControlStopButton = nullptr;
	QPushButton *_annotationButton = nullptr;
	QPushButton *_screenQualityButton = nullptr;
    QPushButton *_screenAudioButton = nullptr;
    livekit::ScreenShareAudioError _lastScreenAudioError = livekit::ScreenShareAudioError::None;
	std::unique_ptr<AnnotationOverlayWindow> _annotationOverlay;
	std::optional<livekit::ScreenBinding> _annotationBinding;
	bool _annotationOffscreenForDiagnostics = false;
	bool _defaultScreenSharePending = false;
	QLabel *_inviteHintBanner = nullptr;
	QLabel *_recoveryBanner = nullptr;
	QTimer *_recoveryBannerFadeTimer = nullptr;
	QLabel *_missedCallBanner = nullptr;
	QTimer *_missedCallBannerTimer = nullptr;
	QLabel *_liveCaptionBanner = nullptr;
	QTimer *_liveCaptionFadeTimer = nullptr;
	QString _liveCaptionFullText;
	bool _wasReconnecting = false;
	RoomBottomBarWidget *_bottomBar = nullptr;
	WhiteboardPanel *_whiteboardPanel = nullptr;
	bool _whiteboardVisible = false;
	OpenMeeting::ParticipantsSidebarWidget *_participantsSidebar = nullptr;
	OpenMeeting::MeetingChatSidebarWidget *_chatSidebar = nullptr;
	std::shared_ptr<OpenMeeting::AgentCoordinator> _agentCoordinator;
	OpenMeeting::CopilotSidebarWidget *_copilotSidebar = nullptr;
	OpenMeeting::MeetingTranslatorService *_meetingTranslator = nullptr;
	OpenMeeting::RoomAgentPublisher *_roomAgentPublisher = nullptr;
	bool _hasRemoteRoomAgent = false;
	QString _remoteRoomAgentName;
	QString _remoteRoomAgentState;
	int _remoteRoomAgentCount = 0;
	bool _autoTranslationEvaluated = false;
	bool _e2eeAgentBroadcastAuthorized = false;
	QString _liveCaptionSecondaryText;
	ActiveSidebar _activeSidebar = ActiveSidebar::None;
	bool _copilotExpanded = false;

	void switchSidebar(ActiveSidebar target);
	void openCopilotSettingsDialog();
	void setupMeetingTranslationServices();
	void refreshTopBarAgentStatus();
	void showAgentControlMenu(const QPoint &globalPos);
	void maybeAutoStartMeetingTranslation();
	bool startLocalMeetingTranslation(bool interactive);
	void stopLocalMeetingTranslation();
	void enableRoomAgentRoleInteractive(bool forceTakeover = false);
	void disableRoomAgentRole();
	bool isCurrentUserMeetingHost() const;
	void updateRecoveryStateUi(OpenMeeting::MeetingState state, const QString &detail = QString());

	// 本地摄像头采集与模拟流
	std::shared_ptr<livekit::CameraSourceManager> _cameraManager;
	std::shared_ptr<livekit::ICameraCapturer> _dshowCap;
	std::unique_ptr<CameraSwitchCompletionOwner> _cameraCompletionOwner;
	OpenMeeting::SessionManager *_cameraSessionManager = nullptr;
	CameraLogEffect _cameraLogEffect;
	CameraWarningEffect _cameraWarningEffect;
	InvitationNoticeEffect _invitationNoticeEffect;
	QString _currentCameraPath;
	DeviceSwitchTelemetry _cameraSwitchTelemetry;
	DeviceSwitchTelemetry _microphoneSwitchTelemetry;
	DeviceSwitchTelemetry _speakerSwitchTelemetry;
	std::uint64_t _pendingMicrophoneDeviceGeneration = 0;
	std::weak_ptr<livekit::Room> _speakerConfiguredRoom;
	bool _speakerStateInitialized = false;
	bool _speakerRequestedMuted = true;
#ifdef Q_OS_LINUX
	std::shared_ptr<std::atomic<std::uint64_t>> _microphoneMuteIntent =
		std::make_shared<std::atomic<std::uint64_t>>(0);
	std::shared_ptr<std::atomic<std::uint64_t>> _microphoneSwitchIntent =
		std::make_shared<std::atomic<std::uint64_t>>(0);
	std::shared_ptr<std::atomic<std::uint64_t>> _speakerDeviceIntent =
		std::make_shared<std::atomic<std::uint64_t>>(0);
	std::shared_ptr<std::atomic<std::uint64_t>> _speakerMuteIntent =
		std::make_shared<std::atomic<std::uint64_t>>(0);
	bool _speakerDevicePending = false;
	bool _microphoneMutePending = false;
	bool _microphoneSwitchPending = false;
	bool _microphoneStateCommit = false;
#endif
	bool _usingRealCamera = false;
	QTimer *_localGenTimer = nullptr;
	int _localFrameStep = 0;

	// 本地麦克风采集
	std::shared_ptr<livekit::MicrophoneCapture> _wasapiCap;
	std::shared_ptr<OpenMeeting::QtCallbackGate<MeetingRoomWindow>> _captureUiCallbacks;
	bool _microphoneAvailable = false;
	bool _speakerAvailable = false;

	// LiveKit 异步通信核心
	std::unique_ptr<asio::io_context> _ioContext;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> _workGuard;
	std::shared_ptr<livekit::Room> _room;
	std::shared_ptr<livekit::RoomListener> _roomListener;
	std::thread _ioThread;
	std::atomic<bool> _sessionRunning{false};

	std::shared_ptr<livekit::LocalVideoTrack> _localVideoTrack;
	std::shared_ptr<livekit::LocalAudioTrack> _localAudioTrack;
	std::shared_ptr<livekit::VideoSource> _localVideoSource;
	std::shared_ptr<livekit::AudioSource> _localAudioSource;

	// 远端参会者独立音量与静音管理
	std::unordered_map<QString, float> _remoteVolumes;
	std::unordered_set<QString> _locallyMutedUsers;

#if defined(Q_OS_WIN)
	HWND _handle = nullptr;
#endif
};

} // namespace MeetingUI
