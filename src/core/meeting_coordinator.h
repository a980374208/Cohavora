#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QTimer>

#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>

#include <asio.hpp>
#include "src/core/room.h"
#include "src/core/data_stream.h"
#include "src/core/local_audio_track.h"
#include "src/core/local_video_track.h"
#include "src/core/meeting_session_runtime.h"
#include "src/core/meeting_startup_transaction.h"
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
    StartingLocalMedia, // LiveKit 房间已连接，正在原子发布本地音频和视频
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

// Qt/business projection of the authoritative native RoomInfo snapshot.
// Keep protobuf types out of the UI layer.
struct MeetingRoomInfo {
    QString sid;
    QString name;
    QString metadata;
    uint32_t emptyTimeout = 0;
    uint32_t departureTimeout = 0;
    uint32_t maxParticipants = 0;
    int64_t creationTimeMs = 0;
    uint32_t numParticipants = 0;
    uint32_t numPublishers = 0;
    bool activeRecording = false;
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
    bool isAudioStreamPaused = false;
    bool isVideoStreamPaused = false;
    livekit::ConnectionQuality connectionQuality = livekit::ConnectionQuality::Unknown;
    float connectionQualityScore = 0.0f;
    livekit::ParticipantPermission permissions;
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
    const MeetingRoomInfo &roomInfo() const { return _roomInfo; }
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

    // 消息时序单调序列发生器
    int64_t nextSequenceNumber();

    // 房间内信令通道发送 (DataChannel)
    void sendNotifyData(const openmeeting::meeting::NotifyMeetingData &data, bool reliable = true);
    void sendChatMessage(const QString &content, const QString &messageId = QString(), int64_t seq = 0);
    void sendChatMediaMessage(const QString &messageId, const QString &mediaType, const QString &fileName, const QByteArray &data, int64_t seq = 0);
    void sendChatMediaMessage(const QString &mediaType, const QString &fileName, const QByteArray &data) {
        sendChatMediaMessage(QString(), mediaType, fileName, data, 0);
    }
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

    // 现代数据流发送工厂方法 (DataStream Writers)
    std::shared_ptr<livekit::TextStreamWriter> createTextStreamWriter(
        const QString &topic = QString(),
        const std::map<std::string, std::string> &attributes = {},
        const QString &streamId = QString(),
        std::optional<size_t> totalSize = std::nullopt,
        const QString &replyToId = QString(),
        const std::vector<std::string> &destinationIdentities = {});

    std::shared_ptr<livekit::ByteStreamWriter> createByteStreamWriter(
        const QString &name,
        const QString &topic = QString(),
        const std::map<std::string, std::string> &attributes = {},
        const QString &streamId = QString(),
        std::optional<size_t> totalSize = std::nullopt,
        const QString &mimeType = "application/octet-stream",
        const std::vector<std::string> &destinationIdentities = {});

signals:
    // 状态流转与全局通知
    void stateChanged(MeetingState newState, const QString &detail);
    void errorOccurred(const QString &title, const QString &message);
    void meetingJoinedSuccessfully(const QString &meetingId);
    void meetingLeft();
    // LiveKit 服务端主动断开（例如同一 identity 重复进入同一房间）。
    // UI 仅消费客户端枚举，不依赖 protobuf 协议细节。
    void meetingKickOff(livekit::RoomDisconnectReason reason);

    // 房间元数据与设置
    void meetingDetailUpdated(const MeetingDetail &detail);
    void roomInfoUpdated(const MeetingRoomInfo &info);

    // 参会人与媒体轨道事件
    void participantJoined(const QString &identity, const QString &name);
    void participantLeft(const QString &identity);
    void participantsUpdated(const std::vector<ParticipantInfo> &participants);
    // 控制面事件：渲染 Session 自行持有 Track subscription，Coordinator
    // 不保存或转换逐帧视频数据。
    void remoteVideoTrackAvailable(const QString &identity, std::shared_ptr<livekit::Track> track);
    void remoteVideoTrackUnavailable(const QString &identity, const QString &trackSid);
    void remoteTrackMuted(const QString &identity, bool isVideo, bool muted);
    void participantConnectionQualityChanged(const QString &identity, int quality, float score);
    void participantTrackStreamStateChanged(const QString &identity, const QString &trackSid,
                                            bool isVideo, bool paused);
    void participantPermissionsChanged(const QString &identity,
                                       const livekit::ParticipantPermission &permission);
    void trackSubscriptionPermissionChanged(const QString &participantIdentity,
                                            const QString &participantSid,
                                            const QString &trackSid,
                                            bool allowed);
    void activeSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers);

    // 本地媒体状态变动（供 UI 底栏与视频画框联动）
    void localAudioMuteChanged(bool muted);
    void localVideoEnableChanged(bool enabled);

    // 业务信令事件 (从 DataChannel NotifyMeetingData 解包)
    void kickedOff(const QString &reason, int reasonCode);
    void remoteMuteRequested(bool isVideo, bool mute, const QString &operatorId);
    void hostRoleChanged(const QString &newHostUserId, const QString &operatorName);
    void chatMessageReceived(const QString &senderIdentity, const QString &senderName, const QString &message, int64_t seq = 0);
    void chatMediaMessageReceived(const QString &senderIdentity, const QString &senderName,
                                  const QString &mediaType, const QString &fileName, const QByteArray &data);
    void chatMessageSendProgress(const QString &messageId, int progress);
    void chatMessageSendSuccess(const QString &messageId);
    void chatMessageSendFailed(const QString &messageId, const QString &error);

    // 接收端多媒体实时分片传输事件
    void chatMediaReceivingStarted(const QString &transferId, const QString &senderIdentity, const QString &senderName,
                                   const QString &mediaType, const QString &fileName, qint64 totalSize, int64_t seq = 0);
    void chatMediaReceivingProgress(const QString &transferId, int progress);
    void chatMediaReceivingCompleted(const QString &transferId, const QString &senderIdentity, const QString &senderName,
                                     const QString &mediaType, const QString &fileName, const QByteArray &data);
    void chatMediaReceivingFailed(const QString &transferId, const QString &reason);

    // 现代数据流 (DataStream) 接收信号
    void textStreamReceived(std::shared_ptr<livekit::TextStreamReader> reader, const QString &senderIdentity);
    void byteStreamReceived(std::shared_ptr<livekit::ByteStreamReader> reader, const QString &senderIdentity);

private:
    void setState(MeetingState s, const QString &detail = QString());
    void startRoomSession(const QString &url, const QString &token);
    void stopRoomSession();
    void completeRoomStartupOnUiThread(uint64_t sessionGeneration,
                                       std::shared_ptr<livekit::LocalAudioTrack> audioTrack,
                                       std::shared_ptr<livekit::LocalVideoTrack> videoTrack);
    void failRoomStartupOnUiThread(uint64_t sessionGeneration,
                                   const QString &title,
                                   const QString &detail);
    void parseRoomMetadata(const std::string &metadata);
    void handleDuplicateIdentityKickOff(const QString &detail);
    void handleSessionInvalidated(SessionInvalidationReason reason);
    void enqueueDataReceived(const std::shared_ptr<MeetingSessionRuntime> &session,
                             const std::vector<uint8_t> &data,
                             const std::string &participantSid,
                             const std::string &participantIdentity = "",
                             const QString &participantName = "");
    void handleDataReceivedOnSessionStrand(const std::shared_ptr<MeetingSessionRuntime> &session,
                                           const std::vector<uint8_t> &data,
                                           const std::string &participantSid,
                                           const std::string &participantIdentity,
                                           const QString &participantName);
    void cancelInboundTransfersForParticipant(const QString &participantIdentity);
    // Queued Qt callbacks use this immutable token instead of retaining a
    // MeetingSessionRuntime. The runtime owns an ASIO strand, so allowing it
    // to outlive its io_context through a delayed Qt event is unsafe.
    bool isCurrentSessionGenerationOnUiThread(uint64_t sessionGeneration) const;

    class CoordinatorRoomListener;
    friend class CoordinatorRoomListener;

    MeetingState _state = MeetingState::Idle;
    QString _currentMeetingId;
    QString _currentPassword;
    QString _currentDisplayName;
    MediaPreferences _mediaPrefs;
    MeetingDetail _meetingDetail;
    MeetingRoomInfo _roomInfo;

    bool _audioMuted = false;
    bool _videoEnabled = true;
    // 全局账号会话被撤销后，忽略仍在途的 HTTP 入会回调，防止已经关闭的
    // 会议窗口重新创建 Room 或重新发布媒体。
    bool _sessionInvalidated = false;

    std::map<QString, ParticipantInfo> _participants;
    void ensureLocalParticipant();
    void updateParticipantListAndNotify();

    // 出站大文件/多媒体平滑分片调度队列
    struct MediaSendChunkTask {
        QString messageId;
        QString mediaType;
        QString fileName;
        QString transferId;
        qint64 totalSize = 0;
        int64_t seq = 0;
        int currentChunk = 0;
        int totalChunks = 0;
        QString base64Payload;
        int chunkSize = 24 * 1024;
    };
    std::deque<MediaSendChunkTask> _mediaSendQueue;
    QTimer *_mediaSendTimer = nullptr;
    void processNextMediaSendChunk();

    std::atomic<int64_t> _msgSequenceCounter{0};

    // LiveKit 异步通信与媒体资源
    std::unique_ptr<asio::io_context> _ioContext;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> _workGuard;
    // The Qt thread owns this pointer. Callback-owned transfer state inside the
    // runtime is accessed only through its ASIO strand.
    std::shared_ptr<MeetingSessionRuntime> _sessionRuntime;
    uint64_t _nextSessionGeneration = 0;
    std::shared_ptr<livekit::Room> _room;
    std::shared_ptr<CoordinatorRoomListener> _roomListener;
    std::thread _ioThread;
    std::atomic<bool> _sessionRunning{false};
    // Qt-thread owned. A reconnect event may arrive while initial local media
    // publication is still in progress; only a committed startup may surface
    // as InMeeting.
    bool _startupCommitted = false;

    // 本地媒体源与轨道
    std::shared_ptr<livekit::WasapiAudioCapture> _wasapiCap;
    std::shared_ptr<livekit::DShowVideoCapture> _dshowCap;
    std::shared_ptr<livekit::LocalAudioTrack> _localAudioTrack;
    std::shared_ptr<livekit::LocalVideoTrack> _localVideoTrack;
    std::shared_ptr<livekit::AudioSource> _localAudioSource;
    std::shared_ptr<livekit::VideoSource> _localVideoSource;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(livekit::RoomDisconnectReason)
Q_DECLARE_METATYPE(OpenMeeting::MeetingRoomInfo)
Q_DECLARE_METATYPE(livekit::ParticipantPermission)
