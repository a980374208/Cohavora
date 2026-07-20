#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <chrono>
#include <asio.hpp>
#include "signal_client.h"
#include "participant.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/peer_connection_interface.h"

namespace livekit {

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
    virtual void OnParticipantConnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnParticipantDisconnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackMuted(std::shared_ptr<Participant> participant, std::shared_ptr<TrackPublication> publication, bool muted) {}
    virtual void OnReconnecting() {}
    virtual void OnReconnected() {}
    virtual void OnLocalTrackRepublished(const std::string& previous_sid, std::shared_ptr<TrackPublication> publication) {}
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

    void AddListener(std::shared_ptr<RoomListener> listener);
    void RemoveListener(std::shared_ptr<RoomListener> listener);

    asio::any_io_executor executor() const { return executor_; }

    // 内部 WebRTC 观察者回调接口
    void OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
    void OnRenegotiationNeeded(int pc_type);

private:
    void HandleSignalEvent(const SignalEvent& event);
    void HandleSignalMessage(std::shared_ptr<proto::SignalResponse> msg);
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

    // === 新增：线程安全锁 ===
    mutable std::mutex room_mutex_;

    // === 新增：重连控制 ===
    int reconnect_attempts_ = 0;
    static constexpr int kMaxReconnectAttempts = 5;
    static constexpr std::chrono::milliseconds kBaseReconnectDelay{100};
    static constexpr std::chrono::milliseconds kMaxReconnectDelay{1000};
    bool reconnect_active_ = false;

    // === 新增：Track 恢复记录 ===
    struct PublishedTrackRecord {
        std::shared_ptr<Track> track;
        std::string previous_sid;
    };
    std::vector<PublishedTrackRecord> published_track_records_;

    // === 新增私有方法 ===
    asio::awaitable<void> AttemptReconnect();
    asio::awaitable<void> RepublishLocalTracks(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    asio::awaitable<void> RestartIceConnections(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response);
    void RecordPublishedTracks();
    std::vector<std::shared_ptr<RoomListener>> GetListenersSnapshot() const;
};

} // namespace livekit
