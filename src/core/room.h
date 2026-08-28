#pragma once

#include <string>
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <asio.hpp>
#include "signal_client.h"
#include "participant.h"
#include "crash_handler.h"
#include "safe_spawn.h"
#include "chat_message.h"
#include "rpc_types.h"
#include "frame_cryptor.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/peer_connection_interface.h"
#include "api/data_channel_interface.h"

namespace livekit {

struct RoomStatsReport;

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting
};

class RoomListener {
public:
    virtual ~RoomListener() = default;

    virtual void OnConnected() {}
    virtual void OnDisconnected(const std::string& reason) {}
    virtual void OnReconnecting() {}
    virtual void OnReconnected() {}

    virtual void OnParticipantConnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnParticipantDisconnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnParticipantAttributesChanged(const std::map<std::string, std::string>& changed_attributes, std::shared_ptr<Participant> participant) {}
    virtual void OnParticipantPermissionsChanged(const ParticipantPermission& old_permission, const ParticipantPermission& new_permission, std::shared_ptr<Participant> participant) {}

    virtual void OnTrackPublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackUnpublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackSubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackUnsubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackMuted(std::shared_ptr<Participant> participant, std::shared_ptr<TrackPublication> publication, bool muted) {}

    virtual void OnLocalTrackRepublished(const std::string& previous_sid, std::shared_ptr<TrackPublication> publication) {}

    virtual void OnDataReceived(const std::vector<uint8_t>& payload, std::shared_ptr<RemoteParticipant> participant, const std::string& topic) {}
    virtual void OnChatMessage(const ChatMessage& message, std::shared_ptr<Participant> participant) {}
    virtual void OnDataChannelBufferedAmountLowThresholdChanged(uint64_t amount, bool reliable) {}

    virtual void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<Participant>>& speakers) {}

    virtual void OnE2eeStateChanged(const std::string& participant_identity, const std::string& track_sid, EncryptionState state) {}

    virtual void OnRoomStats(const RoomStatsReport& report) {}
};

class RoomDataChannelObserver;

class Room : public std::enable_shared_from_this<Room> {
public:
    static std::shared_ptr<Room> Create(asio::any_io_executor executor) {
        return std::make_shared<Room>(executor);
    }

    Room(asio::any_io_executor executor);
    ~Room();

    asio::awaitable<bool> Connect(const std::string& url, const std::string& token, const SignalOptions& opts);
    void Disconnect();

    ConnectionState connection_state() const;
    std::shared_ptr<LocalParticipant> local_participant() const;
    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_participants() const;
    void SetLocalParticipantForTesting(std::shared_ptr<LocalParticipant> local) {
        std::lock_guard lock(room_mutex_);
        local_participant_ = local;
    }
    void UpdateParticipantsForTesting(const proto::ParticipantUpdate& update);
    void HandleActiveSpeakerUpdateForTesting(const proto::SpeakersChanged& update);

    void AddListener(std::shared_ptr<RoomListener> listener);
    void RemoveListener(std::shared_ptr<RoomListener> listener);

    asio::any_io_executor executor() const { return executor_; }

    // === 高级通信与 DataChannel 背压流控 ===
    void PublishData(const std::vector<uint8_t>& payload, bool reliable = true,
                     const std::vector<std::string>& destination_identities = {}, const std::string& topic = "");
    void SetDataChannelBufferedAmountLowThreshold(uint64_t threshold, bool reliable = true);
    uint64_t GetDataChannelBufferedAmount(bool reliable = true) const;

    // === 新增：RPC 消息解包与发包管理 ===
    asio::awaitable<std::string> SendRpcRequest(const RpcPacket& packet);
    void OnIncomingRpcPacket(const RpcPacket& packet);

    // === 新增：RTCStats 实时质量与统计报表采集 ===
    asio::awaitable<RoomStatsReport> GetStats();
    RoomStatsReport GetStatsSync();

    // === 新增：E2EE 端到端加密管理器 ===
    void EnableE2ee(const E2eeOptions& options);
    std::shared_ptr<E2eeManager> e2ee_manager() const { return e2ee_manager_; }

    void AddTrackToPublisher(std::shared_ptr<Track> track);
    void SendPublishOffer();
    void NegotiatePublisher();
    void ExecuteNegotiatePublisher();
    void OnNegotiationFailed();

    // 内部 WebRTC 观察者回调接口
    void OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void OnIceConnected();
    void OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
    void OnRenegotiationNeeded(int pc_type);
    void OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable);
    void OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic);
    void OnRemoteDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel);

    using LogHandler = std::function<void(const std::string& cat, const std::string& tag, const std::string& msg)>;
    void SetLogHandler(LogHandler handler) {
        std::lock_guard lock(room_mutex_);
        log_handler_ = std::move(handler);
    }
    void Log(const std::string& cat, const std::string& tag, const std::string& msg);

private:
    LogHandler log_handler_;
    void HandleSignalEvent(const SignalEvent& event);
    void HandleSignalMessage(std::shared_ptr<proto::SignalResponse> msg);
    void UpdateParticipants(const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants);
    void UpdateParticipants(const proto::ParticipantUpdate& update);
    void UpdateTrackMute(const proto::MuteTrackRequest& mute);
    void HandleActiveSpeakerUpdate(const proto::SpeakersChanged& speakers_changed);

    // 协商和 Trickle 信令分发
    void SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void HandleOfferSignal(const proto::SessionDescription& offer);
    void HandleAnswerSignal(const proto::SessionDescription& answer);
    void HandleTrickleSignal(const proto::TrickleRequest& trickle);
    void HandleMediaSectionsRequirement(const proto::MediaSectionsRequirement& req);
    void NegotiateSubscriber(uint32_t num_audios, uint32_t num_videos);

    asio::any_io_executor executor_;
    std::shared_ptr<SignalClient> signal_client_;
    ConnectionState connection_state_ = ConnectionState::Disconnected;
    std::shared_ptr<LocalParticipant> local_participant_;
    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_participants_;
    std::vector<std::shared_ptr<RoomListener>> listeners_;
    std::shared_ptr<E2eeManager> e2ee_manager_;

    // WebRTC PeerConnection 资源
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher_pc_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber_pc_;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer_;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer_;
    enum class NegotiationState {
        Idle,
        InProgress,
        PendingRetry
    };
    NegotiationState negotiation_state_ = NegotiationState::Idle;

    // Subscriber PC 协商状态（客户端发起 Subscriber Offer 模式）
    bool subscriber_negotiating_ = false;
    uint32_t current_sub_audios_ = 0;
    uint32_t current_sub_videos_ = 0;
    uint32_t pending_sub_audios_ = 0;
    uint32_t pending_sub_videos_ = 0;

    // Early ICE Candidate 暂存队列结构
    struct PendingIceCandidate {
        std::string sdp_mid;
        int sdp_mline_index = 0;
        std::string sdp;
    };
    std::vector<PendingIceCandidate> pending_sub_ice_candidates_;
    std::vector<PendingIceCandidate> pending_pub_ice_candidates_;

    // DataChannel 句柄与背压控制水线
    webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable_dc_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> lossy_dc_;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> remote_data_channels_;
    std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers_;
    std::vector<std::shared_ptr<void>> remote_track_sinks_;
    uint64_t reliable_buffered_low_threshold_ = 16384;
    uint64_t lossy_buffered_low_threshold_ = 16384;

    // === RPC Pending 跟踪数据结构 ===
    struct PendingRpcCall {
        std::shared_ptr<asio::steady_timer> timer;
        std::function<void(const RpcPacket&)> completion_cb;
        bool finished = false;
    };
    mutable std::mutex pending_rpc_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingRpcCall>> pending_rpc_calls_;

    // 线程安全锁
    mutable std::recursive_mutex room_mutex_;

    // 媒体分发专用轻量锁（与信令/状态机大锁 room_mutex_ 完全解耦，防止 60FPS 回调产生 AB-BA 死锁）
    mutable std::mutex remote_media_mutex_;
    std::vector<std::weak_ptr<Track>> remote_video_tracks_;
    std::vector<std::weak_ptr<Track>> remote_audio_tracks_;

    // 重连控制
    int reconnect_attempts_ = 0;
    static constexpr int kMaxReconnectAttempts = 5;
    static constexpr std::chrono::milliseconds kBaseReconnectDelay{100};
    static constexpr std::chrono::milliseconds kMaxReconnectDelay{1000};
    bool reconnect_active_ = false;

    // Track 恢复记录
    struct PublishedTrackRecord {
        std::shared_ptr<Track> track;
        std::string previous_sid;
    };
    std::vector<PublishedTrackRecord> published_track_records_;

    // 下行 Track 防重挂载
    std::set<std::string> processed_remote_track_ids_;

    // === 对齐 Flutter: PendingTrackQueue 暂存队列与 Stream ID 解包 ===
    struct PendingTrack {
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
        webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
        std::string participant_sid;
        std::string track_sid;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::map<std::string, std::vector<PendingTrack>> pending_track_queue_;

    static std::pair<std::string, std::string> UnpackStreamId(const std::string& packed);
    void AttachRemoteTrackToParticipant(
        std::shared_ptr<RemoteParticipant> participant,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
        webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
        const std::string& track_sid);
    void FlushPendingTracks(const std::string& participant_sid);
    void RemoveExpiredPendingTracks();

    // Simulcast 参数下发同步
    static void ApplySimulcastParameters(webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender, const VideoPublishOptions& opts);

    // 内部私有方法
    asio::awaitable<void> AttemptReconnect();
    asio::awaitable<void> RepublishLocalTracks(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    asio::awaitable<void> RestartIceConnections(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    void RecordPublishedTracks();
    std::vector<std::shared_ptr<RoomListener>> GetListenersSnapshot() const;

private:
    // === DataStream 大包切片重组数据结构 ===
    struct IncomingDataStreamTracker {
        std::string stream_id;
        std::string topic;
        uint64_t total_length = 0;
        std::string sender_identity;
        std::string sender_sid;
        std::chrono::steady_clock::time_point start_time;
        std::map<uint64_t, std::vector<uint8_t>> chunks;
        uint64_t current_received_bytes = 0;
    };
    mutable std::mutex incoming_streams_mutex_;
    std::unordered_map<std::string, std::shared_ptr<IncomingDataStreamTracker>> incoming_streams_;
    void CleanupStaleDataStreams();
};

} // namespace livekit
