#include "room.h"
#include "webrtc_manager.h"
#include "stats_collector.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "remote_track_publication.h"
#include "rtc_audio_source.h"
#include "rtc_video_source.h"
#include "render/owned_i420_frame.h"
#include "telemetry.h"
#include "log_redaction.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include <nlohmann/json.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <atomic>
#include <future>
#include <cctype>
#include <iostream>
#include <algorithm>
#include "api/jsep.h"
#include "api/video/video_sink_interface.h"

namespace livekit {

namespace {

RoomInfo RoomInfoFromProto(const proto::Room& room) {
    RoomInfo info;
    info.sid = room.sid();
    info.name = room.name();
    info.metadata = room.metadata();
    info.empty_timeout = room.empty_timeout();
    info.departure_timeout = room.departure_timeout();
    info.max_participants = room.max_participants();
    info.creation_time_ms = room.creation_time_ms();
    info.num_participants = room.num_participants();
    info.num_publishers = room.num_publishers();
    info.active_recording = room.active_recording();
    return info;
}

ConnectionQuality ConnectionQualityFromProto(proto::ConnectionQuality quality) {
    switch (quality) {
    case proto::ConnectionQuality::POOR:
        return ConnectionQuality::Poor;
    case proto::ConnectionQuality::GOOD:
        return ConnectionQuality::Good;
    case proto::ConnectionQuality::EXCELLENT:
        return ConnectionQuality::Excellent;
    case proto::ConnectionQuality::LOST:
        return ConnectionQuality::Lost;
    default:
        return ConnectionQuality::Unknown;
    }
}

TrackPublication::StreamState StreamStateFromProto(proto::StreamState state) {
    switch (state) {
    case proto::StreamState::PAUSED:
        return TrackPublication::StreamState::Paused;
    case proto::StreamState::ACTIVE:
    default:
        return TrackPublication::StreamState::Active;
    }
}

class RoomPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    RoomPeerConnectionObserver(std::shared_ptr<Room> room, int pc_type)
        : room_(room), pc_type_(pc_type) {}

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
        if (!data_channel) return;
        if (auto room = room_.lock()) {
            asio::post(room->executor(), [room, data_channel]() {
                room->OnRemoteDataChannel(data_channel);
            });
        }
    }
    
    void OnRenegotiationNeeded() override {
        if (auto room = room_.lock()) {
            asio::post(room->executor(), [room, type = pc_type_]() {
                room->OnRenegotiationNeeded(type);
            });
        }
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        if (auto room = room_.lock()) {
            std::string state_str = "Unknown";
            switch (new_state) {
                case webrtc::PeerConnectionInterface::kIceConnectionNew: state_str = "New"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionChecking: state_str = "Checking"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionConnected: state_str = "Connected"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionCompleted: state_str = "Completed"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionFailed: state_str = "Failed"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: state_str = "Disconnected"; break;
                case webrtc::PeerConnectionInterface::kIceConnectionClosed: state_str = "Closed"; break;
                default: break;
            }
            room->Log("WEBRTC", "ICE_STATE", "PC (" + std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") + ") ICE 状态变为: " + state_str);
            if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
                new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
                asio::post(room->executor(), [room]() {
                    room->OnIceConnected();
                });
            }
        }
    }
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        if (auto room = room_.lock()) {
            std::string state_str = "Unknown";
            switch (new_state) {
                case webrtc::PeerConnectionInterface::PeerConnectionState::kNew: state_str = "New"; break;
                case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting: state_str = "Connecting"; break;
                case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected: state_str = "Connected"; break;
                case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected: state_str = "Disconnected"; break;
                case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed: state_str = "Failed"; break;
                case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed: state_str = "Closed"; break;
                default: break;
            }
            room->Log("WEBRTC", "PC_STATE", "PC (" + std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") + ") 传输总体状态变为: " + state_str);
            room->OnPeerConnectionStateChanged(pc_type_, new_state);
        }
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        std::string sdp_mid = candidate->sdp_mid();
        int sdp_mline_index = candidate->sdp_mline_index();

        if (auto room = room_.lock()) {
            room->Log("SIGNAL", "LOCAL_ICE", "收集到本地 ICE 候选: target=" +
                std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") +
                ", detail=[omitted]");
            asio::post(room->executor(), [room, sdp, sdp_mid, sdp_mline_index, type = pc_type_]() {
                room->OnLocalIceCandidate(sdp, sdp_mid, sdp_mline_index, type);
            });
        }
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        if (auto room = room_.lock()) {
            if (transceiver && transceiver->receiver()) {
                auto receiver = transceiver->receiver();
                auto track = receiver->track();
                room->Log("WEBRTC", "ON_TRACK", "WebRTC Transceiver OnTrack (Kind: " + (track ? std::string(track->kind()) : "null") + ", ID: " + (track ? track->id() : "null") + ")");
                asio::post(room->executor(), [room, receiver, track]() {
                    room->OnRemoteTrackAdded(receiver, track);
                });
            }
        }
    }

    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {
        if (auto room = room_.lock()) {
            if (receiver) {
                auto track = receiver->track();
                room->Log("WEBRTC", "ON_ADD_TRACK", "WebRTC Receiver OnAddTrack (Kind: " + (track ? std::string(track->kind()) : "null") + ", ID: " + (track ? track->id() : "null") + ")");
                asio::post(room->executor(), [room, receiver, track]() {
                    room->OnRemoteTrackAdded(receiver, track);
                });
            }
        }
    }

private:
    std::weak_ptr<Room> room_;
    int pc_type_; // 0 = Publisher, 1 = Subscriber
};

} // namespace

class RoomDataChannelObserver : public webrtc::DataChannelObserver {
public:
    RoomDataChannelObserver(std::shared_ptr<Room> room, bool reliable)
        : room_(room), reliable_(reliable) {}

    void OnStateChange() override {}

    void OnMessage(const webrtc::DataBuffer& buffer) override {
        if (auto room = room_.lock()) {
            std::vector<uint8_t> payload(buffer.data.data(), buffer.data.data() + buffer.data.size());
            asio::post(room->executor(), [room, payload]() {
                room->OnIncomingDataPacket(payload, "", "");
            });
        }
    }

    void OnBufferedAmountChange(uint64_t previous_amount) override {
        if (auto room = room_.lock()) {
            asio::post(room->executor(), [room, previous_amount, reliable = reliable_]() {
                room->OnDataChannelBufferedAmountLow(previous_amount, reliable);
            });
        }
    }

private:
    std::weak_ptr<Room> room_;
    bool reliable_;
};

const char* ToString(RoomDisconnectReason reason) {
    switch (reason) {
    case RoomDisconnectReason::UserLeave: return "USER_LEAVE";
    case RoomDisconnectReason::NetworkError: return "NETWORK_ERROR";
    case RoomDisconnectReason::ServerShutdown: return "SERVER_SHUTDOWN";
    case RoomDisconnectReason::DuplicateIdentity: return "DUPLICATE_IDENTITY";
    case RoomDisconnectReason::ParticipantRemoved: return "PARTICIPANT_REMOVED";
    case RoomDisconnectReason::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

RoomDisconnectReason Room::ToRoomDisconnectReason(proto::DisconnectReason reason) {
    switch (reason) {
    case proto::CLIENT_INITIATED:
        return RoomDisconnectReason::UserLeave;
    case proto::SERVER_SHUTDOWN:
        return RoomDisconnectReason::ServerShutdown;
    case proto::DUPLICATE_IDENTITY:
        return RoomDisconnectReason::DuplicateIdentity;
    case proto::PARTICIPANT_REMOVED:
        return RoomDisconnectReason::ParticipantRemoved;
    default:
        return RoomDisconnectReason::Unknown;
    }
}

Room::Room(asio::any_io_executor executor)
    : executor_(executor) {
    CrashHandler::InstallSignalHandlers();
}

void Room::Log(const std::string& cat, const std::string& tag, const std::string& msg) {
    LogHandler h;
    {
        std::lock_guard lock(room_mutex_);
        h = log_handler_;
    }
    if (h) {
        h(secure_log::SanitizeForOutput(cat),
          secure_log::SanitizeForOutput(tag),
          secure_log::SanitizeForOutput(msg));
    }
}

Room::~Room() {
    Disconnect();
}

ConnectionState Room::connection_state() const {
    std::lock_guard lock(room_mutex_);
    return connection_state_;
}

std::shared_ptr<LocalParticipant> Room::local_participant() const {
    std::lock_guard lock(room_mutex_);
    return local_participant_;
}

std::map<std::string, std::shared_ptr<RemoteParticipant>> Room::remote_participants() const {
    std::lock_guard lock(room_mutex_);
    return remote_participants_;
}

std::shared_ptr<const proto::JoinResponse> Room::join_response() const {
    std::lock_guard lock(room_mutex_);
    return join_response_;
}

std::vector<std::string> Room::enabled_publish_codecs() const {
    std::lock_guard lock(room_mutex_);
    return enabled_publish_codecs_;
}

RoomInfo Room::room_info() const {
    std::lock_guard lock(room_mutex_);
    return room_info_;
}

std::optional<TrackSubscriptionPermission> Room::track_subscription_permission(
    const std::string& participant_sid,
    const std::string& track_sid) const {
    std::lock_guard lock(room_mutex_);
    const auto it = track_subscription_permissions_.find({participant_sid, track_sid});
    if (it == track_subscription_permissions_.end()) {
        return std::nullopt;
    }
    return TrackSubscriptionPermission{participant_sid, track_sid, it->second};
}

std::vector<std::shared_ptr<RoomListener>> Room::GetListenersSnapshot() const {
    std::lock_guard lock(room_mutex_);
    return listeners_;
}

void Room::AddListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard lock(room_mutex_);
    if (listener) {
        listeners_.push_back(listener);
    }
}

void Room::RemoveListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard lock(room_mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

std::shared_ptr<MembershipState> Room::EnsureMembershipLocked(
    const std::shared_ptr<Participant>& participant,
    bool is_local) {
    if (!participant) return nullptr;

    const bool canonical = is_local
        ? local_participant_.get() == participant.get()
        : [&] {
            const auto it = remote_participants_.find(participant->sid());
            return it != remote_participants_.end() && it->second.get() == participant.get();
        }();
    if (!canonical) return nullptr;

    const auto existing = participant_memberships_.find(participant.get());
    if (existing != participant_memberships_.end() &&
        existing->second->active.load(std::memory_order_acquire)) {
        return existing->second;
    }

    ParticipantKey key;
    key.native_room_generation = session_generation_.load(std::memory_order_acquire);
    key.incarnation = next_participant_incarnation_++;
    key.sid = participant->sid();
    key.identity = participant->identity();
    auto state = std::make_shared<MembershipState>(std::move(key));
    participant_memberships_[participant.get()] = state;
    return state;
}

std::shared_ptr<MembershipState> Room::FindMembershipLocked(
    const std::shared_ptr<Participant>& participant) const {
    if (!participant) return nullptr;
    const auto it = participant_memberships_.find(participant.get());
    if (it == participant_memberships_.end()) return nullptr;
    return it->second;
}

std::shared_ptr<TrackMembershipState> Room::EnsureTrackMembershipLocked(
    const std::shared_ptr<Participant>& participant,
    const std::shared_ptr<TrackPublication>& publication) {
    if (!participant || !publication) return nullptr;
    auto participant_state = FindMembershipLocked(participant);
    if (!participant_state ||
        !participant_state->active.load(std::memory_order_acquire) ||
        participant->get_publication(publication->sid()).get() != publication.get()) {
        return nullptr;
    }

    const auto existing = track_memberships_.find(publication.get());
    if (existing != track_memberships_.end() &&
        existing->second->active.load(std::memory_order_acquire)) {
        return existing->second;
    }

    TrackKey key;
    key.participant = participant_state->key;
    key.publication_incarnation = next_publication_incarnation_++;
    key.publication_sid = publication->sid();
    auto state = std::make_shared<TrackMembershipState>(std::move(key));
    track_memberships_[publication.get()] = state;
    return state;
}

void Room::RetireTrackMembershipLocked(
    const std::shared_ptr<TrackPublication>& publication) {
    if (!publication) return;
    const auto it = track_memberships_.find(publication.get());
    if (it == track_memberships_.end()) return;
    it->second->active.store(false, std::memory_order_release);
    track_memberships_.erase(it);
}

ParticipantEvent Room::MakeParticipantEventLocked(
    ParticipantEventKind kind,
    const std::shared_ptr<Participant>& participant,
    bool is_local) {
    ParticipantEvent event;
    event.kind = kind;
    auto state = EnsureMembershipLocked(participant, is_local);
    if (!state) return event;
    event.participant.key = state->key;
    event.participant.ticket = state;
    event.participant.state = participant->SnapshotState();
    event.participant.is_local = is_local;
    return event;
}

ParticipantEvent Room::MakeTrackEventLocked(
    ParticipantEventKind kind,
    const std::shared_ptr<Participant>& participant,
    const std::shared_ptr<TrackPublication>& publication,
    bool is_local) {
    auto event = MakeParticipantEventLocked(kind, participant, is_local);
    auto track_state = EnsureTrackMembershipLocked(participant, publication);
    if (!track_state) return event;
    event.track_key = track_state->key;
    event.track_ticket = track_state;
    event.publication = publication->SnapshotState();
    return event;
}

void Room::RetireParticipantLocked(const std::shared_ptr<Participant>& participant) {
    if (!participant) return;
    const auto membership = participant_memberships_.find(participant.get());
    if (membership == participant_memberships_.end()) return;
    const auto key = membership->second->key;
    membership->second->active.store(false, std::memory_order_release);
    participant_memberships_.erase(membership);

    for (auto it = track_memberships_.begin(); it != track_memberships_.end();) {
        if (it->second->key.participant == key) {
            it->second->active.store(false, std::memory_order_release);
            it = track_memberships_.erase(it);
        } else {
            ++it;
        }
    }
}

void Room::RetireAllMembershipsLocked(std::deque<ParticipantEvent>& retired_events) {
    for (auto& [participant, state] : participant_memberships_) {
        state->active.store(false, std::memory_order_release);
    }
    for (auto& [publication, state] : track_memberships_) {
        state->active.store(false, std::memory_order_release);
    }
    participant_memberships_.clear();
    track_memberships_.clear();
    // The caller supplies an empty local queue and releases its payloads after
    // the Room transaction. Keep the scheduled flag: a posted/running drainer
    // still owns dispatch, including any successor events added before it ends.
    retired_events.swap(participant_events_);
}

void Room::EnqueueParticipantEventLocked(ParticipantEvent event) {
    event.native_room_generation = session_generation_.load(std::memory_order_acquire);
    event.event_sequence = next_participant_event_sequence_++;
    participant_events_.push_back(std::move(event));
    if (participant_event_drain_scheduled_) return;

    participant_event_drain_scheduled_ = true;
    std::weak_ptr<Room> weak = weak_from_this();
    asio::post(executor_, [weak] {
        if (auto room = weak.lock()) room->DrainParticipantEvents();
    });
}

void Room::EnqueueRosterLocked() {
    if (local_participant_) {
        EnqueueParticipantEventLocked(MakeParticipantEventLocked(
            ParticipantEventKind::Upsert, local_participant_, true));
    }
    for (const auto& [sid, participant] : remote_participants_) {
        EnqueueParticipantEventLocked(MakeParticipantEventLocked(
            ParticipantEventKind::Upsert, participant, false));
        for (const auto& [track_sid, publication] : participant->tracks()) {
            const auto track = publication ? publication->track() : nullptr;
            if (track && track->rtc_track()) {
                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                    ParticipantEventKind::TrackAvailable,
                    participant,
                    publication,
                    false));
            }
        }
    }
}

void Room::DrainParticipantEvents() {
    std::vector<ParticipantEvent> batch;
    std::vector<std::shared_ptr<RoomListener>> listeners;
    {
        std::lock_guard lock(room_mutex_);
        if (participant_event_drain_paused_for_testing_) {
            ++participant_event_paused_attempts_for_testing_;
            return;
        }
        const auto count = std::min<std::size_t>(64, participant_events_.size());
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(std::move(participant_events_.front()));
            participant_events_.pop_front();
        }
        listeners = listeners_;
    }

    for (const auto& event : batch) {
        for (const auto& listener : listeners) {
            if (!listener) continue;
            try {
                listener->OnParticipantEvent(event);
            } catch (...) {
                // A listener cannot strand the single dispatcher.
            }
        }
    }

    std::lock_guard lock(room_mutex_);
    if (participant_events_.empty()) {
        participant_event_drain_scheduled_ = false;
        return;
    }
    std::weak_ptr<Room> weak = weak_from_this();
    asio::post(executor_, [weak] {
        if (auto room = weak.lock()) room->DrainParticipantEvents();
    });
}

SenderContext Room::ResolveSenderContextLocked(
    const std::string& participant_sid,
    const std::string& participant_identity,
    std::shared_ptr<Participant>* participant) {
    SenderContext sender;
    sender.transport_sid = participant_sid;
    sender.transport_identity = participant_identity;
    sender.key.native_room_generation =
        session_generation_.load(std::memory_order_acquire);
    sender.key.sid = participant_sid;
    sender.key.identity = participant_identity;
    std::shared_ptr<Participant> resolved;
    bool is_local = false;

    if (!participant_sid.empty()) {
        if (local_participant_ && local_participant_->sid() == participant_sid) {
            resolved = local_participant_;
            is_local = true;
        } else {
            const auto remote = remote_participants_.find(participant_sid);
            if (remote != remote_participants_.end()) resolved = remote->second;
        }
    } else if (!participant_identity.empty()) {
        if (local_participant_ && local_participant_->identity() == participant_identity) {
            resolved = local_participant_;
            is_local = true;
        } else {
            for (const auto& [sid, candidate] : remote_participants_) {
                if (candidate && candidate->identity() == participant_identity) {
                    resolved = candidate;
                    break;
                }
            }
        }
    }

    if (resolved) {
        sender.origin = is_local ? SenderOrigin::Local : SenderOrigin::Remote;
        if (auto membership = EnsureMembershipLocked(resolved, is_local)) {
            sender.key = membership->key;
            sender.ticket = membership;
            const auto snapshot = resolved->SnapshotState();
            sender.display_name = snapshot.name.empty() ? snapshot.identity : snapshot.name;
        }
    } else if (participant_sid.empty() && participant_identity.empty()) {
        sender.origin = SenderOrigin::Server;
    } else {
        sender.origin = SenderOrigin::Unresolved;
    }
    if (participant) *participant = std::move(resolved);
    return sender;
}

asio::awaitable<bool> Room::Connect(const std::string& url, const std::string& token, const SignalOptions& opts) {
    try {
        co_await ConnectAsync(url, token, opts);
        co_return true;
    } catch (const std::exception&) {
        Log("ERROR", "CONNECT_FAILED", secure_log::ExceptionSummary("room_connect"));
        co_return false;
    }
}

asio::awaitable<void> Room::ConnectAsync(const std::string& url, const std::string& token, const SignalOptions& opts) {
    uint64_t generation = 0;
    bool notify_connected = true;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Disconnected) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::InvalidState,
                                 "connect_start",
                                 "room is not disconnected");
        }
        // AttemptReconnect() 复用 ConnectAsync 做全量重建。若其间收到了
        // 服务端终态 LeaveRequest，绝不能在这里重置 reconnect_disabled_ 并
        // 重新加入房间；显式的新 Connect 调用则允许开始一个新会话。
        if (reconnect_active_ && reconnect_disabled_) {
            throw OperationError(OperationKind::Reconnect,
                                 OperationErrorCode::Cancelled,
                                 "connect_start",
                                 "reconnect was cancelled by server leave");
        }
        connection_state_ = ConnectionState::Connecting;
        disconnect_reason_ = RoomDisconnectReason::Unknown;
        room_info_ = RoomInfo{};
        track_subscription_permissions_.clear();
        if (!reconnect_active_) {
            reconnect_disabled_ = false;
            server_disconnect_finalizing_ = false;
        }
        generation = session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        operation_timeouts_ = opts.timeouts;
        require_media_connection_ = opts.create_webrtc_pc;
    }

    try {
        auto self = shared_from_this();
        auto conn_res = co_await SignalClient::Connect(url, token, opts, std::nullopt, [self, generation](const SignalEvent& event) {
            self->HandleSignalEvent(event, generation);
        });

        if (conn_res.error || !conn_res.join_response) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::SignalConnectFailed,
                                 "signal_join",
                                 conn_res.error ? conn_res.error.message() : "JoinResponse is missing",
                                 true);
        }

        auto join_res = conn_res.join_response;

        if (!join_res->has_participant() || join_res->participant().sid().empty()) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::JoinRejected,
                                 "consume_join_response",
                                 "JoinResponse is missing the local participant",
                                 true);
        }

        {
            std::lock_guard lock(room_mutex_);
            if (generation != session_generation_.load(std::memory_order_acquire) ||
                connection_state_ != ConnectionState::Connecting) {
                throw OperationError(OperationKind::Connect,
                                     OperationErrorCode::Cancelled,
                                     "join_commit",
                                     "connect operation was cancelled");
            }
        signal_client_ = conn_res.client;
        join_response_ = join_res;
        if (join_res->has_room()) {
            room_info_ = RoomInfoFromProto(join_res->room());
        }
        enabled_publish_codecs_.clear();
        for (const auto& codec : join_res->enabled_publish_codecs()) {
            std::string mime = codec.mime();
            std::transform(mime.begin(), mime.end(), mime.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!mime.empty()) enabled_publish_codecs_.push_back(std::move(mime));
        }
        local_participant_ = std::make_shared<LocalParticipant>(
            join_res->participant().sid(),
            join_res->participant().identity(),
            [self](const proto::SignalRequest& req) {
                if (self->signal_client_) {
                    self->signal_client_->Send(req);
                }
            }
        );

        ParticipantPermission local_permission;
        if (join_res->participant().has_permission()) {
            const auto& permission = join_res->participant().permission();
            local_permission.can_subscribe = permission.can_subscribe();
            local_permission.can_publish = permission.can_publish();
            local_permission.can_publish_data = permission.can_publish_data();
            local_permission.can_update_metadata = permission.can_update_metadata();
            local_permission.hidden = permission.hidden();
        }
        local_participant_->set_permission(local_permission);
        local_participant_->set_name(join_res->participant().name());
        local_participant_->set_metadata(join_res->participant().metadata());
        local_participant_->set_attributes(std::map<std::string, std::string>(
            join_res->participant().attributes().begin(),
            join_res->participant().attributes().end()));

        // 绑定 LocalParticipant 发布 DataChannel 数据包 Handler
        local_participant_->SetPublishDataHandler(
            [self](const std::vector<uint8_t>& payload, bool reliable,
                   const std::vector<std::string>& destination_identities, const std::string& topic) {
                self->PublishData(payload, reliable, destination_identities, topic);
            }
        );

        // 绑定 LocalParticipant 发布 Native Track Handler
        local_participant_->SetAsyncPublishTrackHandler(
            [self](std::shared_ptr<Track> track,
                   const proto::SignalRequest& request)
                -> asio::awaitable<std::shared_ptr<TrackPublication>> {
                co_return co_await self->PublishLocalTrackAsync(std::move(track), request);
            }
        );

        local_participant_->SetAsyncPublishTracksBatchHandler(
            [self](std::vector<LocalParticipant::BatchTrackItem> items)
                -> asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> {
                co_return co_await self->PublishLocalTracksBatchAsync(std::move(items));
            }
        );

        BindLocalUnpublishHandler();

        // 绑定 LocalParticipant 发送 RPC 请求 Handler
        local_participant_->SetSendRpcHandler(
            [self](const RpcPacket& packet) -> asio::awaitable<std::string> {
                co_return co_await self->SendRpcRequest(packet);
            }
        );

        if (opts.create_webrtc_pc) {
            if (WebRTCManager::Instance().Initialize()) {
                webrtc::PeerConnectionInterface::RTCConfiguration config;
                config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
                config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
                config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
                config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyEnabled;
                if (join_res->has_client_configuration() &&
                    join_res->client_configuration().force_relay() == proto::ClientConfigSetting::ENABLED) {
                    config.type = webrtc::PeerConnectionInterface::kRelay;
                }
                for (int i = 0; i < join_res->ice_servers_size(); ++i) {
                    const auto& ice_srv = join_res->ice_servers(i);
                    webrtc::PeerConnectionInterface::IceServer server;
                    for (int j = 0; j < ice_srv.urls_size(); ++j) {
                        server.urls.push_back(ice_srv.urls(j));
                    }
                    server.username = ice_srv.username();
                    server.password = ice_srv.credential();
                    config.servers.push_back(server);
                }

                publisher_observer_ = std::make_shared<RoomPeerConnectionObserver>(shared_from_this(), 0);
                subscriber_observer_ = std::make_shared<RoomPeerConnectionObserver>(shared_from_this(), 1);

                webrtc::PeerConnectionDependencies pub_deps(publisher_observer_.get());
                auto pub_res = WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(config, std::move(pub_deps));
                if (pub_res.ok()) {
                    publisher_pc_ = pub_res.MoveValue();

                    webrtc::DataChannelInit rel_init;
                    rel_init.ordered = true;
                    reliable_dc_ = publisher_pc_->CreateDataChannel("_reliable", &rel_init);
                    if (reliable_dc_) {
                        auto obs = std::make_shared<RoomDataChannelObserver>(shared_from_this(), true);
                        reliable_dc_->RegisterObserver(obs.get());
                        data_channel_observers_.push_back(obs);
                    }

                    webrtc::DataChannelInit lossy_init;
                    lossy_init.ordered = false;
                    lossy_init.maxRetransmits = 0;
                    lossy_dc_ = publisher_pc_->CreateDataChannel("_lossy", &lossy_init);
                    if (lossy_dc_) {
                        auto obs = std::make_shared<RoomDataChannelObserver>(shared_from_this(), false);
                        lossy_dc_->RegisterObserver(obs.get());
                        data_channel_observers_.push_back(obs);
                    }
                } else {
                    throw OperationError(OperationKind::Connect,
                                         OperationErrorCode::PeerConnectionCreateFailed,
                                         "create_publisher_pc",
                                         pub_res.error().message());
                }

                if (signal_client_->is_single_pc_mode_active()) {
                    subscriber_pc_ = publisher_pc_;
                    Log("WEBRTC", "SINGLE_PC", "启用 Single PC 模式，subscriber_pc_ 共享 publisher_pc_");
                } else {
                    webrtc::PeerConnectionDependencies sub_deps(subscriber_observer_.get());
                    auto sub_res = WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(config, std::move(sub_deps));
                    if (sub_res.ok()) {
                        subscriber_pc_ = sub_res.MoveValue();
                        Log("WEBRTC", "SUB_PC_CREATED", "Subscriber PeerConnection 创建成功");
                    } else {
                        Log("ERROR", "SUB_PC_FAIL",
                            secure_log::OpaqueSummary("create_subscriber_pc"));
                        throw OperationError(OperationKind::Connect,
                                             OperationErrorCode::PeerConnectionCreateFailed,
                                             "create_subscriber_pc",
                                             sub_res.error().message());
                    }
                }

                primary_pc_type_ = signal_client_->is_single_pc_mode_active()
                    ? 0
                    : (join_res->subscriber_primary() ? 1 : 0);
            } else {
                throw OperationError(OperationKind::Connect,
                                     OperationErrorCode::PeerConnectionCreateFailed,
                                     "initialize_webrtc",
                                     "WebRTCManager initialization failed");
            }
        }

        UpdateParticipants(join_res->other_participants());
    }

    if (signal_client_) {
        signal_client_->SetEventReady();
        proto::SignalRequest perm_req;
        auto* perm = perm_req.mutable_subscription_permission();
        perm->set_all_participants(true);
        signal_client_->Send(perm_req);
        Log("SIGNAL", "SUB_PERM", "已向 LiveKit 发送全员订阅权限 SubscriptionPermission (all_participants=true)");

        // JoinResponse determines which transport must be established eagerly.
        // subscriber-primary is lazy unless the server explicitly requests
        // fast_publish; single-PC always needs its sole transport negotiated.
        if (require_media_connection_ &&
            (signal_client_->is_single_pc_mode_active() ||
             !join_res->subscriber_primary() ||
             join_res->fast_publish())) {
            co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);
        }
    }

    if (require_media_connection_) {
        co_await WaitForPrimaryPeerConnection(operation_timeouts_.peer_connection, generation);
    }

    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connecting) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::Cancelled,
                                 "connect_commit",
                                 "connect operation was cancelled before commit");
        }
        connection_state_ = ConnectionState::Connected;
        notify_connected = !suppress_next_connected_event_;
        suppress_next_connected_event_ = false;
        EnqueueRosterLocked();
    }

    if (notify_connected) {
        auto listeners_snapshot = GetListenersSnapshot();
        for (const auto& listener : listeners_snapshot) {
            listener->OnConnected();
        }
    }

    // The Room is externally connected before any delta accumulated during the
    // handshake is delivered. This prevents participant/track callbacks from
    // racing ahead of OnConnected while still preserving real post-Join deltas.
    FlushDeferredRoomMessages();

        co_return;
    } catch (...) {
        std::shared_ptr<SignalClient> signal;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
        std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
        std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
        std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
        std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers;
        std::vector<RemoteTrackSinkBinding> track_sinks;
        std::deque<ParticipantEvent> retired_events;
        {
            std::lock_guard lock(room_mutex_);
            if (generation == session_generation_.load(std::memory_order_acquire)) {
                session_generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            signal = std::move(signal_client_);
            join_response_.reset();
            enabled_publish_codecs_.clear();
            RetireAllMembershipsLocked(retired_events);
            local_participant_.reset();
            pending_local_unpublishes_.clear();
            ClearRemotePublicationMediaBindingsLocked();
            remote_participants_.clear();
            publisher = std::move(publisher_pc_);
            subscriber = std::move(subscriber_pc_);
            publisher_observer = std::move(publisher_observer_);
            subscriber_observer = std::move(subscriber_observer_);
            if (reliable_dc_) {
                data_channels.push_back(reliable_dc_);
                reliable_dc_ = nullptr;
            }
            if (lossy_dc_) {
                data_channels.push_back(lossy_dc_);
                lossy_dc_ = nullptr;
            }
            for (auto& dc : remote_data_channels_) {
                if (dc) data_channels.push_back(dc);
            }
            remote_data_channels_.clear();
            data_channel_observers = std::move(data_channel_observers_);
            track_sinks = std::move(remote_track_sinks_);
            processed_remote_track_ids_.clear();
            pending_track_queue_.clear();
            deferred_room_messages_.clear();
            suppress_next_connected_event_ = false;
            connection_state_ = ConnectionState::Disconnected;
        }
        retired_events.clear();
        CancelPendingOperations(OperationErrorCode::Cancelled,
                                "connect_rollback",
                                "connect transaction rolled back");
        if (signal) signal->Close();
        for (auto& dc : data_channels) {
            if (dc) {
                dc->UnregisterObserver();
                dc->Close();
            }
        }
        data_channels.clear();
        if (publisher) publisher->Close();
        if (subscriber && subscriber != publisher) subscriber->Close();
        DetachRemoteTrackSinks(std::move(track_sinks));
        publisher = nullptr;
        subscriber = nullptr;
        publisher_observer.reset();
        subscriber_observer.reset();
        data_channel_observers.clear();
        throw;
    }
}

void Room::Disconnect() {
    std::deque<ParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) {
            retired_events.swap(participant_events_);
            return; // The lock is destroyed before the local payload queue.
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = RoomDisconnectReason::UserLeave;
        reconnect_disabled_ = true;
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
        listeners_snapshot = listeners_;
        signal = std::move(signal_client_);
        join_response_.reset();
        enabled_publish_codecs_.clear();
        publisher = std::move(publisher_pc_);
        subscriber = std::move(subscriber_pc_);
        publisher_observer = std::move(publisher_observer_);
        subscriber_observer = std::move(subscriber_observer_);
        RetireAllMembershipsLocked(retired_events);
        local_participant_.reset();
        pending_local_unpublishes_.clear();
        ClearRemotePublicationMediaBindingsLocked();
        remote_participants_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        for (auto& dc : remote_data_channels_) {
            if (dc) data_channels.push_back(dc);
        }
        remote_data_channels_.clear();
        data_channel_observers = std::move(data_channel_observers_);
        track_sinks = std::move(remote_track_sinks_);
        processed_remote_track_ids_.clear();
        pending_track_queue_.clear();
        deferred_room_messages_.clear();
    }

    retired_events.clear();

    CancelPendingOperations(OperationErrorCode::Cancelled,
                            "disconnect",
                            "room disconnected");

    if (signal && signal->is_connected()) {
        proto::SignalRequest request;
        auto* leave = request.mutable_leave();
        leave->set_can_reconnect(false);
        leave->set_reason(proto::CLIENT_INITIATED);
        leave->set_action(proto::LeaveRequest_Action_DISCONNECT);

        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        livekit::safe_co_spawn(executor_, [signal, request = std::move(request), promise]() -> asio::awaitable<void> {
            try {
                co_await signal->SendAsync(request);
                promise->set_value();
            } catch (...) {
                try { promise->set_exception(std::current_exception()); } catch (...) {}
            }
        });
        future.wait_for(std::chrono::milliseconds(300));
    }

    if (signal) signal->Close();

    for (auto& dc : data_channels) {
        if (dc) {
            dc->UnregisterObserver();
            dc->Close();
        }
    }
    data_channels.clear();

    if (publisher) publisher->Close();
    if (subscriber && subscriber != publisher) subscriber->Close();
    DetachRemoteTrackSinks(std::move(track_sinks));

    publisher = nullptr;
    subscriber = nullptr;
    publisher_observer.reset();
    subscriber_observer.reset();
    data_channel_observers.clear();

    {
        std::lock_guard mlock(remote_media_mutex_);
        remote_video_tracks_.clear();
        remote_audio_tracks_.clear();
    }

    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected(RoomDisconnectReason::UserLeave,
                                 "Client Initiated Disconnect");
    }
}

asio::awaitable<void> Room::DisconnectAsync() {
    std::deque<ParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) {
            retired_events.swap(participant_events_);
            co_return; // Payloads outlive the lock, including this early exit.
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = RoomDisconnectReason::UserLeave;
        reconnect_disabled_ = true;
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
        listeners_snapshot = listeners_;
        signal = std::move(signal_client_);
        join_response_.reset();
        enabled_publish_codecs_.clear();
        publisher = std::move(publisher_pc_);
        subscriber = std::move(subscriber_pc_);
        publisher_observer = std::move(publisher_observer_);
        subscriber_observer = std::move(subscriber_observer_);
        RetireAllMembershipsLocked(retired_events);
        local_participant_.reset();
        pending_local_unpublishes_.clear();
        ClearRemotePublicationMediaBindingsLocked();
        remote_participants_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        for (auto& dc : remote_data_channels_) {
            if (dc) data_channels.push_back(dc);
        }
        remote_data_channels_.clear();
        data_channel_observers = std::move(data_channel_observers_);
        track_sinks = std::move(remote_track_sinks_);
        processed_remote_track_ids_.clear();
        pending_track_queue_.clear();
        deferred_room_messages_.clear();
    }

    retired_events.clear();

    CancelPendingOperations(OperationErrorCode::Cancelled,
                            "disconnect",
                            "room disconnected");

    if (signal && signal->is_connected()) {
        proto::SignalRequest request;
        auto* leave = request.mutable_leave();
        leave->set_can_reconnect(false);
        leave->set_reason(proto::CLIENT_INITIATED);
        leave->set_action(proto::LeaveRequest_Action_DISCONNECT);
        auto sent = std::make_shared<AwaitableState<void>>(executor_);
        livekit::safe_co_spawn(executor_, [signal, request = std::move(request), sent]() -> asio::awaitable<void> {
            try {
                co_await signal->SendAsync(request);
                CompleteAwaitable(sent);
            } catch (...) {
                FailAwaitable(sent, std::current_exception());
            }
        });
        try {
            co_await WaitAwaitable<void>(sent,
                                         operation_timeouts_.disconnect_grace,
                                         OperationKind::Disconnect,
                                         OperationErrorCode::SessionClosed,
                                         "send_leave");
        } catch (const std::exception&) {
            Log("WARNING", "LEAVE_SEND", secure_log::ExceptionSummary("send_leave"));
        }
    }

    if (signal) signal->Close();

    for (auto& dc : data_channels) {
        if (dc) {
            dc->UnregisterObserver();
            dc->Close();
        }
    }
    data_channels.clear();

    if (publisher) publisher->Close();
    if (subscriber && subscriber != publisher) subscriber->Close();
    DetachRemoteTrackSinks(std::move(track_sinks));

    publisher = nullptr;
    subscriber = nullptr;
    publisher_observer.reset();
    subscriber_observer.reset();
    data_channel_observers.clear();

    {
        std::lock_guard lock(remote_media_mutex_);
        remote_video_tracks_.clear();
        remote_audio_tracks_.clear();
    }
    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected(RoomDisconnectReason::UserLeave,
                                 "Client Initiated Disconnect");
    }
}

void Room::BeginServerDisconnect(RoomDisconnectReason reason, std::string detail) {
    bool should_finalize = false;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected ||
            server_disconnect_finalizing_) {
            return;
        }
        disconnect_reason_ = reason;
        reconnect_disabled_ = true;
        server_disconnect_finalizing_ = true;
        should_finalize = true;
    }

    if (!should_finalize) return;

    Log("SIGNAL", "LEAVE_DISCONNECT",
        "[Room] 服务端要求退出房间: reason=" + std::string(ToString(reason)) +
        ", detail=" + detail);
    livekit::safe_co_spawn(executor_,
        [self = shared_from_this(), reason, detail = std::move(detail)]()
            -> asio::awaitable<void> {
            co_await self->FinalizeServerDisconnectAsync(reason, detail);
        });
}

asio::awaitable<void> Room::FinalizeServerDisconnectAsync(
    RoomDisconnectReason reason, std::string detail) {
    std::deque<ParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) {
            server_disconnect_finalizing_ = false;
            retired_events.swap(participant_events_);
            co_return;
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = reason;
        reconnect_attempts_ = 0;
        reconnect_active_ = false;
        server_disconnect_finalizing_ = false;
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
        listeners_snapshot = listeners_;
        signal = std::move(signal_client_);
        join_response_.reset();
        enabled_publish_codecs_.clear();
        publisher = std::move(publisher_pc_);
        subscriber = std::move(subscriber_pc_);
        publisher_observer = std::move(publisher_observer_);
        subscriber_observer = std::move(subscriber_observer_);
        RetireAllMembershipsLocked(retired_events);
        local_participant_.reset();
        pending_local_unpublishes_.clear();
        ClearRemotePublicationMediaBindingsLocked();
        remote_participants_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        for (auto& dc : remote_data_channels_) {
            if (dc) data_channels.push_back(dc);
        }
        remote_data_channels_.clear();
        data_channel_observers = std::move(data_channel_observers_);
        track_sinks = std::move(remote_track_sinks_);
        processed_remote_track_ids_.clear();
        pending_track_queue_.clear();
        deferred_room_messages_.clear();
        suppress_next_connected_event_ = false;
    }

    retired_events.clear();

    CancelPendingOperations(OperationErrorCode::SessionClosed,
                            "server_leave",
                            detail);
    if (signal) signal->Close();
    for (auto& dc : data_channels) {
        if (dc) {
            dc->UnregisterObserver();
            dc->Close();
        }
    }
    if (publisher) publisher->Close();
    if (subscriber && subscriber != publisher) subscriber->Close();
    DetachRemoteTrackSinks(std::move(track_sinks));
    {
        std::lock_guard lock(remote_media_mutex_);
        remote_video_tracks_.clear();
        remote_audio_tracks_.clear();
    }

    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected(reason, detail);
    }
}

bool Room::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                       const std::vector<std::string>& destination_identities, const std::string& topic) {
    static constexpr size_t kMaxChunkSize = 15000;
    static constexpr size_t kMaxDataStreamSize = 16 * 1024 * 1024;
    static std::atomic<uint64_t> stream_sequence{0};

    if (payload.size() > kMaxDataStreamSize) {
        Log("DATA", "PAYLOAD_TOO_LARGE", "拒绝发送超过 16 MiB 的 DataStream");
        return false;
    }

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    std::shared_ptr<LocalParticipant> local_participant;
    {
        std::lock_guard lock(room_mutex_);
        dc = reliable ? reliable_dc_ : lossy_dc_;
        local_participant = local_participant_;
    }

    const std::string local_identity = local_participant ? local_participant->identity() : std::string{};
    const std::string local_sid = local_participant ? local_participant->sid() : std::string{};
    auto send_packet = [&](const std::vector<uint8_t>& bytes) -> bool {
        if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
            webrtc::DataBuffer buffer(
                webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()),
                /*binary=*/true);
            if (!dc->Send(buffer)) {
                Log("DATA", "SEND_FAILED", "DataChannel 拒绝发送数据包");
                return false;
            }
            return true;
        } else {
            OnIncomingDataPacket(bytes, local_sid, topic);
            return true;
        }
    };

    if (payload.size() > kMaxChunkSize) {
        // === DataStream Chunking 大包切片分发逻辑 ===
        std::string stream_id = "ds_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(stream_sequence.fetch_add(1, std::memory_order_relaxed));

        // 1. 发送 Header 帧
        proto::DataPacket header_pkt;
        header_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant) {
            header_pkt.set_participant_identity(local_identity);
            header_pkt.set_participant_sid(local_sid);
        }
        auto* header = header_pkt.mutable_stream_header();
        header->set_stream_id(stream_id);
        header->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        header->set_topic(topic);
        header->set_total_length(payload.size());

        std::vector<uint8_t> header_bytes(header_pkt.ByteSizeLong());
        header_pkt.SerializeToArray(header_bytes.data(), static_cast<int>(header_bytes.size()));

        if (!send_packet(header_bytes)) {
            return false;
        }

        // 2. 切片发送 Chunks
        size_t total_chunks = (payload.size() + kMaxChunkSize - 1) / kMaxChunkSize;
        for (size_t i = 0; i < total_chunks; ++i) {
            size_t offset = i * kMaxChunkSize;
            size_t chunk_len = std::min(kMaxChunkSize, payload.size() - offset);

            proto::DataPacket chunk_pkt;
            chunk_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
            if (local_participant) {
                chunk_pkt.set_participant_identity(local_identity);
                chunk_pkt.set_participant_sid(local_sid);
            }
            auto* chunk = chunk_pkt.mutable_stream_chunk();
            chunk->set_stream_id(stream_id);
            chunk->set_chunk_index(i);
            chunk->set_content(payload.data() + offset, chunk_len);

            std::vector<uint8_t> chunk_bytes(chunk_pkt.ByteSizeLong());
            chunk_pkt.SerializeToArray(chunk_bytes.data(), static_cast<int>(chunk_bytes.size()));

            if (!send_packet(chunk_bytes)) {
                return false;
            }
        }

        // 3. 发送 Trailer 结束帧
        proto::DataPacket trailer_pkt;
        trailer_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant) {
            trailer_pkt.set_participant_identity(local_identity);
            trailer_pkt.set_participant_sid(local_sid);
        }
        auto* trailer = trailer_pkt.mutable_stream_trailer();
        trailer->set_stream_id(stream_id);

        std::vector<uint8_t> trailer_bytes(trailer_pkt.ByteSizeLong());
        trailer_pkt.SerializeToArray(trailer_bytes.data(), static_cast<int>(trailer_bytes.size()));

        if (!send_packet(trailer_bytes)) {
            return false;
        }
        return true;
    }

    proto::DataPacket packet;
    packet.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
    if (local_participant) {
        packet.set_participant_identity(local_identity);
        packet.set_participant_sid(local_sid);
    }

    auto* user_packet = packet.mutable_user();
    user_packet->set_topic(topic);
    user_packet->set_payload(payload.data(), payload.size());
    for (const auto& dest : destination_identities) {
        user_packet->add_destination_identities(dest);
    }

    std::vector<uint8_t> data(packet.ByteSizeLong());
    packet.SerializeToArray(data.data(), static_cast<int>(data.size()));

    return send_packet(data);
}

bool Room::PublishDataPacket(const proto::DataPacket& packet, bool reliable) {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    std::shared_ptr<LocalParticipant> local_participant;
    {
        std::lock_guard lock(room_mutex_);
        dc = reliable ? reliable_dc_ : lossy_dc_;
        local_participant = local_participant_;
    }

    proto::DataPacket final_pkt = packet;
    final_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
    if (local_participant) {
        if (final_pkt.participant_identity().empty()) {
            final_pkt.set_participant_identity(local_participant->identity());
        }
        if (final_pkt.participant_sid().empty()) {
            final_pkt.set_participant_sid(local_participant->sid());
        }
    }

    std::vector<uint8_t> bytes(final_pkt.ByteSizeLong());
    final_pkt.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));

    if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
        webrtc::DataBuffer buffer(
            webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()),
            /*binary=*/true);
        if (!dc->Send(buffer)) {
            Log("DATA", "SEND_FAILED", "DataChannel 拒绝发送数据包");
            return false;
        }
        return true;
    } else {
        std::string topic;
        if (final_pkt.has_stream_header()) topic = final_pkt.stream_header().topic();
        else if (final_pkt.has_user()) topic = final_pkt.user().topic();
        std::string local_sid = local_participant ? local_participant->sid() : std::string{};
        OnIncomingDataPacket(bytes, local_sid, topic);
        return true;
    }
}

std::shared_ptr<TextStreamWriter> Room::CreateTextStreamWriter(
    const std::string& topic,
    const std::map<std::string, std::string>& attributes,
    const std::string& stream_id,
    std::optional<std::size_t> total_size,
    const std::string& reply_to_id,
    const std::vector<std::string>& destination_identities) {

    std::string sender_id;
    {
        std::lock_guard lock(room_mutex_);
        if (local_participant_) {
            sender_id = local_participant_->identity();
        }
    }

    auto weak_self = weak_from_this();
    auto publisher = [weak_self](const proto::DataPacket& packet, bool reliable) -> bool {
        if (auto self = weak_self.lock()) {
            return self->PublishDataPacket(packet, reliable);
        }
        return false;
    };

    return std::make_shared<TextStreamWriter>(
        std::move(publisher), topic, attributes, stream_id,
        total_size, reply_to_id, destination_identities, sender_id);
}

std::shared_ptr<ByteStreamWriter> Room::CreateByteStreamWriter(
    const std::string& name,
    const std::string& topic,
    const std::map<std::string, std::string>& attributes,
    const std::string& stream_id,
    std::optional<std::size_t> total_size,
    const std::string& mime_type,
    const std::vector<std::string>& destination_identities) {

    std::string sender_id;
    {
        std::lock_guard lock(room_mutex_);
        if (local_participant_) {
            sender_id = local_participant_->identity();
        }
    }

    auto weak_self = weak_from_this();
    auto publisher = [weak_self](const proto::DataPacket& packet, bool reliable) -> bool {
        if (auto self = weak_self.lock()) {
            return self->PublishDataPacket(packet, reliable);
        }
        return false;
    };

    return std::make_shared<ByteStreamWriter>(
        std::move(publisher), name, topic, attributes, stream_id,
        total_size, mime_type, destination_identities, sender_id);
}

asio::awaitable<std::string> Room::SendRpcRequest(const RpcPacket& packet) {
    auto self = shared_from_this();
    auto pending = std::make_shared<PendingRpcCall>();
    pending->timer = std::make_shared<asio::steady_timer>(
        executor_,
        std::chrono::milliseconds(static_cast<int64_t>(packet.timeout_sec * 1000.0))
    );

    {
        std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
        pending_rpc_calls_[packet.request_id] = pending;
    }

    std::string encoded = packet.Encode();
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    RpcPacket response_packet = co_await asio::async_initiate<decltype(asio::use_awaitable), void(RpcPacket)>(
        [self, pending, packet, data](auto handler) mutable {
            auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));
            
            pending->completion_cb = [self, pending, request_id = packet.request_id, handler_ptr](const RpcPacket& resp) {
                bool should_call = false;
                {
                    std::lock_guard<std::mutex> lock(self->pending_rpc_mutex_);
                    if (!pending->finished) {
                        pending->finished = true;
                        should_call = true;
                    }
                }
                if (should_call) {
                    std::error_code ec;
                    pending->timer->cancel(ec);
                    (*handler_ptr)(resp);
                }
            };

            pending->timer->async_wait([self, pending, request_id = packet.request_id, handler_ptr](const std::error_code& ec) {
                bool should_call = false;
                {
                    std::lock_guard<std::mutex> lock(self->pending_rpc_mutex_);
                    if (!pending->finished) {
                        pending->finished = true;
                        should_call = true;
                    }
                }
                if (should_call && !ec) {
                    RpcPacket timeout_resp;
                    timeout_resp.has_error = true;
                    timeout_resp.error_code = static_cast<int>(RpcErrorCode::TIMEOUT);
                    timeout_resp.error_message = "RPC call timed out";
                    (*handler_ptr)(timeout_resp);
                }
            });

            // 在 completion_cb 与定时器就绪后，再执行网络发包
            self->PublishData(data, /*reliable=*/true, {packet.destination_identity}, /*topic=*/"lk.rpc");
        },
        asio::use_awaitable
    );

    {
        std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
        pending_rpc_calls_.erase(packet.request_id);
    }

    if (response_packet.has_error) {
        throw RpcError(static_cast<RpcErrorCode>(response_packet.error_code), response_packet.error_message);
    }

    co_return response_packet.payload;
}

void Room::OnIncomingRpcPacket(const RpcPacket& packet) {
    if (packet.type == RpcPacketType::Response) {
        std::shared_ptr<PendingRpcCall> pending;
        {
            std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
            auto it = pending_rpc_calls_.find(packet.request_id);
            if (it != pending_rpc_calls_.end()) {
                pending = it->second;
                pending_rpc_calls_.erase(it);
            }
        }
        if (pending) {
            std::error_code ec;
            pending->timer->cancel(ec);
            if (pending->completion_cb) {
                pending->completion_cb(packet);
            }
        }
    } else if (packet.type == RpcPacketType::Request) {
        std::shared_ptr<LocalParticipant> local;
        {
            std::lock_guard lock(room_mutex_);
            local = local_participant_;
        }
        if (!local) return;

        auto handler = local->getRpcHandler(packet.method);
        auto self = shared_from_this();

        if (!handler) {
            RpcPacket err_resp;
            err_resp.type = RpcPacketType::Response;
            err_resp.request_id = packet.request_id;
            err_resp.method = packet.method;
            err_resp.caller_identity = local->identity();
            err_resp.destination_identity = packet.caller_identity;
            err_resp.has_error = true;
            err_resp.error_code = static_cast<int>(RpcErrorCode::UNSUPPORTED_METHOD);
            err_resp.error_message = "Method '" + packet.method + "' is not supported by " + local->identity();

            std::string encoded = err_resp.Encode();
            std::vector<uint8_t> data(encoded.begin(), encoded.end());
            PublishData(data, /*reliable=*/true, {packet.caller_identity}, "lk.rpc");
            return;
        }

        RpcInvocationData inv_data;
        inv_data.request_id = packet.request_id;
        inv_data.caller_identity = packet.caller_identity;
        inv_data.payload = packet.payload;
        inv_data.response_timeout_sec = packet.timeout_sec;

        livekit::safe_co_spawn(executor_, [self, local, handler, inv_data, packet]() -> asio::awaitable<void> {
            RpcPacket resp;
            resp.type = RpcPacketType::Response;
            resp.request_id = packet.request_id;
            resp.method = packet.method;
            resp.caller_identity = local->identity();
            resp.destination_identity = packet.caller_identity;

            try {
                resp.payload = co_await handler(inv_data);
            } catch (const RpcError& e) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(e.code());
                resp.error_message = e.message();
            } catch (const std::exception& e) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(RpcErrorCode::APPLICATION_ERROR);
                resp.error_message = e.what();
            } catch (...) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(RpcErrorCode::APPLICATION_ERROR);
                resp.error_message = "Unknown exception in RPC handler";
            }

            std::string encoded = resp.Encode();
            std::vector<uint8_t> data(encoded.begin(), encoded.end());
            self->PublishData(data, /*reliable=*/true, {packet.caller_identity}, "lk.rpc");
        });
    }
}

void Room::SetDataChannelBufferedAmountLowThreshold(uint64_t threshold, bool reliable) {
    std::lock_guard lock(room_mutex_);
    if (reliable) {
        reliable_buffered_low_threshold_ = threshold;
    } else {
        lossy_buffered_low_threshold_ = threshold;
    }
}

uint64_t Room::GetDataChannelBufferedAmount(bool reliable) const {
    std::lock_guard lock(room_mutex_);
    auto dc = reliable ? reliable_dc_ : lossy_dc_;
    if (dc) {
        return dc->buffered_amount();
    }
    return 0;
}

void Room::OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable) {
    auto snapshot = GetListenersSnapshot();
    uint64_t current_amount = GetDataChannelBufferedAmount(reliable);
    for (const auto& listener : snapshot) {
        listener->OnDataChannelBufferedAmountLowThresholdChanged(current_amount, reliable);
    }
}

void Room::OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic) {
    std::vector<uint8_t> real_payload = payload;
    std::string real_topic = topic;
    std::string real_sender_sid = participant_sid;
    std::string sender_identity;
    bool is_structured_chat = false;

    // 尝试反序列化 Protobuf DataPacket
    proto::DataPacket data_pkt;
    if (data_pkt.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        if (!data_pkt.participant_identity().empty()) {
            sender_identity = data_pkt.participant_identity();
        }
        if (!data_pkt.participant_sid().empty()) {
            real_sender_sid = data_pkt.participant_sid();
        }

        if (data_pkt.has_stream_header()) {
            const auto& header = data_pkt.stream_header();
            incoming_data_streams_.Begin(
                header.stream_id(),
                header.topic(),
                header.total_length(),
                sender_identity,
                real_sender_sid);

            // 构造并派发上层流式 Reader
            std::shared_ptr<Participant> p;
            SenderContext sender;
            {
                std::lock_guard lock(room_mutex_);
                sender = ResolveSenderContextLocked(
                    real_sender_sid, sender_identity, &p);
            }
            auto listeners_snapshot = GetListenersSnapshot();

            if (header.has_text_header()) {
                TextStreamInfo info;
                info.stream_id = header.stream_id();
                info.topic = header.topic();
                info.mime_type = header.mime_type();
                info.timestamp = header.timestamp();
                if (header.has_total_length()) info.total_length = header.total_length();
                for (const auto& [k, v] : header.attributes()) {
                    info.attributes[k] = v;
                }
                info.sender_identity = sender_identity;
                info.sender_sid = real_sender_sid;
                const auto& th = header.text_header();
                info.operation_type = th.operation_type();
                info.version = th.version();
                info.reply_to_stream_id = th.reply_to_stream_id();
                for (const auto& att : th.attached_stream_ids()) {
                    info.attached_stream_ids.push_back(att);
                }
                info.generated = th.generated();

                auto reader = std::make_shared<TextStreamReader>(std::move(info));
                {
                    std::lock_guard lk(streams_mutex_);
                    active_text_readers_[header.stream_id()] = reader;
                }
                {
                    std::lock_guard lock(room_mutex_);
                    ParticipantEvent event = p
                        ? MakeParticipantEventLocked(
                            ParticipantEventKind::TextStreamOpened,
                            p,
                            p.get() == local_participant_.get())
                        : ParticipantEvent{};
                    event.kind = ParticipantEventKind::TextStreamOpened;
                    event.sender = sender;
                    event.text_reader = reader;
                    EnqueueParticipantEventLocked(std::move(event));
                }
                for (const auto& listener : listeners_snapshot) {
                    if (!listener->ConsumesParticipantEvents()) {
                        listener->OnTextStreamOpened(reader, p);
                    }
                }
            } else {
                ByteStreamInfo info;
                info.stream_id = header.stream_id();
                info.topic = header.topic();
                info.mime_type = header.mime_type();
                info.timestamp = header.timestamp();
                if (header.has_total_length()) info.total_length = header.total_length();
                for (const auto& [k, v] : header.attributes()) {
                    info.attributes[k] = v;
                }
                info.sender_identity = sender_identity;
                info.sender_sid = real_sender_sid;
                if (header.has_byte_header()) {
                    info.name = header.byte_header().name();
                }

                auto reader = std::make_shared<ByteStreamReader>(std::move(info));
                {
                    std::lock_guard lk(streams_mutex_);
                    active_byte_readers_[header.stream_id()] = reader;
                }
                {
                    std::lock_guard lock(room_mutex_);
                    ParticipantEvent event = p
                        ? MakeParticipantEventLocked(
                            ParticipantEventKind::ByteStreamOpened,
                            p,
                            p.get() == local_participant_.get())
                        : ParticipantEvent{};
                    event.kind = ParticipantEventKind::ByteStreamOpened;
                    event.sender = sender;
                    event.byte_reader = reader;
                    EnqueueParticipantEventLocked(std::move(event));
                }
                for (const auto& listener : listeners_snapshot) {
                    if (!listener->ConsumesParticipantEvents()) {
                        listener->OnByteStreamOpened(reader, p);
                    }
                }
            }
            return;
        } else if (data_pkt.has_stream_chunk()) {
            const auto& chunk = data_pkt.stream_chunk();
            const auto& content = chunk.content();

            // 派发给活跃 reader
            {
                std::lock_guard lk(streams_mutex_);
                if (auto it = active_text_readers_.find(chunk.stream_id()); it != active_text_readers_.end()) {
                    it->second->OnChunkUpdate(content);
                }
                if (auto it = active_byte_readers_.find(chunk.stream_id()); it != active_byte_readers_.end()) {
                    it->second->OnChunkUpdate(reinterpret_cast<const uint8_t*>(content.data()), content.size());
                }
            }

            auto assembled = incoming_data_streams_.AddChunk(
                chunk.stream_id(),
                chunk.chunk_index(),
                std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(content.data()),
                    content.size()));
            if (!assembled) {
                return;
            }
            real_payload = std::move(assembled->payload);
            real_topic = std::move(assembled->topic);
            sender_identity = std::move(assembled->sender_identity);
            real_sender_sid = std::move(assembled->sender_sid);
        } else if (data_pkt.has_stream_trailer()) {
            const auto& trailer = data_pkt.stream_trailer();

            // 关闭活跃 reader
            {
                std::lock_guard lk(streams_mutex_);
                std::map<std::string, std::string> attrs(trailer.attributes().begin(), trailer.attributes().end());
                if (auto it = active_text_readers_.find(trailer.stream_id()); it != active_text_readers_.end()) {
                    it->second->OnStreamClose(trailer.reason(), attrs);
                    active_text_readers_.erase(it);
                }
                if (auto it = active_byte_readers_.find(trailer.stream_id()); it != active_byte_readers_.end()) {
                    it->second->OnStreamClose(trailer.reason(), attrs);
                    active_byte_readers_.erase(it);
                }
            }

            auto assembled = incoming_data_streams_.Finish(trailer.stream_id());
            if (!assembled) {
                return;
            }
            real_payload = std::move(assembled->payload);
            real_topic = std::move(assembled->topic);
            sender_identity = std::move(assembled->sender_identity);
            real_sender_sid = std::move(assembled->sender_sid);
        } else if (data_pkt.has_user()) {
            const auto& user_pkt = data_pkt.user();
            const std::string& p_bytes = user_pkt.payload();
            real_payload.assign(p_bytes.begin(), p_bytes.end());
            real_topic = user_pkt.topic();
        } else if (data_pkt.has_chat_message()) {
            const auto& pb_chat = data_pkt.chat_message();
            real_payload.assign(pb_chat.message().begin(), pb_chat.message().end());
            real_topic = "lk.chat";
            is_structured_chat = true;
        }
    }

    // 1. 优先校验解包 LiveKit RPC 报文 (Topic 为 lk.rpc)
    if (real_topic == "lk.rpc") {
        std::string text_payload(real_payload.begin(), real_payload.end());
        auto rpc_pkt_opt = RpcPacket::Decode(text_payload);
        if (rpc_pkt_opt.has_value()) {
            OnIncomingRpcPacket(rpc_pkt_opt.value());
            return;
        }
    }

    auto listeners_snapshot = GetListenersSnapshot();
    std::shared_ptr<RemoteParticipant> remote_p;
    std::shared_ptr<LocalParticipant> local_p;
    std::shared_ptr<Participant> resolved_participant;
    SenderContext sender_context;
    {
        std::lock_guard lock(room_mutex_);
        sender_context = ResolveSenderContextLocked(
            real_sender_sid, sender_identity, &resolved_participant);
        remote_p = std::dynamic_pointer_cast<RemoteParticipant>(resolved_participant);
        local_p = std::dynamic_pointer_cast<LocalParticipant>(resolved_participant);
    }

    // 2. 解析并派发结构化 Chat 消息
    bool chat_dispatched = false;
    if (is_structured_chat) {
        const auto& pb_chat = data_pkt.chat_message();
        ChatMessage chat;
        chat.id = pb_chat.id();
        chat.timestamp = pb_chat.timestamp();
        chat.edit_timestamp = pb_chat.edit_timestamp();
        chat.message = pb_chat.message();
        chat.sender_identity = !sender_identity.empty() ? sender_identity : (remote_p ? remote_p->identity() : real_sender_sid);
        std::shared_ptr<Participant> p = remote_p ? remote_p : (local_p && (local_p->sid() == real_sender_sid || local_p->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_p) : nullptr);
        for (const auto& listener : listeners_snapshot) {
            listener->OnChatMessage(chat, p);
        }
        chat_dispatched = true;
    } else if (real_topic == "lk.chat" || real_topic == "lk-chat-topic" || real_topic.empty()) {
        std::string text_payload(real_payload.begin(), real_payload.end());
        auto chat_opt = ChatMessage::Decode(text_payload, !sender_identity.empty() ? sender_identity : real_sender_sid);
        if (chat_opt.has_value()) {
            std::shared_ptr<Participant> p = remote_p ? remote_p : (local_p && (local_p->sid() == real_sender_sid || local_p->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_p) : nullptr);
            for (const auto& listener : listeners_snapshot) {
                listener->OnChatMessage(chat_opt.value(), p);
            }
            chat_dispatched = true;
        }
    }

    // 3. 派发原始 OnDataReceived 事件给 RoomListener
    if (!chat_dispatched && real_topic != "lk.chat" && real_topic != "lk-chat-topic" && real_topic != "lk.rpc") {
        {
            std::lock_guard lock(room_mutex_);
            ParticipantEvent event = resolved_participant
                ? MakeParticipantEventLocked(
                    ParticipantEventKind::DataReceived,
                    resolved_participant,
                    resolved_participant.get() == local_participant_.get())
                : ParticipantEvent{};
            event.kind = ParticipantEventKind::DataReceived;
            event.sender = sender_context;
            event.data = real_payload;
            event.topic = real_topic;
            EnqueueParticipantEventLocked(std::move(event));
        }
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnDataReceived(real_payload, remote_p, real_topic);
            }
        }
    }
}

void Room::OnIceConnected() {
    Log("WEBRTC", "ICE_READY", "WebRTC 媒体底层连接已就绪，向 SFU 激活所有下行视频流并请求关键帧...");
    std::lock_guard lock(room_mutex_);
    if (signal_client_) {
        for (const auto& kv : remote_participants_) {
            if (kv.second) {
                std::vector<std::string> video_sids;
                for (const auto& pub_kv : kv.second->tracks()) {
                    if (pub_kv.second && pub_kv.second->track() && pub_kv.second->track()->kind() == TrackKind::Video) {
                        video_sids.push_back(pub_kv.first);
                        signal_client_->SendUpdateTrackSettings(pub_kv.first, false, proto::VideoQuality::HIGH, 1280, 720, 30, 0);
                        Log("SIGNAL", "TRACK_ACTIVE", "已向 SFU 激活视频流 Track SID=" + pub_kv.first + " (Participant: " + kv.second->identity() + ")");
                    }
                }
                if (!video_sids.empty()) {
                    signal_client_->SendUpdateSubscription(video_sids, true, kv.first);
                }
            }
        }
    }
}

void Room::OnPeerConnectionStateChanged(
    int pc_type,
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
    std::vector<std::shared_ptr<AwaitableState<void>>> success;
    std::vector<std::shared_ptr<AwaitableState<void>>> failure;
    {
        std::lock_guard lock(room_mutex_);
        const auto generation = session_generation_.load(std::memory_order_acquire);
        for (const auto& waiter : pending_pc_waits_) {
            if (waiter.generation != generation || waiter.pc_type != pc_type) continue;
            if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
                success.push_back(waiter.completion);
            } else if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
                       state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
                failure.push_back(waiter.completion);
            }
        }
    }
    for (const auto& completion : success) CompleteAwaitable(completion);
    for (const auto& completion : failure) {
        FailAwaitable(completion, std::make_exception_ptr(OperationError(
            OperationKind::Connect,
            OperationErrorCode::PeerConnectionCreateFailed,
            "peer_connection_state",
            "primary peer connection failed before becoming connected",
            true)));
    }
}

asio::awaitable<void> Room::WaitForPrimaryPeerConnection(
    std::chrono::milliseconds timeout,
    uint64_t generation) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    int pc_type = 0;
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::Cancelled,
                                 "wait_primary_pc",
                                 "session was replaced while waiting for peer connection");
        }
        pc_type = primary_pc_type_;
        pc = pc_type == 1 ? subscriber_pc_ : publisher_pc_;
        if (!pc) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::PeerConnectionCreateFailed,
                                 "wait_primary_pc",
                                 "primary peer connection is null");
        }
        pending_pc_waits_.push_back({generation, pc_type, completion});
    }

    auto state = pc->peer_connection_state();
    if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
        CompleteAwaitable(completion);
    } else if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
               state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
        FailAwaitable(completion, std::make_exception_ptr(OperationError(
            OperationKind::Connect,
            OperationErrorCode::PeerConnectionCreateFailed,
            "wait_primary_pc",
            "primary peer connection is already failed",
            true)));
    }

    try {
        co_await WaitAwaitable<void>(completion, timeout,
                                     OperationKind::Connect,
                                     OperationErrorCode::PeerConnectionTimeout,
                                     "wait_primary_pc");
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        pending_pc_waits_.erase(std::remove_if(pending_pc_waits_.begin(), pending_pc_waits_.end(),
            [&](const PendingPeerConnectionWait& item) { return item.completion == completion; }),
            pending_pc_waits_.end());
        throw;
    }

    std::lock_guard lock(room_mutex_);
    pending_pc_waits_.erase(std::remove_if(pending_pc_waits_.begin(), pending_pc_waits_.end(),
        [&](const PendingPeerConnectionWait& item) { return item.completion == completion; }),
        pending_pc_waits_.end());
}

void Room::CancelPendingOperations(OperationErrorCode code,
                                   const std::string& stage,
                                   const std::string& message) {
    std::vector<std::shared_ptr<AwaitableState<void>>> void_states;
    std::vector<std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>>> publish_states;
    {
        std::lock_guard lock(room_mutex_);
        for (const auto& waiter : pending_pc_waits_) void_states.push_back(waiter.completion);
        pending_pc_waits_.clear();
        void_states.insert(void_states.end(),
                           negotiation_waiters_.begin(),
                           negotiation_waiters_.end());
        negotiation_waiters_.clear();
        negotiation_state_ = NegotiationState::Idle;
        for (const auto& [cid, state] : pending_track_publishes_) publish_states.push_back(state);
        pending_track_publishes_.clear();
    }
    for (const auto& state : void_states) {
        FailAwaitable(state, std::make_exception_ptr(OperationError(
            OperationKind::Connect, code, stage, message, true)));
    }
    for (const auto& state : publish_states) {
        FailAwaitable(state, std::make_exception_ptr(OperationError(
            OperationKind::PublishTrack, code, stage, message, true)));
    }
}

void Room::FlushDeferredRoomMessages() {
    std::vector<std::shared_ptr<proto::SignalResponse>> messages;
    {
        std::lock_guard lock(room_mutex_);
        messages.swap(deferred_room_messages_);
    }
    for (auto& message : messages) HandleSignalMessage(std::move(message));
}

void Room::OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    SendTrickleCandidate(sdp, sdp_mid, sdp_mline_index, pc_type);
}

void Room::OnRemoteDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    if (!data_channel) return;
    bool reliable = (data_channel->label() == "_reliable" || data_channel->label() == "reliable");
    auto obs = std::make_shared<RoomDataChannelObserver>(shared_from_this(), reliable);
    data_channel->RegisterObserver(obs.get());
    std::cout << "[WebRTC DataChannel] Registered observer on remote DataChannel: " << data_channel->label() << std::endl;
    std::lock_guard lock(room_mutex_);
    data_channel_observers_.push_back(obs);
    remote_data_channels_.push_back(data_channel);
}

namespace {

class NativeAudioTrackSink : public webrtc::AudioTrackSinkInterface {
public:
    explicit NativeAudioTrackSink(std::function<void(const AudioFrame&)> callback)
        : callback_(std::move(callback)) {}

    void OnData(const void* audio_data,
                int bits_per_sample,
                int sample_rate,
                size_t number_of_channels,
                size_t number_of_frames) override {
        if (!callback_) return;

        int num_samples = static_cast<int>(number_of_frames * number_of_channels);

        AudioFrame frame = AudioFrame::create(sample_rate, static_cast<int>(number_of_channels), static_cast<int>(number_of_frames));
        const int16_t* pcm_data = static_cast<const int16_t*>(audio_data);
        if (!frame.data().empty()) {
            std::copy(pcm_data, pcm_data + num_samples, frame.data().begin());
        }

        ///********/
        //double sum = 0.0;
        //for (int i = 0; i < num_samples; ++i) {
        //    sum += static_cast<double>(pcm_data[i]) * pcm_data[i];
        //}
        //double rms = std::sqrt(sum / num_samples);
        //// 打印音量诊断（大于 300 说明有明显说话声）
        //if (rms > 300.0) {
        //    std::cout << "[AUDIO ACTIVE VOICE] 正在说话! 音量 RMS = " << rms << std::endl;
        //}
        ///*********/



        callback_(frame);
    }

private:
    std::function<void(const AudioFrame&)> callback_;
};

class NativeVideoTrackSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit NativeVideoTrackSink(std::function<void(render::OwnedI420Frame::Ptr)> callback)
        : callback_(std::move(callback)) {}

    void OnFrame(const webrtc::VideoFrame& rtc_frame) override {
        if (!callback_) return;

        // Do not run an I420-to-RGBA pixel loop on WebRTC's media worker.
        // CopyFrom preserves stride-correct planes and metadata in a frame that
        // can safely outlive the decoder buffer.
        if (auto frame = render::OwnedI420Frame::CopyFrom(rtc_frame)) {
            callback_(std::move(frame));
        }
    }

private:
    std::function<void(render::OwnedI420Frame::Ptr)> callback_;
};

} // namespace

void Room::DetachRemoteTrackSinks(std::vector<RemoteTrackSinkBinding> bindings) noexcept {
    for (auto& binding : bindings) {
        if (!binding.detach) continue;
        try {
            auto& manager = WebRTCManager::Instance();
            auto* detach_thread = binding.detach_thread == RemoteTrackSinkThread::Signaling
                ? manager.signaling_thread()
                : manager.worker_thread();
            if (detach_thread) {
                detach_thread->BlockingCall([detach = std::move(binding.detach)]() mutable {
                    detach();
                });
            } else {
                binding.detach();
            }
        } catch (const std::exception&) {
            std::cerr << "[WEBRTC] Failed to detach remote track sink: "
                      << secure_log::ExceptionSummary("detach_remote_track_sink") << std::endl;
        } catch (...) {
            std::cerr << "[WEBRTC] Failed to detach remote track sink: unknown error"
                      << std::endl;
        }
    }
}

std::vector<Room::RemoteTrackSinkBinding> Room::TakeRemoteTrackSinksForTrackKeys(
    const std::vector<TrackKey>& track_keys) {
    std::vector<RemoteTrackSinkBinding> removed;
    if (track_keys.empty()) return removed;

    std::lock_guard lock(room_mutex_);
    for (auto it = remote_track_sinks_.begin(); it != remote_track_sinks_.end();) {
        if (std::find(track_keys.begin(), track_keys.end(), it->track_key) ==
            track_keys.end()) {
            ++it;
            continue;
        }
        removed.push_back(std::move(*it));
        it = remote_track_sinks_.erase(it);
    }
    for (const auto& binding : removed) {
        const bool still_bound = std::any_of(
            remote_track_sinks_.begin(), remote_track_sinks_.end(),
            [&binding](const RemoteTrackSinkBinding& candidate) {
                return candidate.rtc_track_id == binding.rtc_track_id;
            });
        if (!still_bound) processed_remote_track_ids_.erase(binding.rtc_track_id);
    }
    return removed;
}

std::vector<Room::RemoteTrackSinkBinding> Room::TakeRemoteTrackSinkForBindingSerial(
    uint64_t binding_serial) {
    std::vector<RemoteTrackSinkBinding> removed;
    if (binding_serial == 0) return removed;

    std::lock_guard lock(room_mutex_);
    const auto binding = std::find_if(
        remote_track_sinks_.begin(), remote_track_sinks_.end(),
        [binding_serial](const RemoteTrackSinkBinding& candidate) {
            return candidate.binding_serial == binding_serial;
        });
    if (binding == remote_track_sinks_.end()) return removed;
    const std::string rtc_track_id = binding->rtc_track_id;
    removed.push_back(std::move(*binding));
    remote_track_sinks_.erase(binding);
    const bool still_bound = std::any_of(
        remote_track_sinks_.begin(), remote_track_sinks_.end(),
        [&rtc_track_id](const RemoteTrackSinkBinding& candidate) {
            return candidate.rtc_track_id == rtc_track_id;
        });
    if (!still_bound) processed_remote_track_ids_.erase(rtc_track_id);
    return removed;
}

std::shared_ptr<RemoteTrackPublication> Room::CreateRemoteTrackPublication(
    std::shared_ptr<Track> track,
    const std::string& participant_sid,
    const std::string& track_sid,
    const std::string& name,
    proto::TrackType type) {
    const auto generation = session_generation_.load(std::memory_order_acquire);
    std::weak_ptr<Room> weak_room = weak_from_this();
    auto controller = [weak_room, participant_sid, generation](
                          RemoteTrackPublication* publication,
                          const RemotePublicationControlRequest& request) {
        const auto room = weak_room.lock();
        if (!room) return RemotePublicationControlDispatch::Rejected;
        return room->QueueRemotePublicationControl(
            publication, participant_sid, generation, request);
    };
    return std::make_shared<RemoteTrackPublication>(
        std::move(track), track_sid, name, type, generation, std::move(controller));
}

RemotePublicationControlDispatch Room::QueueRemotePublicationControl(
    RemoteTrackPublication* publication,
    const std::string& participant_sid,
    uint64_t generation,
    const RemotePublicationControlRequest& request) {
    if (!publication) return RemotePublicationControlDispatch::Rejected;

    std::shared_ptr<RemoteTrackPublication> canonical;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire) ||
            !signal_client_) {
            return RemotePublicationControlDispatch::Rejected;
        }
        const auto participant = remote_participants_.find(participant_sid);
        if (participant == remote_participants_.end()) {
            return RemotePublicationControlDispatch::Rejected;
        }
        canonical = participant->second->get_remote_publication(publication->sid());
        if (!canonical || canonical.get() != publication ||
            canonical->session_generation() != generation) {
            return RemotePublicationControlDispatch::Rejected;
        }
    }

    // All live controls flow through the Room executor. The delayed task
    // repeats ownership/generation validation, so an unpublish/disconnect in
    // between cannot send a request or mutate a stale publication.
    std::weak_ptr<Room> weak_room = weak_from_this();
    std::weak_ptr<RemoteTrackPublication> weak_publication = canonical;
    asio::post(executor_, [weak_room, weak_publication, participant_sid, generation, request]() {
        const auto room = weak_room.lock();
        const auto canonical = weak_publication.lock();
        if (!room || !canonical) return;

        std::shared_ptr<SignalClient> signal;
        {
            std::lock_guard lock(room->room_mutex_);
            if (room->connection_state_ != ConnectionState::Connected ||
                generation != room->session_generation_.load(std::memory_order_acquire) ||
                !room->signal_client_) {
                return;
            }
            const auto participant = room->remote_participants_.find(participant_sid);
            if (participant == room->remote_participants_.end() ||
                participant->second->get_remote_publication(canonical->sid()) != canonical ||
                canonical->session_generation() != generation) {
                return;
            }
            signal = room->signal_client_;
        }

        if (request.kind == RemotePublicationControlRequest::Kind::Subscription) {
            if (request.subscribed.has_value() && !*request.subscribed) {
                // This executes only after the delayed operation has repeated
                // canonical-map and generation validation. Detach before the
                // signal so no renderer can receive a late frame.
                canonical->DetachMedia(/*notify_listener=*/true);
            }
            signal->SendUpdateSubscription({canonical->sid()},
                                           request.subscribed.value_or(true),
                                           participant_sid);
        } else {
            signal->SendUpdateTrackSettings(
                canonical->sid(),
                !request.enabled.value_or(canonical->is_enabled()),
                request.quality.value_or(canonical->current_quality()),
                request.width.value_or(canonical->current_width()),
                request.height.value_or(canonical->current_height()),
                0,
                request.priority.value_or(canonical->priority()));
        }
        canonical->CommitControl(request);
    });
    return RemotePublicationControlDispatch::Queued;
}

void Room::RemoveRemoteMediaTrackReferences(
    const std::vector<std::shared_ptr<Track>>& removed_tracks) {
    if (removed_tracks.empty()) return;
    const auto remove_tracks = [&removed_tracks](std::vector<std::weak_ptr<Track>>& tracks) {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&removed_tracks](const std::weak_ptr<Track>& weak) {
            const auto track = weak.lock();
            return !track || std::find(removed_tracks.begin(), removed_tracks.end(), track) !=
                removed_tracks.end();
        }), tracks.end());
    };

    std::lock_guard lock(remote_media_mutex_);
    remove_tracks(remote_video_tracks_);
    remove_tracks(remote_audio_tracks_);
}

void Room::ClearRemotePublicationMediaBindingsLocked() {
    for (const auto& [participant_sid, participant] : remote_participants_) {
        if (!participant) continue;
        for (const auto& [track_sid, publication] : participant->tracks()) {
            if (const auto remote_publication =
                    std::dynamic_pointer_cast<RemoteTrackPublication>(publication)) {
                remote_publication->ClearMediaBinding();
            }
        }
    }
    current_remote_binding_serials_.clear();
}

void Room::DetachRemotePublicationMedia(RemoteTrackPublication* publication,
                                        bool notify_listener,
                                        uint64_t binding_serial) {
    if (!publication) return;

    std::shared_ptr<RemoteParticipant> participant;
    std::shared_ptr<RemoteTrackPublication> canonical;
    std::shared_ptr<Track> track;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> detached_rtc_track;
    std::vector<std::shared_ptr<RoomListener>> listeners;
    {
        std::lock_guard lock(room_mutex_);
        for (const auto& [participant_sid, candidate] : remote_participants_) {
            const auto remote_publication = candidate->get_remote_publication(publication->sid());
            if (remote_publication && remote_publication.get() == publication) {
                participant = candidate;
                canonical = remote_publication;
                break;
            }
        }
        if (!participant || !canonical || !canonical->has_media_binding()) return;
        const auto current_serial = current_remote_binding_serials_.find(canonical.get());
        if (current_serial == current_remote_binding_serials_.end() ||
            current_serial->second != binding_serial) {
            return;
        }

        track = canonical->track();
        const auto track_membership = track_memberships_.find(canonical.get());
        if (track_membership == track_memberships_.end()) return;
        current_remote_binding_serials_.erase(current_serial);
        canonical->ClearMediaBinding();
        // Retire this binding's logical media references in the same Room
        // transaction as its serial. A concurrent/reentrant attach may reuse
        // the canonical Track after this point. Keep the RTC reference alive
        // so setting nullptr cannot destroy an RTC object under room_mutex_.
        // set_rtc_track(nullptr) performs no RTC set_enabled call.
        if (track) {
            detached_rtc_track = track->rtc_track();
            track->set_rtc_track(nullptr);
        }
        // Matches retain_binding's Room -> remote_media_mutex_ lock order.
        RemoveRemoteMediaTrackReferences({track});
        EnqueueParticipantEventLocked(MakeTrackEventLocked(
            ParticipantEventKind::TrackUnavailable,
            participant,
            canonical,
            false));
        listeners = listeners_;
    }

    // Move physical sinks out of Room before calling WebRTC RemoveSink. The
    // publication is already marked detached, making repeated unsubscribe and
    // replacement calls idempotent.
    DetachRemoteTrackSinks(TakeRemoteTrackSinkForBindingSerial(binding_serial));

    if (notify_listener && track) {
        for (const auto& listener : listeners) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnTrackUnsubscribed(track, canonical, participant);
            }
        }
    }
}

void Room::AddTrackToPublisher(std::shared_ptr<Track> track) {
    if (!track) return;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::string stream_id;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        stream_id = "livekit_stream_" + (local_participant_ ? local_participant_->identity() : "local");
    }

    if (!pub_pc) return;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track = track->rtc_track();
    if (!rtc_track && WebRTCManager::Instance().factory()) {
        if (track->kind() == TrackKind::Audio) {
            auto audio_track = std::dynamic_pointer_cast<LocalAudioTrack>(track);
            if (audio_track && audio_track->source()) {
                auto rtc_src = RtcAudioSource::Create(audio_track->source());
                auto rtc_audio_track = WebRTCManager::Instance().factory()->CreateAudioTrack(track->name(), rtc_src.get());
                track->set_rtc_track(rtc_audio_track);
                rtc_track = rtc_audio_track;
            }
        } else if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track && video_track->source()) {
                auto rtc_src = RtcVideoSource::Create(video_track->source());
                auto rtc_video_track = WebRTCManager::Instance().factory()->CreateVideoTrack(rtc_src, track->name());
                track->set_rtc_track(rtc_video_track);
                rtc_track = rtc_video_track;
            }
        }
    }

    if (rtc_track) {
        VideoPublishOptions publish_opts;
        if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track) {
                publish_opts = video_track->publish_options();
            }
        }

        struct AddTrackParams {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
            std::string stream_id;
            VideoPublishOptions publish_opts;
            std::shared_ptr<Room> room;
        };
        auto* p = new AddTrackParams{pub_pc, rtc_track, stream_id, publish_opts, shared_from_this()};
        WebRTCManager::Instance().signaling_thread()->PostTask([p]() {
            if (p->pc && p->track) {
                if (p->track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind && p->publish_opts.simulcast && !p->publish_opts.layers.empty()) {
                    webrtc::RtpTransceiverInit init;
                    init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                    init.stream_ids = { p->stream_id };

                    for (const auto& layer : p->publish_opts.layers) {
                        webrtc::RtpEncodingParameters encoding;
                        encoding.active = true;
                        encoding.rid = layer.rid;
                        encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                        encoding.max_bitrate_bps = layer.max_bitrate_bps;
                        encoding.max_framerate = layer.max_fps;
                        if (!p->publish_opts.scalability_mode.empty()) {
                            encoding.scalability_mode = p->publish_opts.scalability_mode;
                        }
                        init.send_encodings.push_back(encoding);
                    }

                    auto transceiver_res = p->pc->AddTransceiver(p->track, init);
                    if (transceiver_res.ok()) {
                        std::cout << "[SIMULCAST TRANSCEIVER] Added Simulcast Transceiver for video track with " 
                                  << p->publish_opts.layers.size() << " layers!\n";
                    } else {
                        std::cerr << "[SIMULCAST TRANSCEIVER] AddTransceiver failed: "
                                  << secure_log::OpaqueSummary("add_transceiver") << "\n";
                        p->pc->AddTrack(p->track, { p->stream_id });
                    }
                } else {
                    p->pc->AddTrack(p->track, { p->stream_id });
                }
                std::cout << "[WebRTC] Added native track (" << p->track->kind() << ") to Publisher PeerConnection." << std::endl;
                p->room->Log("TRACK", "PUB_ATTACH", "已将 Track [" + p->track->id() + "] (" + p->track->kind() + ") 添加至 Publisher PeerConnection");
                p->room->SendPublishOffer();
            }
            delete p;
        });
    }
}

void Room::ApplySimulcastParameters(webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender, const VideoPublishOptions& opts) {
    if (!sender) return;
    webrtc::RtpParameters parameters = sender->GetParameters();
    if (parameters.encodings.empty()) {
        std::cout << "[SIMULCAST SET_PARAMS] Warning: RtpSender has no encodings to configure.\n";
        return;
    }

    bool updated = false;
    for (const auto& layer : opts.layers) {
        for (auto& enc : parameters.encodings) {
            if (enc.rid == layer.rid || (parameters.encodings.size() == 1 && layer.rid == "f")) {
                enc.active = true;
                if (layer.scale_resolution_down_by > 0) {
                    enc.scale_resolution_down_by = layer.scale_resolution_down_by;
                }
                if (layer.max_bitrate_bps > 0) {
                    enc.max_bitrate_bps = layer.max_bitrate_bps;
                }
                if (layer.max_fps > 0) {
                    enc.max_framerate = layer.max_fps;
                }
                if (!opts.scalability_mode.empty()) {
                    enc.scalability_mode = opts.scalability_mode;
                }
                updated = true;
            }
        }
    }

    if (updated) {
        auto status = sender->SetParameters(parameters);
        if (status.ok()) {
            std::cout << "[SIMULCAST SET_PARAMS] Successfully applied RtpParameters for " 
                      << parameters.encodings.size() << " encodings!\n";
        } else {
            std::cerr << "[SIMULCAST SET_PARAMS] SetParameters failed: "
                      << secure_log::OpaqueSummary("set_rtp_parameters") << "\n";
        }
    }
}

void Room::NegotiatePublisher() {
    auto generation = session_generation_.load(std::memory_order_acquire);
    auto timeout = operation_timeouts_.negotiation;
    livekit::safe_co_spawn(executor_, [self = shared_from_this(), generation, timeout]() -> asio::awaitable<void> {
        try {
            co_await self->NegotiatePublisherAsync(timeout, generation);
        } catch (const std::exception&) {
            self->Log("ERROR", "NEGOTIATION_ASYNC",
                      secure_log::ExceptionSummary("publisher_negotiation"));
        }
    });
}

asio::awaitable<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>
Room::AddTrackToPublisherAsync(std::shared_ptr<Track> track, uint64_t generation) {
    if (!track) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "track is null");
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::string stream_id;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connected) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "install_sender",
                                 "room session changed while publishing");
        }
        pc = publisher_pc_;
        stream_id = "livekit_stream_" +
            (local_participant_ ? local_participant_->identity() : "local");
    }
    if (!pc || !WebRTCManager::Instance().factory() ||
        !WebRTCManager::Instance().signaling_thread()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "publisher peer connection is unavailable");
    }

    auto rtc_track = track->rtc_track();
    if (!rtc_track) {
        if (track->kind() == TrackKind::Audio) {
            auto audio_track = std::dynamic_pointer_cast<LocalAudioTrack>(track);
            if (audio_track && audio_track->source()) {
                auto rtc_src = RtcAudioSource::Create(audio_track->source());
                rtc_track = WebRTCManager::Instance().factory()->CreateAudioTrack(
                    track->name(), rtc_src.get());
            }
        } else if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track && video_track->source()) {
                auto rtc_src = RtcVideoSource::Create(video_track->source());
                rtc_track = WebRTCManager::Instance().factory()->CreateVideoTrack(
                    rtc_src, track->name());
            }
        }
        if (rtc_track) track->set_rtc_track(rtc_track);
    }
    if (!rtc_track) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "failed to create native WebRTC track");
    }

    VideoPublishOptions publish_options;
    if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
        publish_options = video->publish_options();
    }

    auto completion = std::make_shared<AwaitableState<
        webrtc::scoped_refptr<webrtc::RtpSenderInterface>>>(executor_);
    // WebRTC is linked with a static CRT and PostTask type-erases the closure
    // inside webrtc::Thread. Keep the closure trivially destructible: putting
    // std::string/std::vector directly in it can make absl::AnyInvocable free
    // their storage through a different CRT at task teardown.
    struct AddTrackTaskParams {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track;
        std::string stream_id;
        VideoPublishOptions publish_options;
        uint64_t generation = 0;
    };
    auto* params = new AddTrackTaskParams{
        shared_from_this(), completion, pc, rtc_track, stream_id,
        std::move(publish_options), generation};
    WebRTCManager::Instance().signaling_thread()->PostTask(
        [params]() {
            // Destruction happens in this translation unit rather than in the
            // AnyInvocable manager instantiated across the WebRTC ABI boundary.
            std::unique_ptr<AddTrackTaskParams> owned(params);
            auto& task = *owned;
            if (task.generation != task.room->session_generation_.load(std::memory_order_acquire)) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::Cancelled,
                    "install_sender",
                    "session changed before sender installation")));
                return;
            }

            webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
            webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
            std::string error;
            if (task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
                task.publish_options.simulcast && !task.publish_options.layers.empty()) {
                webrtc::RtpTransceiverInit init;
                init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                init.stream_ids = {task.stream_id};
                for (const auto& layer : task.publish_options.layers) {
                    webrtc::RtpEncodingParameters encoding;
                    encoding.active = true;
                    encoding.rid = layer.rid;
                    encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                    encoding.max_bitrate_bps = layer.max_bitrate_bps;
                    encoding.max_framerate = layer.max_fps;
                    if (!task.publish_options.scalability_mode.empty()) {
                        encoding.scalability_mode = task.publish_options.scalability_mode;
                    }
                    init.send_encodings.push_back(std::move(encoding));
                }
                auto result = task.pc->AddTransceiver(task.rtc_track, init);
                if (result.ok()) {
                    transceiver = result.MoveValue();
                    sender = transceiver->sender();
                } else {
                    error = result.error().message();
                }
            } else {
                auto result = task.pc->AddTrack(task.rtc_track, {task.stream_id});
                if (result.ok()) {
                    sender = result.MoveValue();
                    for (const auto& candidate : task.pc->GetTransceivers()) {
                        if (candidate && candidate->sender() == sender) {
                            transceiver = candidate;
                            break;
                        }
                    }
                } else {
                    error = result.error().message();
                }
            }

            if (!sender) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::StateUncertain,
                    "install_sender",
                    error.empty() ? "WebRTC did not return an RTP sender" : error,
                    true)));
                return;
            }
            if (transceiver &&
                task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
                !task.publish_options.video_codec.empty()) {
                auto get_prefs = [](const std::string& codec_name) {
                    std::string selected = codec_name;
                    std::transform(selected.begin(), selected.end(), selected.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    auto capabilities = WebRTCManager::Instance().factory()->GetRtpSenderCapabilities(
                        webrtc::MediaType::VIDEO);
                    std::vector<webrtc::RtpCodecCapability> preferences;
                    std::set<int> selected_payload_types;
                    for (const auto& codec : capabilities.codecs) {
                        std::string name = codec.name;
                        std::transform(name.begin(), name.end(), name.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (name == selected) {
                            preferences.push_back(codec);
                            if (codec.preferred_payload_type) {
                                selected_payload_types.insert(*codec.preferred_payload_type);
                            }
                        }
                    }
                    for (const auto& codec : capabilities.codecs) {
                        if (!codec.IsResiliencyCodec()) continue;
                        std::string name = codec.name;
                        std::transform(name.begin(), name.end(), name.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (name == "rtx") {
                            const auto apt = codec.parameters.find("apt");
                            if (apt == codec.parameters.end()) continue;
                            try {
                                if (!selected_payload_types.contains(std::stoi(apt->second))) continue;
                            } catch (...) {
                                continue;
                            }
                        }
                        preferences.push_back(codec);
                    }
                    return preferences;
                };

                auto preferences = get_prefs(task.publish_options.video_codec);
                if (!preferences.empty()) {
                    auto codec_status = transceiver->SetCodecPreferences(preferences);
                    if (!codec_status.ok()) {
                        task.pc->RemoveTrackOrError(sender);
                        FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                            OperationKind::PublishTrack,
                            OperationErrorCode::StateUncertain,
                            "apply_join_codec_policy",
                            codec_status.message(),
                            true)));
                        return;
                    }
                }

                // GAP-03: Multi-Codec Simulcast & Backup Codecs Transceiver
                if (task.publish_options.simulcast && task.publish_options.simulcast_codecs.size() > 1) {
                    for (size_t c_idx = 1; c_idx < task.publish_options.simulcast_codecs.size(); ++c_idx) {
                        const auto& backup_spec = task.publish_options.simulcast_codecs[c_idx];
                        if (backup_spec.layers.empty()) continue;

                        webrtc::RtpTransceiverInit backup_init;
                        backup_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                        backup_init.stream_ids = {task.stream_id};
                        bool is_active = (task.publish_options.backup_codec_policy == BackupCodecPolicy::Simulcast);

                        for (const auto& layer : backup_spec.layers) {
                            webrtc::RtpEncodingParameters encoding;
                            encoding.active = is_active;
                            encoding.rid = layer.rid;
                            encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                            encoding.max_bitrate_bps = layer.max_bitrate_bps;
                            encoding.max_framerate = layer.max_fps;
                            if (!backup_spec.scalability_mode.empty()) {
                                encoding.scalability_mode = backup_spec.scalability_mode;
                            }
                            backup_init.send_encodings.push_back(std::move(encoding));
                        }

                        auto backup_res = task.pc->AddTransceiver(task.rtc_track, backup_init);
                        if (backup_res.ok()) {
                            auto backup_transceiver = backup_res.MoveValue();
                            auto backup_prefs = get_prefs(backup_spec.codec);
                            if (!backup_prefs.empty()) {
                                backup_transceiver->SetCodecPreferences(backup_prefs);
                            }
                            std::cout << "[BACKUP CODEC] Added Backup Transceiver for codec=" << backup_spec.codec
                                      << " (layers=" << backup_spec.layers.size()
                                      << ", active=" << (is_active ? "ON" : "OFF (on-demand)") << ")\n";
                            task.room->Log("TRACK", "BACKUP_CODEC_ATTACH",
                                           "已为视频轨挂载备用编码器 Transceiver: Codec=" + backup_spec.codec +
                                           ", Layers=" + std::to_string(backup_spec.layers.size()) +
                                           ", Policy=" + (is_active ? "Simulcast" : "PreferRegression"));
                        }
                    }
                }
            }
            CompleteAwaitable(task.completion, std::move(sender));
        });

    co_return co_await WaitAwaitable<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>(
        completion,
        operation_timeouts_.publish,
        OperationKind::PublishTrack,
        OperationErrorCode::TrackPublishTimeout,
        "install_sender");
}

asio::awaitable<std::shared_ptr<TrackPublication>> Room::PublishLocalTrackAsync(
    std::shared_ptr<Track> track,
    const proto::SignalRequest& request) {
    if (!track || !request.has_add_track()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "publish_validate",
                             "invalid AddTrack request");
    }

    proto::SignalRequest effective_request(request);
    if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
        std::vector<std::string> enabled_codecs;
        {
            std::lock_guard lock(room_mutex_);
            enabled_codecs = enabled_publish_codecs_;
        }
        if (!enabled_codecs.empty()) {
            auto options = video->publish_options();
            std::string requested = options.video_codec;
            std::transform(requested.begin(), requested.end(), requested.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const auto is_requested = [&requested](const std::string& mime) {
                return mime == requested || mime == "video/" + requested;
            };
            if (std::none_of(enabled_codecs.begin(), enabled_codecs.end(), is_requested)) {
                const auto fallback = std::find_if(
                    enabled_codecs.begin(), enabled_codecs.end(),
                    [](const std::string& mime) { return mime.rfind("video/", 0) == 0; });
                if (fallback == enabled_codecs.end()) {
                    throw OperationError(OperationKind::PublishTrack,
                                         OperationErrorCode::PermissionDenied,
                                         "join_publish_codecs",
                                         "server did not enable a video publish codec");
                }
                options.video_codec = fallback->substr(6);
                video->set_publish_options(options);
                for (auto& codec : *effective_request.mutable_add_track()->mutable_simulcast_codecs()) {
                    codec.set_codec(options.video_codec);
                }
                Log("SIGNAL", "PUBLISH_CODEC_FALLBACK",
                    "服务端未启用请求的编码 " + requested + "，改用 " + options.video_codec);
            }
        }
    }

    const auto generation = session_generation_.load(std::memory_order_acquire);
    const std::string cid = effective_request.add_track().cid();
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<LocalParticipant> local;
    auto ack = std::make_shared<AwaitableState<proto::TrackPublishedResponse>>(executor_);
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "room is not connected");
        }
        if (pending_track_publishes_.contains(cid)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "a publish operation for the same CID is already in progress");
        }
        signal = signal_client_;
        local = local_participant_;
        pending_track_publishes_[cid] = ack;
    }

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
    bool server_acknowledged = false;
    try {
        co_await signal->SendAsync(effective_request);
        auto response = co_await WaitAwaitable<proto::TrackPublishedResponse>(
            ack,
            operation_timeouts_.publish,
            OperationKind::PublishTrack,
            OperationErrorCode::TrackPublishTimeout,
            "wait_track_published_ack");
        server_acknowledged = true;

        {
            std::lock_guard lock(room_mutex_);
            pending_track_publishes_.erase(cid);
        }

        sender = co_await AddTrackToPublisherAsync(track, generation);
        co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);

        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "publish_commit",
                                 "session changed before publication commit");
        }

        track->set_sid(response.track().sid());
        auto publication = std::make_shared<TrackPublication>(
            track, response.track().sid(), response.track().name());
        {
            std::lock_guard lock(room_mutex_);
            if (!local || local != local_participant_) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "publish_commit",
                                     "local participant changed before publication commit");
            }
            local->add_publication(publication);
        }
        co_return publication;
    } catch (...) {
        {
            std::lock_guard lock(room_mutex_);
            pending_track_publishes_.erase(cid);
        }
        if (sender && WebRTCManager::Instance().signaling_thread()) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard lock(room_mutex_);
                pc = publisher_pc_;
            }
            WebRTCManager::Instance().signaling_thread()->BlockingCall([pc, sender]() {
                if (pc) pc->RemoveTrackOrError(sender);
            });
        }
        if (server_acknowledged) {
            Log("ERROR", "PUBLISH_ROLLBACK",
                "TrackPublished ACK 后本地事务失败；已移除 sender 并发起 Publisher SDP 重协商以收敛服务端轨状态");
            NegotiatePublisher();
        }
        throw;
    }
}

asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> Room::PublishLocalTracksBatchAsync(
    std::vector<LocalParticipant::BatchTrackItem> items) {
    if (items.empty()) {
        co_return std::vector<std::shared_ptr<TrackPublication>>{};
    }

    std::vector<proto::SignalRequest> effective_requests;
    effective_requests.reserve(items.size());
    std::vector<std::string> enabled_codecs;
    {
        std::lock_guard lock(room_mutex_);
        enabled_codecs = enabled_publish_codecs_;
    }

    for (auto& item : items) {
        if (!item.track || !item.request || !item.request->has_add_track()) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "invalid AddTrack request in batch item");
        }
        proto::SignalRequest eff_req(*item.request);
        if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(item.track)) {
            if (!enabled_codecs.empty()) {
                auto options = video->publish_options();
                std::string requested = options.video_codec;
                std::transform(requested.begin(), requested.end(), requested.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                const auto is_requested = [&requested](const std::string& mime) {
                    return mime == requested || mime == "video/" + requested;
                };
                if (std::none_of(enabled_codecs.begin(), enabled_codecs.end(), is_requested)) {
                    const auto fallback = std::find_if(
                        enabled_codecs.begin(), enabled_codecs.end(),
                        [](const std::string& mime) { return mime.rfind("video/", 0) == 0; });
                    if (fallback == enabled_codecs.end()) {
                        throw OperationError(OperationKind::PublishTrack,
                                             OperationErrorCode::PermissionDenied,
                                             "join_publish_codecs",
                                             "server did not enable a video publish codec");
                    }
                    options.video_codec = fallback->substr(6);
                    video->set_publish_options(options);
                    for (auto& codec : *eff_req.mutable_add_track()->mutable_simulcast_codecs()) {
                        codec.set_codec(options.video_codec);
                    }
                    Log("SIGNAL", "PUBLISH_CODEC_FALLBACK",
                        "服务端未启用请求的编码 " + requested + "，改用 " + options.video_codec);
                }
            }
        }
        effective_requests.push_back(std::move(eff_req));
    }

    const auto generation = session_generation_.load(std::memory_order_acquire);
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<LocalParticipant> local;
    struct PendingAckInfo {
        std::string cid;
        std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>> ack;
    };
    std::vector<PendingAckInfo> pending_acks;
    pending_acks.reserve(items.size());

    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "room is not connected");
        }
        for (const auto& eff_req : effective_requests) {
            const std::string cid = eff_req.add_track().cid();
            if (pending_track_publishes_.contains(cid)) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::InvalidState,
                                     "publish_validate",
                                     "a publish operation for CID " + cid + " is already in progress");
            }
            auto ack = std::make_shared<AwaitableState<proto::TrackPublishedResponse>>(executor_);
            pending_track_publishes_[cid] = ack;
            pending_acks.push_back({cid, ack});
        }
        signal = signal_client_;
        local = local_participant_;
    }

    std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
    std::vector<proto::TrackPublishedResponse> responses;
    responses.reserve(items.size());
    bool server_acknowledged_any = false;

    try {
        for (const auto& eff_req : effective_requests) {
            co_await signal->SendAsync(eff_req);
        }

        for (const auto& pa : pending_acks) {
            auto response = co_await WaitAwaitable<proto::TrackPublishedResponse>(
                pa.ack,
                operation_timeouts_.publish,
                OperationKind::PublishTrack,
                OperationErrorCode::TrackPublishTimeout,
                "wait_track_published_ack");
            server_acknowledged_any = true;
            responses.push_back(std::move(response));
        }

        {
            std::lock_guard lock(room_mutex_);
            for (const auto& pa : pending_acks) {
                pending_track_publishes_.erase(pa.cid);
            }
        }

        for (const auto& item : items) {
            auto sender = co_await AddTrackToPublisherAsync(item.track, generation);
            senders.push_back(sender);
        }

        // 单次全量 SDP 重协商
        co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);

        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "publish_commit",
                                 "session changed before batch publication commit");
        }

        std::vector<std::shared_ptr<TrackPublication>> publications;
        publications.reserve(items.size());
        {
            std::lock_guard lock(room_mutex_);
            if (!local || local != local_participant_) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "publish_commit",
                                     "local participant changed before batch publication commit");
            }
            for (size_t i = 0; i < items.size(); ++i) {
                items[i].track->set_sid(responses[i].track().sid());
                auto pub = std::make_shared<TrackPublication>(
                    items[i].track, responses[i].track().sid(), responses[i].track().name());
                local->add_publication(pub);
                publications.push_back(pub);
            }
        }

        Log("TRACK", "BATCH_PUBLISHED",
            "本地音视频批量发布成功: 共 " + std::to_string(publications.size()) + " 条轨道，仅执行单次 SDP 协商");
        co_return publications;
    } catch (...) {
        {
            std::lock_guard lock(room_mutex_);
            for (const auto& pa : pending_acks) {
                pending_track_publishes_.erase(pa.cid);
            }
        }
        if (!senders.empty() && WebRTCManager::Instance().signaling_thread()) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard lock(room_mutex_);
                pc = publisher_pc_;
            }
            WebRTCManager::Instance().signaling_thread()->BlockingCall([pc, senders]() {
                if (pc) {
                    for (const auto& sender : senders) {
                        if (sender) pc->RemoveTrackOrError(sender);
                    }
                }
            });
        }
        if (server_acknowledged_any) {
            Log("ERROR", "PUBLISH_BATCH_ROLLBACK",
                "批量发布事务异常；已回滚本地 senders 并发起 Publisher SDP 重协商以收敛服务端轨状态");
            NegotiatePublisher();
        }
        throw;
    }
}

asio::awaitable<void> Room::RemoveLocalTrackFromPublisherAsync(
    std::shared_ptr<Track> track,
    uint64_t generation) {
    if (!track || !track->rtc_track()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "remove_sender_validate",
                             "local track has no native WebRTC track");
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connected) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "remove_sender_validate",
                                 "room session changed while unpublishing");
        }
        pc = publisher_pc_;
    }
    if (!pc || !WebRTCManager::Instance().signaling_thread()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "remove_sender_validate",
                             "publisher peer connection is unavailable");
    }

    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    struct RemoveTrackTaskParams {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track;
        uint64_t generation = 0;
    };
    auto* params = new RemoveTrackTaskParams{
        shared_from_this(), completion, pc, track->rtc_track(), generation};
    WebRTCManager::Instance().signaling_thread()->PostTask([params]() {
        std::unique_ptr<RemoveTrackTaskParams> owned(params);
        auto& task = *owned;
        if (task.generation != task.room->session_generation_.load(std::memory_order_acquire)) {
            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                OperationKind::UnpublishTrack,
                OperationErrorCode::Cancelled,
                "remove_sender",
                "session changed before sender removal")));
            return;
        }

        std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
        for (const auto& sender : task.pc->GetSenders()) {
            if (!sender || !sender->track()) continue;
            if (sender->track() == task.rtc_track ||
                sender->track()->id() == task.rtc_track->id()) {
                senders.push_back(sender);
            }
        }
        if (senders.empty()) {
            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                OperationKind::UnpublishTrack,
                OperationErrorCode::InvalidState,
                "remove_sender",
                "no publisher sender owns the requested local track")));
            return;
        }

        for (const auto& sender : senders) {
            const auto result = task.pc->RemoveTrackOrError(sender);
            if (!result.ok()) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::UnpublishTrack,
                    OperationErrorCode::StateUncertain,
                    "remove_sender",
                    result.message(),
                    true)));
                return;
            }
        }
        CompleteAwaitable(task.completion);
    });

    co_await WaitAwaitable<void>(
        completion,
        operation_timeouts_.publish,
        OperationKind::UnpublishTrack,
        OperationErrorCode::TrackUnpublishTimeout,
        "remove_sender");
}

void Room::BindLocalUnpublishHandler() {
    auto self = shared_from_this();
    local_participant_->SetAsyncUnpublishTrackHandler(
        [self](std::string track_sid)
            -> asio::awaitable<std::shared_ptr<TrackPublication>> {
            co_return co_await self->UnpublishLocalTrackAsync(std::move(track_sid));
        });
}

asio::awaitable<std::shared_ptr<TrackPublication>> Room::UnpublishLocalTrackAsync(
    std::string track_sid) {
    if (track_sid.empty()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "unpublish_validate",
                             "track SID is empty");
    }

    const auto generation = session_generation_.load(std::memory_order_acquire);
    std::shared_ptr<LocalParticipant> local;
    std::shared_ptr<TrackPublication> publication;
    std::shared_ptr<Track> track;
    std::shared_ptr<LocalUnpublishTestHooks> test_hooks;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "room is not connected");
        }
        local = local_participant_;
        publication = local ? local->get_publication(track_sid) : nullptr;
        track = publication ? publication->track() : nullptr;
        if (!local || !publication || !track) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "track SID is not an active local publication");
        }
        if (pending_local_unpublishes_.contains(track_sid)) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "an unpublish operation for this track is already in progress");
        }
        pending_local_unpublishes_.emplace(track_sid,
                                            PendingLocalUnpublish{generation, publication, false});
        test_hooks = local_unpublish_test_hooks_;
    }

    bool sender_removed = false;
    try {
        if (test_hooks) {
            co_await test_hooks->remove_sender(track, generation);
        } else {
            co_await RemoveLocalTrackFromPublisherAsync(track, generation);
        }
        sender_removed = true;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (it == pending_local_unpublishes_.end() ||
                it->second.generation != generation ||
                generation != session_generation_.load(std::memory_order_acquire)) {
                throw OperationError(OperationKind::UnpublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "unpublish_negotiate",
                                     "session changed after sender removal");
            }
            it->second.sender_removed = true;
        }

        if (test_hooks) {
            co_await test_hooks->negotiate(operation_timeouts_.negotiation, generation);
        } else {
            co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);
        }

        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (generation != session_generation_.load(std::memory_order_acquire) ||
                connection_state_ != ConnectionState::Connected ||
                !local || local != local_participant_ ||
                it == pending_local_unpublishes_.end() ||
                it->second.publication != publication) {
                throw OperationError(OperationKind::UnpublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "unpublish_commit",
                                     "session changed before local unpublish commit");
            }

            local->remove_publication(track_sid);
            published_track_records_.erase(
                std::remove_if(published_track_records_.begin(),
                               published_track_records_.end(),
                               [&track_sid, &track](const PublishedTrackRecord& record) {
                                   return record.previous_sid == track_sid || record.track == track;
                               }),
                published_track_records_.end());
            pending_local_unpublishes_.erase(it);
            listeners_snapshot = listeners_;
        }

        for (const auto& listener : listeners_snapshot) {
            listener->OnLocalTrackUnpublished(publication);
        }
        publication->set_track(nullptr);
        track->set_sid("");
        Log("TRACK", "LOCAL_UNPUBLISHED",
            "本地 Track 已由 Publisher SDP Answer 确认取消发布: " + track_sid);
        co_return publication;
    } catch (const std::exception& error) {
        bool state_uncertain = false;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (it != pending_local_unpublishes_.end()) {
                state_uncertain = sender_removed || it->second.sender_removed;
                if (!state_uncertain) {
                    pending_local_unpublishes_.erase(it);
                }
            }
        }
        if (state_uncertain) {
            Log("ERROR", "UNPUBLISH_STATE_UNCERTAIN",
                "本地 sender 已移除但未收到 SDP Answer；将通过后续恢复按服务端状态收敛；" +
                    secure_log::ExceptionSummary("unpublish_negotiate"));
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::StateUncertain,
                                 "unpublish_negotiate",
                                 "publisher sender was removed before unpublish negotiation completed",
                                 true);
        }
        throw;
    }
}

void Room::OnNegotiationFailed() {
    CompleteNegotiation("publisher negotiation failed");
}

asio::awaitable<void> Room::NegotiatePublisherAsync(
    std::chrono::milliseconds timeout,
    uint64_t generation,
    bool ice_restart) {
    std::shared_ptr<AwaitableState<void>> completion;
    bool should_start = false;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::Negotiate,
                                 OperationErrorCode::Cancelled,
                                 "negotiate_start",
                                 "session changed before negotiation started");
        }
        if (!publisher_pc_ || !signal_client_) {
            throw OperationError(OperationKind::Negotiate,
                                 OperationErrorCode::InvalidState,
                                 "negotiate_start",
                                 "publisher peer connection or signal client is missing");
        }
        completion = std::make_shared<AwaitableState<void>>(executor_);
        negotiation_waiters_.push_back(completion);
        negotiation_ice_restart_requested_ = negotiation_ice_restart_requested_ || ice_restart;
        if (negotiation_state_ == NegotiationState::Idle) {
            negotiation_state_ = NegotiationState::InProgress;
            should_start = true;
        } else {
            // Keep every waiter pending until the final coalesced round reaches stable.
            negotiation_state_ = NegotiationState::PendingRetry;
        }
    }
    if (should_start) ExecuteNegotiatePublisher();

    try {
        co_await WaitAwaitable<void>(completion, timeout,
                                     OperationKind::Negotiate,
                                     OperationErrorCode::NegotiationFailed,
                                     "publisher_negotiation");
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        negotiation_waiters_.erase(
            std::remove(negotiation_waiters_.begin(), negotiation_waiters_.end(), completion),
            negotiation_waiters_.end());
        throw;
    }
}

void Room::CompleteNegotiation(const std::string& error) {
    std::vector<std::shared_ptr<AwaitableState<void>>> waiters;
    {
        std::lock_guard lock(room_mutex_);
        waiters.swap(negotiation_waiters_);
        negotiation_state_ = NegotiationState::Idle;
        subscriber_negotiating_ = false;
    }
    for (const auto& completion : waiters) {
        if (error.empty()) {
            CompleteAwaitable(completion);
        } else {
            FailAwaitable(completion, std::make_exception_ptr(OperationError(
                OperationKind::Negotiate,
                OperationErrorCode::NegotiationFailed,
                "publisher_negotiation",
                error,
                true)));
        }
    }
}

void Room::ExecuteNegotiatePublisher() {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::shared_ptr<SignalClient> client;
    bool ice_restart = false;
    bool unavailable = false;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        client = signal_client_;
        ice_restart = negotiation_ice_restart_requested_;
        negotiation_ice_restart_requested_ = false;
        if (!pub_pc || !client) {
            unavailable = true;
        } else if (pub_pc->signaling_state() != webrtc::PeerConnectionInterface::SignalingState::kStable) {
            negotiation_state_ = NegotiationState::PendingRetry;
            auto self = shared_from_this();
            auto retry_timer = std::make_shared<asio::steady_timer>(executor_, std::chrono::milliseconds(50));
            retry_timer->async_wait([self, retry_timer](const asio::error_code& ec) {
                if (!ec) {
                    bool should_run = false;
                    {
                        std::lock_guard lock(self->room_mutex_);
                        if (self->negotiation_state_ == NegotiationState::PendingRetry) {
                            self->negotiation_state_ = NegotiationState::InProgress;
                            should_run = true;
                        }
                    }
                    if (should_run) {
                        self->ExecuteNegotiatePublisher();
                    }
                }
            });
            return;
        }
    }

    if (unavailable) {
        CompleteNegotiation("publisher peer connection or signal client became unavailable");
        return;
    }

    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(pub_pc, executor_,
        [self, client, pub_pc](const std::string& sdp, const std::string& err) {
            if (!err.empty()) {
                std::cerr << "[WebRTC] CreateOffer error: "
                          << secure_log::OpaqueSummary("create_offer") << std::endl;
                self->Log("ERROR", "OFFER_FAIL", secure_log::OpaqueSummary("create_offer"));
                self->OnNegotiationFailed();
                return;
            }

            WebRTCManager::Instance().SetLocalDescription(pub_pc, "offer", sdp, self->executor_,
                [self, client, pub_pc, sdp](const std::string& set_local_err) {
                    if (!set_local_err.empty()) {
                        std::cerr << "[WebRTC] SetLocalDescription offer error: "
                                  << secure_log::OpaqueSummary("set_local_offer") << std::endl;
                        self->Log("ERROR", "LOCAL_DESC_FAIL",
                                  secure_log::OpaqueSummary("set_local_offer"));
                        self->OnNegotiationFailed();
                        return;
                    }

                    proto::SignalRequest req;
                    auto* offer_msg = req.mutable_offer();
                    offer_msg->set_type("offer");
                    offer_msg->set_sdp(sdp);

                    // 1. 从 WebRTC Transceivers 读取 MID -> Track ID 映射
                    for (const auto& transceiver : pub_pc->GetTransceivers()) {
                        if (transceiver && transceiver->mid().has_value() && transceiver->sender() && transceiver->sender()->track()) {
                            std::string mid = *transceiver->mid();
                            std::string track_id = transceiver->sender()->track()->id();
                            (*offer_msg->mutable_mid_to_track_id())[mid] = track_id;
                            std::cout << "[WebRTC] Transceiver MID '" << mid << "' mapped to Track '" << track_id << "'" << std::endl;
                        }
                    }

                    // 2. 从 SDP 解析 a=mid: 和 a=msid: 双重绑定映射
                    std::istringstream sdp_stream(sdp);
                    std::string line;
                    std::string cur_mid = "";
                    while (std::getline(sdp_stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.rfind("a=mid:", 0) == 0) {
                            cur_mid = line.substr(6);
                        } else if (line.rfind("a=msid:", 0) == 0 && !cur_mid.empty()) {
                            std::string msid_content = line.substr(7);
                            std::istringstream msid_ss(msid_content);
                            std::string stream_id, track_id;
                            if (msid_ss >> stream_id >> track_id) {
                                (*offer_msg->mutable_mid_to_track_id())[cur_mid] = track_id;
                            }
                        }
                    }

                    livekit::safe_co_spawn(self->executor_, [self, client, req = std::move(req)]() -> asio::awaitable<void> {
                        try {
                            co_await client->SendAsync(req);
                        } catch (const std::exception& error) {
                            self->CompleteNegotiation(error.what());
                        }
                    });
                    std::cout << "[WebRTC] -> Sent publisher SDP Offer to LiveKit server with mid_to_track_id mapping!" << std::endl;
                    self->Log("SIGNAL", "SDP_OFFER_SENT",
                              secure_log::SdpSummary("publisher_offer_sent", sdp));
                });
        }, ice_restart);
}

void Room::SendPublishOffer() {
    NegotiatePublisher();
}

std::pair<std::string, std::string> Room::UnpackStreamId(const std::string& packed) {
    auto sep_pos = packed.find('|');
    if (sep_pos != std::string::npos) {
        return {packed.substr(0, sep_pos), packed.substr(sep_pos + 1)};
    }
    if (packed.rfind("PA_", 0) == 0) {
        return {packed, ""};
    }
    return {"", ""};
}

void Room::RemoveExpiredPendingTracks() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = pending_track_queue_.begin(); it != pending_track_queue_.end();) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [now](const PendingTrack& pt) {
            return now > pt.expires_at;
        }), list.end());
        if (list.empty()) {
            it = pending_track_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

void Room::AttachRemoteTrackToParticipant(
    std::shared_ptr<RemoteParticipant> participant,
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::string& track_sid) {
    if (!participant || !track) return;

    TrackKind kind = (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) ? TrackKind::Audio : TrackKind::Video;

    // 优先使用传入的有效 track_sid (以 TR_ 开头)，若不是以 TR_ 开头，尝试从 participant 已有的 publications 中查找对应类型的有效 SID
    std::string track_id = track_sid;
    if (track_id.rfind("TR_", 0) != 0) {
        for (const auto& [sid, p] : participant->tracks()) {
            if (p && p->track() && p->track()->kind() == kind && sid.rfind("TR_", 0) == 0) {
                track_id = sid;
                break;
            }
        }
    }
    if (track_id.empty()) {
        track_id = track->id();
    }

    std::shared_ptr<TrackPublication> pub;
    std::shared_ptr<Track> r_track;
    std::shared_ptr<RemoteTrackPublication> remote_publication;
    uint64_t replaced_binding_serial = 0;
    ParticipantKey participant_key;
    TrackKey track_key;
    uint64_t binding_serial = 0;

    {
        std::lock_guard lock(room_mutex_);
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        if (connection_state_ == ConnectionState::Disconnected ||
            canonical == remote_participants_.end() ||
            canonical->second.get() != participant.get() ||
            !membership ||
            !membership->active.load(std::memory_order_acquire)) {
            return;
        }
        // 查找已有 publication
        pub = participant->get_publication(track_id);
        if (!pub) {
            // 尝试按类型查找未绑定 rtc_track 的已有 publication
            for (const auto& [sid, p] : participant->tracks()) {
                if (p && p->track() && p->track()->kind() == kind && !p->track()->rtc_track()) {
                    pub = p;
                    track_id = sid;
                    break;
                }
            }
        }

        if (!pub) {
            r_track = std::make_shared<Track>(track_id, (kind == TrackKind::Video ? "camera_video" : "microphone_audio"), kind);
            pub = CreateRemoteTrackPublication(
                r_track,
                participant->sid(),
                track_id,
                r_track->name(),
                kind == TrackKind::Audio ? proto::TrackType::AUDIO : proto::TrackType::VIDEO);
            participant->add_publication(pub);
        } else {
            r_track = pub->track();
            if (!r_track) {
                r_track = std::make_shared<Track>(track_id, pub->name().empty() ? (kind == TrackKind::Video ? "camera_video" : "microphone_audio") : pub->name(), kind);
                pub->set_track(r_track);
            }
        }
        remote_publication = std::dynamic_pointer_cast<RemoteTrackPublication>(pub);
        const auto track_membership = EnsureTrackMembershipLocked(participant, pub);
        if (!track_membership) return;
        participant_key = membership->key;
        track_key = track_membership->key;
        binding_serial = next_remote_track_binding_serial_++;
        if (remote_publication && remote_publication->has_media_binding() &&
            remote_publication->media_track_id() != track->id()) {
            const auto existing_serial =
                current_remote_binding_serials_.find(remote_publication.get());
            if (existing_serial != current_remote_binding_serials_.end()) {
                replaced_binding_serial = existing_serial->second;
            }
        }
    }

    // A replaced WebRTC receiver must detach its previous raw sink before the
    // canonical Track points at the new receiver. This keeps one sink binding
    // per remote publication/SID rather than accumulating stale decoders.
    if (replaced_binding_serial != 0 && remote_publication) {
        DetachRemotePublicationMedia(
            remote_publication.get(), /*notify_listener=*/false, replaced_binding_serial);
    }

    uint64_t superseded_binding_serial = 0;
    const auto retain_binding = [&](RemoteTrackSinkBinding& binding) {
        std::lock_guard lock(room_mutex_);
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        const auto track_membership = track_memberships_.find(pub.get());
        if (connection_state_ == ConnectionState::Disconnected ||
            canonical == remote_participants_.end() ||
            canonical->second.get() != participant.get() ||
            participant->get_publication(track_id) != pub ||
            !membership || membership->key != participant_key ||
            track_membership == track_memberships_.end() ||
            track_membership->second->key != track_key ||
            !track_membership->second->active.load(std::memory_order_acquire)) {
            return false;
        }

        r_track->set_rtc_track(track);
        if (kind == TrackKind::Audio && audio_output_muted_) {
            r_track->set_muted(true);
        }
        {
            std::lock_guard media_lock(remote_media_mutex_);
            auto& media_tracks = kind == TrackKind::Video
                ? remote_video_tracks_
                : remote_audio_tracks_;
            const auto existing = std::find_if(
                media_tracks.begin(), media_tracks.end(), [&r_track](const std::weak_ptr<Track>& weak) {
                    return weak.lock() == r_track;
                });
            if (existing == media_tracks.end()) media_tracks.push_back(r_track);
        }
        processed_remote_track_ids_.insert(track->id());
        remote_track_sinks_.push_back(std::move(binding));
        if (remote_publication) {
            const auto existing_serial =
                current_remote_binding_serials_.find(remote_publication.get());
            if (existing_serial != current_remote_binding_serials_.end() &&
                existing_serial->second != binding_serial) {
                superseded_binding_serial = existing_serial->second;
            }
            current_remote_binding_serials_[remote_publication.get()] = binding_serial;
            std::weak_ptr<Room> weak_room = weak_from_this();
            remote_publication->SetMediaBinding(
                track->id(),
                [weak_room, binding_serial](RemoteTrackPublication* publication,
                                            bool notify_listener) {
                    if (const auto room = weak_room.lock()) {
                        room->DetachRemotePublicationMedia(
                            publication, notify_listener, binding_serial);
                    }
                });
        }
        return true;
    };

    if (kind == TrackKind::Audio) {
        auto audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
        if (audio_track) {
            audio_track->set_enabled(!r_track->muted());
        }
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto last_voice_log_time = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        std::weak_ptr<Room> weak_room = weak_from_this();
        auto sink = std::make_shared<NativeAudioTrackSink>([r_track, has_logged, last_voice_log_time, weak_room, participant](const AudioFrame& frame) {
            if (r_track) {
                r_track->notifyAudioFrame(frame);
            }
            if (!has_logged->exchange(true)) {
                std::cout << "[RECV AUDIO] Started receiving audio PCM stream for track " << r_track->sid() << ", sample_rate=" << frame.sampleRate() << "Hz, channels=" << frame.numChannels() << std::endl;
                if (auto room = weak_room.lock()) {
                    room->Log("WEBRTC", "AUDIO_FRAME", "收到远端音频首帧数据 (采样率: " + std::to_string(frame.sampleRate()) + "Hz, 声道: " + std::to_string(frame.numChannels()) + ", 帧采样数: " + std::to_string(frame.totalSamples()) + ")");
                }
            }
            // 计算音频能量 RMS 并进行周期性语音诊断
            const auto& samples = frame.data();
            if (!samples.empty()) {
                double sum = 0.0;
                for (size_t i = 0; i < samples.size(); ++i) {
                    sum += static_cast<double>(samples[i]) * samples[i];
                }
                double rms = std::sqrt(sum / static_cast<double>(samples.size()));
                if (rms > 250.0) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_voice_log_time).count() > 2500) {
                        *last_voice_log_time = now;
                        if (auto room = weak_room.lock()) {
                            room->Log("MEDIA", "AUDIO_VOICE", "参会人 [" + participant->identity() + "] 正在讲话 (PCM RMS 能量=" + std::to_string(static_cast<int>(rms)) + ", 48kHz 混音播放中)");
                        }
                    }
                }
            }
        });
        audio_track->AddSink(sink.get());
        RemoteTrackSinkBinding binding{
            track_key,
            binding_serial,
            track->id(),
            RemoteTrackSinkThread::Signaling,
            [track, sink]() {
                auto* attached_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
                if (attached_track) attached_track->RemoveSink(sink.get());
            }};
        const bool retained = retain_binding(binding);
        if (!retained) {
            if (binding.detach) binding.detach();
            return;
        }
        if (superseded_binding_serial != 0) {
            DetachRemoteTrackSinks(
                TakeRemoteTrackSinkForBindingSerial(superseded_binding_serial));
        }
        Log("WEBRTC", "AUDIO_ATTACH", "远端音频轨已绑定至参会人 [" + participant->identity() + "], Track SID=" + track_id + ", 已挂载 NativeAudioTrackSink");

        if (signal_client_) {
            signal_client_->SendUpdateSubscription({track_id}, true, participant->sid());
            Log("SIGNAL", "AUDIO_TRACK_ACTIVE", "已向 SFU 激活下行音频流订阅: Track SID=" + track_id + " (Participant: " + participant->identity() + ")");
        }
    } else {
        auto video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        std::weak_ptr<Room> weak_room = weak_from_this();
        auto sink = std::make_shared<NativeVideoTrackSink>([r_track, has_logged, weak_room](render::OwnedI420Frame::Ptr frame) {
            if (r_track) {
                r_track->notifyI420VideoFrame(frame);
            }
            if (!has_logged->exchange(true)) {
                std::cout << "[RECV VIDEO] Receiving video stream for track " << r_track->sid() << ", resolution=" << frame->width() << "x" << frame->height() << std::endl;
                if (auto room = weak_room.lock()) {
                    room->Log("WEBRTC", "VIDEO_FRAME", "WebRTC 解码器开始输出远端视频流 (" + std::to_string(frame->width()) + "x" + std::to_string(frame->height()) + ")");
                }
                Telemetry::Instance().OnFirstRemoteFrameReceived("video");
            }
        });
        video_track->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
        RemoteTrackSinkBinding binding{
            track_key,
            binding_serial,
            track->id(),
            RemoteTrackSinkThread::MediaWorker,
            [track, sink]() {
                auto* attached_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
                if (attached_track) attached_track->RemoveSink(sink.get());
            }};
        const bool retained = retain_binding(binding);
        if (!retained) {
            if (binding.detach) binding.detach();
            return;
        }
        if (superseded_binding_serial != 0) {
            DetachRemoteTrackSinks(
                TakeRemoteTrackSinkForBindingSerial(superseded_binding_serial));
        }
        Log("WEBRTC", "VIDEO_ATTACH", "远端视频轨已绑定至参会人 [" + participant->identity() + "], Track SID=" + track_id);

        if (signal_client_) {
            signal_client_->SendUpdateTrackSettings(track_id, false, proto::VideoQuality::HIGH, 1280, 720, 30, 0);
            signal_client_->SendUpdateSubscription({track_id}, true, participant->sid());
            Log("SIGNAL", "TRACK_ACTIVE", "已向 SFU 激活下行视频流: Track SID=" + track_id + " (Participant: " + participant->identity() + ")");
        }
    }

    // 触发 RoomListener 回调 (OnTrackPublished 已在 UpdateParticipants 中派发，此处仅派发 OnTrackSubscribed)
    std::vector<std::shared_ptr<RoomListener>> listeners;
    bool still_current = false;
    {
        std::lock_guard lock(room_mutex_);
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        const auto track_membership = track_memberships_.find(pub.get());
        still_current = connection_state_ != ConnectionState::Disconnected &&
            canonical != remote_participants_.end() &&
            canonical->second.get() == participant.get() &&
            participant->get_publication(track_id) == pub &&
            membership && membership->key == participant_key &&
            track_membership != track_memberships_.end() &&
            track_membership->second->key == track_key &&
            track_membership->second->active.load(std::memory_order_acquire) &&
            (!remote_publication ||
             (current_remote_binding_serials_.contains(remote_publication.get()) &&
              current_remote_binding_serials_.at(remote_publication.get()) == binding_serial));
        if (still_current) {
            EnqueueParticipantEventLocked(MakeTrackEventLocked(
                ParticipantEventKind::TrackAvailable,
                participant,
                pub,
                false));
            listeners = listeners_;
        }
    }
    if (!still_current) {
        if (remote_publication) {
            DetachRemotePublicationMedia(
                remote_publication.get(), /*notify_listener=*/false, binding_serial);
        } else {
            DetachRemoteTrackSinks(TakeRemoteTrackSinkForBindingSerial(binding_serial));
        }
        return;
    }
    for (const auto& l : listeners) {
        if (!l->ConsumesParticipantEvents()) {
            l->OnTrackSubscribed(r_track, pub, participant);
        }
    }
}

void Room::FlushPendingTracks(const std::string& participant_sid) {
    std::vector<PendingTrack> pending_to_flush;
    std::shared_ptr<RemoteParticipant> participant;
    {
        std::lock_guard lock(room_mutex_);
        RemoveExpiredPendingTracks();
        auto pit = remote_participants_.find(participant_sid);
        if (pit == remote_participants_.end()) return;
        participant = pit->second;

        // 仅精确匹配该参会人的暂存队列
        auto qit = pending_track_queue_.find(participant_sid);
        if (qit != pending_track_queue_.end()) {
            pending_to_flush = std::move(qit->second);
            pending_track_queue_.erase(qit);
        }
    }

    if (!pending_to_flush.empty()) {
        Log("TRACK", "FLUSH_PENDING", "开始冲刷参会人 [" + participant->identity() + "] 的 " + std::to_string(pending_to_flush.size()) + " 条暂存媒体轨");
        for (const auto& item : pending_to_flush) {
            AttachRemoteTrackToParticipant(participant, item.track, item.receiver, item.track_sid);
        }
    }
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    if (!track) return;
    
    std::string track_id = track->id();
    {
        std::lock_guard lock(room_mutex_);
        if (processed_remote_track_ids_.find(track_id) != processed_remote_track_ids_.end()) {
            return; // 已经成功挂载并处理过这个 track
        }
    }

    // 从 receiver 的 stream_ids 解包 (msid: <participantSid>|<trackSid>)
    std::string stream_id;
    if (receiver) {
        auto streams = receiver->stream_ids();
        if (!streams.empty()) {
            stream_id = streams[0];
        }
    }

    auto [participant_sid, track_sid] = UnpackStreamId(stream_id);
    
    // 如果解出来的 participant_sid 为空 (说明是本地预分配的无主临时虚拟轨，尚未绑定远端 SSRC/MSID)，直接忽略
    if (participant_sid.empty()) {
        Log("WEBRTC", "ON_TRACK_IGNORE", "忽略未绑定远端 SSRC/MSID 的本地虚拟 Track: TrackID=" + track_id + ", StreamID=" + stream_id + ", Kind=" + std::string(track->kind()));
        return;
    }

    if (track_sid.empty()) {
        track_sid = track_id;
    }

    Log("WEBRTC", "ON_TRACK_RESOLVE", "下行 Track 解析成功: StreamID=" + stream_id + ", ParticipantSID=" + participant_sid + ", TrackSID=" + track_sid + ", Kind=" + std::string(track->kind()));

    std::shared_ptr<RemoteParticipant> participant;
    {
        std::lock_guard lock(room_mutex_);
        auto it = remote_participants_.find(participant_sid);
        if (it != remote_participants_.end()) {
            participant = it->second;
        }

        if (!participant) {
            // 参会人尚未建立（信令在路上）：放入以 participant_sid 为精确 Key 的 PendingTrackQueue 暂存！
            RemoveExpiredPendingTracks();
            PendingTrack pt;
            pt.track = track;
            pt.receiver = receiver;
            pt.participant_sid = participant_sid;
            pt.track_sid = track_sid;
            pt.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            pending_track_queue_[participant_sid].push_back(pt);
            Log("TRACK", "ENQUEUE_PENDING", "参会人 [" + participant_sid + "] 尚未就绪，真实媒体轨已暂存至 PendingTrackQueue (Track SID=" + track_sid + ")");
            return;
        }
    }

    // 参会人已就绪，立即绑定挂载
    AttachRemoteTrackToParticipant(participant, track, receiver, track_sid);
}

void Room::OnRenegotiationNeeded(int pc_type) {
    if (pc_type != 0) return; // 只有 Publisher PC 需要由 Client 发送 Offer
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        if (negotiation_state_ != NegotiationState::Idle) {
            return; // 正在协商中，不产生多余的重协商风暴
        }
    }
    if (pub_pc && pub_pc->signaling_state() == webrtc::PeerConnectionInterface::SignalingState::kStable) {
        NegotiatePublisher();
    }
}

void Room::HandleSignalEvent(const SignalEvent& event, uint64_t event_generation) {
    // SignalClient callbacks can be queued after a disconnect or after the
    // full-restart path has already installed a replacement client. The
    // callback is bound to the ConnectAsync generation that created it, so an
    // old transport must never mutate a newer room session.
    if (event_generation != 0 &&
        event_generation != session_generation_.load(std::memory_order_acquire)) {
        Log("SIGNAL", "STALE_EVENT", "忽略已替换会话的信令事件");
        return;
    }

    if (event.type == SignalEvent::Close) {
        bool should_reconnect = false;
        bool connect_failed = false;
        bool server_disconnect_finalizing = false;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard lock(room_mutex_);
            server_disconnect_finalizing = server_disconnect_finalizing_;
            if (server_disconnect_finalizing) {
                // LeaveRequest cleanup owns the final notification. A following
                // websocket close is expected and must not start a reconnect.
            } else if (!reconnect_disabled_ &&
                       connection_state_ == ConnectionState::Connected &&
                       reconnect_attempts_ < kMaxReconnectAttempts) {
                connection_state_ = ConnectionState::Reconnecting;
                reconnect_attempts_++;
                should_reconnect = true;
            } else if (connection_state_ == ConnectionState::Connecting) {
                connection_state_ = ConnectionState::Disconnected;
                session_generation_.fetch_add(1, std::memory_order_acq_rel);
                connect_failed = true;
            }
            listeners_snapshot = listeners_;
        }

        if (server_disconnect_finalizing) {
            return;
        } else if (connect_failed) {
            CancelPendingOperations(OperationErrorCode::SessionClosed,
                                    "signal_close_during_connect",
                                    event.close_reason.empty() ? "signal closed during connect" : event.close_reason);
        } else if (should_reconnect) {
            for (const auto& listener : listeners_snapshot) {
                listener->OnReconnecting();
            }
            livekit::safe_co_spawn(executor_, [self = shared_from_this()]() -> asio::awaitable<void> {
                co_await self->AttemptReconnect();
            });
        } else {
            bool notify_disconnect = false;
            {
                std::lock_guard lock(room_mutex_);
                if (connection_state_ != ConnectionState::Disconnected && connection_state_ != ConnectionState::Reconnecting) {
                    connection_state_ = ConnectionState::Disconnected;
                    notify_disconnect = true;
                }
            }
            if (notify_disconnect) {
                for (const auto& listener : listeners_snapshot) {
                    listener->OnDisconnected(RoomDisconnectReason::NetworkError,
                                             event.close_reason);
                }
            }
        }
    } else if (event.type == SignalEvent::Message) {
        HandleSignalMessage(event.message);
    }
}

void Room::HandleSignalMessage(std::shared_ptr<proto::SignalResponse> msg) {
    if (!msg) return;

    // Internal control messages must always reach their transaction waiters.
    // User-visible room updates stay buffered until Connect commits.
    const bool internal_control = msg->has_answer() || msg->has_offer() ||
        msg->has_trickle() || msg->has_track_published() ||
        msg->has_media_sections_requirement() || msg->has_reconnect() ||
        msg->has_leave() || msg->has_refresh_token() ||
        msg->has_pong() || msg->has_pong_resp();
    if (!internal_control) {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Connecting) {
            deferred_room_messages_.push_back(std::move(msg));
            return;
        }
    }
    // --- 全量原始消息类型诊断 ---
    {
        std::string type_tag = "UNKNOWN";
        if (msg->has_join())                         type_tag = "JOIN";
        else if (msg->has_answer())                  type_tag = "ANSWER";
        else if (msg->has_offer())                   type_tag = "OFFER";
        else if (msg->has_trickle())                 type_tag = "TRICKLE";
        else if (msg->has_update())                  type_tag = "PARTICIPANT_UPDATE";
        else if (msg->has_track_published())         type_tag = "TRACK_PUBLISHED";
        else if (msg->has_leave())                   type_tag = "LEAVE";
        else if (msg->has_mute())                    type_tag = "MUTE";
        else if (msg->has_speakers_changed())        type_tag = "SPEAKERS_CHANGED";
        else if (msg->has_room_update())             type_tag = "ROOM_UPDATE";
        else if (msg->has_connection_quality())      type_tag = "CONN_QUALITY";
        else if (msg->has_stream_state_update())     type_tag = "STREAM_STATE";
        else if (msg->has_subscribed_quality_update()) type_tag = "QUALITY_UPDATE";
        else if (msg->has_subscription_permission_update()) type_tag = "SUB_PERM_UPDATE";
        else if (msg->has_track_unpublished())       type_tag = "TRACK_UNPUBLISHED";
        else if (msg->has_reconnect())               type_tag = "RECONNECT";
        else if (msg->has_subscription_response())   type_tag = "SUB_RESP";
        else if (msg->has_request_response())        type_tag = "REQUEST_RESP";
        else if (msg->has_track_subscribed())        type_tag = "TRACK_SUBSCRIBED";
        else if (msg->has_media_sections_requirement()) type_tag = "MEDIA_SECTIONS_REQ";
        else if (msg->has_pong() || msg->has_pong_resp()) type_tag = "PONG";
        else if (msg->has_refresh_token())           type_tag = "REFRESH_TOKEN";
        else if (msg->has_room_moved())              type_tag = "ROOM_MOVED";

        if (type_tag == "UNKNOWN") {
            Log("SIGNAL", "RAW_MSG", "[Signal] 收到服务端未识别消息 (Case=" +
                std::to_string(msg->message_case()) + ", detail=[omitted])");
        } else if (type_tag != "PONG") {
            Log("SIGNAL", "RAW_MSG", "[Signal] 收到服务端消息: " + type_tag);
        }
    }

    if (msg->has_leave()) {
        const auto& leave = msg->leave();
        const auto reason = ToRoomDisconnectReason(leave.reason());
        std::string action_name = "UNKNOWN";
        switch (leave.action()) {
        case proto::LeaveRequest_Action_DISCONNECT: action_name = "DISCONNECT"; break;
        case proto::LeaveRequest_Action_RESUME: action_name = "RESUME"; break;
        case proto::LeaveRequest_Action_RECONNECT: action_name = "RECONNECT"; break;
        default: break;
        }
        const std::string detail = "LeaveRequest reason=" +
            std::string(ToString(reason)) + "(" +
            std::to_string(static_cast<int>(leave.reason())) + ")" +
            ", action=" + action_name + "(" +
            std::to_string(static_cast<int>(leave.action())) + ")";
        Log("SIGNAL", "LEAVE_RECEIVED", "[Room] Receive " + detail);

        // DUPLICATE_IDENTITY is an explicit, terminal server decision. Do not
        // wait for the websocket to close and accidentally enter the network
        // reconnect path. Other explicit DISCONNECT actions are terminal too;
        // legacy servers may set can_reconnect instead of action.
        const bool server_requests_disconnect =
            leave.action() == proto::LeaveRequest_Action_DISCONNECT &&
            !leave.can_reconnect();
        if (reason == RoomDisconnectReason::DuplicateIdentity ||
            server_requests_disconnect) {
            BeginServerDisconnect(reason, detail);
        }
        return;
    }

    if (msg->has_update()) {
        Log("SIGNAL", "PARTICIPANT_UPDATE", "收到服务端 ParticipantUpdate 信令 (参会人更新数量: " + std::to_string(msg->update().participants_size()) + ")");
        UpdateParticipants(msg->update().participants());
    } else if (msg->has_mute()) {
        Log("SIGNAL", "MUTE_UPDATE", "收到 Track Mute 更新: SID=" + msg->mute().sid());
        UpdateTrackMute(msg->mute());
    } else if (msg->has_speakers_changed()) {
        HandleActiveSpeakerUpdate(msg->speakers_changed());
    } else if (msg->has_offer()) {
        HandleOfferSignal(msg->offer());
    } else if (msg->has_answer()) {
        HandleAnswerSignal(msg->answer());
    } else if (msg->has_trickle()) {
        HandleTrickleSignal(msg->trickle());
    } else if (msg->has_track_published()) {
        const auto& tp = msg->track_published();
        Log("SIGNAL", "TRACK_PUB_ACK", "收到服务端 TrackPublished ACK: cid=" + tp.cid() + ", track_sid=" + tp.track().sid());
        std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>> pending;
        {
            std::lock_guard lock(room_mutex_);
            auto it = pending_track_publishes_.find(tp.cid());
            if (it != pending_track_publishes_.end()) pending = it->second;
        }
        if (pending) CompleteAwaitable(pending, tp);
    } else if (msg->has_subscription_response()) {
        const auto& sr = msg->subscription_response();
        std::string err_str;
        switch (sr.err()) {
            case proto::SE_TRACK_NOTFOUND: err_str = "SE_TRACK_NOTFOUND (2)"; break;
            case proto::SE_CODEC_UNSUPPORTED: err_str = "SE_CODEC_UNSUPPORTED (1)"; break;
            default: err_str = "SE_UNKNOWN (0) - 订阅成功或状态未知"; break;
        }
        Log("SIGNAL", "SUB_RESP", "收到服务端 SubscriptionResponse: Track=" + sr.track_sid() + ", Err=" + err_str);
    } else if (msg->has_subscription_permission_update()) {
        Log("SIGNAL", "SUB_PERM_UPDATE", "收到服务端 SubscriptionPermissionUpdate (Allowed: " + std::string(msg->subscription_permission_update().allowed() ? "YES" : "NO") + ")");
        UpdateTrackSubscriptionPermission(msg->subscription_permission_update());
    } else if (msg->has_stream_state_update()) {
        Log("SIGNAL", "STREAM_STATE", "收到服务端 StreamStateUpdate 状态更新");
        UpdateTrackStreamStates(msg->stream_state_update());
    } else if (msg->has_room_update()) {
        Log("SIGNAL", "ROOM_UPDATE", "收到服务端 RoomUpdate 房间信息变更");
        UpdateRoomInfo(msg->room_update().room());
    } else if (msg->has_connection_quality()) {
        Log("SIGNAL", "CONN_QUALITY", "收到服务端 ConnectionQualityUpdate 状态更新");
        UpdateConnectionQuality(msg->connection_quality());
    } else if (msg->has_subscribed_quality_update()) {
        const auto& squ = msg->subscribed_quality_update();
        std::string track_sid = squ.track_sid();
        Log("SIGNAL", "QUALITY_UPDATE", "收到 SFU Dynacast 质量调控需求: Track SID=" + track_sid);

        std::map<std::string, std::map<livekit::proto::VideoQuality, bool>> codec_quality_map;
        std::map<livekit::proto::VideoQuality, bool> fallback_quality_states;

        for (const auto& sc : squ.subscribed_codecs()) {
            std::string c_lower = sc.codec();
            std::transform(c_lower.begin(), c_lower.end(), c_lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            for (const auto& q : sc.qualities()) {
                codec_quality_map[c_lower][q.quality()] = q.enabled();
                fallback_quality_states[q.quality()] = q.enabled();
            }
        }
        for (const auto& q : squ.subscribed_qualities()) {
            fallback_quality_states[q.quality()] = q.enabled();
        }

        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
        {
            std::lock_guard lock(room_mutex_);
            pub_pc = publisher_pc_;
        }

        if (pub_pc) {
            auto senders = pub_pc->GetSenders();
            for (auto& sender : senders) {
                if (!sender || !sender->track() || sender->track()->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
                    continue;
                }

                webrtc::RtpParameters parameters = sender->GetParameters();
                if (parameters.encodings.empty()) continue;

                // Match specific codec qualities if available
                const std::map<livekit::proto::VideoQuality, bool>* target_qualities = &fallback_quality_states;
                if (!parameters.codecs.empty()) {
                    std::string s_codec = parameters.codecs[0].name;
                    std::transform(s_codec.begin(), s_codec.end(), s_codec.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    auto it = codec_quality_map.find(s_codec);
                    if (it != codec_quality_map.end()) {
                        target_qualities = &it->second;
                    }
                }

                bool params_changed = false;
                std::string active_summary;

                for (const auto& [quality, enabled] : *target_qualities) {
                    std::string target_rid;
                    if (quality == livekit::proto::VideoQuality::HIGH) target_rid = "f";
                    else if (quality == livekit::proto::VideoQuality::MEDIUM) target_rid = "h";
                    else if (quality == livekit::proto::VideoQuality::LOW) target_rid = "q";

                    for (auto& enc : parameters.encodings) {
                        if (enc.rid == target_rid || (parameters.encodings.size() == 1 && target_rid == "f")) {
                            if (enc.active != enabled) {
                                enc.active = enabled;
                                params_changed = true;
                            }
                        }
                    }
                }

                if (params_changed) {
                    for (const auto& enc : parameters.encodings) {
                        active_summary += (enc.rid.empty() ? "single" : enc.rid) + ":" + (enc.active ? "ON " : "OFF ");
                    }
                    sender->SetParameters(parameters);
                    Log("DYNACAST", "LAYER_UPDATE", "Dynacast 动态调节生效 [" + active_summary + "]");
                }
            }
        }
    } else if (msg->has_media_sections_requirement()) {
        const auto& msr = msg->media_sections_requirement();
        Log("SIGNAL", "MEDIA_SEC_REQ", "收到 MediaSectionsRequirement: audio=" + std::to_string(msr.num_audios()) + ", video=" + std::to_string(msr.num_videos()));
        HandleMediaSectionsRequirement(msr);
    }
}

void Room::UpdateParticipants(const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants) {
    std::vector<std::shared_ptr<RemoteParticipant>> newly_connected;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<TrackPublication>>> newly_published_tracks;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<TrackPublication>>> unpublished_tracks;
    std::vector<std::shared_ptr<RemoteParticipant>> disconnected;
    struct AttrChange {
        std::shared_ptr<Participant> participant;
        std::map<std::string, std::string> attrs;
    };
    std::vector<AttrChange> changed_attributes_events;

    struct PermChange {
        std::shared_ptr<Participant> participant;
        ParticipantPermission old_perm;
        ParticipantPermission new_perm;
    };
    std::vector<PermChange> changed_permissions_events;

    struct MetadataChange {
        std::shared_ptr<Participant> participant;
        std::string old_metadata;
        std::string new_metadata;
    };
    std::vector<MetadataChange> changed_metadata_events;

    struct MuteChange {
        std::shared_ptr<Participant> participant;
        std::shared_ptr<TrackPublication> publication;
        bool muted;
    };
    std::vector<MuteChange> changed_mute_events;

    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::vector<TrackKey> removed_track_keys;
    std::vector<std::shared_ptr<Track>> removed_tracks;

    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connecting) {
            listeners_snapshot = listeners_;
        }

        for (const auto& p_info : participants) {
            std::map<std::string, std::string> new_attrs(p_info.attributes().begin(), p_info.attributes().end());
            ParticipantPermission new_perm;
            if (p_info.has_permission()) {
                const auto& pb_perm = p_info.permission();
                new_perm.can_subscribe = pb_perm.can_subscribe();
                new_perm.can_publish = pb_perm.can_publish();
                new_perm.can_publish_data = pb_perm.can_publish_data();
                new_perm.can_update_metadata = pb_perm.can_update_metadata();
                new_perm.hidden = pb_perm.hidden();
            }

            if (local_participant_ && p_info.sid() == local_participant_->sid()) {
                if (!p_info.name().empty()) {
                    local_participant_->set_name(p_info.name());
                }
                std::string old_meta = local_participant_->metadata();
                if (old_meta != p_info.metadata()) {
                    local_participant_->set_metadata(p_info.metadata());
                    changed_metadata_events.push_back({local_participant_, old_meta, p_info.metadata()});
                }

                auto old_attrs = local_participant_->attributes();
                if (old_attrs != new_attrs) {
                    local_participant_->set_attributes(new_attrs);
                    changed_attributes_events.push_back({local_participant_, new_attrs});
                }
                auto old_perm = local_participant_->permission();
                if (old_perm.can_publish != new_perm.can_publish ||
                    old_perm.can_subscribe != new_perm.can_subscribe ||
                    old_perm.can_publish_data != new_perm.can_publish_data ||
                    old_perm.can_update_metadata != new_perm.can_update_metadata ||
                    old_perm.hidden != new_perm.hidden) {
                    local_participant_->set_permission(new_perm);
                    changed_permissions_events.push_back({local_participant_, old_perm, new_perm});
                }
                EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                    ParticipantEventKind::Upsert, local_participant_, true));
                continue;
            }

            auto it = remote_participants_.find(p_info.sid());
            if (p_info.state() == proto::ParticipantInfo::DISCONNECTED) {
                if (it != remote_participants_.end()) {
                    for (const auto& [sid, publication] : it->second->tracks()) {
                        if (publication) {
                            if (const auto track = publication->track()) {
                                removed_tracks.push_back(track);
                            }
                            const auto track_membership =
                                track_memberships_.find(publication.get());
                            if (track_membership != track_memberships_.end()) {
                                removed_track_keys.push_back(
                                    track_membership->second->key);
                            }
                        }
                        if (const auto remote_publication =
                                std::dynamic_pointer_cast<RemoteTrackPublication>(publication)) {
                            current_remote_binding_serials_.erase(remote_publication.get());
                            remote_publication->ClearMediaBinding();
                        }
                    }
                    auto departure = MakeParticipantEventLocked(
                        ParticipantEventKind::Departure, it->second, false);
                    disconnected.push_back(it->second);
                    RetireParticipantLocked(it->second);
                    remote_participants_.erase(it);
                    EnqueueParticipantEventLocked(std::move(departure));
                }
            } else {
                std::shared_ptr<RemoteParticipant> remote;
                if (it == remote_participants_.end()) {
                    remote = std::make_shared<RemoteParticipant>(p_info.sid(), p_info.identity());
                    remote->set_name(p_info.name());
                    remote->set_metadata(p_info.metadata());
                    remote->set_attributes(new_attrs);
                    remote->set_permission(new_perm);
                    remote_participants_[p_info.sid()] = remote;
                    newly_connected.push_back(remote);
                } else {
                    remote = it->second;
                    if (!p_info.name().empty()) {
                        remote->set_name(p_info.name());
                    }
                    std::string old_meta = remote->metadata();
                    if (old_meta != p_info.metadata()) {
                        remote->set_metadata(p_info.metadata());
                        changed_metadata_events.push_back({remote, old_meta, p_info.metadata()});
                    }

                    auto old_attrs = remote->attributes();
                    if (old_attrs != new_attrs) {
                        remote->set_attributes(new_attrs);
                        changed_attributes_events.push_back({remote, new_attrs});
                    }
                    auto old_perm = remote->permission();
                    if (old_perm.can_publish != new_perm.can_publish ||
                        old_perm.can_subscribe != new_perm.can_subscribe ||
                        old_perm.can_publish_data != new_perm.can_publish_data ||
                        old_perm.can_update_metadata != new_perm.can_update_metadata ||
                        old_perm.hidden != new_perm.hidden) {
                        remote->set_permission(new_perm);
                        changed_permissions_events.push_back({remote, old_perm, new_perm});
                    }
                }

                // 同步解析远端用户的 Track 列表（支持新增、静音状态变更、取消发布识别）
                std::set<std::string> current_track_sids;
                for (int t = 0; t < p_info.tracks_size(); ++t) {
                    const auto& t_info = p_info.tracks(t);
                    current_track_sids.insert(t_info.sid());
                    auto pub = remote->get_publication(t_info.sid());
                    if (!pub) {
                        TrackKind kind = (t_info.type() == proto::TrackType::AUDIO) ? TrackKind::Audio : TrackKind::Video;
                        auto remote_track = std::make_shared<Track>(t_info.sid(), t_info.name(), kind);
                        remote_track->set_muted(t_info.muted());
                        // RemoteParticipant owns the actual controllable
                        // publication. Do not create a parallel controller by
                        // SID: subscription/quality settings must target this
                        // exact object and its eventual sink binding.
                        pub = CreateRemoteTrackPublication(remote_track,
                                                           p_info.sid(),
                                                           t_info.sid(),
                                                           t_info.name(),
                                                           t_info.type());
                        remote->add_publication(pub);
                        newly_published_tracks.push_back({remote, pub});
                        Log("TRACK", "NEW_TRACK", "参会人 [" + remote->identity() + "] 发布新 Track: " + t_info.name() + " (" + (kind == TrackKind::Video ? "VIDEO" : "AUDIO") + ", SID: " + t_info.sid() + ", Muted: " + (t_info.muted() ? "true" : "false") + ")");
                    } else {
                        // 检测静音/画面开关状态变化
                        if (pub->track() && pub->track()->muted() != t_info.muted()) {
                            pub->track()->set_muted(t_info.muted());
                            changed_mute_events.push_back({remote, pub, t_info.muted()});
                            EnqueueParticipantEventLocked(MakeTrackEventLocked(
                                ParticipantEventKind::TrackMuted,
                                remote,
                                pub,
                                false));
                            Log("TRACK", "MUTE_CHANGED", "参会人 [" + remote->identity() + "] Track [" + t_info.sid() + "] 状态变更为: " + (t_info.muted() ? "静音/关闭" : "开启"));
                        }
                    }
                }

                // 检测被远端取消发布的 Track (Unpublished)
                auto existing_tracks = remote->tracks();
                for (const auto& [sid, pub] : existing_tracks) {
                    if (current_track_sids.find(sid) == current_track_sids.end()) {
                        if (pub && pub->track()) removed_tracks.push_back(pub->track());
                        if (const auto remote_publication =
                                std::dynamic_pointer_cast<RemoteTrackPublication>(pub)) {
                            current_remote_binding_serials_.erase(remote_publication.get());
                            remote_publication->ClearMediaBinding();
                        }
                        auto unavailable = MakeTrackEventLocked(
                            ParticipantEventKind::TrackUnavailable,
                            remote,
                            pub,
                            false);
                        if (unavailable.track_key.publication_incarnation != 0) {
                            removed_track_keys.push_back(unavailable.track_key);
                        }
                        RetireTrackMembershipLocked(pub);
                        remote->remove_publication(sid);
                        EnqueueParticipantEventLocked(std::move(unavailable));
                        unpublished_tracks.push_back({remote, pub});
                        Log("TRACK", "UNPUBLISHED", "参会人 [" + remote->identity() + "] 取消发布 Track: " + sid);
                    }
                }
                EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                    ParticipantEventKind::Upsert, remote, false));
            }
        }
    }

    DetachRemoteTrackSinks(TakeRemoteTrackSinksForTrackKeys(removed_track_keys));
    RemoveRemoteMediaTrackReferences(removed_tracks);

    for (const auto& p : newly_connected) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) listener->OnParticipantConnected(p);
        }
        FlushPendingTracks(p->sid());
    }

    bool has_new_video_pub = false;
    for (const auto& [p, pub] : newly_published_tracks) {
        FlushPendingTracks(p->sid());
        if (signal_client_) {
            const bool auto_sub = signal_client_->options().auto_subscribe;
            if (auto_sub) {
                if (pub->track() && pub->track()->kind() == TrackKind::Video) {
                    signal_client_->SendUpdateTrackSettings(pub->sid(), false, proto::VideoQuality::HIGH, 1280, 720, 30, 0);
                    has_new_video_pub = true;
                }
                signal_client_->SendUpdateSubscription({pub->sid()}, true, p->sid());
            }
        }
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) listener->OnTrackPublished(p, pub);
            if (!listener->ConsumesParticipantEvents() &&
                pub && pub->track() && pub->track()->rtc_track()) {
                listener->OnTrackSubscribed(pub->track(), pub, p);
            }
        }
    }

    // 会议中新增远端轨时，确保 Single PC 模式下主动发起 SDP 协商以获取 SFU 下发的 SSRC 与 MSID
    if (!newly_published_tracks.empty() && signal_client_ && signal_client_->is_single_pc_mode_active()) {
        Log("SIGNAL", "NEW_TRACK_RENEG", "检测到远端发布新 Track (" + std::to_string(newly_published_tracks.size()) + " 条)，立即触发 Publisher 重新协商获取下行媒体流");
        NegotiatePublisher();
    }

    for (const auto& evt : changed_mute_events) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnTrackMuted(evt.participant, evt.publication, evt.muted);
            }
        }
    }

    for (const auto& [p, pub] : unpublished_tracks) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) listener->OnTrackUnpublished(p, pub);
            if (!listener->ConsumesParticipantEvents() && pub && pub->track()) {
                listener->OnTrackUnsubscribed(pub->track(), pub, p);
            }
        }
    }

    for (const auto& evt : changed_attributes_events) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnParticipantAttributesChanged(evt.attrs, evt.participant);
            }
        }
    }

    for (const auto& evt : changed_permissions_events) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnParticipantPermissionsChanged(evt.old_perm, evt.new_perm, evt.participant);
            }
        }
    }

    for (const auto& evt : changed_metadata_events) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnParticipantMetadataChanged(evt.participant, evt.old_metadata, evt.new_metadata);
            }
        }
    }

    for (const auto& p : disconnected) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) listener->OnParticipantDisconnected(p);
        }
    }
}

void Room::UpdateParticipants(const proto::ParticipantUpdate& update) {
    UpdateParticipants(update.participants());
}

void Room::UpdateTrackMute(const proto::MuteTrackRequest& mute) {
    std::shared_ptr<Participant> target_participant;
    std::shared_ptr<TrackPublication> target_pub;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard lock(room_mutex_);
        listeners_snapshot = listeners_;

        if (local_participant_) {
            target_pub = local_participant_->get_publication(mute.sid());
            if (target_pub) {
                target_participant = local_participant_;
            }
        }

        if (!target_participant) {
            for (const auto& kv : remote_participants_) {
                target_pub = kv.second->get_publication(mute.sid());
                if (target_pub) {
                    target_participant = kv.second;
                    break;
                }
            }
        }
        if (target_participant && target_pub) {
            if (const auto track = target_pub->track()) {
                track->set_muted(mute.muted());
            }
            EnqueueParticipantEventLocked(MakeTrackEventLocked(
                ParticipantEventKind::TrackMuted,
                target_participant,
                target_pub,
                target_participant.get() == local_participant_.get()));
            EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                ParticipantEventKind::Upsert,
                target_participant,
                target_participant.get() == local_participant_.get()));
        }
    }

    if (target_participant && target_pub) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnTrackMuted(target_participant, target_pub, mute.muted());
            }
        }
    }
}

void Room::UpdateParticipantsForTesting(const proto::ParticipantUpdate& update) {
    UpdateParticipants(update.participants());
}

void Room::HandleSignalMessageForTesting(const proto::SignalResponse& message) {
    HandleSignalMessage(std::make_shared<proto::SignalResponse>(message));
}

void Room::EnableE2ee(const E2eeOptions& options) {
    std::lock_guard lock(room_mutex_);
    e2ee_manager_ = std::make_shared<E2eeManager>(options);
    auto self = shared_from_this();
    e2ee_manager_->SetStateChangedHandler([self](const std::string& identity, EncryptionState state) {
        auto snapshot = self->GetListenersSnapshot();
        for (const auto& listener : snapshot) {
            listener->OnE2eeStateChanged(identity, "", state);
        }
    });
}

void Room::HandleActiveSpeakerUpdateForTesting(const proto::SpeakersChanged& update) {
    HandleActiveSpeakerUpdate(update);
}

void Room::HandleActiveSpeakerUpdate(const proto::SpeakersChanged& update) {
    std::vector<std::shared_ptr<Participant>> active_speakers;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    ParticipantEvent value_event;
    value_event.kind = ParticipantEventKind::ActiveSpeakers;

    {
        std::lock_guard lock(room_mutex_);
        listeners_snapshot = listeners_;

        for (int i = 0; i < update.speakers_size(); ++i) {
            const auto& speaker = update.speakers(i);
            std::shared_ptr<Participant> target_p;

            if (local_participant_ && local_participant_->sid() == speaker.sid()) {
                target_p = local_participant_;
            } else {
                auto it = remote_participants_.find(speaker.sid());
                if (it != remote_participants_.end()) {
                    target_p = it->second;
                }
            }

            if (target_p) {
                target_p->set_speaking(speaker.active());
                target_p->set_audio_level(speaker.level());

                if (speaker.active()) {
                    active_speakers.push_back(target_p);
                    const bool is_local = target_p.get() == local_participant_.get();
                    if (auto membership = EnsureMembershipLocked(target_p, is_local)) {
                        ActiveSpeakerInfo info;
                        info.key = membership->key;
                        info.ticket = membership;
                        info.sid = membership->key.sid;
                        info.identity = membership->key.identity;
                        info.is_local = is_local;
                        info.speaking = true;
                        info.audio_level = speaker.level();
                        value_event.speakers.push_back(std::move(info));
                    }
                }
            }
        }
        std::sort(value_event.speakers.begin(), value_event.speakers.end(),
                  [](const ActiveSpeakerInfo& a, const ActiveSpeakerInfo& b) {
                      return a.audio_level > b.audio_level;
                  });
        EnqueueParticipantEventLocked(std::move(value_event));
    }

    std::sort(active_speakers.begin(), active_speakers.end(), [](const std::shared_ptr<Participant>& a, const std::shared_ptr<Participant>& b) {
        return a->audio_level() > b->audio_level();
    });

    for (const auto& listener : listeners_snapshot) {
        if (!listener->ConsumesParticipantEvents()) {
            listener->OnActiveSpeakersChanged(active_speakers);
        }
    }
}

void Room::SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    proto::SignalRequest req;
    auto* trickle = req.mutable_trickle();
    
    nlohmann::json candidate_json;
    candidate_json["candidate"] = sdp;
    candidate_json["sdpMid"] = sdp_mid;
    candidate_json["sdp_mid"] = sdp_mid;
    candidate_json["sdpMLineIndex"] = sdp_mline_index;
    candidate_json["sdp_m_line_index"] = sdp_mline_index;
    
    trickle->set_candidateinit(candidate_json.dump());
    trickle->set_target(pc_type == 0 ? proto::SignalTarget::PUBLISHER : proto::SignalTarget::SUBSCRIBER);

    if (signal_client_) {
        signal_client_->Send(req);
    }
}

void Room::HandleOfferSignal(const proto::SessionDescription& offer) {
    // ⚠ 最早期日志 - 确认此函数被调用
    Log("SIGNAL", "OFFER_CALLED",
        secure_log::SdpSummary("remote_offer_received", offer.sdp()));
    std::shared_ptr<SignalClient> client;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
        client = signal_client_;
        pc = (offer.type() == "offer" && subscriber_pc_) ? subscriber_pc_ : publisher_pc_;
        
        if (client && client->is_single_pc_mode_active()) {
            Log("WARNING", "UNEXPECTED_OFFER", "在 Single PC 模式下收到了服务端的 Offer，忽略该消息");
            return;
        }
    }

    if (!pc || !client) {
        std::cerr << "Room::HandleOfferSignal: warning, PeerConnection or SignalClient is null" << std::endl;
        Log("ERROR", "SDP_ERR", "HandleOfferSignal: PeerConnection 或 SignalClient 为空");
        return;
    }

    std::cout << "[WebRTC] Received SDP Offer from server, setting RemoteDescription..." << std::endl;
    Log("SIGNAL", "SDP_OFFER_RECV",
        secure_log::SdpSummary("subscriber_offer_received", offer.sdp()));
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pc, offer.type(), offer.sdp(), executor_,
        [self, client, pc](const std::string& set_remote_err) {
            if (!set_remote_err.empty()) {
                std::cerr << "Room: SetRemoteDescription offer error: "
                          << secure_log::OpaqueSummary("set_remote_offer") << std::endl;
                self->Log("ERROR", "SET_REMOTE_ERR",
                          secure_log::OpaqueSummary("set_remote_offer"));
                return;
            }

            std::cout << "[WebRTC] SetRemoteDescription offer succeeded. Generating SDP Answer..." << std::endl;
            self->Log("SIGNAL", "OFFER_APPLIED", "服务端 Offer 设置成功，正在生成 SDP Answer...");
            WebRTCManager::Instance().CreateAnswer(pc, self->executor_,
                [self, client, pc](const std::string& sdp, const std::string& create_ans_err) {
                    if (!create_ans_err.empty()) {
                        std::cerr << "Room: CreateAnswer error: "
                                  << secure_log::OpaqueSummary("create_answer") << std::endl;
                        self->Log("ERROR", "CREATE_ANS_ERR",
                                  secure_log::OpaqueSummary("create_answer"));
                        return;
                    }

                    std::cout << "[WebRTC] CreateAnswer succeeded. Setting LocalDescription..." << std::endl;
                    WebRTCManager::Instance().SetLocalDescription(pc, "answer", sdp, self->executor_,
                        [self, client, pc, sdp](const std::string& set_local_err) {
                            if (!set_local_err.empty()) {
                                std::cerr << "Room: SetLocalDescription answer error: "
                                          << secure_log::OpaqueSummary("set_local_answer") << std::endl;
                                self->Log("ERROR", "SET_LOCAL_ANS_ERR",
                                          secure_log::OpaqueSummary("set_local_answer"));
                                return;
                            }

                            proto::SignalRequest req;
                            auto* answer_msg = req.mutable_answer();
                            answer_msg->set_type("answer");
                            answer_msg->set_sdp(sdp);
                            client->Send(req);
                            std::cout << "[WebRTC] -> Successfully created and sent SDP Answer back to LiveKit Server!" << std::endl;
                            self->Log("SIGNAL", "SDP_ANSWER_SENT",
                                      secure_log::SdpSummary("subscriber_answer_sent", sdp));

                            // 重放暂存的 Subscriber 早期 ICE 候选
                            std::vector<PendingIceCandidate> pending_cands;
                            {
                                std::lock_guard lock(self->room_mutex_);
                                pending_cands = std::move(self->pending_sub_ice_candidates_);
                            }
                            if (!pending_cands.empty()) {
                                self->Log("SIGNAL", "ICE_FLUSH", "开始重放暂存的 " + std::to_string(pending_cands.size()) + " 个 Subscriber 早期候选...");
                                std::vector<std::pair<std::string, bool>> results;
                                WebRTCManager::Instance().signaling_thread()->BlockingCall([pc, &pending_cands, &results]() {
                                    for (const auto& pcand : pending_cands) {
                                        webrtc::SdpParseError p_err;
                                        std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(pcand.sdp_mid, pcand.sdp_mline_index, pcand.sdp, &p_err));
                                        bool ok = false;
                                        if (cand && pc) {
                                            ok = pc->AddIceCandidate(cand.get());
                                        }
                                        results.push_back({pcand.sdp_mid, ok});
                                    }
                                });
                                for (const auto& res : results) {
                                self->Log("SIGNAL", "ICE_SUB_REPLAY",
                                          std::string("重放早期候选结果=") +
                                              (res.second ? "成功" : "失败"));
                                }
                            }

                            // 扫描并激活所有 Subscriber Transceiver 下的 Receiver Track，确保后加入用户的音频被及时挂载与播放
                            auto transceivers = pc->GetTransceivers();
                            for (const auto& t : transceivers) {
                                if (t && t->receiver() && t->receiver()->track()) {
                                    auto r_track = t->receiver()->track();
                                    r_track->set_enabled(true);
                                    std::weak_ptr<Room> weak_this = self;
                                    asio::post(self->executor_, [weak_this, receiver = t->receiver(), r_track]() {
                                        if (auto room = weak_this.lock()) {
                                            room->OnRemoteTrackAdded(receiver, r_track);
                                        }
                                    });
                                }
                            }
                        });
                });
        });
}

static std::vector<std::string> ExtractSdpMLines(const std::string& sdp) {
    std::vector<std::string> mlines;
    std::istringstream stream(sdp);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("m=", 0) == 0) {
            auto space_pos = line.find(' ');
            if (space_pos != std::string::npos) {
                mlines.push_back(line.substr(2, space_pos - 2));
            } else {
                mlines.push_back(line.substr(2));
            }
        }
    }
    return mlines;
}

void Room::HandleAnswerSignal(const proto::SessionDescription& answer) {
    Log("SIGNAL", "SDP_ANSWER_RECV",
        secure_log::SdpSummary("remote_answer_received", answer.sdp()));
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    bool pub_negotiating = false;
    bool sub_negotiating = false;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
        pub_negotiating = (negotiation_state_ != NegotiationState::Idle);
        sub_negotiating = subscriber_negotiating_;
    }

    if (signal_client_ && signal_client_->is_single_pc_mode_active()) {
        Log("SIGNAL", "SDP_ANS_ROUTING", "Single PC 模式：直接将 Answer 路由至 publisher_pc_");
        auto self = shared_from_this();
        WebRTCManager::Instance().SetRemoteDescription(pub_pc, "answer", answer.sdp(), executor_,
            [self, pub_pc](const std::string& err) {
                if (!err.empty()) {
                    self->Log("ERROR", "PUB_REMOTE_ERR",
                              secure_log::OpaqueSummary("set_publisher_remote_answer"));
                    self->CompleteNegotiation(err);
                } else {
                    self->Log("SIGNAL", "PUB_STABLE", "Publisher PC 协商完成 (Answer 应用成功)");
                    std::vector<PendingIceCandidate> pending_cands;
                    bool need_retry = false;
                    {
                        std::lock_guard lock(self->room_mutex_);
                        self->subscriber_negotiating_ = false;
                        if (self->negotiation_state_ == NegotiationState::PendingRetry) {
                            self->negotiation_state_ = NegotiationState::InProgress;
                            need_retry = true;
                        }
                        pending_cands = std::move(self->pending_pub_ice_candidates_);
                    }
                    if (need_retry) {
                        self->Log("SIGNAL", "NEG_RETRY", "检测到挂起的协商请求，开始新一轮重试");
                        self->ExecuteNegotiatePublisher();
                    } else {
                        self->CompleteNegotiation("");
                    }
                    
                    // 单 PC 模式下，本地主动发起的 recvonly transceiver 不会触发 OnTrack，需手动提取
                    auto transceivers = pub_pc->GetTransceivers();
                    self->Log("SIGNAL", "TRANS_SCAN", "Single PC Answer 协商成功，扫描 " + std::to_string(transceivers.size()) + " 个 transceivers");
                    for (const auto& t : transceivers) {
                        if (t && (t->direction() == webrtc::RtpTransceiverDirection::kRecvOnly ||
                            (t->current_direction().has_value() && *t->current_direction() == webrtc::RtpTransceiverDirection::kRecvOnly))) {
                            if (t->receiver() && t->receiver()->track()) {
                                std::weak_ptr<Room> weak_this = self;
                                asio::post(self->executor_, [weak_this, receiver = t->receiver(), track = t->receiver()->track()]() {
                                    if (auto room = weak_this.lock()) {
                                        room->OnRemoteTrackAdded(receiver, track);
                                    }
                                });
                            }
                        }
                    }

                    if (!pending_cands.empty()) {
                        self->Log("SIGNAL", "ICE_FLUSH", "开始重放暂存的 " + std::to_string(pending_cands.size()) + " 个 Publisher 早期候选...");
                        std::vector<std::pair<std::string, bool>> results;
                        WebRTCManager::Instance().signaling_thread()->BlockingCall([pub_pc, &pending_cands, &results]() {
                            for (const auto& pcand : pending_cands) {
                                webrtc::SdpParseError p_err;
                                std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(pcand.sdp_mid, pcand.sdp_mline_index, pcand.sdp, &p_err));
                                bool ok = false;
                                if (cand && pub_pc) {
                                    ok = pub_pc->AddIceCandidate(cand.get());
                                }
                                results.push_back({pcand.sdp_mid, ok});
                            }
                        });
                        for (const auto& res : results) {
                            self->Log("SIGNAL", "ICE_PUB_REPLAY",
                                      std::string("重放早期候选结果=") +
                                          (res.second ? "成功" : "失败"));
                        }
                    }
                }
            });
        return;
    }

    auto answer_mlines = ExtractSdpMLines(answer.sdp());
    
    // 提取 Publisher 和 Subscriber 当前 local description 的 m-lines 进行特征比对
    std::vector<std::string> pub_mlines;
    std::vector<std::string> sub_mlines;
    std::string pub_desc_str, sub_desc_str;
    if (pub_pc) {
        WebRTCManager::Instance().signaling_thread()->BlockingCall([&pub_pc, &pub_mlines, &pub_desc_str]() {
            auto desc = pub_pc->pending_local_description();
            if (!desc) desc = pub_pc->local_description();
            if (desc) {
                desc->ToString(&pub_desc_str);
                pub_mlines = ExtractSdpMLines(pub_desc_str);
            }
        });
    }
    if (sub_pc) {
        WebRTCManager::Instance().signaling_thread()->BlockingCall([&sub_pc, &sub_mlines, &sub_desc_str]() {
            auto desc = sub_pc->pending_local_description();
            if (!desc) desc = sub_pc->local_description();
            if (desc) {
                desc->ToString(&sub_desc_str);
                sub_mlines = ExtractSdpMLines(sub_desc_str);
            }
        });
    }

    Log("SIGNAL", "SDP_ANS_ROUTING",
        secure_log::SdpSummary("answer_route_remote", answer.sdp()) + ", " +
        secure_log::SdpSummary("answer_route_publisher", pub_desc_str) + ", " +
        secure_log::SdpSummary("answer_route_subscriber", sub_desc_str) +
        ", publisher_negotiating=" + std::to_string(pub_negotiating) +
        ", subscriber_negotiating=" + std::to_string(sub_negotiating));

    bool has_application = false;
    for (const auto& m : answer_mlines) {
        if (m == "application") {
            has_application = true;
            break;
        }
    }

    bool route_to_sub = false;
    if (has_application) {
        // 包含 DataChannel 的必为 Publisher
        route_to_sub = false;
    } else if (sub_negotiating) {
        // 无 DataChannel 且正在进行 Subscriber 协商的必为 Subscriber
        route_to_sub = true;
    } else if (answer_mlines == sub_mlines && !sub_mlines.empty()) {
        route_to_sub = true;
    } else {
        route_to_sub = false;
    }

    if (route_to_sub && sub_pc) {
        // Subscriber Answer: 服务端回应我们发出的 Subscriber Offer
        Log("SIGNAL", "SUB_ANS_RECV", "收到 Subscriber SDP Answer (" + std::to_string(answer.sdp().length()) + " 字节, m-lines=" + std::to_string(answer_mlines.size()) + "), 正在应用到 Subscriber PC...");
        auto self = shared_from_this();
        WebRTCManager::Instance().SetRemoteDescription(sub_pc, answer.type(), answer.sdp(), executor_,
            [self, sub_pc, answer_sdp = answer.sdp()](const std::string& err) {
                std::vector<PendingIceCandidate> pending_cands;
                {
                    std::lock_guard lock(self->room_mutex_);
                    self->subscriber_negotiating_ = false;
                    if (err.empty()) {
                        pending_cands = std::move(self->pending_sub_ice_candidates_);
                    }
                }

                if (!err.empty()) {
                    self->Log("ERROR", "SUB_ANS_ERR",
                              secure_log::OpaqueSummary("set_subscriber_remote_answer"));
                    self->Log("ERROR", "SUB_ANS_LINES",
                              secure_log::SdpSummary("failed_subscriber_answer", answer_sdp));
                } else {
                    self->Log("WEBRTC", "SUB_STABLE", "Subscriber RemoteDescription (Answer) 应用成功，信令状态已稳定 STABLE");

                    // 在锁外部安全重放早期候选，彻底避免死锁
                    if (!pending_cands.empty()) {
                        self->Log("SIGNAL", "ICE_FLUSH", "开始重放暂存的 " + std::to_string(pending_cands.size()) + " 个 Subscriber 早期候选...");
                        std::vector<std::pair<std::string, bool>> results;
                        WebRTCManager::Instance().signaling_thread()->BlockingCall([sub_pc, &pending_cands, &results]() {
                            for (const auto& pcand : pending_cands) {
                                webrtc::SdpParseError p_err;
                                std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(pcand.sdp_mid, pcand.sdp_mline_index, pcand.sdp, &p_err));
                                bool ok = false;
                                if (cand && sub_pc) {
                                    ok = sub_pc->AddIceCandidate(cand.get());
                                }
                                results.push_back({pcand.sdp_mid, ok});
                            }
                        });
                        for (const auto& res : results) {
                                self->Log("SIGNAL", "ICE_SUB_REPLAY",
                                          std::string("重放早期候选结果=") +
                                              (res.second ? "成功" : "失败"));
                        }
                    }
                }
            });
        return;
    }

    // Publisher Answer
    Log("SIGNAL", "SDP_ANSWER_RECV", "收到 Publisher 的远端 SDP Answer (" + std::to_string(answer.sdp().length()) + " 字节, m-lines=" + std::to_string(answer_mlines.size()) + ")");
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pub_pc, answer.type(), answer.sdp(), executor_,
        [self, pub_pc](const std::string& err) {
            bool need_retry = false;
            std::vector<PendingIceCandidate> pending_cands;
            {
                std::lock_guard lock(self->room_mutex_);
                if (self->negotiation_state_ == NegotiationState::PendingRetry) {
                    self->negotiation_state_ = NegotiationState::InProgress;
                    need_retry = true;
                } else {
                    self->negotiation_state_ = NegotiationState::Idle;
                }
                if (err.empty()) {
                    pending_cands = std::move(self->pending_pub_ice_candidates_);
                }
            }

            if (!err.empty()) {
                std::cerr << "Room: SetRemoteDescription answer error: "
                          << secure_log::OpaqueSummary("set_publisher_remote_answer") << std::endl;
                self->Log("ERROR", "ANS_ERR",
                          secure_log::OpaqueSummary("set_publisher_remote_answer"));
                self->CompleteNegotiation(err);
            } else {
                std::cout << "[WebRTC] Publisher remote description applied successfully! PC signaling state is STABLE." << std::endl;
                self->Log("WEBRTC", "PUB_STABLE", "Publisher RemoteDescription 应用成功，信令状态已恢复 STABLE");

                // 在锁外部安全重放早期候选
                if (!pending_cands.empty()) {
                    WebRTCManager::Instance().signaling_thread()->BlockingCall([pub_pc, &pending_cands]() {
                        for (const auto& pcand : pending_cands) {
                            webrtc::SdpParseError p_err;
                            std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(pcand.sdp_mid, pcand.sdp_mline_index, pcand.sdp, &p_err));
                            if (cand && pub_pc) {
                                pub_pc->AddIceCandidate(cand.get());
                            }
                        }
                    });
                }
            }

            if (need_retry) {
                self->Log("SIGNAL", "NEG_RETRY", "Publisher Answer applied, scheduling pending negotiation retry");
                self->ExecuteNegotiatePublisher();
            } else if (err.empty()) {
                self->CompleteNegotiation("");
            }
        });
}

void Room::HandleTrickleSignal(const proto::TrickleRequest& trickle) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
    }

    try {
        auto json_cand = nlohmann::json::parse(trickle.candidateinit());
        std::string sdp = json_cand.value("candidate", "");
        std::string sdp_mid = json_cand.value("sdpMid", "");
        if (sdp_mid.empty()) sdp_mid = json_cand.value("sdp_mid", "");
        int sdp_mline_index = json_cand.value("sdpMLineIndex", 0);

        Log("SIGNAL", "TRICKLE_RECV", "收到服务端 ICE 候选: target=" +
            std::string(trickle.target() == proto::SignalTarget::PUBLISHER ? "PUBLISHER" : "SUBSCRIBER") +
            ", detail=[omitted]");

        auto self = shared_from_this();
        WebRTCManager::Instance().signaling_thread()->BlockingCall([self, pub_pc, sub_pc, trickle_target = trickle.target(), sdp_mid, sdp_mline_index, sdp]() {
            webrtc::SdpParseError err;
            std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &err));
            if (!cand) {
                self->Log("ERROR", "ICE_PARSE_FAIL",
                          secure_log::OpaqueSummary("parse_ice_candidate"));
                return;
            }

            bool is_single = false;
            {
                std::lock_guard lock(self->room_mutex_);
                is_single = self->signal_client_ && self->signal_client_->is_single_pc_mode_active();
            }

            if (is_single) {
                if (pub_pc) {
                    bool ok = pub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        self->pending_pub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_PUB_QUEUE",
                                  "Publisher PC 暂未就绪，已暂存早期 ICE 候选: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_PUB_ADD",
                                  "向 Publisher PC 添加 ICE 候选: 结果=成功 (Single PC)");
                    }
                }
                return;
            }

            if (trickle_target == proto::SignalTarget::SUBSCRIBER) {
                if (sub_pc) {
                    bool ok = sub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        self->pending_sub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_SUB_QUEUE",
                                  "Subscriber PC 暂未就绪，已暂存早期 ICE 候选: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD",
                                  "向 Subscriber PC 添加 ICE 候选: 结果=成功");
                    }
                }
            } else {
                if (pub_pc) {
                    bool ok = pub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        self->pending_pub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                    }
                }
                // 同时尝试添加至 sub_pc，防止服务端 target 缺省为 0 导致 Subscriber 缺少候选
                if (sub_pc) {
                    bool ok = sub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        self->pending_sub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_SUB_QUEUE",
                                  "Subscriber PC 暂未就绪，已暂存早期 ICE 候选: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD",
                                  "向 Subscriber PC 尝试添加 ICE 候选: 结果=成功");
                    }
                }
            }
        });
    } catch (...) {
        std::cerr << "Room: Failed to parse trickle candidate JSON" << std::endl;
    }
}

// The SFU sends this only for the client-offer/single-PC flow. The counts are
// additional media sections required by the server, not desired totals.
void Room::HandleMediaSectionsRequirement(const proto::MediaSectionsRequirement& req) {
    const uint32_t num_audios = req.num_audios();
    const uint32_t num_videos = req.num_videos();
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    bool single_pc = false;
    {
        std::lock_guard lock(room_mutex_);
        single_pc = signal_client_ && signal_client_->is_single_pc_mode_active();
        publisher = publisher_pc_;
    }

    if (!single_pc) {
        Log("SIGNAL", "MEDIA_SEC_SKIP",
            "忽略 dual-PC 模式下的 MediaSectionsRequirement；Subscriber SDP 由服务端发起");
        return;
    }
    if (!publisher || (num_audios == 0 && num_videos == 0)) {
        Log("SIGNAL", "MEDIA_SEC_SKIP", "MediaSectionsRequirement 无需新增媒体段");
        return;
    }

    std::string add_error;
    WebRTCManager::Instance().signaling_thread()->BlockingCall(
        [publisher, num_audios, num_videos, &add_error]() {
        for (uint32_t i = 0; i < num_audios; ++i) {
            webrtc::RtpTransceiverInit init;
            init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
            auto result = publisher->AddTransceiver(webrtc::MediaType::AUDIO, init);
            if (!result.ok() && add_error.empty()) add_error = result.error().message();
        }
        for (uint32_t i = 0; i < num_videos; ++i) {
            webrtc::RtpTransceiverInit init;
            init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
            auto result = publisher->AddTransceiver(webrtc::MediaType::VIDEO, init);
            if (!result.ok() && add_error.empty()) add_error = result.error().message();
        }
    });

    if (!add_error.empty()) {
        Log("ERROR", "MEDIA_SEC_ADD_FAIL",
            secure_log::OpaqueSummary("add_recvonly_media_section"));
        return;
    }

    Log("SIGNAL", "MEDIA_SEC_ADDED",
        "按服务端请求新增 recvonly 媒体段: audio=" + std::to_string(num_audios) +
        ", video=" + std::to_string(num_videos));
    // NegotiatePublisher coalesces repeated requirements received while an
    // offer is in flight, so no server request is dropped.
    NegotiatePublisher();
}

proto::SyncState Room::BuildSyncState() const {
    proto::SyncState state;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sync_pc;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> lossy;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> remote_channels;
    std::shared_ptr<LocalParticipant> local;
    bool single_pc = false;
    bool auto_subscribe = true;
    {
        std::lock_guard lock(room_mutex_);
        single_pc = signal_client_ && signal_client_->is_single_pc_mode_active();
        auto_subscribe = signal_client_ ? signal_client_->options().auto_subscribe : true;
        sync_pc = single_pc ? publisher_pc_ : subscriber_pc_;
        reliable = reliable_dc_;
        lossy = lossy_dc_;
        remote_channels = remote_data_channels_;
        local = local_participant_;
    }

    state.mutable_subscription()->set_subscribe(!auto_subscribe);

    if (local) {
        for (const auto& [sid, publication] : local->tracks()) {
            if (!publication || !publication->track()) continue;
            auto* published = state.add_publish_tracks();
            published->set_cid(publication->track()->name());
            auto* info = published->mutable_track();
            info->set_sid(publication->sid());
            info->set_name(publication->name());
            info->set_muted(publication->track()->muted());
            info->set_type(publication->track()->kind() == TrackKind::Audio
                ? proto::TrackType::AUDIO : proto::TrackType::VIDEO);
            switch (publication->track()->source()) {
                case TrackSource::Microphone:
                    info->set_source(proto::TrackSource::MICROPHONE);
                    break;
                case TrackSource::ScreenShareVideo:
                    info->set_source(proto::TrackSource::SCREEN_SHARE);
                    break;
                case TrackSource::ScreenShareAudio:
                    info->set_source(proto::TrackSource::SCREEN_SHARE_AUDIO);
                    break;
                default:
                    info->set_source(publication->track()->kind() == TrackKind::Audio
                        ? proto::TrackSource::MICROPHONE : proto::TrackSource::CAMERA);
                    break;
            }
        }
    }

    auto add_data_channel = [&state](
        const webrtc::scoped_refptr<webrtc::DataChannelInterface>& channel,
        proto::SignalTarget target) {
        if (!channel || channel->id() < 0) return;
        auto* info = state.add_data_channels();
        info->set_label(channel->label());
        info->set_id(static_cast<uint32_t>(channel->id()));
        info->set_target(target);
    };
    add_data_channel(reliable, proto::SignalTarget::PUBLISHER);
    add_data_channel(lossy, proto::SignalTarget::PUBLISHER);
    for (const auto& channel : remote_channels) {
        add_data_channel(channel, proto::SignalTarget::SUBSCRIBER);
    }

    if (!sync_pc || !WebRTCManager::Instance().signaling_thread()) {
        return state;
    }

    WebRTCManager::Instance().signaling_thread()->BlockingCall([sync_pc, single_pc, &state]() {
        const auto* local_desc = sync_pc->current_local_description();
        const auto* remote_desc = sync_pc->current_remote_description();
        if (!local_desc) local_desc = sync_pc->local_description();
        if (!remote_desc) remote_desc = sync_pc->remote_description();

        auto copy_description = [](const webrtc::SessionDescriptionInterface* source,
                                   proto::SessionDescription* destination) {
            if (!source || !destination) return;
            std::string sdp;
            if (!source->ToString(&sdp)) return;
            destination->set_type(source->type());
            destination->set_sdp(std::move(sdp));
        };

        if (single_pc) {
            // Client offered on the publisher transport; SFU answered.
            if (local_desc) copy_description(local_desc, state.mutable_offer());
            if (remote_desc) copy_description(remote_desc, state.mutable_answer());
        } else {
            // SFU offered on the subscriber transport; client answered.
            if (remote_desc) copy_description(remote_desc, state.mutable_offer());
            if (local_desc) copy_description(local_desc, state.mutable_answer());
        }
    });
    return state;
}

asio::awaitable<void> Room::AttemptReconnect() {
    std::string reconnect_url;
    std::string reconnect_token;
    SignalOptions reconnect_options;
    {
        std::lock_guard lock(room_mutex_);
        if (reconnect_active_ || reconnect_disabled_) co_return;
        reconnect_active_ = true;
        if (signal_client_) {
            reconnect_url = signal_client_->url();
            reconnect_token = signal_client_->token();
            reconnect_options = signal_client_->options();
        }
    }

    RecordPublishedTracks();

    int attempts = 0;
    bool full_restart = false;
    {
        std::lock_guard lock(room_mutex_);
        full_restart = join_response_ && join_response_->has_client_configuration() &&
            join_response_->client_configuration().resume_connection() ==
                proto::ClientConfigSetting::DISABLED;
    }
    std::string last_error = "reconnect attempts exhausted";
    const auto reconnect_deadline = std::chrono::steady_clock::now() +
        operation_timeouts_.reconnect_total;

    while (attempts < kMaxReconnectAttempts &&
           std::chrono::steady_clock::now() < reconnect_deadline) {
        attempts++;
        auto delay = kBaseReconnectDelay * (1 << (attempts - 1));
        if (delay > kMaxReconnectDelay) delay = kMaxReconnectDelay;
        // Deterministic operation id based jitter avoids synchronized reconnect storms.
        delay += std::chrono::milliseconds(
            static_cast<int>(operation_sequence_.fetch_add(1, std::memory_order_relaxed) % 75));

        asio::steady_timer timer(executor_, delay);
        std::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        {
            std::lock_guard lock(room_mutex_);
            if (reconnect_disabled_ ||
                (connection_state_ == ConnectionState::Disconnected && !full_restart)) {
                reconnect_active_ = false;
                co_return;
            }
        }

        const auto remaining_total = std::chrono::duration_cast<std::chrono::milliseconds>(
            reconnect_deadline - std::chrono::steady_clock::now());
        if (remaining_total <= std::chrono::milliseconds::zero()) break;
        const auto attempt_budget = std::min(
            operation_timeouts_.reconnect_attempt, remaining_total);
        const auto attempt_deadline = std::chrono::steady_clock::now() + attempt_budget;

        if (!full_restart) {
            try {
                std::shared_ptr<SignalClient> signal;
                {
                    std::lock_guard lock(room_mutex_);
                    signal = signal_client_;
                }
                if (!signal) {
                    throw OperationError(OperationKind::Reconnect,
                                         OperationErrorCode::SessionClosed,
                                         "signal_resume",
                                         "signal client is missing");
                }

                auto restart_res = co_await signal->Restart(attempt_budget);
                if (restart_res.error || !restart_res.reconnect_response) {
                    throw OperationError(OperationKind::Reconnect,
                                         OperationErrorCode::SignalConnectFailed,
                                         "signal_resume",
                                         restart_res.error ? restart_res.error.message()
                                                           : "ReconnectResponse is missing",
                                         true);
                }

                proto::SignalRequest sync_request;
                *sync_request.mutable_sync_state() = BuildSyncState();
                co_await signal->SendAsync(sync_request);
                signal->SetReconnected();
                signal->SetEventReady();
                const auto media_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                    attempt_deadline - std::chrono::steady_clock::now());
                if (media_budget <= std::chrono::milliseconds::zero()) {
                    throw OperationError(OperationKind::Reconnect,
                                         OperationErrorCode::PeerConnectionTimeout,
                                         "resume_media",
                                         "reconnect attempt timed out before media recovery",
                                         true);
                }
                co_await RestartIceConnections(restart_res.reconnect_response, media_budget);

                {
                    std::lock_guard lock(room_mutex_);
                    if (connection_state_ != ConnectionState::Reconnecting ||
                        reconnect_disabled_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "resume_commit",
                                             "reconnect was cancelled before commit");
                    }
                    connection_state_ = ConnectionState::Connected;
                    reconnect_attempts_ = 0;
                    reconnect_active_ = false;
                    EnqueueRosterLocked();
                }

                auto listeners_snapshot = GetListenersSnapshot();
                for (const auto& listener : listeners_snapshot) listener->OnReconnected();
                co_return;
            } catch (const std::exception& error) {
                last_error = error.what();
                Log("WARNING", "RESUME_FAILED",
                    secure_log::ExceptionSummary("resume_reconnect") +
                        "; switching to full restart");
                full_restart = true;
            }
        }

        if (full_restart) {
            try {
                std::deque<ParticipantEvent> retired_events;
                std::shared_ptr<SignalClient> old_signal;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> old_publisher;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> old_subscriber;
                std::shared_ptr<webrtc::PeerConnectionObserver> old_publisher_observer;
                std::shared_ptr<webrtc::PeerConnectionObserver> old_subscriber_observer;
                std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> old_data_channels;
                std::vector<std::shared_ptr<RoomDataChannelObserver>> old_data_channel_observers;
                std::vector<RemoteTrackSinkBinding> old_track_sinks;
                {
                    std::lock_guard lock(room_mutex_);
                    if (reconnect_disabled_) {
                        reconnect_active_ = false;
                        co_return;
                    }
                    old_signal = std::move(signal_client_);
                    old_publisher = std::move(publisher_pc_);
                    old_subscriber = std::move(subscriber_pc_);
                    old_publisher_observer = std::move(publisher_observer_);
                    old_subscriber_observer = std::move(subscriber_observer_);
                    RetireAllMembershipsLocked(retired_events);
                    local_participant_.reset();
                    pending_local_unpublishes_.clear();
                    ClearRemotePublicationMediaBindingsLocked();
                    remote_participants_.clear();
                    if (reliable_dc_) {
                        old_data_channels.push_back(reliable_dc_);
                        reliable_dc_ = nullptr;
                    }
                    if (lossy_dc_) {
                        old_data_channels.push_back(lossy_dc_);
                        lossy_dc_ = nullptr;
                    }
                    for (auto& dc : remote_data_channels_) {
                        if (dc) old_data_channels.push_back(dc);
                    }
                    remote_data_channels_.clear();
                    old_data_channel_observers = std::move(data_channel_observers_);
                    old_track_sinks = std::move(remote_track_sinks_);
                    processed_remote_track_ids_.clear();
                    pending_track_queue_.clear();
                    connection_state_ = ConnectionState::Disconnected;
                    suppress_next_connected_event_ = true;
                    session_generation_.fetch_add(1, std::memory_order_acq_rel);
                }
                retired_events.clear();
                CancelPendingOperations(OperationErrorCode::Cancelled,
                                        "full_restart",
                                        "old session replaced by full restart");
                if (old_signal) old_signal->Close();
                for (auto& dc : old_data_channels) {
                    if (dc) {
                        dc->UnregisterObserver();
                        dc->Close();
                    }
                }
                old_data_channels.clear();
                if (old_publisher) old_publisher->Close();
                if (old_subscriber && old_subscriber != old_publisher) old_subscriber->Close();
                DetachRemoteTrackSinks(std::move(old_track_sinks));
                old_publisher = nullptr;
                old_subscriber = nullptr;
                old_publisher_observer.reset();
                old_subscriber_observer.reset();
                old_data_channel_observers.clear();

                co_await ConnectAsync(reconnect_url, reconnect_token, reconnect_options);

                std::shared_ptr<LocalParticipant> local;
                std::vector<std::shared_ptr<RoomListener>> listeners;
                {
                    std::lock_guard lock(room_mutex_);
                    if (reconnect_disabled_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "full_restart_commit",
                                             "reconnect was cancelled by server leave");
                    }
                    local = local_participant_;
                    listeners = listeners_;
                }
                if (!local) {
                    throw OperationError(OperationKind::Reconnect,
                                         OperationErrorCode::StateUncertain,
                                         "full_restart_republish",
                                         "local participant is missing after full restart");
                }

                for (const auto& record : published_track_records_) {
                    if (!record.track) continue;
                    auto publication = co_await local->PublishTrackAsync(record.track);
                    for (const auto& listener : listeners) {
                        listener->OnLocalTrackRepublished(record.previous_sid, publication);
                    }
                }

                {
                    std::lock_guard lock(room_mutex_);
                    reconnect_attempts_ = 0;
                    reconnect_active_ = false;
                    if (local_participant_) {
                        EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                            ParticipantEventKind::Upsert,
                            local_participant_,
                            true));
                    }
                }

                for (const auto& listener : listeners) listener->OnReconnected();
                co_return;
            } catch (const std::exception& error) {
                last_error = error.what();
                Log("WARNING", "FULL_RESTART_FAILED",
                    secure_log::ExceptionSummary("full_restart"));
                std::lock_guard lock(room_mutex_);
                if (connection_state_ == ConnectionState::Disconnected &&
                    !reconnect_disabled_) {
                    connection_state_ = ConnectionState::Reconnecting;
                }
                suppress_next_connected_event_ = false;
            }
        }
    }

    {
        std::lock_guard lock(room_mutex_);
        if (reconnect_disabled_) {
            reconnect_active_ = false;
            co_return;
        }
    }

    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<RoomDataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::deque<ParticipantEvent> retired_events;
    {
        std::lock_guard lock(room_mutex_);
        signal = std::move(signal_client_);
        publisher = std::move(publisher_pc_);
        subscriber = std::move(subscriber_pc_);
        publisher_observer = std::move(publisher_observer_);
        subscriber_observer = std::move(subscriber_observer_);
        RetireAllMembershipsLocked(retired_events);
        local_participant_.reset();
        pending_local_unpublishes_.clear();
        ClearRemotePublicationMediaBindingsLocked();
        remote_participants_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        for (auto& dc : remote_data_channels_) {
            if (dc) data_channels.push_back(dc);
        }
        remote_data_channels_.clear();
        data_channel_observers = std::move(data_channel_observers_);
        track_sinks = std::move(remote_track_sinks_);
        processed_remote_track_ids_.clear();
        pending_track_queue_.clear();
        connection_state_ = ConnectionState::Disconnected;
        reconnect_active_ = false;
        suppress_next_connected_event_ = false;
        listeners_snapshot = listeners_;
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    retired_events.clear();
    CancelPendingOperations(OperationErrorCode::ReconnectExhausted,
                            "reconnect_exhausted",
                            last_error);
    if (signal) signal->Close();
    for (auto& dc : data_channels) {
        if (dc) {
            dc->UnregisterObserver();
            dc->Close();
        }
    }
    data_channels.clear();
    if (publisher) publisher->Close();
    if (subscriber && subscriber != publisher) subscriber->Close();
    DetachRemoteTrackSinks(std::move(track_sinks));
    publisher = nullptr;
    subscriber = nullptr;
    publisher_observer.reset();
    subscriber_observer.reset();
    data_channel_observers.clear();
    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected(RoomDisconnectReason::NetworkError,
                                 "Reconnect failed: " + last_error);
    }
}

void Room::UpdateRoomInfo(const proto::Room& room) {
    RoomInfo updated;
    std::string old_metadata;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    bool metadata_changed = false;
    bool room_changed = false;

    {
        std::lock_guard lock(room_mutex_);
        updated = room_info_;

        // Match the Rust SDK's treatment of an empty SID: a RoomUpdate does
        // not erase a known SID unless the server supplies a replacement.
        if (!room.sid().empty()) {
            updated.sid = room.sid();
        }
        updated.name = room.name();
        updated.metadata = room.metadata();
        updated.empty_timeout = room.empty_timeout();
        updated.departure_timeout = room.departure_timeout();
        updated.max_participants = room.max_participants();
        updated.creation_time_ms = room.creation_time_ms();
        updated.num_participants = room.num_participants();
        updated.num_publishers = room.num_publishers();
        updated.active_recording = room.active_recording();

        old_metadata = room_info_.metadata;
        metadata_changed = old_metadata != updated.metadata;
        room_changed = room_info_ != updated;
        if (!room_changed) {
            return;
        }

        room_info_ = updated;
        listeners_snapshot = listeners_;
    }

    // State is committed and listeners are snapshotted before callback
    // delivery. No listener can run while room_mutex_ is held.
    if (metadata_changed) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnRoomMetadataChanged(updated, old_metadata, updated.metadata);
        }
    }
    for (const auto& listener : listeners_snapshot) {
        listener->OnRoomUpdated(updated);
    }
}

void Room::UpdateConnectionQuality(const proto::ConnectionQualityUpdate& update) {
    struct Change {
        std::shared_ptr<Participant> participant;
        ConnectionQuality quality = ConnectionQuality::Unknown;
        float score = 0.0f;
    };

    std::vector<Change> changes;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard lock(room_mutex_);
        for (const auto& quality_update : update.updates()) {
            std::shared_ptr<Participant> participant;
            if (local_participant_ &&
                local_participant_->sid() == quality_update.participant_sid()) {
                participant = local_participant_;
            } else {
                const auto remote = remote_participants_.find(quality_update.participant_sid());
                if (remote != remote_participants_.end()) {
                    participant = remote->second;
                }
            }

            if (!participant) {
                Log("SIGNAL", "CONN_QUALITY_UNKNOWN_PARTICIPANT",
                    "忽略未知参会人的连接质量更新: " + quality_update.participant_sid());
                continue;
            }

            const auto quality = ConnectionQualityFromProto(quality_update.quality());
            const float score = quality_update.score();
            if (participant->connection_quality() == quality &&
                participant->connection_quality_score() == score) {
                continue;
            }

            participant->set_connection_quality(quality, score);
            changes.push_back({participant, quality, score});
            EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                ParticipantEventKind::ConnectionQuality,
                participant,
                participant.get() == local_participant_.get()));
        }
        if (!changes.empty()) {
            listeners_snapshot = listeners_;
        }
    }

    for (const auto& change : changes) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnConnectionQualityChanged(change.participant,
                                                     change.quality,
                                                     change.score);
            }
        }
    }
}

void Room::UpdateTrackStreamStates(const proto::StreamStateUpdate& update) {
    struct Change {
        std::shared_ptr<Participant> participant;
        std::shared_ptr<TrackPublication> publication;
        TrackPublication::StreamState state;
    };

    std::vector<Change> changes;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard lock(room_mutex_);
        for (const auto& stream_state : update.stream_states()) {
            std::shared_ptr<Participant> participant;
            if (local_participant_ &&
                local_participant_->sid() == stream_state.participant_sid()) {
                participant = local_participant_;
            } else {
                const auto remote = remote_participants_.find(stream_state.participant_sid());
                if (remote != remote_participants_.end()) {
                    participant = remote->second;
                }
            }

            const auto publication = participant
                ? participant->get_publication(stream_state.track_sid())
                : nullptr;
            if (!participant || !publication) {
                Log("SIGNAL", "STREAM_STATE_UNKNOWN_TRACK",
                    "忽略未知参会人或 Track 的流状态更新: participant=" +
                    stream_state.participant_sid() + ", track=" + stream_state.track_sid());
                continue;
            }

            const auto state = StreamStateFromProto(stream_state.state());
            if (publication->stream_state() == state) {
                continue;
            }

            publication->set_stream_state(state);
            changes.push_back({participant, publication, state});
            EnqueueParticipantEventLocked(MakeTrackEventLocked(
                ParticipantEventKind::TrackStreamState,
                participant,
                publication,
                participant.get() == local_participant_.get()));
        }
        if (!changes.empty()) {
            listeners_snapshot = listeners_;
        }
    }

    for (const auto& change : changes) {
        for (const auto& listener : listeners_snapshot) {
            if (!listener->ConsumesParticipantEvents()) {
                listener->OnTrackStreamStateChanged(change.participant,
                                                    change.publication,
                                                    change.state);
            }
        }
    }
}

void Room::UpdateTrackSubscriptionPermission(
    const proto::SubscriptionPermissionUpdate& update) {
    const TrackSubscriptionPermission permission{
        update.participant_sid(), update.track_sid(), update.allowed()};
    std::shared_ptr<Participant> participant;
    std::shared_ptr<TrackPublication> publication;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    bool changed = false;

    {
        std::lock_guard lock(room_mutex_);
        const auto key = std::make_pair(permission.participant_sid,
                                        permission.track_sid);
        const auto existing = track_subscription_permissions_.find(key);
        changed = existing == track_subscription_permissions_.end() ||
            existing->second != permission.allowed;
        if (!changed) {
            return;
        }
        track_subscription_permissions_[key] = permission.allowed;

        if (local_participant_ &&
            local_participant_->sid() == permission.participant_sid) {
            participant = local_participant_;
        } else {
            const auto remote = remote_participants_.find(permission.participant_sid);
            if (remote != remote_participants_.end()) {
                participant = remote->second;
            }
        }
        if (participant) {
            publication = participant->get_publication(permission.track_sid);
            if (publication) {
                publication->set_subscription_allowed(permission.allowed);
                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                    ParticipantEventKind::TrackSubscriptionPermission,
                    participant,
                    publication,
                    participant.get() == local_participant_.get()));
            }
        }
        listeners_snapshot = listeners_;
    }

    for (const auto& listener : listeners_snapshot) {
        if (!listener->ConsumesParticipantEvents()) {
            listener->OnTrackSubscriptionPermissionChanged(permission,
                                                           participant,
                                                           publication);
        }
    }
}

void Room::RecordPublishedTracks() {
    std::lock_guard lock(room_mutex_);
    published_track_records_.clear();

    if (!local_participant_) return;

    for (const auto& kv : local_participant_->tracks()) {
        if (pending_local_unpublishes_.contains(kv.first)) {
            continue;
        }
        const auto& pub = kv.second;
        if (pub && pub->track()) {
            published_track_records_.push_back({
                pub->track(),
                pub->sid()
            });
        }
    }
}

asio::awaitable<void> Room::RepublishLocalTracks(
    std::shared_ptr<proto::ReconnectResponse> reconnect_response) {

    std::vector<PublishedTrackRecord> records;
    std::shared_ptr<LocalParticipant> local;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard lock(room_mutex_);
        records = published_track_records_;
        local = local_participant_;
        listeners_snapshot = listeners_;
    }

    if (!local) co_return;

    for (auto& record : records) {
        if (!record.track) continue;

        auto new_pub = co_await local->PublishTrackAsync(record.track);
        for (const auto& listener : listeners_snapshot) {
            listener->OnLocalTrackRepublished(record.previous_sid, new_pub);
        }
    }
}

asio::awaitable<void> Room::RestartIceConnections(
    std::shared_ptr<proto::ReconnectResponse> reconnect_response,
    std::chrono::milliseconds attempt_timeout) {
    if (!reconnect_response) {
        throw OperationError(OperationKind::Reconnect,
                             OperationErrorCode::JoinRejected,
                             "reconnect_response",
                             "ReconnectResponse is missing");
    }

    // Signaling-only mode is deliberately supported by tests and headless users.
    // A successful reconnect transaction ends at the signaling SyncState ACK in
    // this mode; there is no media transport whose ICE state can be restarted.
    if (!require_media_connection_) {
        co_return;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration new_config;
    new_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    new_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    new_config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
    new_config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyEnabled;
    if (reconnect_response->has_client_configuration() &&
        reconnect_response->client_configuration().force_relay() == proto::ClientConfigSetting::ENABLED) {
        new_config.type = webrtc::PeerConnectionInterface::kRelay;
    }
    for (int i = 0; i < reconnect_response->ice_servers_size(); ++i) {
        const auto& ice_srv = reconnect_response->ice_servers(i);
        webrtc::PeerConnectionInterface::IceServer server;
        for (int j = 0; j < ice_srv.urls_size(); ++j) {
            server.urls.push_back(ice_srv.urls(j));
        }
        server.username = ice_srv.username();
        server.password = ice_srv.credential();
        new_config.servers.push_back(server);
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    uint64_t generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
        generation = session_generation_.load(std::memory_order_acquire);
    }

    if (!pub_pc) {
        throw OperationError(OperationKind::Reconnect,
                             OperationErrorCode::PeerConnectionCreateFailed,
                             "restart_ice",
                             "publisher peer connection is missing");
    }

    std::string configuration_error;
    WebRTCManager::Instance().signaling_thread()->BlockingCall([pub_pc, sub_pc, &new_config, &configuration_error]() {
        auto pub_result = pub_pc->SetConfiguration(new_config);
        if (!pub_result.ok()) configuration_error = pub_result.message();
        if (sub_pc && sub_pc != pub_pc) {
            auto sub_result = sub_pc->SetConfiguration(new_config);
            if (!sub_result.ok() && configuration_error.empty()) {
                configuration_error = sub_result.message();
            }
        }
    });
    if (!configuration_error.empty()) {
        throw OperationError(OperationKind::Reconnect,
                             OperationErrorCode::PeerConnectionCreateFailed,
                             "set_reconnect_configuration",
                             configuration_error,
                             true);
    }

    const auto deadline = std::chrono::steady_clock::now() + attempt_timeout;
    co_await NegotiatePublisherAsync(attempt_timeout,
                                     generation,
                                     true);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        throw OperationError(OperationKind::Reconnect,
                             OperationErrorCode::PeerConnectionTimeout,
                             "restart_ice",
                             "reconnect attempt timed out before peer connection recovery",
                             true);
    }
    co_await WaitForPrimaryPeerConnection(
        std::min(operation_timeouts_.peer_connection, remaining), generation);
}

namespace {

void AppendStatsReport(RoomStatsReport& room_report,
                       const StatsReport& stats,
                       bool publisher) {
    for (const auto& candidate_pair : stats.candidate_pairs) {
        if (!candidate_pair.current_pair) {
            continue;
        }
        if (publisher) {
            room_report.publisher_rtt_ms =
                candidate_pair.current_round_trip_time * 1000.0;
            room_report.available_outgoing_bitrate =
                candidate_pair.available_outgoing_bitrate;
        } else {
            room_report.subscriber_rtt_ms =
                candidate_pair.current_round_trip_time * 1000.0;
        }
    }

    if (publisher) {
        for (const auto& outbound : stats.outbound_rtp) {
            room_report.total_bytes_sent += outbound.bytes_sent;
        }
    } else {
        for (const auto& inbound : stats.inbound_rtp) {
            room_report.total_bytes_received += inbound.bytes_received;
        }
    }
    room_report.reports.push_back(stats);
}

} // namespace

RoomStatsReport Room::GetStatsSync() {
    RoomStatsReport room_report;
    room_report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;

    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
    }

    if (pub_pc) {
        auto state = std::make_shared<RtcStatsState>();
        auto pub_cb = RtcStatsCollectorBridge::Create(state);

        WebRTCManager::Instance().signaling_thread()->PostTask([pub_pc, pub_cb]() {
            pub_pc->GetStats(pub_cb.get());
        });

        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock, std::chrono::milliseconds(1500), [&]() { return state->done; })) {
            AppendStatsReport(room_report, state->report, /*publisher=*/true);
        }
    }

    if (sub_pc) {
        auto state = std::make_shared<RtcStatsState>();
        auto sub_cb = RtcStatsCollectorBridge::Create(state);

        WebRTCManager::Instance().signaling_thread()->PostTask([sub_pc, sub_cb]() {
            sub_pc->GetStats(sub_cb.get());
        });

        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock, std::chrono::milliseconds(1500), [&]() { return state->done; })) {
            AppendStatsReport(room_report, state->report, /*publisher=*/false);
        }
    }

    return room_report;
}

asio::awaitable<RoomStatsReport> Room::GetStats() {
    RoomStatsReport room_report;
    room_report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    {
        std::lock_guard lock(room_mutex_);
        publisher = publisher_pc_;
        subscriber = subscriber_pc_;
    }

    using namespace asio::experimental::awaitable_operators;
    auto [publisher_stats, subscriber_stats] = co_await (
        CollectRtcStats(publisher, executor_) &&
        CollectRtcStats(subscriber, executor_));

    if (publisher_stats) {
        AppendStatsReport(room_report, *publisher_stats, /*publisher=*/true);
    }
    if (subscriber_stats) {
        AppendStatsReport(room_report, *subscriber_stats, /*publisher=*/false);
    }
    co_return room_report;
}

void Room::SetParticipantVolume(const std::string& identity_or_sid, double volume) {
    std::string changed_identity;
    {
        std::lock_guard lock(room_mutex_);
        for (auto& [sid, p] : remote_participants_) {
            if (p->identity() == identity_or_sid || p->sid() == identity_or_sid) {
                for (auto& [tsid, pub] : p->tracks()) {
                    if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                        pub->track()->set_volume(volume);
                        changed_identity = p->identity();
                    }
                }
            }
        }
    }
    if (!changed_identity.empty()) {
        Log("MEDIA", "VOLUME_SET", "已设置参会人 [" + changed_identity + "] 音量为 " + std::to_string(static_cast<int>(volume * 100)) + "%");
    }
}

void Room::SetParticipantMuted(const std::string& identity_or_sid, bool muted) {
    std::string changed_identity;
    {
        std::lock_guard lock(room_mutex_);
        for (auto& [sid, p] : remote_participants_) {
            if (p->identity() == identity_or_sid || p->sid() == identity_or_sid) {
                for (auto& [tsid, pub] : p->tracks()) {
                    if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                        pub->track()->set_muted(muted);
                        changed_identity = p->identity();
                    }
                }
            }
        }
    }
    if (!changed_identity.empty()) {
        Log("MEDIA", "MUTE_SET", "已设置参会人 [" + changed_identity + "] 本地静音=" + (muted ? "true" : "false"));
    }
}

void Room::SetAudioOutputMuted(bool muted) {
    {
        std::lock_guard lock(room_mutex_);
        audio_output_muted_ = muted;
        for (auto& [sid, p] : remote_participants_) {
            for (auto& [tsid, pub] : p->tracks()) {
                if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                    pub->track()->set_muted(muted);
                }
            }
        }
    }
    Log("MEDIA", "SPEAKER_MUTE", "已设置房间音频播放输出静音=" + std::string(muted ? "true" : "false"));
}

void Room::SimulateScenario(SimulateScenarioType scenario) {
    auto self = shared_from_this();
    livekit::safe_co_spawn(executor_, [self, scenario]() -> asio::awaitable<void> {
        co_await self->SimulateScenarioAsync(scenario);
    });
}

asio::awaitable<void> Room::SimulateScenarioAsync(SimulateScenarioType scenario) {
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<LocalParticipant> local;
    std::shared_ptr<E2eeManager> e2ee;
    {
        std::lock_guard lock(room_mutex_);
        signal = signal_client_;
        local = local_participant_;
        e2ee = e2ee_manager_;
    }

    switch (scenario) {
    case SimulateScenarioType::SignalReconnect: {
        Log("SIMULATE", "SIGNAL_RECONNECT", "触发信令断开以模拟软重连 (Soft Reconnect / Resume)");
        HandleSignalEvent(SignalEvent{SignalEvent::Close, nullptr, "simulated signal reconnect"});
        break;
    }
    case SimulateScenarioType::FullReconnect: {
        Log("SIMULATE", "FULL_RECONNECT", "触发模拟硬重连 (Hard Reconnect / Full Restart)");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_leave_request_full_reconnect(true);
            signal->Send(req);
        }
        HandleSignalEvent(SignalEvent{SignalEvent::Close, nullptr, "simulated full reconnect"});
        break;
    }
    case SimulateScenarioType::SpeakerUpdate: {
        Log("SIMULATE", "SPEAKER_UPDATE", "发送 SimulateScenario.speaker_update = 3");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_speaker_update(3);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::NodeFailure: {
        Log("SIMULATE", "NODE_FAILURE", "发送 SimulateScenario.node_failure = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_node_failure(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::Migration: {
        Log("SIMULATE", "MIGRATION", "发送 SimulateScenario.migration = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_migration(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::ServerLeave: {
        Log("SIMULATE", "SERVER_LEAVE", "发送 SimulateScenario.server_leave = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_server_leave(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::SwitchCandidate: {
        Log("SIMULATE", "SWITCH_CANDIDATE", "发送 SimulateScenario.switch_candidate_protocol = TCP");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_switch_candidate_protocol(proto::CandidateProtocol::TCP);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::E2eeKeyRatchet: {
        Log("SIMULATE", "E2EE_RATCHET", "触发 E2EE 密钥步进 (Key Ratchet)");
        if (e2ee) {
            e2ee->RatchetKey();
            Log("SIMULATE", "E2EE_RATCHET", "E2EE 密钥已步进更新");
        } else {
            Log("SIMULATE", "E2EE_RATCHET", "当前未启用 E2EE 管理器，忽略 Ratchet 操作");
        }
        break;
    }
    case SimulateScenarioType::ParticipantName: {
        std::string new_name = "Simulated " + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 10000);
        Log("SIMULATE", "PARTICIPANT_NAME", "模拟本地参会人改名: " + new_name);
        if (local) {
            local->set_name(new_name);
            if (signal) {
                proto::SignalRequest req;
                auto* meta = req.mutable_update_metadata();
                meta->set_name(new_name);
                signal->Send(req);
            }
        }
        break;
    }
    case SimulateScenarioType::ParticipantMetadata: {
        std::string new_meta = "{\"simulated\":true,\"ts\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + "}";
        Log("SIMULATE", "PARTICIPANT_METADATA", "模拟本地参会人元数据更新: " + new_meta);
        if (local) {
            local->set_metadata(new_meta);
            if (signal) {
                proto::SignalRequest req;
                auto* meta = req.mutable_update_metadata();
                meta->set_metadata(new_meta);
                signal->Send(req);
            }
        }
        break;
    }
    case SimulateScenarioType::Clear: {
        Log("SIMULATE", "CLEAR", "发送 SimulateScenario.subscriber_bandwidth = 0 清除人为限速与限制");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_subscriber_bandwidth(0);
            signal->Send(req);
        }
        break;
    }
    }
    co_return;
}

} // namespace livekit
