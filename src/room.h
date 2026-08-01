#pragma once

#include <string>
#include <memory>
#include <map>
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

    virtual void OnTrackPublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackUnpublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackSubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackUnsubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackMuted(std::shared_ptr<Participant> participant, std::shared_ptr<TrackPublication> publication, bool muted) {}

    virtual void OnLocalTrackRepublished(const std::string& previous_sid, std::shared_ptr<TrackPublication> publication) {}

    virtual void OnDataReceived(const std::vector<uint8_t>& payload, std::shared_ptr<RemoteParticipant> participant, const std::string& topic) {}
    virtual void OnChatMessage(const ChatMessage& message, std::shared_ptr<Participant> participant) {}

    virtual void OnDataChannelBufferedAmountLowThresholdChanged(uint64_t amount, bool reliable) {}

    virtual void OnRoomStats(const RoomStatsReport& report) {}
};

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
        std::lock_guard<std::mutex> lock(room_mutex_);
        local_participant_ = local;
    }

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

    void AddTrackToPublisher(std::shared_ptr<Track> track);

    // 内部 WebRTC 观察者回调接口
    void OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
    void OnRenegotiationNeeded(int pc_type);
    void OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable);
    void OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic);

private:
    void HandleSignalEvent(const SignalEvent& event);
    void HandleSignalMessage(std::shared_ptr<proto::SignalResponse> msg);
    void UpdateParticipants(const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants);
    void UpdateParticipants(const proto::ParticipantUpdate& update);
    void UpdateTrackMute(const proto::MuteTrackRequest& mute);

    // 协商和 Trickle 信令分发
    void SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void HandleOfferSignal(const proto::SessionDescription& offer);
    void HandleAnswerSignal(const proto::SessionDescription& answer);
    void HandleTrickleSignal(const proto::TrickleRequest& trickle);

private:
    asio::any_io_executor executor_;
    std::shared_ptr<SignalClient> signal_client_;
    ConnectionState connection_state_ = ConnectionState::Disconnected;
    std::shared_ptr<LocalParticipant> local_participant_;
    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_participants_;
    std::vector<std::shared_ptr<RoomListener>> listeners_;

    // WebRTC PeerConnection 资源
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher_pc_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber_pc_;
    std::unique_ptr<webrtc::PeerConnectionObserver> publisher_observer_;
    std::unique_ptr<webrtc::PeerConnectionObserver> subscriber_observer_;

    // DataChannel 句柄与背压控制水线
    webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable_dc_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> lossy_dc_;
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
    mutable std::mutex room_mutex_;

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

    // 内部私有方法
    asio::awaitable<void> AttemptReconnect();
    asio::awaitable<void> RepublishLocalTracks(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    asio::awaitable<void> RestartIceConnections(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    void RecordPublishedTracks();
    std::vector<std::shared_ptr<RoomListener>> GetListenersSnapshot() const;
};

} // namespace livekit
