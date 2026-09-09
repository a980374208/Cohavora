#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QByteArray>
#include <QtGui/QImage>

#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

#include <asio.hpp>
#include "src/core/room.h"
#include "src/core/local_audio_track.h"
#include "src/core/local_video_track.h"
#include "src/rtc/video_frame.h"
#include "src/media/wasapi_capture.h"
#include "src/media/dshow_capture.h"
#include "src/net/http_types.h"
#include "src/net/session_manager.h"
#include "openmeeting_meeting.pb.h"

namespace OpenMeeting {

enum class MeetingState {
    Idle,               // 闲置/已就绪
    Validating,         // 第一阶段：向业务后端鉴权（校验会议号、密码、准入状态）
    FetchingCredentials,// 第二阶段：换取 LiveKit 凭据 (URL 与 Token)
    ConnectingRoom,     // 第三阶段：异步连接 LiveKit 房间与 WebRTC 协商
    InMeeting,          // 成功入会，信令通道与音视频就绪
    Reconnecting,       // 网络波动重连中
    Leaving,            // 正在退出或结束会议
    Failed              // 流程异常或入会失败
};

// 会议详情数据包（从 Room Metadata 反序列化）
struct MeetingDetail {
    QString meetingId;
    QString meetingName;
    QString creatorUserId;
    QString hostUserId;
    int64_t startTime = 0;
    int64_t endTime = 0;
    bool disableMicrophoneOnJoin = false;
    bool disableCameraOnJoin = false;
    bool lockMeeting = false;
    bool canJoinEarly = true;
};

// 参会人实时属性实体
struct ParticipantInfo {
    QString identity;
    QString name;
    bool isLocal = false;
    bool isHost = false;
    bool isAudioMuted = false;
    bool isVideoEnabled = false;
    bool isSpeaking = false;
    float audioLevel = 0.0f;
};

class MeetingCoordinator : public QObject {
    Q_OBJECT
public:
    static std::shared_ptr<MeetingCoordinator> create(QObject *parent = nullptr);
    explicit MeetingCoordinator(QObject *parent = nullptr);
    ~MeetingCoordinator() override;

    // 当前状态查询
    MeetingState state() const { return _state; }
    QString stateString() const;
    QString currentMeetingId() const { return _currentMeetingId; }
    QString currentDisplayName() const { return _currentDisplayName; }
    const MeetingDetail &meetingDetail() const { return _meetingDetail; }
    bool isHost() const;

    // 核心入会流程
    void joinMeetingAsync(const QString &meetingId,
                          const QString &password,
                          const QString &displayName,
                          const MediaPreferences &prefs);

    void createAndJoinQuickMeetingAsync(const QString &title,
                                       int durationSeconds,
                                       const MediaPreferences &prefs);

    // 直连/离线入会模式（用于测试或指定 LiveKit 网关凭证）
    void connectDirectlyAsync(const QString &url,
                             const QString &token,
                             const QString &meetingId,
                             const QString &displayName,
                             const MediaPreferences &prefs);

    // 优雅退会管理 (普通离开 vs 主持人结束全员会议)
    void leaveMeetingAsync(bool endMeetingForAll = false);

    // 本地媒体控制
    void setLocalAudioMuted(bool muted);
    void setLocalVideoEnabled(bool enabled);
    bool isLocalAudioMuted() const { return _audioMuted; }
    bool isLocalVideoEnabled() const { return _videoEnabled; }

    // 房间内信令通道发送 (DataChannel)
    void sendNotifyData(const openmeeting::meeting::NotifyMeetingData &data, bool reliable = true);
    void sendChatMessage(const QString &content);
    void requestParticipantMute(const QString &targetUserId, bool isVideo, bool mute);
    void requestParticipantCamera(const QString &targetUserId, bool enable);
    void requestParticipantMicrophone(const QString &targetUserId, bool enable);
    void muteAllParticipants(bool muteMic, bool allowSelfUnmute = true);
    void transferHost(const QString &newHostUserId);
    void kickParticipant(const QString &targetUserId, const QString &reason);

    // 参会人列表读取
    std::vector<ParticipantInfo> participants() const;

    // 底层 LiveKit 房间与媒体源访问
    std::shared_ptr<livekit::Room> room() const { return _room; }
    std::shared_ptr<livekit::AudioSource> localAudioSource() const { return _localAudioSource; }
    std::shared_ptr<livekit::VideoSource> localVideoSource() const { return _localVideoSource; }

signals:
    // 状态流转与全局通知
    void stateChanged(MeetingState newState, const QString &detail);
    void errorOccurred(const QString &title, const QString &message);
    void meetingJoinedSuccessfully(const QString &meetingId);
    void meetingLeft();

    // 房间元数据与设置
    void meetingDetailUpdated(const MeetingDetail &detail);

    // 参会人与媒体轨道事件
    void participantJoined(const QString &identity, const QString &name);
    void participantLeft(const QString &identity);
    void participantsUpdated(const std::vector<ParticipantInfo> &participants);
    void remoteVideoFrameReceived(const QString &identity, const QImage &frame);
    void remoteTrackMuted(const QString &identity, bool isVideo, bool muted);
    void activeSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers);

    // 本地媒体状态变动（供 UI 底栏与视频画框联动）
    void localAudioMuteChanged(bool muted);
    void localVideoEnableChanged(bool enabled);

    // 业务信令事件 (从 DataChannel NotifyMeetingData 解包)
    void kickedOff(const QString &reason, int reasonCode);
    void remoteMuteRequested(bool isVideo, bool mute, const QString &operatorId);
    void hostRoleChanged(const QString &newHostUserId, const QString &operatorName);
    void chatMessageReceived(const QString &senderIdentity, const QString &message);

private:
    void setState(MeetingState s, const QString &detail = QString());
    void startRoomSession(const QString &url, const QString &token);
    void stopRoomSession();
    void parseRoomMetadata(const std::string &metadata);
    void handleDataReceived(const std::vector<uint8_t> &data, const std::string &participantSid);

    class CoordinatorRoomListener;
    friend class CoordinatorRoomListener;

    MeetingState _state = MeetingState::Idle;
    QString _currentMeetingId;
    QString _currentPassword;
    QString _currentDisplayName;
    MediaPreferences _mediaPrefs;
    MeetingDetail _meetingDetail;

    bool _audioMuted = false;
    bool _videoEnabled = true;

    std::map<QString, ParticipantInfo> _participants;
    void ensureLocalParticipant();
    void updateParticipantListAndNotify();

    // LiveKit 异步通信与媒体资源
    std::unique_ptr<asio::io_context> _ioContext;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> _workGuard;
    std::shared_ptr<livekit::Room> _room;
    std::shared_ptr<CoordinatorRoomListener> _roomListener;
    std::thread _ioThread;
    std::atomic<bool> _sessionRunning{false};

    // 本地媒体源与轨道
    std::shared_ptr<livekit::WasapiAudioCapture> _wasapiCap;
    std::shared_ptr<livekit::DShowVideoCapture> _dshowCap;
    std::shared_ptr<livekit::LocalAudioTrack> _localAudioTrack;
    std::shared_ptr<livekit::LocalVideoTrack> _localVideoTrack;
    std::shared_ptr<livekit::AudioSource> _localAudioSource;
    std::shared_ptr<livekit::VideoSource> _localVideoSource;
};

} // namespace OpenMeeting
