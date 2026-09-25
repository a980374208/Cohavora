#pragma once

#include <QtCore/QCoreApplication>

#include "base/basic_types.h"
#include "ui/widgets/rp_window.h"
#include "src/rtc/video_frame.h"
#include "src/core/room.h"
#include "src/core/local_audio_track.h"
#include "src/core/local_video_track.h"
#include "src/media/dshow_capture.h"
#include "src/media/dshow_enumerator.h"
#include "src/media/wasapi_enumerator.h"
#include "src/media/wasapi_capture.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/session_shutdown_service.h"
#include "src/render/video_render_session.h"
#include "src/ui/participants_sidebar_widget.h"
#include "src/ui/meeting_chat_sidebar_widget.h"
#include "src/ui/render/video_canvas.h"
#include "src/ui/camera_switch_completion_owner.h"
#include <mmsystem.h>

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
#include <QtGui/QPainter>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <functional>
#include <optional>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

class CameraOwnerTestAccess;
class ParticipantWindowTestAccess;

namespace MeetingUI {

class WhiteboardPanel;
class AnnotationOverlayWindow;

enum class VideoViewMode {
	Auto,       // 根据参会人数自动选择
	Grid,       // 宫格分屏并排
	Pip,        // 画中画悬浮窗
	Speaker     // 演讲者单人聚焦
};

enum class ActiveSidebar {
	None,          // 侧边栏折叠，视频舞台全宽
	Participants,  // 仅显示参会人列表
	Chat           // 仅显示会议聊天记录
};

enum class InvitationMode {
	Disabled,
	BusinessMeetingId,
};

// ----------------------------------------------------
// VideoTileWidget: 单个视频/头像渲染画框组件
// ----------------------------------------------------
class VideoTileWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent = nullptr,
		bool isScreenShare = false);
	~VideoTileWidget() override;

	void setDisplayName(const QString &name);
	QString displayName() const { return _displayName; }

	bool isLocal() const { return _isLocal; }
	bool isVideoActive() const { return _isVideoActive; }
	void setVideoActive(bool active);

	bool isAudioMuted() const { return _isAudioMuted; }
	void setAudioMuted(bool muted);
	void setConnectionQuality(livekit::ConnectionQuality quality);
	void setVideoStreamPaused(bool paused);
	bool isVideoStreamPaused() const { return _isVideoStreamPaused; }
	void setVideoSubscriptionError(livekit::TrackPublication::SubscriptionError error);
	livekit::TrackPublication::SubscriptionError videoSubscriptionError() const {
		return _videoSubscriptionError;
	}

	void setSpeaking(bool speaking, float level = 0.0f);
	bool isSpeaking() const { return _isSpeaking; }
	float audioLevel() const { return _audioLevel; }

	void setFrame(
		const QImage &image,
		livekit::render::VideoRenderFrame::Ptr renderFrame = {});

	// 远端独立音量与静音管理
	float remoteVolume() const { return _remoteVolume; }
	bool isLocallyMuted() const { return _isLocallyMuted; }

	// Pin 钉住与身份
	void setPinned(bool pinned);
	bool isPinned() const { return _isPinned; }

	QString identity() const { return _identity; }
	void setIdentity(const QString &id) { _identity = id; }
	QString renderKey() const { return _renderKey.isEmpty() ? _identity : _renderKey; }
	void setRenderKey(const QString &key) { _renderKey = key; }

	// PIP 小窗交互
	void setPipMode(bool pip);
	bool isPipMode() const { return _isPip; }

	// DX11 硬件加速模式支持
	void setHardwareCanvasMode(bool enabled);
	bool isHardwareCanvasMode() const { return _useHardwareCanvas; }
	// UI-thread-only decoration cache. Video pixels never pass through this image.
	QImage hardwareDecoration(const QSize &pixels, bool hasFrame, bool hovered);
	QRect pinButtonRect() const;

signals:
	void tileDoubleClicked();
	void tileClicked();
	void pinToggled(bool pinned);
	void remoteVolumeChanged(float volume);
	void remoteLocalMuteToggled(bool muted);
	void presentationChanged();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	friend class ::ParticipantWindowTestAccess;
	void drawAvatarPlaceholder(QPainter &p, const QRect &r);
	void drawVideoFrame(QPainter &p, const QRect &r);
	void drawBottomNameTag(QPainter &p, const QRect &r);
	void drawNetworkQualityBadge(QPainter &p, const QRect &r);
	void drawVideoPlaceholder(QPainter &p, const QRect &r);
	void paintCard(QPainter &p, bool decorationOnly, bool hasFrame, bool hovered);
	void invalidatePresentation();
	void setupVolumeControls();
	void updateRenderExpectation();

	QString _identity;
	QString _renderKey;
	bool _isScreenShare = false;
	QString _displayName;
	bool _isLocal = false;
	bool _isVideoActive = false;
	bool _isAudioMuted = false;
	bool _isVideoStreamPaused = false;
	livekit::TrackPublication::SubscriptionError _videoSubscriptionError =
		livekit::TrackPublication::SubscriptionError::None;
	livekit::ConnectionQuality _connectionQuality = livekit::ConnectionQuality::Unknown;
	bool _isSpeaking = false;
	bool _isPinned = false;
	float _audioLevel = 0.0f;
	bool _isPip = false;
	bool _useHardwareCanvas = true;

	// 远端独立音量控制
	float _remoteVolume = 1.0f;
	bool _isLocallyMuted = false;
	QPushButton *_volBtn = nullptr;
	QPushButton *_pinBtn = nullptr;
	QWidget *_volPopup = nullptr;
	QSlider *_volSlider = nullptr;
	QLabel *_volLabel = nullptr;
	QPushButton *_muteRemoteBtn = nullptr;

	// 频域多柱跳动波形组件
	AudioVisualizerWidget *_visualizer = nullptr;

	QImage _currentFrame;
	livekit::render::VideoRenderFrame::Ptr _currentRenderFrame;
	std::mutex _frameMutex;
	bool _hasLoggedFirstPaint = false;
	QImage _hardwareDecoration;
	QSize _decorationLogicalSize;
	bool _decorationHasFrame = false;
	bool _decorationHovered = false;
};

// ----------------------------------------------------
// RoomTopBarWidget: 顶部状态栏与工具按钮
// ----------------------------------------------------
class RoomTopBarWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit RoomTopBarWidget(QWidget *parent = nullptr);
	~RoomTopBarWidget() override = default;
	int heightForWidth(int width) const override;

	void updateDuration(int seconds);
	void setActiveSpeaker(const QString &speakerName);
	void setMeetingId(const QString &meetingId);
	void setTelemetrySnapshot(const QVariantMap &snapshot);

	rpl::producer<VideoViewMode> viewModeChanged() const { return _viewModeStream.events(); }
	rpl::producer<> consoleClicked() const { return _consoleStream.events(); }
	rpl::producer<> minimizeClicked() const { return _minStream.events(); }
	rpl::producer<> maximizeClicked() const { return _maxStream.events(); }
	rpl::producer<> closeClicked() const { return _closeStream.events(); }
	rpl::producer<livekit::SimulateScenarioType> simulateScenarioRequested() const { return _simulateScenarioStream.events(); }

	void showSimulateScenarioMenu(const QPoint &globalPos);
	void showTelemetryMenu(const QPoint &globalPos);

signals:
	// Native child HWNDs (such as Dx11VideoCanvas) may prevent the top-level
	// WM_NCHITTEST path from reaching this QWidget. Blank title-bar presses
	// therefore request a system drag explicitly as a reliable fallback.
	void windowDragRequested();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	friend class ::ParticipantWindowTestAccess;
	enum class HoverBtn {
		None, Quality, Layout, Console, Simulate, Min, Max, Close
	};

	HoverBtn _hoverBtn = HoverBtn::None;
	int _durationSeconds = 0;
	QString _speakerName;
	QString _meetingId;
	QVariantMap _telemetrySnapshot;
	bool _copiedAnim = false;
	VideoViewMode _currentViewMode = VideoViewMode::Grid;

	QRect _layoutRect;
	QRect _qualityRect;
	QRect _consoleRect;
	QRect _simulateRect;
	QRect _minRect;
	QRect _maxRect;
	QRect _closeRect;
	QRect _speakerCapsuleRect;
	QRect _meetingIdRect;

	rpl::event_stream<VideoViewMode> _viewModeStream;
	rpl::event_stream<> _consoleStream;
	rpl::event_stream<> _minStream;
	rpl::event_stream<> _maxStream;
	rpl::event_stream<> _closeStream;
	rpl::event_stream<livekit::SimulateScenarioType> _simulateScenarioStream;
};

// ----------------------------------------------------
// RoomBottomBarWidget: 底部会议控制栏
// ----------------------------------------------------
class RoomBottomBarWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit RoomBottomBarWidget(QWidget *parent = nullptr);
	~RoomBottomBarWidget() override = default;
	int heightForWidth(int width) const override;

	void setAudioMuted(bool muted);
	bool isAudioMuted() const { return _audioMuted; }

	void setSpeakerMuted(bool muted);
	bool isSpeakerMuted() const { return _speakerMuted; }
	void setSpeakerDeviceId(const QString &deviceId) { _currentSpeakerId = deviceId; }
	void setMicrophoneDeviceId(const QString &deviceId) { _currentMicId = deviceId; }

	void setVideoEnabled(bool enabled);
	bool isVideoEnabled() const { return _videoEnabled; }
	void setScreenShareState(livekit::ScreenShareState state);

	void setParticipantCount(int count);
	void setChatUnreadCount(int count);
	int chatUnreadCount() const { return _chatUnreadCount; }

	static bool HasAvailableAudioDevice();
	static bool HasAvailableSpeakerDevice();
	static bool HasAvailableVideoDevice();

	// 事件流
	rpl::producer<bool> toggleAudioRequested() const { return _toggleAudioStream.events(); }
	rpl::producer<bool> toggleSpeakerRequested() const { return _toggleSpeakerStream.events(); }
	rpl::producer<bool> toggleVideoRequested() const { return _toggleVideoStream.events(); }
	rpl::producer<> shareScreenClicked() const { return _shareScreenStream.events(); }
	rpl::producer<> inviteClicked() const { return _inviteStream.events(); }
	rpl::producer<> participantsClicked() const { return _participantsStream.events(); }
	rpl::producer<> chatClicked() const { return _chatStream.events(); }
	rpl::producer<> whiteboardClicked() const { return _whiteboardStream.events(); }
	rpl::producer<> endMeetingClicked() const { return _endMeetingStream.events(); }
	rpl::producer<QString> sendChatRequested() const { return _sendChatStream.events(); }
	rpl::producer<QString> microphoneDeviceChanged() const { return _micDeviceStream.events(); }
	rpl::producer<QString> speakerDeviceChanged() const { return _speakerDeviceStream.events(); }
	rpl::producer<QString> videoDeviceChanged() const { return _videoDeviceStream.events(); }
	rpl::producer<livekit::SimulateScenarioType> simulateScenarioRequested() const { return _simulateScenarioStream.events(); }

	void showAudioDeviceMenu(const QPoint &globalPos);
	void showSpeakerDeviceMenu(const QPoint &globalPos);
	void showVideoDeviceMenu(const QPoint &globalPos);
	void showSimulateScenarioMenu(const QPoint &globalPos);

	void setInRecovery(bool inRecovery);
	bool inRecovery() const { return _inRecovery; }

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	friend class ::CameraOwnerTestAccess;
	friend class ::ParticipantWindowTestAccess;
	void appendSpeakerDeviceActions(QMenu &menu);
	bool canStopScreenShare() const {
		using State = livekit::ScreenShareState;
		return _screenShareState == State::Starting || _screenShareState == State::Active ||
			_screenShareState == State::StopFailed;
	}

	struct ToolItem {
		int id;
		QString title;
		QString activeTitle;
		QRect rect;
		bool hasDropdown = false;
	};

	int _hoveredId = -1;
	bool _audioMuted = false;
	bool _speakerMuted = false;
	bool _videoEnabled = true;
	livekit::ScreenShareState _screenShareState = livekit::ScreenShareState::Idle;
	int _participantCount = 1;
	int _chatUnreadCount = 0;

	QLineEdit *_chatInput = nullptr;
	QPushButton *_handBtn = nullptr;

	std::vector<ToolItem> _toolItems;
	QRect _endMeetingRect;
	bool _endHovered = false;

	QString _currentMicId;
	QString _currentSpeakerId;

	rpl::event_stream<bool> _toggleAudioStream;
	rpl::event_stream<bool> _toggleSpeakerStream;
	rpl::event_stream<bool> _toggleVideoStream;
	rpl::event_stream<> _shareScreenStream;
	rpl::event_stream<> _inviteStream;
	rpl::event_stream<> _participantsStream;
	rpl::event_stream<> _chatStream;
	rpl::event_stream<> _whiteboardStream;
	rpl::event_stream<> _endMeetingStream;
	rpl::event_stream<QString> _sendChatStream;
	rpl::event_stream<QString> _micDeviceStream;
	rpl::event_stream<QString> _speakerDeviceStream;
	rpl::event_stream<QString> _videoDeviceStream;
	rpl::event_stream<livekit::SimulateScenarioType> _simulateScenarioStream;
	QString _currentCameraPath;
	bool _inRecovery = false;
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
	};

	// Prepares capture and presentation only. The entry owner must explicitly
	// start admission through the coordinator after constructing the window.
	explicit MeetingRoomWindow(const Config &config,
	                           std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator = nullptr,
	                           QWidget *parent = nullptr);
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
	livekit::render::RenderDiagnostics renderDiagnostics() const { return _renderDiagnostics; }

protected:
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
	friend class ::CameraOwnerTestAccess;
	friend class ::ParticipantWindowTestAccess;
	struct ParticipantWindowTestTag final {};
	MeetingRoomWindow(
		ParticipantWindowTestTag,
		const Config &config,
		std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
		QWidget *parent = nullptr);

	struct CameraOwnerTestTag final {};
	using CameraLogEffect = std::function<void(bool error, const QString &tag, const QString &message)>;
	using CameraWarningEffect = std::function<void(QWidget *parent, const QString &title, const QString &message)>;
	using InvitationNoticeEffect = std::function<void(bool success, const QString &title, const QString &message)>;

	MeetingRoomWindow(
		CameraOwnerTestTag,
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
	void setupVideoPagingControls();
	void setWhiteboardVisible(bool visible);
	void scheduleViewportIntent(bool immediate = false);
	void submitViewportIntent();
	void applyAcceptedVideoDemandPlan(livekit::VideoDemandPlan plan);
	void syncVisibleRemoteTiles();
	void reconcileRemoteRenderSelection();
	bool isVideoStageVisible() const;
	bool isTrackVisible(const livekit::TrackKey &key) const;
	std::optional<livekit::TrackKey> trackKeyForRenderKey(const QString &renderKey) const;
	VideoTileWidget *ensureRemoteCameraTile(const QString &identity, const QString &name);
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
	void togglePinForRenderKey(const QString &renderKey);
	void setupCoordinatorBindings();
	void showDepartureNotice(const QString &title, const QString &message,
		QMessageBox::Icon icon = QMessageBox::Warning);
	void setupInvitationBinding();
	void handleInviteClicked();
	void showInvitationNotice(bool success, const QString &title, const QString &message);
	void restoreParticipantPresentations();
	void applyParticipantPresentation(const OpenMeeting::ParticipantPresentation &presentation);
	void attachRemoteVideo(const OpenMeeting::ParticipantPresentation &presentation,
		const OpenMeeting::RemoteVideoTrackPresentation &track);
	void removeRemoteVideo(const QString &trackSid);
	VideoTileWidget *remoteVideoTile(const QString &trackSid) const;
	bool canRenderRemoteVideo(const QString &trackSid) const;
	livekit::TrackPublication::SubscriptionError remoteVideoSubscriptionError(
		const livekit::TrackKey &key) const;
	void refreshRemoteVideoPresentations();
	void applyScreenShareSnapshot(livekit::ScreenShareSnapshot snapshot);
	void handleScreenShareSources(const std::vector<livekit::DesktopSource> &sources);
	void applyRemoteParticipantJoined(const QString &identity, const QString &name,
		const OpenMeeting::ParticipantPresentation *presentation);
	void setupCameraCompletionOwner(OpenMeeting::SessionManager &sessionManager);
	void setupAudioPreferencesBinding(OpenMeeting::SessionManager &sessionManager);
	void applyAudioProcessingPreferences(const OpenMeeting::MediaPreferences &preferences);
	bool selectSpeakerDevice(const QString &deviceId);
	void requestMicrophoneSwitch(const QString &deviceId);
	void applyMicrophoneAvailability(bool available);
	void bindMicrophoneCaptureState();
	void setSpeakerOutputMuted(bool muted);
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
	bool _logicalWindowVisible = true;
	QTimer *_viewportIntentTimer = nullptr;
	QWidget *_videoPagingControls = nullptr;
	QToolButton *_previousVideoPage = nullptr;
	QToolButton *_nextVideoPage = nullptr;
	QComboBox *_videoPageSizeControl = nullptr;
	QLabel *_videoPageLabel = nullptr;

	// 参会状态
	int _participantCount = 1;
	QString _pinnedRenderKey;
	std::optional<livekit::TrackKey> _pinnedTrackKey;

	// UI 组件
	RoomTopBarWidget *_topBar = nullptr;
	QWidget *_stageContainer = nullptr;
	livekit::render::VideoCanvas *_videoCanvas = nullptr;
	VideoTileWidget *_localTile = nullptr;
	std::map<QString, std::unique_ptr<VideoTileWidget>> _remoteTiles;
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
	std::map<QString, std::unique_ptr<VideoTileWidget>> _remoteScreenTiles;
	std::unique_ptr<VideoTileWidget> _localScreenTile;
	std::shared_ptr<livekit::render::VideoRenderRouter> _localScreenPreview;
	QLabel *_screenShareBanner = nullptr;
	QPushButton *_annotationButton = nullptr;
	std::unique_ptr<AnnotationOverlayWindow> _annotationOverlay;
	std::optional<livekit::ScreenBinding> _annotationBinding;
	bool _annotationOffscreenForTesting = false;
	bool _defaultScreenSharePending = false;
	QLabel *_inviteHintBanner = nullptr;
	QLabel *_recoveryBanner = nullptr;
	QTimer *_recoveryBannerFadeTimer = nullptr;
	bool _wasReconnecting = false;
	RoomBottomBarWidget *_bottomBar = nullptr;
	WhiteboardPanel *_whiteboardPanel = nullptr;
	bool _whiteboardVisible = false;
	OpenMeeting::ParticipantsSidebarWidget *_participantsSidebar = nullptr;
	OpenMeeting::MeetingChatSidebarWidget *_chatSidebar = nullptr;
	ActiveSidebar _activeSidebar = ActiveSidebar::None;

	void switchSidebar(ActiveSidebar target);
	void updateRecoveryStateUi(OpenMeeting::MeetingState state, const QString &detail = QString());

	// 本地摄像头采集与模拟流
	std::shared_ptr<livekit::CameraSourceManager> _cameraManager;
	std::shared_ptr<livekit::DShowVideoCapture> _dshowCap;
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
	bool _usingRealCamera = false;
	QTimer *_localGenTimer = nullptr;
	int _localFrameStep = 0;

	// 本地麦克风采集
	std::shared_ptr<livekit::WasapiAudioCapture> _wasapiCap;
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
