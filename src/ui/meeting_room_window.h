#pragma once

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
#include <mmsystem.h>

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
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

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace MeetingUI {

enum class VideoViewMode {
	Auto,       // 根据参会人数自动选择
	Grid,       // 宫格分屏并排
	Pip,        // 画中画悬浮窗
	Speaker     // 演讲者单人聚焦
};

// ----------------------------------------------------
// VideoTileWidget: 单个视频/头像渲染画框组件
// ----------------------------------------------------
class VideoTileWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent = nullptr);
	~VideoTileWidget() override = default;

	void setDisplayName(const QString &name);
	QString displayName() const { return _displayName; }

	bool isLocal() const { return _isLocal; }
	bool isVideoActive() const { return _isVideoActive; }
	void setVideoActive(bool active);

	bool isAudioMuted() const { return _isAudioMuted; }
	void setAudioMuted(bool muted);

	void setSpeaking(bool speaking, float level = 0.0f);
	bool isSpeaking() const { return _isSpeaking; }
	float audioLevel() const { return _audioLevel; }

	void setFrame(const QImage &image);

	// 远端独立音量与静音管理
	float remoteVolume() const { return _remoteVolume; }
	bool isLocallyMuted() const { return _isLocallyMuted; }

	// Pin 钉住与身份
	void setPinned(bool pinned) { _isPinned = pinned; update(); }
	bool isPinned() const { return _isPinned; }

	QString identity() const { return _identity; }
	void setIdentity(const QString &id) { _identity = id; }

	// PIP 小窗交互
	void setPipMode(bool pip) { _isPip = pip; update(); }
	bool isPipMode() const { return _isPip; }

signals:
	void tileDoubleClicked();
	void tileClicked();
	void pinToggled(bool pinned);
	void remoteVolumeChanged(float volume);
	void remoteLocalMuteToggled(bool muted);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void drawAvatarPlaceholder(QPainter &p, const QRect &r);
	void drawVideoFrame(QPainter &p, const QRect &r);
	void drawBottomNameTag(QPainter &p, const QRect &r);
	void drawNetworkQualityBadge(QPainter &p, const QRect &r);
	void setupVolumeControls();

	QString _identity;
	QString _displayName;
	bool _isLocal = false;
	bool _isVideoActive = false;
	bool _isAudioMuted = false;
	bool _isSpeaking = false;
	bool _isPinned = false;
	float _audioLevel = 0.0f;
	bool _isPip = false;

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
	std::mutex _frameMutex;
	bool _hasLoggedFirstPaint = false;
};

// ----------------------------------------------------
// RoomTopBarWidget: 顶部状态栏与工具按钮
// ----------------------------------------------------
class RoomTopBarWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit RoomTopBarWidget(QWidget *parent = nullptr);
	~RoomTopBarWidget() override = default;

	void updateDuration(int seconds);
	void setActiveSpeaker(const QString &speakerName);

	rpl::producer<VideoViewMode> viewModeChanged() const { return _viewModeStream.events(); }
	rpl::producer<> consoleClicked() const { return _consoleStream.events(); }
	rpl::producer<> minimizeClicked() const { return _minStream.events(); }
	rpl::producer<> maximizeClicked() const { return _maxStream.events(); }
	rpl::producer<> closeClicked() const { return _closeStream.events(); }
	rpl::producer<> settingsClicked() const { return _settingsStream.events(); }
	rpl::producer<livekit::SimulateScenarioType> simulateScenarioRequested() const { return _simulateScenarioStream.events(); }

	void showSimulateScenarioMenu(const QPoint &globalPos);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class HoverBtn {
		None, Layout, HostTools, Console, Simulate, Settings, Fullscreen, Min, Max, Close
	};

	HoverBtn _hoverBtn = HoverBtn::None;
	int _durationSeconds = 0;
	QString _speakerName;
	VideoViewMode _currentViewMode = VideoViewMode::Grid;

	QRect _layoutRect;
	QRect _hostToolsRect;
	QRect _consoleRect;
	QRect _simulateRect;
	QRect _settingsRect;
	QRect _fullscreenRect;
	QRect _minRect;
	QRect _maxRect;
	QRect _closeRect;
	QRect _speakerCapsuleRect;

	rpl::event_stream<VideoViewMode> _viewModeStream;
	rpl::event_stream<> _consoleStream;
	rpl::event_stream<> _minStream;
	rpl::event_stream<> _maxStream;
	rpl::event_stream<> _closeStream;
	rpl::event_stream<> _settingsStream;
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

	void setAudioMuted(bool muted);
	bool isAudioMuted() const { return _audioMuted; }

	void setSpeakerMuted(bool muted);
	bool isSpeakerMuted() const { return _speakerMuted; }

	void setVideoEnabled(bool enabled);
	bool isVideoEnabled() const { return _videoEnabled; }

	void setParticipantCount(int count);

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
	rpl::producer<> recordClicked() const { return _recordStream.events(); }
	rpl::producer<> appsClicked() const { return _appsStream.events(); }
	rpl::producer<> endMeetingClicked() const { return _endMeetingStream.events(); }
	rpl::producer<QString> sendChatRequested() const { return _sendChatStream.events(); }
	rpl::producer<QString> microphoneDeviceChanged() const { return _micDeviceStream.events(); }
	rpl::producer<int> speakerDeviceChanged() const { return _speakerDeviceStream.events(); }
	rpl::producer<livekit::SimulateScenarioType> simulateScenarioRequested() const { return _simulateScenarioStream.events(); }

	void showAudioDeviceMenu(const QPoint &globalPos);
	void showSpeakerDeviceMenu(const QPoint &globalPos);
	void showSimulateScenarioMenu(const QPoint &globalPos);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
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
	int _participantCount = 1;
	bool _isRecording = false;

	QLineEdit *_chatInput = nullptr;
	QPushButton *_handBtn = nullptr;

	std::vector<ToolItem> _toolItems;
	QRect _endMeetingRect;
	bool _endHovered = false;

	QString _currentMicId;
	int _currentSpeakerIndex = 0;

	rpl::event_stream<bool> _toggleAudioStream;
	rpl::event_stream<bool> _toggleSpeakerStream;
	rpl::event_stream<bool> _toggleVideoStream;
	rpl::event_stream<> _shareScreenStream;
	rpl::event_stream<> _inviteStream;
	rpl::event_stream<> _participantsStream;
	rpl::event_stream<> _chatStream;
	rpl::event_stream<> _recordStream;
	rpl::event_stream<> _appsStream;
	rpl::event_stream<> _endMeetingStream;
	rpl::event_stream<QString> _sendChatStream;
	rpl::event_stream<QString> _micDeviceStream;
	rpl::event_stream<int> _speakerDeviceStream;
	rpl::event_stream<livekit::SimulateScenarioType> _simulateScenarioStream;
};

// ----------------------------------------------------
// MeetingRoomWindow: 现代化会议室主视窗
// ----------------------------------------------------
class MeetingRoomWindow : public Ui::RpWidget {
	Q_OBJECT
public:
	struct Config {
		QString serverUrl;
		QString token;
		QString displayName = QString::fromUtf8("LiveKit用户");
		bool audioMuted = false;
		bool videoEnabled = true;
		QString videoCodec = "vp8"; // "vp8", "h264", "vp9", "av1"
		QString backupCodec = "vp8";
		livekit::BackupCodecPolicy backupCodecPolicy = livekit::BackupCodecPolicy::PreferRegression;
	};

	explicit MeetingRoomWindow(const Config &config, QWidget *parent = nullptr);
	~MeetingRoomWindow() override;

	void receiveRemoteVideoFrame(const QImage &frame, const QString &user);
	void receiveLocalVideoFrame(const QImage &frame);

	void onRemoteParticipantJoined(const QString &identity);
	void onRemoteParticipantLeft(const QString &identity);
	void onRemoteTrackMuted(bool isVideo, bool muted);
	void updateActiveSpeakers(const std::vector<std::shared_ptr<livekit::Participant>> &speakers);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private slots:
	void onTimerTick();
	void onLocalVideoGenerated();

private:
	void setupNativeWindow();
	void initLayout();
	void updateVideoLayout();
	void startLiveKitSession();
	void stopLiveKitSession();

	Config _config;
	int _elapsedSeconds = 0;
	QTimer *_meetingTimer = nullptr;
	VideoViewMode _viewMode = VideoViewMode::Grid;

	// 参会状态
	int _participantCount = 1;
	QString _pinnedIdentity;

	// UI 组件
	RoomTopBarWidget *_topBar = nullptr;
	QWidget *_stageContainer = nullptr;
	VideoTileWidget *_localTile = nullptr;
	std::map<QString, std::unique_ptr<VideoTileWidget>> _remoteTiles;
	QLabel *_inviteHintBanner = nullptr;
	RoomBottomBarWidget *_bottomBar = nullptr;

	// 本地摄像头采集与模拟流
	std::shared_ptr<livekit::DShowVideoCapture> _dshowCap;
	bool _usingRealCamera = false;
	QTimer *_localGenTimer = nullptr;
	int _localFrameStep = 0;

	// 本地麦克风采集
	std::shared_ptr<livekit::WasapiAudioCapture> _wasapiCap;

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
