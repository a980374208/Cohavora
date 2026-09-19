#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <functional>
#include <optional>
#include <tuple>
#include <asio.hpp>
#include "signal_client.h"
#include "participant.h"
#include "participant_event.h"
#include "crash_handler.h"
#include "safe_spawn.h"
#include "chat_message.h"
#include "rpc_types.h"
#include "frame_cryptor.h"
#include "data_stream_assembler.h"
#include "data_stream.h"
#include "operation.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/peer_connection_interface.h"
#include "api/data_channel_interface.h"

namespace livekit {

struct RoomStatsReport;
class RemoteTrackPublication;
struct RemotePublicationControlRequest;
enum class RemotePublicationControlDispatch;

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting
};

// Snapshot of the server's room state. This is intentionally a native value
// type instead of a protobuf message so callers do not depend on signaling
// generated headers or retain mutable protocol state.
struct RoomInfo {
    std::string sid;
    std::string name;
    std::string metadata;
    uint32_t empty_timeout = 0;
    uint32_t departure_timeout = 0;
    uint32_t max_participants = 0;
    int64_t creation_time_ms = 0;
    uint32_t num_participants = 0;
    uint32_t num_publishers = 0;
    bool active_recording = false;

    bool operator==(const RoomInfo& other) const {
        return sid == other.sid &&
            name == other.name &&
            metadata == other.metadata &&
            empty_timeout == other.empty_timeout &&
            departure_timeout == other.departure_timeout &&
            max_participants == other.max_participants &&
            creation_time_ms == other.creation_time_ms &&
            num_participants == other.num_participants &&
            num_publishers == other.num_publishers &&
            active_recording == other.active_recording;
    }

    bool operator!=(const RoomInfo& other) const { return !(*this == other); }
};

struct TrackSubscriptionPermission {
    std::string participant_sid;
    std::string track_sid;
    bool allowed = true;
};

// Client-facing disconnect semantics. Keep this independent of the protobuf
// enum so application/UI layers do not need to depend on the signaling schema.
enum class RoomDisconnectReason {
    Unknown,
    UserLeave,
    NetworkError,
    ServerShutdown,
    DuplicateIdentity,
    ParticipantRemoved,
};

const char* ToString(RoomDisconnectReason reason);

enum class SimulateScenarioType {
    SignalReconnect,
    FullReconnect,
    SpeakerUpdate,
    NodeFailure,
    Migration,
    ServerLeave,
    SwitchCandidate,
    E2eeKeyRatchet,
    ParticipantName,
    ParticipantMetadata,
    Clear,
};

class RoomListener {
public:
    virtual ~RoomListener() = default;

    virtual void OnConnected() {}
    // New typed callback. The legacy overload remains so existing SDK callers
    // that only consume a textual detail stay source-compatible.
    virtual void OnDisconnected(RoomDisconnectReason reason, const std::string& detail) {
        (void)reason;
        OnDisconnected(detail);
    }
    virtual void OnDisconnected(const std::string& reason) {}
    virtual void OnReconnecting() {}
    virtual void OnReconnected() {}

    virtual void OnRoomMetadataChanged(const RoomInfo& room,
                                       const std::string& old_metadata,
                                       const std::string& new_metadata) {}
    virtual void OnRoomUpdated(const RoomInfo& room) {}

    virtual void OnParticipantConnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnParticipantDisconnected(std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnParticipantAttributesChanged(const std::map<std::string, std::string>& changed_attributes, std::shared_ptr<Participant> participant) {}
    virtual void OnParticipantPermissionsChanged(const ParticipantPermission& old_permission, const ParticipantPermission& new_permission, std::shared_ptr<Participant> participant) {}
    virtual void OnParticipantMetadataChanged(std::shared_ptr<Participant> participant, const std::string& old_metadata, const std::string& new_metadata) {}

    virtual void OnTrackPublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackUnpublished(std::shared_ptr<RemoteParticipant> participant, std::shared_ptr<TrackPublication> publication) {}
    virtual void OnLocalTrackUnpublished(std::shared_ptr<TrackPublication> publication) {}
    virtual void OnTrackSubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackUnsubscribed(std::shared_ptr<Track> track, std::shared_ptr<TrackPublication> publication, std::shared_ptr<RemoteParticipant> participant) {}
    virtual void OnTrackMuted(std::shared_ptr<Participant> participant, std::shared_ptr<TrackPublication> publication, bool muted) {}
    virtual void OnTrackStreamStateChanged(std::shared_ptr<Participant> participant,
                                           std::shared_ptr<TrackPublication> publication,
                                           TrackPublication::StreamState state) {}
    virtual void OnTrackSubscriptionPermissionChanged(
        const TrackSubscriptionPermission& permission,
        std::shared_ptr<Participant> participant,
        std::shared_ptr<TrackPublication> publication) {}

    virtual void OnConnectionQualityChanged(std::shared_ptr<Participant> participant,
                                            ConnectionQuality quality,
                                            float score) {}

    virtual void OnLocalTrackRepublished(const std::string& previous_sid, std::shared_ptr<TrackPublication> publication) {}

    virtual void OnDataReceived(const std::vector<uint8_t>& payload, std::shared_ptr<RemoteParticipant> participant, const std::string& topic) {}
    virtual void OnChatMessage(const ChatMessage& message, std::shared_ptr<Participant> participant) {}
    virtual void OnDataChannelBufferedAmountLowThresholdChanged(uint64_t amount, bool reliable) {}
    virtual void OnTextStreamOpened(std::shared_ptr<TextStreamReader> reader, std::shared_ptr<Participant> participant) {}
    virtual void OnByteStreamOpened(std::shared_ptr<ByteStreamReader> reader, std::shared_ptr<Participant> participant) {}

    virtual void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<Participant>>& speakers) {}
    virtual void OnParticipantEvent(const ParticipantEvent& event) {}
    virtual bool ConsumesParticipantEvents() const { return false; }

    virtual void OnE2eeStateChanged(const std::string& participant_identity, const std::string& track_sid, EncryptionState state) {}

    virtual void OnRoomStats(const RoomStatsReport& report) {}
};

class RoomDataChannelObserver;
class RoomStreamDeliveryTestAccess;

class Room : public std::enable_shared_from_this<Room> {
public:
    static std::shared_ptr<Room> Create(asio::any_io_executor executor) {
        return std::make_shared<Room>(executor);
    }

    Room(asio::any_io_executor executor);
    ~Room();

    asio::awaitable<bool> Connect(const std::string& url, const std::string& token, const SignalOptions& opts);
    asio::awaitable<void> ConnectAsync(const std::string& url, const std::string& token, const SignalOptions& opts);
    void Disconnect();
    asio::awaitable<void> DisconnectAsync();

    ConnectionState connection_state() const;
    std::shared_ptr<LocalParticipant> local_participant() const;
    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_participants() const;
    std::shared_ptr<const proto::JoinResponse> join_response() const;
    std::vector<std::string> enabled_publish_codecs() const;
    RoomInfo room_info() const;
    std::optional<TrackSubscriptionPermission> track_subscription_permission(
        const std::string& participant_sid,
        const std::string& track_sid) const;
    void SetLocalParticipantForTesting(std::shared_ptr<LocalParticipant> local) {
        std::lock_guard lock(room_mutex_);
        RetireParticipantLocked(local_participant_);
        local_participant_ = local;
        if (local_participant_) {
            EnsureMembershipLocked(local_participant_, true);
        }
    }
    void UpdateParticipantsForTesting(const proto::ParticipantUpdate& update);
    void HandleActiveSpeakerUpdateForTesting(const proto::SpeakersChanged& update);
    // Deterministic state-event injection for unit tests. Production events
    // still enter through the generation-checked SignalClient callback.
    void HandleSignalMessageForTesting(const proto::SignalResponse& message);

    void AddListener(std::shared_ptr<RoomListener> listener);
    void RemoveListener(std::shared_ptr<RoomListener> listener);

    asio::any_io_executor executor() const { return executor_; }

    // === 场景模拟 (Simulate Scenario - 对标 Flutter sendSimulateScenario) ===
    void SimulateScenario(SimulateScenarioType scenario);
    asio::awaitable<void> SimulateScenarioAsync(SimulateScenarioType scenario);

    // === 高级通信与 DataChannel 背压流控 ===
    bool PublishData(const std::vector<uint8_t>& payload, bool reliable = true,
                     const std::vector<std::string>& destination_identities = {}, const std::string& topic = "");
    bool PublishDataPacket(const proto::DataPacket& packet, bool reliable = true);
    void SetDataChannelBufferedAmountLowThreshold(uint64_t threshold, bool reliable = true);
    uint64_t GetDataChannelBufferedAmount(bool reliable = true) const;

    // === 现代流式数据发送与工厂 (Text / Byte Streams) ===
    std::shared_ptr<TextStreamWriter> CreateTextStreamWriter(
        const std::string& topic = "",
        const std::map<std::string, std::string>& attributes = {},
        const std::string& stream_id = "",
        std::optional<std::size_t> total_size = std::nullopt,
        const std::string& reply_to_id = "",
        const std::vector<std::string>& destination_identities = {});

    std::shared_ptr<ByteStreamWriter> CreateByteStreamWriter(
        const std::string& name,
        const std::string& topic = "",
        const std::map<std::string, std::string>& attributes = {},
        const std::string& stream_id = "",
        std::optional<std::size_t> total_size = std::nullopt,
        const std::string& mime_type = "application/octet-stream",
        const std::vector<std::string>& destination_identities = {});

    // === 新增：RPC 消息解包与发包管理 ===
    asio::awaitable<std::string> SendRpcRequest(const RpcPacket& packet);
    void OnIncomingRpcPacket(const RpcPacket& packet);

    // === 新增：RTCStats 实时质量与统计报表采集 ===
    asio::awaitable<RoomStatsReport> GetStats();
    RoomStatsReport GetStatsSync();

    // === 远端参会人独立音量与静音管理 ===
    void SetParticipantVolume(const std::string& identity_or_sid, double volume);
    void SetParticipantMuted(const std::string& identity_or_sid, bool muted);
    void SetAudioOutputMuted(bool muted);

    // === 新增：E2EE 端到端加密管理器 ===
    void EnableE2ee(const E2eeOptions& options);
    std::shared_ptr<E2eeManager> e2ee_manager() const { return e2ee_manager_; }

    void AddTrackToPublisher(std::shared_ptr<Track> track);
    asio::awaitable<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> AddTrackToPublisherAsync(
        std::shared_ptr<Track> track,
        uint64_t generation);
    asio::awaitable<std::shared_ptr<TrackPublication>> PublishLocalTrackAsync(
        std::shared_ptr<Track> track,
        const proto::SignalRequest& request);
    asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> PublishLocalTracksBatchAsync(
        std::vector<LocalParticipant::BatchTrackItem> items);
    asio::awaitable<std::shared_ptr<TrackPublication>> UnpublishLocalTrackAsync(
        std::string track_sid);
    void SendPublishOffer();
    void NegotiatePublisher();
    asio::awaitable<void> NegotiatePublisherAsync(
        std::chrono::milliseconds timeout,
        uint64_t generation,
        bool ice_restart = false);
    void ExecuteNegotiatePublisher(uint64_t generation = 0);
    void OnNegotiationFailed(uint64_t generation = 0);

    // 内部 WebRTC 观察者回调接口
    void OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type);
    void OnIceConnected();
    void OnPeerConnectionStateChanged(
        int pc_type,
        webrtc::PeerConnectionInterface::PeerConnectionState state);
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
    friend class RoomPeerConnectionObserver;
    friend class RoomDataChannelObserver;
    friend class RoomUnpublishTestAccess;
    friend class ParticipantSnapshotRoomTestAccess;
    friend class RoomIrSec001TestAccess;
    friend class RoomConnectAttemptTestAccess;
    friend class RoomStreamDeliveryTestAccess;
    // Only the named test-access friend can install these two transport-boundary
    // hooks. Production keeps them null and uses the existing native methods.
    struct LocalUnpublishTestHooks {
        std::function<asio::awaitable<void>(std::shared_ptr<Track>, uint64_t)> remove_sender;
        std::function<asio::awaitable<void>(std::chrono::milliseconds, uint64_t)> negotiate;
    };
    std::shared_ptr<LocalUnpublishTestHooks> local_unpublish_test_hooks_;
    struct ConnectAttemptTestHooks {
        std::function<asio::awaitable<void>(uint64_t)> before_join_commit;
        std::function<asio::awaitable<void>(bool, uint64_t)> before_subscription_send;
        std::function<asio::awaitable<void>(const proto::SyncState&)> before_subscription_sync_send;
        std::function<void(uint64_t)> before_signal_event_commit;
        std::function<void(uint64_t, ConnectionState)> before_lifecycle_listener_delivery;
        std::function<void(uint64_t)> before_native_event_commit;
        std::function<void(uint64_t)> before_republish_listener_delivery;
    };
    std::shared_ptr<ConnectAttemptTestHooks> connect_attempt_test_hooks_;
    struct StreamDeliveryTestHooks {
        std::function<void()> before_admission;
        std::function<void()> after_admission;
        std::function<void(uint64_t)> before_full_restart_data_channel_wait;
        std::function<asio::awaitable<void>(std::chrono::milliseconds, uint64_t)>
            negotiate_full_restart_publisher;
    };
    std::shared_ptr<StreamDeliveryTestHooks> stream_delivery_test_hooks_;
    void BindLocalUnpublishHandler();

    LogHandler log_handler_;
    enum class LifecycleListenerEventKind {
        Connected,
        Disconnected,
        Reconnecting,
        Reconnected
    };
    struct LifecycleListenerDelivery {
        LifecycleListenerEventKind kind = LifecycleListenerEventKind::Connected;
        uint64_t generation = 0;
        ConnectionState required_state = ConnectionState::Disconnected;
        RoomDisconnectReason reason = RoomDisconnectReason::Unknown;
        std::string detail;
        std::vector<std::shared_ptr<RoomListener>> listeners;
    };
    void DeliverLifecycleListenerEvent(LifecycleListenerDelivery delivery);
    void DeliverRepublishedTrack(uint64_t generation, const std::string& previous_sid,
                                std::shared_ptr<TrackPublication> publication);
    bool IsNativeGenerationCurrentLocked(uint64_t generation) const;
    struct ListenerDeliveryContext {
        uint64_t generation;
        std::optional<ConnectionState> required_state = std::nullopt;
        bool require_installed_owner = false;
        bool require_reconnect = false;
        std::shared_ptr<E2eeManager> e2ee_owner;
    };
    bool AdmitListener(const ListenerDeliveryContext& context,
                       const std::shared_ptr<RoomListener>& listener) const;
    template <typename Callback>
    void DeliverListener(const ListenerDeliveryContext& context,
                         const std::shared_ptr<RoomListener>& listener,
                         Callback&& callback, bool legacy_only = false) {
        if (!AdmitListener(context, listener)) return;
        // This virtual query is also application code: never hold room_mutex_,
        // and re-admit after it in case it removed a listener or replaced Room.
        if (legacy_only) {
            if (listener->ConsumesParticipantEvents()) return;
            if (!AdmitListener(context, listener)) return;
        }
        // Admission is the linearization point. An admitted invocation may
        // finish; replacement suppresses every subsequent admission.
        callback(*listener);
    }
    void BeforeNativeEventCommit(uint64_t generation);
    std::shared_ptr<webrtc::PeerConnectionObserver> CreatePeerConnectionObserver(int pc_type, uint64_t generation);
    std::shared_ptr<webrtc::DataChannelObserver> CreateDataChannelObserver(
        bool reliable,
        uint64_t generation,
        webrtc::DataChannelInterface* channel = nullptr);
    void PostRemoteTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                         webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, uint64_t generation);
    void OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                            webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, uint64_t generation);
    void OnRemoteDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel, uint64_t generation);
    void OnLocalIceCandidate(const std::string& sdp, const std::string& mid, int index, int pc_type, uint64_t generation);
    void OnIceConnected(uint64_t generation);
    void OnPeerConnectionStateChanged(int pc_type, webrtc::PeerConnectionInterface::PeerConnectionState state, uint64_t generation);
    void OnRenegotiationNeeded(int pc_type, uint64_t generation);
    void OnDataChannelStateChanged(bool reliable,
                                   webrtc::DataChannelInterface::DataState state,
                                   uint64_t generation,
                                   const webrtc::DataChannelInterface* channel);
    void OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable, uint64_t generation);
    void OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& sid, const std::string& topic, uint64_t generation);
    void OnIncomingDataPacketAt(
        const std::vector<uint8_t>& payload,
        const std::string& sid,
        const std::string& topic,
        uint64_t generation,
        IncomingDataStreamAssembler::TimePoint now);
    void ScheduleIncomingStreamCleanupLocked(uint64_t generation);
    size_t PurgeIncomingStreamsLocked(
        IncomingDataStreamAssembler::TimePoint now,
        uint64_t generation);
    void RetireIncomingReaderLocked(const std::string& stream_id,
                                    const std::string& reason);
    void OnIncomingRpcPacket(const RpcPacket& packet, uint64_t generation);
    bool PublishData(const std::vector<uint8_t>& payload, bool reliable,
                     const std::vector<std::string>& destinations, const std::string& topic, uint64_t generation);
    enum class DataPacketSendResult {
        Accepted,
        SessionInvalid,
        ChannelUnavailable,
        SerializationFailed,
        ChannelRejected,
    };
    DataPacketSendResult PublishDataPacket(const proto::DataPacket& packet,
                                           bool reliable,
                                           uint64_t expected_generation);
    void NegotiatePublisher(uint64_t generation);
    asio::awaitable<std::shared_ptr<TrackPublication>> PublishLocalTrackAsync(
        std::shared_ptr<Track> track, const proto::SignalRequest& request, uint64_t generation);
    void HandleSignalEvent(const SignalEvent& event, uint64_t event_generation = 0);
    void HandleSignalMessage(
        std::shared_ptr<proto::SignalResponse> msg,
        uint64_t event_generation = 0);
    bool IsSignalGenerationCurrentLocked(uint64_t event_generation) const;
    void UpdateParticipants(
        const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants,
        uint64_t event_generation = 0);
    void UpdateParticipants(
        const proto::ParticipantUpdate& update,
        uint64_t event_generation = 0);
    void UpdateTrackMute(
        const proto::MuteTrackRequest& mute,
        uint64_t event_generation = 0);
    void HandleActiveSpeakerUpdate(
        const proto::SpeakersChanged& speakers_changed,
        uint64_t event_generation = 0);
    void UpdateRoomInfo(const proto::Room& room, uint64_t event_generation = 0);
    void UpdateConnectionQuality(
        const proto::ConnectionQualityUpdate& update,
        uint64_t event_generation = 0);
    void UpdateTrackStreamStates(
        const proto::StreamStateUpdate& update,
        uint64_t event_generation = 0);
    void UpdateTrackSubscriptionPermission(
        const proto::SubscriptionPermissionUpdate& update,
        uint64_t event_generation = 0);
    struct SubscriptionIntentKey {
        std::string participant_sid;
        std::string participant_identity;
        std::string track_sid;

        bool operator<(const SubscriptionIntentKey& other) const {
            return std::tie(participant_sid, participant_identity, track_sid) <
                std::tie(other.participant_sid, other.participant_identity, other.track_sid);
        }
    };
    struct RemoteSubscriptionIntent {
        bool subscribed = true;
        uint64_t revision = 0;
        uint64_t last_request_sequence = 0;
    };
    struct PendingSubscriptionUpdate {
        bool subscribed = true;
        uint64_t revision = 0;
    };
    struct SubscriptionSyncSnapshot {
        uint64_t logical_session = 0;
        std::map<SubscriptionIntentKey, uint64_t> revisions;
    };
    SubscriptionIntentKey MakeSubscriptionIntentKeyLocked(
        const std::string& participant_sid,
        const std::string& participant_identity,
        const std::string& track_sid) const;
    RemoteSubscriptionIntent& EnsureSubscriptionIntentLocked(
        const SubscriptionIntentKey& key);
    const RemoteSubscriptionIntent* FindSubscriptionIntentLocked(
        const SubscriptionIntentKey& key) const;
    void QueueSubscriptionUpdateLocked(const SubscriptionIntentKey& key);
    void ScheduleSubscriptionDrainLocked();
    asio::awaitable<void> DrainSubscriptionUpdates(
        uint64_t logical_session,
        uint64_t sender_operation,
        std::shared_ptr<SignalClient> signal);
    void PauseSubscriptionSendingLocked();
    void FinishSubscriptionRecoveryLocked(
        const SubscriptionSyncSnapshot& snapshot,
        const std::shared_ptr<SignalClient>& signal);
    void ResetSubscriptionSessionLocked(bool auto_subscribe);
    void ClearSubscriptionSessionLocked();
    void PruneSubscriptionIntentsLocked();
    std::shared_ptr<MediaBindingState> FindMediaBindingLocked(
        uint64_t binding_serial) const;
    std::shared_ptr<RemoteTrackPublication> CreateRemoteTrackPublication(
        std::shared_ptr<Track> track,
        const std::string& participant_sid,
        const std::string& participant_identity,
        const std::string& track_sid,
        const std::string& name,
        proto::TrackType type);
    RemotePublicationControlDispatch QueueRemotePublicationControl(
        RemoteTrackPublication* publication,
        const std::string& participant_sid,
        uint64_t generation,
        const RemotePublicationControlRequest& request);
    void DetachRemotePublicationMedia(RemoteTrackPublication* publication,
                                      bool notify_listener,
                                      uint64_t binding_serial);
    void RemoveRemoteMediaTrackReferences(
        const std::vector<std::shared_ptr<Track>>& tracks);
    // Called while room_mutex_ is held immediately before the canonical
    // participant map is discarded by disconnect/recovery teardown.
    void ClearRemotePublicationMediaBindingsLocked();
    std::shared_ptr<MembershipState> EnsureMembershipLocked(
        const std::shared_ptr<Participant>& participant,
        bool is_local);
    std::shared_ptr<MembershipState> FindMembershipLocked(
        const std::shared_ptr<Participant>& participant) const;
    std::shared_ptr<TrackMembershipState> EnsureTrackMembershipLocked(
        const std::shared_ptr<Participant>& participant,
        const std::shared_ptr<TrackPublication>& publication);
    void RetireTrackMembershipLocked(const std::shared_ptr<TrackPublication>& publication);
    ParticipantEvent MakeParticipantEventLocked(
        ParticipantEventKind kind,
        const std::shared_ptr<Participant>& participant,
        bool is_local);
    ParticipantEvent MakeTrackEventLocked(
        ParticipantEventKind kind,
        const std::shared_ptr<Participant>& participant,
        const std::shared_ptr<TrackPublication>& publication,
        bool is_local);
    void RetireParticipantLocked(const std::shared_ptr<Participant>& participant);
    void RetireAllMembershipsLocked(std::deque<ParticipantEvent>& retired_events);
    void EnqueueParticipantEventLocked(ParticipantEvent event);
    void EnqueueRosterLocked();
    void DrainParticipantEvents();
    SenderContext ResolveSenderContextLocked(
        const std::string& participant_sid,
        const std::string& participant_identity,
        std::shared_ptr<Participant>* participant);
    asio::awaitable<void> RemoveLocalTrackFromPublisherAsync(
        std::shared_ptr<Track> track,
        uint64_t generation);

    // 协商和 Trickle 信令分发
    void SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type, uint64_t generation);
    void HandleOfferSignal(
        const proto::SessionDescription& offer,
        uint64_t event_generation = 0);
    void HandleAnswerSignal(
        const proto::SessionDescription& answer,
        uint64_t event_generation = 0);
    void HandleTrickleSignal(
        const proto::TrickleRequest& trickle,
        uint64_t event_generation = 0);
    void HandleMediaSectionsRequirement(
        const proto::MediaSectionsRequirement& req,
        uint64_t event_generation = 0);
    proto::SyncState BuildSyncState(SubscriptionSyncSnapshot* snapshot = nullptr) const;
    static RoomDisconnectReason ToRoomDisconnectReason(proto::DisconnectReason reason);
    void BeginServerDisconnect(
        RoomDisconnectReason reason,
        std::string detail,
        uint64_t event_generation);
    asio::awaitable<void> FinalizeServerDisconnectAsync(
        RoomDisconnectReason reason,
        std::string detail,
        uint64_t event_generation);
    asio::awaitable<void> WaitForPrimaryPeerConnection(
        std::chrono::milliseconds timeout,
        uint64_t generation);
    asio::awaitable<void> WaitForReliableDataChannel(
        std::chrono::milliseconds timeout,
        uint64_t generation);
    void CompleteNegotiation(
        const std::string& error,
        uint64_t generation = 0);
    struct PendingOperationCleanup {
        std::vector<std::shared_ptr<AwaitableState<void>>> void_states;
        std::vector<std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>>> publish_states;
    };
    PendingOperationCleanup TakePendingOperationsLocked();
    static void FailPendingOperations(
        PendingOperationCleanup pending,
        OperationErrorCode code,
        const std::string& stage,
        const std::string& message);
    void CancelPendingOperations(OperationErrorCode code, const std::string& stage, const std::string& message);
    void FlushDeferredRoomMessages(uint64_t generation);

    asio::any_io_executor executor_;
    std::shared_ptr<SignalClient> signal_client_;
    std::shared_ptr<proto::JoinResponse> join_response_;
    std::vector<std::string> enabled_publish_codecs_;
    RoomInfo room_info_;
    std::map<std::pair<std::string, std::string>, bool> track_subscription_permissions_;
    ConnectionState connection_state_ = ConnectionState::Disconnected;
    std::shared_ptr<LocalParticipant> local_participant_;
    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_participants_;
    std::vector<std::shared_ptr<RoomListener>> listeners_;
    uint64_t next_participant_incarnation_ = 1;
    uint64_t next_publication_incarnation_ = 1;
    uint64_t next_participant_event_sequence_ = 1;
    std::unordered_map<const Participant*, std::shared_ptr<MembershipState>> participant_memberships_;
    std::unordered_map<const TrackPublication*, std::shared_ptr<TrackMembershipState>> track_memberships_;
    std::deque<ParticipantEvent> participant_events_;
    bool participant_event_drain_scheduled_ = false;
    // Private deterministic test seam: delay taking a native batch without
    // blocking the session worker or changing queue/retirement decisions.
    bool participant_event_drain_paused_for_testing_ = false;
    std::size_t participant_event_paused_attempts_for_testing_ = 0;
    bool audio_output_muted_ = false;
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
    // The negotiation round is shared, while each caller owns its own timeout.
    std::vector<std::shared_ptr<AwaitableState<void>>> negotiation_waiters_;
    bool negotiation_ice_restart_requested_ = false;

    // Subscriber PC negotiation state used by the legacy dual-PC answer router.
    bool subscriber_negotiating_ = false;

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
    std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers_;
    enum class RemoteTrackSinkThread {
        Signaling,
        MediaWorker,
    };
    struct RemoteTrackSinkBinding {
        TrackKey track_key;
        uint64_t binding_serial = 0;
        std::shared_ptr<MediaBindingState> media_binding;
        std::string rtc_track_id;
        RemoteTrackSinkThread detach_thread;
        // Keeps both the WebRTC track and its native sink alive. Calling this
        // unregisters the raw sink pointer before those owners are released.
        std::function<void()> detach;
    };
    std::vector<RemoteTrackSinkBinding> remote_track_sinks_;
    std::unordered_map<const RemoteTrackPublication*, uint64_t>
        current_remote_binding_serials_;
    uint64_t next_remote_track_binding_serial_ = 1;
    static void DetachRemoteTrackSinks(std::vector<RemoteTrackSinkBinding> bindings) noexcept;
    std::vector<RemoteTrackSinkBinding> TakeRemoteTrackSinksForTrackKeys(
        const std::vector<TrackKey>& track_keys);
    std::vector<RemoteTrackSinkBinding> TakeRemoteTrackSinkForBindingSerial(
        uint64_t binding_serial);
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

    struct PendingPeerConnectionWait {
        uint64_t generation = 0;
        int pc_type = 0;
        std::shared_ptr<AwaitableState<void>> completion;
    };
    std::vector<PendingPeerConnectionWait> pending_pc_waits_;
    struct PendingDataChannelWait {
        uint64_t generation = 0;
        std::shared_ptr<AwaitableState<void>> completion;
    };
    std::vector<PendingDataChannelWait> pending_reliable_dc_waits_;

    std::unordered_map<std::string,
        std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>>> pending_track_publishes_;
    // A media sender is changed before the remote SDP answer can commit the
    // public map mutation. Keep that intent through recovery so a failed
    // renegotiation never republishes a track the user removed.
    struct PendingLocalUnpublish {
        uint64_t generation = 0;
        std::shared_ptr<TrackPublication> publication;
        bool sender_removed = false;
    };
    std::unordered_map<std::string, PendingLocalUnpublish> pending_local_unpublishes_;
    std::vector<std::shared_ptr<proto::SignalResponse>> deferred_room_messages_;

    std::atomic<uint64_t> operation_sequence_{1};
    std::atomic<uint64_t> session_generation_{0};
    // Identifies the Connect attempt that installed the current shared
    // Signal/Room/WebRTC bundle. Validity may advance before cleanup runs, so
    // it cannot also serve as the resource owner token.
    uint64_t installed_session_generation_ = 0;
    uint64_t subscription_session_generation_ = 0;
    uint64_t next_subscription_revision_ = 1;
    uint64_t next_subscription_sender_operation_ = 1;
    bool session_auto_subscribe_ = true;
    bool subscription_recovery_barrier_ = false;
    std::string subscription_room_sid_;
    std::map<SubscriptionIntentKey, RemoteSubscriptionIntent> subscription_intents_;
    std::map<SubscriptionIntentKey, PendingSubscriptionUpdate> pending_subscription_updates_;
    bool subscription_sender_active_ = false;
    uint64_t subscription_sender_session_ = 0;
    uint64_t subscription_sender_operation_ = 0;
    OperationTimeouts operation_timeouts_;
    int primary_pc_type_ = 0;
    bool require_media_connection_ = true;
    bool suppress_next_connected_event_ = false;

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
    // A server-issued LeaveRequest with DISCONNECT is authoritative. It must
    // not be mistaken for a transient signal socket close and retried.
    bool reconnect_disabled_ = false;
    bool server_disconnect_finalizing_ = false;
    RoomDisconnectReason disconnect_reason_ = RoomDisconnectReason::Unknown;

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
        uint64_t generation = 0;
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
        const std::string& track_sid, uint64_t generation = 0);
    void FlushPendingTracks(const std::string& participant_sid, uint64_t generation);
    void RemoveExpiredPendingTracks();

    // Simulcast 参数下发同步
    static void ApplySimulcastParameters(webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender, const VideoPublishOptions& opts);

    // 内部私有方法
    asio::awaitable<void> AttemptReconnect(uint64_t owner_generation);
    asio::awaitable<void> RepublishLocalTracks(uint64_t generation);
    asio::awaitable<void> RestartIceConnections(
        std::shared_ptr<proto::ReconnectResponse> reconnect_response,
        std::chrono::milliseconds attempt_timeout);
    void RecordPublishedTracks();
    std::vector<std::shared_ptr<RoomListener>> GetListenersSnapshot() const;

private:
    // Protected by room_mutex_ together with the native generation check.
    std::unique_ptr<IncomingDataStreamAssembler> incoming_data_streams_ =
        std::make_unique<IncomingDataStreamAssembler>();
    std::shared_ptr<DataStreamReaderBudget> incoming_reader_budget_ =
        std::make_shared<DataStreamReaderBudget>();
    std::unordered_map<std::string, std::shared_ptr<TextStreamReader>> active_text_readers_;
    std::unordered_map<std::string, std::shared_ptr<ByteStreamReader>> active_byte_readers_;
    std::unordered_map<std::string, IncomingDataStreamAssembler::TimePoint>
        incoming_stream_deadlines_;
    std::shared_ptr<asio::steady_timer> incoming_stream_cleanup_timer_;
};

} // namespace livekit
