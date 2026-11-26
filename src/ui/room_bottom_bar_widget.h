#pragma once

#include "ui/widgets/rp_window.h"
#include "src/core/room.h"
#include "src/core/screen_share_session.h"
#include "src/core/session_shutdown_service.h"
#include "src/ui/audio_device_discovery.h"
#include "src/ui/camera_device_discovery.h"

#include <QtCore/QPointer>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>

#include <array>
#include <memory>
#include <vector>

class CameraOwnerInspectionAccess;
class ParticipantWindowInspectionAccess;

namespace MeetingUI {

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
	void setScreenShareAvailable(bool available);
	void setWhiteboardHasContent(bool hasContent);

	void setParticipantCount(int count);
	void setChatUnreadCount(int count);
	int chatUnreadCount() const { return _chatUnreadCount; }
	void setActiveSidebar(int sidebarId);

	// 事件流
	rpl::producer<bool> toggleAudioRequested() const { return _toggleAudioStream.events(); }
	rpl::producer<bool> toggleSpeakerRequested() const { return _toggleSpeakerStream.events(); }
	rpl::producer<bool> toggleVideoRequested() const { return _toggleVideoStream.events(); }
	rpl::producer<> shareScreenClicked() const { return _shareScreenStream.events(); }
	rpl::producer<> inviteClicked() const { return _inviteStream.events(); }
	rpl::producer<> participantsClicked() const { return _participantsStream.events(); }
	rpl::producer<> chatClicked() const { return _chatStream.events(); }
	rpl::producer<> whiteboardClicked() const { return _whiteboardStream.events(); }
	rpl::producer<> copilotClicked() const { return _copilotStream.events(); }
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
	void hideEvent(QHideEvent *e) override;

private:
	friend class ::CameraOwnerInspectionAccess;
	friend class ::ParticipantWindowInspectionAccess;
	enum class DeviceKind { Microphone, Speaker, Camera };
	struct PendingDeviceAction {
		std::shared_ptr<void> subscription;
		std::shared_ptr<OpenMeeting::QtCallbackGate<RoomBottomBarWidget>> callbacks;
	};
	std::array<PendingDeviceAction, 3> _pendingDeviceActions;
	CameraDeviceDiscovery *_cameraDiscovery = nullptr;
	AudioDeviceDiscovery *_microphoneDiscovery = nullptr;
	AudioDeviceDiscovery *_speakerDiscovery = nullptr;
	QPointer<QMenu> _deviceMenu;
	void cancelDeviceAction(DeviceKind kind);
	void requestDeviceEnable(DeviceKind kind);
	QMenu *createDeviceMenu(const char *name);
	void showAudioEndpointMenu(const QPoint &globalPos, bool speaker);
	void toggleAudio();
	void toggleVideo();
	void toggleSpeaker();
	bool canStopScreenShare() const {
		using State = livekit::ScreenShareState;
		return _screenShareState == State::AwaitingPermission ||
			_screenShareState == State::Starting || _screenShareState == State::Active ||
			_screenShareState == State::StopFailed;
	}
	bool screenShareControlEnabled() const {
		return (!_inRecovery && _screenShareAvailable) || canStopScreenShare();
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
	bool _screenShareAvailable = true;
	int _participantCount = 1;
	int _chatUnreadCount = 0;
	bool _whiteboardHasContent = false;
	int _activeSidebarId = 0;
	bool _compactDockLabels = false;
	std::array<QRect, 3> _dockGroupRects{};

	QLineEdit *_chatInput = nullptr;
	QPushButton *_handBtn = nullptr;
	QPushButton *_uiaParticipants = nullptr;
	QPushButton *_uiaChat = nullptr;
	QPushButton *_uiaWhiteboard = nullptr;
	QPushButton *_uiaCopilot = nullptr;
	QPushButton *_uiaAudio = nullptr;
	QPushButton *_uiaVideo = nullptr;
	QPushButton *_uiaShare = nullptr;
	QPushButton *_uiaEnd = nullptr;

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
	rpl::event_stream<> _copilotStream;
	rpl::event_stream<> _endMeetingStream;
	rpl::event_stream<QString> _sendChatStream;
	rpl::event_stream<QString> _micDeviceStream;
	rpl::event_stream<QString> _speakerDeviceStream;
	rpl::event_stream<QString> _videoDeviceStream;
	rpl::event_stream<livekit::SimulateScenarioType> _simulateScenarioStream;
	QString _currentCameraPath;
	bool _inRecovery = false;
};

} // namespace MeetingUI
