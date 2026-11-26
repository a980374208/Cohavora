#include "detail/room_internal.h"
#include "webrtc_manager.h"
#include "stats_collector.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "render/remote_video_frame_pipeline.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include <asio/experimental/awaitable_operators.hpp>
#include "api/jsep.h"
#include "pc/session_description.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#include <cstdlib>
#endif
#include <future>
#include <limits>

namespace livekit {

namespace {

TrackSource TrackSourceFromProto(proto::TrackSource source) {
    switch (source) {
    case proto::TrackSource::CAMERA: return TrackSource::Camera;
    case proto::TrackSource::MICROPHONE: return TrackSource::Microphone;
    case proto::TrackSource::SCREEN_SHARE: return TrackSource::ScreenShareVideo;
    case proto::TrackSource::SCREEN_SHARE_AUDIO: return TrackSource::ScreenShareAudio;
    default: return TrackSource::Unknown;
    }
}

ParticipantKind ParticipantKindFromProto(proto::ParticipantInfo::Kind kind) {
    switch (kind) {
    case proto::ParticipantInfo::INGRESS: return ParticipantKind::Ingress;
    case proto::ParticipantInfo::EGRESS: return ParticipantKind::Egress;
    case proto::ParticipantInfo::SIP: return ParticipantKind::Sip;
    case proto::ParticipantInfo::AGENT: return ParticipantKind::Agent;
    case proto::ParticipantInfo::CONNECTOR: return ParticipantKind::Connector;
    case proto::ParticipantInfo::BRIDGE: return ParticipantKind::Bridge;
    case proto::ParticipantInfo::STANDARD:
    default: return ParticipantKind::Standard;
    }
}

std::vector<PublishedVideoLayer> PublishedVideoLayersFromProto(
    const proto::TrackInfo& track) {
    const auto convert = [](const auto& advertised_layers) {
        std::vector<PublishedVideoLayer> layers;
        layers.reserve(advertised_layers.size());
        for (const auto& advertised : advertised_layers) {
            PublishedVideoQuality quality;
            switch (advertised.quality()) {
            case proto::VideoQuality::LOW: quality = PublishedVideoQuality::Low; break;
            case proto::VideoQuality::MEDIUM: quality = PublishedVideoQuality::Medium; break;
            case proto::VideoQuality::HIGH: quality = PublishedVideoQuality::High; break;
            default: continue;
            }
            if (!advertised.width() || !advertised.height()) continue;
            layers.push_back({quality, advertised.width(), advertised.height(),
                              advertised.rid()});
        }
        return layers;
    };
    // Use one codec's advertised layers. Backup-codec layers may have different
    // dimensions and quality labels and must not enter the same candidate set.
    for (const auto& codec : track.codecs()) {
        const auto& expected = track.mime_type();
        const auto& actual = codec.mime_type();
        if (expected.empty() || expected.size() != actual.size() ||
            !std::equal(expected.begin(), expected.end(), actual.begin(),
                [](unsigned char left, unsigned char right) {
                    return std::tolower(left) == std::tolower(right);
                })) continue;
        auto layers = convert(codec.layers());
        if (!layers.empty()) return layers;
    }
    return convert(track.layers());
}

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

} // namespace

class RoomPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    RoomPeerConnectionObserver(std::shared_ptr<Room> room, int pc_type, uint64_t generation)
        : room_(room), pc_type_(pc_type), generation_(generation) {}

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
        if (!data_channel) return;
        if (auto room = room_.lock()) {
            room->callback_gate_->Post([room, data_channel, generation = generation_]() {
                room->OnRemoteDataChannel(data_channel, generation);
            });
        }
    }
    
    void OnRenegotiationNeeded() override {
        if (auto room = room_.lock()) {
            room->callback_gate_->Post([room, type = pc_type_, generation = generation_]() {
                room->OnRenegotiationNeeded(type, generation);
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
            room->Log("WEBRTC", "ICE_STATE", "PC (" + std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") + ") ICE state changed to: " + state_str);
            if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
                new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
                room->callback_gate_->Post([room, generation = generation_]() {
                    room->OnIceConnected(generation);
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
            room->Log("WEBRTC", "PC_STATE", "PC (" + std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") + ") transport state changed to: " + state_str);
            room->callback_gate_->Post([room, type = pc_type_, new_state, generation = generation_]() {
                room->OnPeerConnectionStateChanged(type, new_state, generation);
            });
        }
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        std::string sdp_mid = candidate->sdp_mid();
        int sdp_mline_index = candidate->sdp_mline_index();

        if (auto room = room_.lock()) {
            room->Log("SIGNAL", "LOCAL_ICE", "Local ICE candidate collected: target=" +
                std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") +
                ", detail=[omitted]");
            room->callback_gate_->Post([room, sdp, sdp_mid, sdp_mline_index, type = pc_type_, generation = generation_]() {
                room->OnLocalIceCandidate(sdp, sdp_mid, sdp_mline_index, type, generation);
            });
        }
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        if (auto room = room_.lock()) {
            if (transceiver && transceiver->receiver()) {
                auto receiver = transceiver->receiver();
                auto track = receiver->track();
                room->Log("WEBRTC", "ON_TRACK", "WebRTC Transceiver OnTrack (Kind: " + (track ? std::string(track->kind()) : "null") + ", ID: " + (track ? track->id() : "null") + ")");
                room->PostRemoteTrack(receiver, track, generation_);
            }
        }
    }

    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {
        if (auto room = room_.lock()) {
            if (receiver) {
                auto track = receiver->track();
                room->Log("WEBRTC", "ON_ADD_TRACK", "WebRTC Receiver OnAddTrack (Kind: " + (track ? std::string(track->kind()) : "null") + ", ID: " + (track ? track->id() : "null") + ")");
                room->PostRemoteTrack(receiver, track, generation_);
            }
        }
    }

private:
    std::weak_ptr<Room> room_;
    int pc_type_; // 0 = Publisher, 1 = Subscriber
    const uint64_t generation_;
};

class RoomDataChannelObserver : public webrtc::DataChannelObserver {
public:
    RoomDataChannelObserver(std::shared_ptr<Room> room,
                            RoomDataChannelRole role,
                            uint64_t generation,
                            webrtc::DataChannelInterface* channel)
        : room_(room), role_(role), generation_(generation), channel_(channel) {}

    void OnStateChange() override {
        if (!channel_) return;
        const auto state = channel_->state();
        if (auto room = room_.lock()) {
            room->callback_gate_->Post(
                [room, role = role_, state, generation = generation_,
                 channel = channel_]() {
                    if (role == RoomDataChannelRole::DataTrack) {
                        room->OnDataTrackChannelStateChanged(state, generation, channel);
                    } else {
                        room->OnDataChannelStateChanged(
                            role == RoomDataChannelRole::Reliable, state, generation, channel);
                    }
                });
        }
    }

    void OnMessage(const webrtc::DataBuffer& buffer) override {
        if (auto room = room_.lock()) {
            if (role_ == RoomDataChannelRole::DataTrack) {
                if (!buffer.binary) {
                    return;
                }
                std::vector<uint8_t> payload(buffer.data.data(), buffer.data.data() + buffer.data.size());
                room->callback_gate_->Post([room, payload = std::move(payload), generation = generation_]() {
                    room->OnIncomingDataTrackPacket(payload, generation);
                });
                return;
            }
            std::vector<uint8_t> payload(buffer.data.data(), buffer.data.data() + buffer.data.size());
            room->callback_gate_->Post([room, payload = std::move(payload), generation = generation_]() {
                room->OnIncomingDataPacket(payload, "", "", generation);
            });
        }
    }

    void OnBufferedAmountChange(uint64_t previous_amount) override {
        if (auto room = room_.lock()) {
            room->callback_gate_->Post([room, previous_amount, role = role_, generation = generation_]() {
                if (role == RoomDataChannelRole::DataTrack) {
                    room->OnDataTrackChannelBufferedAmountLow(generation);
                } else {
                    room->OnDataChannelBufferedAmountLow(
                        previous_amount, role == RoomDataChannelRole::Reliable, generation);
                }
            });
        }
    }

private:
    std::weak_ptr<Room> room_;
    RoomDataChannelRole role_;
    const uint64_t generation_;
    webrtc::DataChannelInterface* const channel_;
};

std::shared_ptr<webrtc::PeerConnectionObserver> Room::CreatePeerConnectionObserver(int pc_type, uint64_t generation) {
    return std::make_shared<RoomPeerConnectionObserver>(shared_from_this(), pc_type, generation);
}

void Room::InitializePeerConnectionCapabilities(
    webrtc::PeerConnectionInterface* publisher, bool single_pc_mode, bool video_enabled) {
    if (!single_pc_mode || !video_enabled) return;

    // LiveKit 1.13.6's Pion MediaEngine keeps the codecs from the first video
    // m-line when MediaEngine copying is disabled. Advertise the complete
    // capability set before any strictly codec-limited local publication.
    // Inactive avoids allocating a downstream track without an MSID; Pion also
    // excludes a remote-inactive transceiver from AddTrack reuse. Actual media
    // keeps its own sendonly/recvonly transceivers and publication preferences.
    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kInactive;
    const auto result = publisher->AddTransceiver(webrtc::MediaType::VIDEO, init);
    if (!result.ok()) {
        throw OperationError(OperationKind::Connect,
                             OperationErrorCode::PeerConnectionCreateFailed,
                             "create_video_capability_transceiver",
                             result.error().message());
    }
}

std::shared_ptr<webrtc::DataChannelObserver> Room::CreateDataChannelObserver(
    RoomDataChannelRole role,
    uint64_t generation,
    webrtc::DataChannelInterface* channel) {
    return std::make_shared<RoomDataChannelObserver>(
        shared_from_this(), role, generation, channel);
}

void Room::PostRemoteTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                           webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, uint64_t generation) {
    // Protect asynchronous metadata resolution; duplicates reuse the same hook.
    EnsureReceiverCryptoGate(receiver, generation);
    callback_gate_->Post([weak = weak_from_this(), receiver, track, generation]() {
        if (auto room = weak.lock()) room->OnRemoteTrackAdded(receiver, track, generation);
    });
}

std::shared_ptr<MediaReceiverGate> Room::EnsureReceiverCryptoGate(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, uint64_t generation) {
    if (!receiver) return {};
    std::shared_ptr<MediaReceiverGate> gate;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            !e2ee_manager_ || !e2ee_manager_->enabled()) return {};
        std::erase_if(receiver_crypto_gates_, [](const auto& entry) { return entry.second.expired(); });
        auto& slot = receiver_crypto_gates_[{generation, receiver.get()}];
        if (auto existing = slot.lock()) return existing;
        gate = CreateMediaReceiverGate();
        slot = gate;
    }
    // RtpReceiver dispatches to its worker; never hold room_mutex_ here.
    receiver->SetFrameTransformer(gate->Hook());
    return gate;
}

bool Room::IsNativeGenerationCurrentLocked(uint64_t generation) const {
    // Zero is retained only for legacy synchronous API callers before Connect.
    return !retired() && generation == session_generation_.load(std::memory_order_acquire) &&
        generation == installed_session_generation_;
}

bool Room::AdmitListener(const ListenerDeliveryContext& context,
                         const std::shared_ptr<RoomListener>& listener) const {
    std::lock_guard lock(room_mutex_);
    // Zero is an actual pre-Connect generation here, never a wildcard.
    return !retired() && listener &&
        context.generation == session_generation_.load(std::memory_order_acquire) &&
        (!context.require_installed_owner || context.generation == installed_session_generation_) &&
        (!context.required_state || connection_state_ == *context.required_state) &&
        (!context.require_reconnect || (reconnect_active_ && !reconnect_disabled_)) &&
        (!context.e2ee_owner || context.e2ee_owner == e2ee_manager_) &&
        (!context.encryption_revision ||
            (context.e2ee_owner == e2ee_manager_ && (!context.e2ee_owner ||
             context.e2ee_owner->data_packet_state().policy_revision == *context.encryption_revision))) &&
        (!context.media_encryption_binding ||
            (!context.media_encryption_binding->retired.load(std::memory_order_acquire) &&
             (!context.media_encryption_binding->binding_active ||
              context.media_encryption_binding->binding_active->load(std::memory_order_acquire)))) &&
        std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end();
}

void Room::BeforeNativeEventCommit(uint64_t generation) {
    std::shared_ptr<ConnectAttemptInstrumentation> hooks;
    {
        std::lock_guard lock(room_mutex_);
        hooks = connect_attempt_instrumentation_;
    }
    if (hooks && hooks->before_native_event_commit) hooks->before_native_event_commit(generation);
}

void Room::DeliverRepublishedTrack(uint64_t generation, const std::string& previous_sid,
                                   std::shared_ptr<TrackPublication> publication) {
    std::vector<std::shared_ptr<RoomListener>> listeners;
    std::shared_ptr<ConnectAttemptInstrumentation> hooks;
    {
        std::lock_guard lock(room_mutex_);
        hooks = connect_attempt_instrumentation_;
        listeners = listeners_;
    }
    if (hooks && hooks->before_republish_listener_delivery) hooks->before_republish_listener_delivery(generation);
    for (const auto& listener : listeners) {
        try {
            DeliverListener({generation, ConnectionState::Connected, true, true}, listener,
                [&](RoomListener& target) { target.OnLocalTrackRepublished(previous_sid, publication); });
        } catch (...) {
            // An application listener cannot strand reconnect ownership cleanup.
        }
    }
}

const char* ToString(RoomDisconnectReason reason) {
    switch (reason) {
    case RoomDisconnectReason::UserLeave: return "USER_LEAVE";
    case RoomDisconnectReason::NetworkError: return "NETWORK_ERROR";
    case RoomDisconnectReason::ServerShutdown: return "SERVER_SHUTDOWN";
    case RoomDisconnectReason::DuplicateIdentity: return "DUPLICATE_IDENTITY";
    case RoomDisconnectReason::ParticipantRemoved: return "PARTICIPANT_REMOVED";
    case RoomDisconnectReason::RoomDeleted: return "ROOM_DELETED";
    case RoomDisconnectReason::RoomClosed: return "ROOM_CLOSED";
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
    case proto::ROOM_DELETED:
        return RoomDisconnectReason::RoomDeleted;
    case proto::ROOM_CLOSED:
        return RoomDisconnectReason::RoomClosed;
    default:
        return RoomDisconnectReason::Unknown;
    }
}

Room::Room(asio::any_io_executor executor, std::shared_ptr<void> executor_lifetime)
    : executor_lifetime_(std::move(executor_lifetime))
    , callback_gate_(std::make_shared<ExecutorCallbackGate>(executor, executor_lifetime_))
    , executor_(executor) {
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    const char* receive_probe = std::getenv("COHAVORA_RECEIVED_FRAME_DIAGNOSTIC");
    received_frame_diagnostics_enabled_ = receive_probe &&
        receive_probe[0] == '1' && receive_probe[1] == '\0';
#endif
    CrashHandler::InstallSignalHandlers();
}

void Room::Log(const std::string& cat, const std::string& tag, const std::string& msg) {
    auto admission = callback_gate_->Enter();
    if (!admission) return;
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

void Room::DeliverLifecycleListenerEvent(LifecycleListenerDelivery delivery) {
    std::shared_ptr<ConnectAttemptInstrumentation> instrumentation_hooks;
    {
        std::lock_guard lock(room_mutex_);
        instrumentation_hooks = connect_attempt_instrumentation_;
    }

    // Instrumentation can stop an accepted event after its state commit. Replacement
    // invalidation still happens in the production validation below.
    if (instrumentation_hooks && instrumentation_hooks->before_lifecycle_listener_delivery) {
        instrumentation_hooks->before_lifecycle_listener_delivery(
            delivery.generation, delivery.required_state);
    }

    for (const auto& listener : delivery.listeners) {
        try {
            DeliverListener({delivery.generation, delivery.required_state}, listener, [&](RoomListener& target) {
                switch (delivery.kind) {
                case LifecycleListenerEventKind::Connected:
                    target.OnConnected();
                    break;
                case LifecycleListenerEventKind::Disconnected:
                    target.OnDisconnected(delivery.reason, delivery.detail);
                    break;
                case LifecycleListenerEventKind::Reconnecting:
                    target.OnReconnecting();
                    break;
                case LifecycleListenerEventKind::Reconnected:
                    target.OnReconnected();
                    break;
                }
            });
        } catch (...) {
            // Listener failures cannot interrupt lifecycle ownership cleanup.
        }
    }
}

Room::~Room() {
    Disconnect();
}

void Room::Retire() {
    // Serialize repeated worker requests, but never hold room_mutex_ while
    // waiting for I/O callbacks or WebRTC proxy/owner-thread operations.
    std::unique_lock retire_lock(retirement_mutex_);
    retired_.store(true, std::memory_order_release);
    callback_gate_->Close();
    Disconnect();
    // Also cover a pre-connect Room whose Disconnect fast path has no
    // installed transport but still owns pending timers or receive buffers.
    CancelPendingOperations(OperationErrorCode::Cancelled, "retire", "room is retired");
    callback_gate_->WaitForDrain();
    std::vector<std::shared_ptr<RoomListener>> listeners;
    LogHandler log;
    {
        std::lock_guard lock(room_mutex_);
        listeners.swap(listeners_);
        log = std::move(log_handler_);
    }
    retire_lock.unlock(); // Application-owned callback destructors run unlocked.
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
    if (!retired() && listener) {
        listeners_.push_back(listener);
    }
}

void Room::RemoveListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard lock(room_mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

void Room::SetSessionTelemetry(
    std::weak_ptr<telemetry::SessionTelemetry> telemetry) {
    std::lock_guard lock(room_mutex_);
    session_telemetry_ = std::move(telemetry);
}


void Room::SetDiagnosticContext(diagnostic::Context context) {
    std::lock_guard lock(room_mutex_);
    diagnostic_context_ = context;
}

std::shared_ptr<diagnostic::SdpNegotiationTrace> Room::SdpTraceLocked(
    diagnostic::SdpRole role, uint64_t generation, bool new_round, bool ice_restart) {
    auto& current = role == diagnostic::SdpRole::Publisher
        ? publisher_sdp_trace_ : subscriber_sdp_trace_;
    if (current && !new_round) return current;
    if (current) current->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::Superseded);
    static std::atomic<uint64_t> next_round{0};
    auto context = diagnostic_context_;
    if (context.parent_operation_id.View().empty()) context.parent_operation_id = context.operation_id;
    context.operation_id.Assign("sdp_" + std::to_string(++next_round));
    context.room_generation = generation;
    context.has_room_generation = true;
    current = std::make_shared<diagnostic::SdpNegotiationTrace>(context, role, ice_restart);
    if (!new_round) current->Finish(diagnostic::Outcome::Failure, diagnostic::SdpReason::UnexpectedMessage);
    return current;
}

void Room::EmitDiagnostic(diagnostic::Event event) const noexcept {
    {
        std::lock_guard lock(room_mutex_);
        event.context.anonymous_session_id =
            diagnostic_context_.anonymous_session_id;
        if (event.context.parent_operation_id.View().empty())
            event.context.parent_operation_id =
                diagnostic_context_.parent_operation_id;
        event.context.session_generation =
            diagnostic_context_.session_generation;
        event.context.has_session_generation =
            diagnostic_context_.has_session_generation;
    }
    event.thread_role = diagnostic::ThreadRole::Session;
    diagnostic::EmitBusinessEvent(event);
}

void Room::EmitPublisherVideoFormat(
    diagnostic::VideoFormatDiagnosticRow format,
    uint64_t generation,
    const std::shared_ptr<diagnostic::SdpNegotiationTrace>& trace,
    diagnostic::ThreadRole thread) const noexcept {
    diagnostic::Context context;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            (trace && publisher_sdp_trace_ != trace)) return;
        context = diagnostic_context_;
    }
    if (trace) {
        trace->RecordVideoFormat(format, thread);
        return;
    }
    // Preferences and preflight precede CreateOffer, so there is no SDP round
    // yet. Zero sequence deliberately avoids inventing a negotiation round.
    diagnostic::Event event;
    event.SetVideoFormat(format);
    event.context = context;
    event.context.room_generation = generation;
    event.context.has_room_generation = true;
    event.sdp_role = diagnostic::SdpRole::Publisher;
    event.thread_role = thread;
    event.media_kind = diagnostic::MediaKind::Video;
    event.outcome = format.valid ? diagnostic::Outcome::Success : diagnostic::Outcome::Failure;
    diagnostic::EmitBusinessEvent(event);
}

void Room::TracePublisherSdpFormats(
    const std::string& sdp,
    bool answer,
    uint64_t generation,
    const std::shared_ptr<diagnostic::SdpNegotiationTrace>& trace) const noexcept {
    try {
        webrtc::SdpParseError ignored;
        const auto description = webrtc::CreateSessionDescription(
            answer ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer, sdp, &ignored);
        if (!description || !description->description()) return;
        const auto& contents = description->description()->contents();
        for (std::size_t index = 0; index < contents.size(); ++index) {
            const auto& content = contents[index];
            const auto* media = content.media_description();
            if (content.rejected || !media || !media->as_video()) continue;
            const auto direction = media->direction();
            // An answer's recv direction describes the publisher's sending
            // section. Single-PC incoming-only video belongs to subscribers.
            const bool sending = direction == webrtc::RtpTransceiverDirection::kSendRecv ||
                direction == (answer ? webrtc::RtpTransceiverDirection::kRecvOnly
                                     : webrtc::RtpTransceiverDirection::kSendOnly);
            if (!sending) continue;
            for (const auto& codec : media->codecs()) {
                if (auto row = VideoFormatRow(codec.name, codec.params,
                    answer ? diagnostic::VideoFormatSource::RemoteAnswer : diagnostic::VideoFormatSource::LocalOffer,
                    codec.id, index)) {
                    EmitPublisherVideoFormat(*row, generation, trace);
                }
            }
        }
    } catch (...) {
        // Diagnostic extraction cannot reject or mutate a business SDP.
    }
}

void Room::TracePublisherSenderFormats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    uint64_t generation,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) noexcept {
    try {
        auto* signaling = WebRTCManager::Instance().signaling_thread();
        auto ticket = callback_gate_->Enter();
        if (!pc || !signaling || signaling->IsQuitting() || !ticket) return;
        struct Task {
            std::shared_ptr<Room> room;
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
            uint64_t generation;
            ExecutorCallbackGate::Ticket ticket;
        };
        auto* task = new Task{shared_from_this(), std::move(pc), std::move(trace), generation, std::move(ticket)};
        try {
            // Trivial closure: the owned task is destroyed in this translation
            // unit across the product/WebRTC CRT boundary.
            signaling->PostTask([task] {
                std::unique_ptr<Task> owned(task);
                try {
                    {
                        std::lock_guard lock(task->room->room_mutex_);
                        if (!task->room->IsNativeGenerationCurrentLocked(task->generation) ||
                            task->room->publisher_pc_ != task->pc ||
                            task->room->publisher_sdp_trace_ != task->trace) return;
                    }
                    const auto* remote = task->pc->remote_description();
                    for (const auto& transceiver : task->pc->GetTransceivers()) {
                        if (!transceiver) continue;
                        const auto sender = transceiver->sender();
                        const auto track = sender ? sender->track() : nullptr;
                        const auto direction = transceiver->current_direction();
                        if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind ||
                            !direction || (*direction != webrtc::RtpTransceiverDirection::kSendOnly &&
                                           *direction != webrtc::RtpTransceiverDirection::kSendRecv)) continue;
                        std::size_t media_index = 65535;
                        const auto mid = transceiver->mid();
                        if (mid && remote && remote->description()) {
                            const auto& contents = remote->description()->contents();
                            for (std::size_t index = 0; index < contents.size(); ++index) {
                                if (contents[index].mid() == *mid) { media_index = index; break; }
                            }
                        }
                        // Follow the same configured selection as the final
                        // Level check; actual emitted RTP still requires stats.
                        const auto parameters = sender->GetParameters();
                        std::set<std::tuple<int, std::uint32_t, int, int>> emitted;
                        for (const auto& encoding : parameters.encodings) {
                            const webrtc::RtpCodec* selected = encoding.codec ? &*encoding.codec :
                                (parameters.codecs.empty() ? nullptr : &parameters.codecs.front());
                            if (!selected) continue;
                            int payload = -1;
                            for (const auto& codec : parameters.codecs) {
                                if (CompareCodecName(codec.name.c_str(), selected->name.c_str()) == 0 &&
                                    codec.parameters == selected->parameters) { payload = codec.payload_type; break; }
                            }
                            if (auto row = VideoFormatRow(selected->name, selected->parameters,
                                diagnostic::VideoFormatSource::SenderParameters, payload, media_index)) {
                                if (!emitted.emplace(row->payload_type, row->profile_level_id,
                                    row->packetization_mode, row->level_asymmetry).second) continue;
                                task->room->EmitPublisherVideoFormat(*row, task->generation,
                                    task->trace, diagnostic::ThreadRole::Rtc);
                            }
                        }
                    }
                } catch (...) {}
            });
        } catch (...) {
            delete task;
        }
    } catch (...) {}
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
    event.native_room_generation = state->key.native_room_generation;
    event.participant.key = state->key;
    event.participant.ticket = state;
    event.participant.state = participant->SnapshotState();
    const auto publications = participant->tracks();
    event.participant.publications.reserve(publications.size());
    for (const auto& [_, publication] : publications) {
        auto publication_state = EnsureTrackMembershipLocked(participant, publication);
        if (!publication_state || !publication) continue;
        event.participant.publications.push_back({
            publication_state->key,
            publication_state,
            publication->SnapshotState()});
    }
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

void Room::RetireAllMembershipsLocked(std::deque<QueuedParticipantEvent>& retired_events) {
    for (auto& [participant, state] : participant_memberships_) {
        state->active.store(false, std::memory_order_release);
    }
    for (auto& [publication, state] : track_memberships_) {
        state->active.store(false, std::memory_order_release);
    }
    participant_memberships_.clear();
    track_memberships_.clear();
    active_transcription_segments_.clear();
    active_transcription_streams_.clear();
    // The caller supplies an empty local queue and releases its payloads after
    // the Room transaction. Keep the scheduled flag: a posted/running drainer
    // still owns dispatch, including any successor events added before it ends.
    retired_events.swap(participant_events_);
}

void Room::EnqueueParticipantEventLocked(ParticipantEvent event,
                                        std::shared_ptr<E2eeManager> e2ee_owner,
                                        std::optional<uint64_t> encryption_revision) {
    event.native_room_generation = session_generation_.load(std::memory_order_acquire);
    event.event_sequence = next_participant_event_sequence_++;
    if (encryption_revision) {
        event.sender.encryption_revision = *encryption_revision;
        event.sender.encryption_current = [weak = weak_from_this(),
            owner = std::weak_ptr<E2eeManager>(e2ee_owner),
            had_owner = bool(e2ee_owner), revision = *encryption_revision,
            generation = event.native_room_generation] {
            auto room = weak.lock();
            if (!room) return false;
            std::lock_guard lock(room->room_mutex_);
            auto policy = owner.lock();
            return room->IsNativeGenerationCurrentLocked(generation) &&
                (!had_owner || policy) && policy == room->e2ee_manager_ &&
                (!policy || policy->data_packet_state().policy_revision == revision);
        };
    }
    participant_events_.emplace_back(std::move(event), std::move(e2ee_owner), encryption_revision);
    if (participant_event_drain_scheduled_) return;

    participant_event_drain_scheduled_ = true;
    std::weak_ptr<Room> weak = weak_from_this();
    callback_gate_->Post([weak] {
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
            const auto remote_publication =
                std::dynamic_pointer_cast<RemoteTrackPublication>(publication);
            if (!track || !track->rtc_track() || !remote_publication ||
                !remote_publication->is_subscribed()) {
                continue;
            }
            const auto current =
                current_remote_binding_serials_.find(remote_publication.get());
            const auto media_binding = current == current_remote_binding_serials_.end()
                ? nullptr : FindMediaBindingLocked(current->second);
            if (!media_binding ||
                !media_binding->active.load(std::memory_order_acquire)) {
                continue;
            }
            auto available = MakeTrackEventLocked(
                ParticipantEventKind::TrackAvailable,
                participant,
                publication,
                false);
            available.media_binding_key = media_binding->key;
            available.media_binding_ticket = media_binding;
            EnqueueParticipantEventLocked(std::move(available));
        }
        for (const auto& [dt_sid, dt] : participant->data_tracks()) {
            if (!dt) continue;
            auto dt_evt = MakeParticipantEventLocked(
                ParticipantEventKind::DataTrackPublished, participant, false);
            dt_evt.data_track = dt;
            dt_evt.data_track_sid = dt_sid;
            EnqueueParticipantEventLocked(std::move(dt_evt));
        }
    }
}

void Room::DrainParticipantEvents() {
    std::vector<QueuedParticipantEvent> batch;
    std::vector<std::shared_ptr<RoomListener>> listeners;
    {
        std::lock_guard lock(room_mutex_);
        if (participant_event_drain_paused_) {
            ++participant_event_paused_attempts_;
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
            if (event.kind == ParticipantEventKind::TrackAvailable &&
                !IsMediaBindingTicketActive(
                    event.media_binding_ticket, event.media_binding_key)) {
                break;
            }
            try {
                DeliverListener({event.native_room_generation, {}, false, false,
                                 event.e2ee_owner, {}, event.encryption_revision}, listener,
                    [&](RoomListener& target) {
                        if (event.kind != ParticipantEventKind::TrackAvailable ||
                            IsMediaBindingTicketActive(
                                event.media_binding_ticket, event.media_binding_key)) {
                            target.OnParticipantEvent(event);
                        }
                    });
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
    callback_gate_->Post([weak] {
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

std::pair<std::shared_ptr<Participant>, std::shared_ptr<TrackPublication>>
Room::ResolveTranscriptionTargetLocked(
    const std::string& track_sid,
    const std::string& participant_identity) const {
    auto find_in_participant = [&](const std::shared_ptr<Participant>& p)
        -> std::pair<std::shared_ptr<Participant>, std::shared_ptr<TrackPublication>> {
        if (!p) return {nullptr, nullptr};
        if (!track_sid.empty()) {
            if (auto pub = p->get_publication(track_sid)) {
                return {p, std::move(pub)};
            }
        }
        return {nullptr, nullptr};
    };

    if (!track_sid.empty()) {
        if (auto match = find_in_participant(local_participant_); match.first) {
            return match;
        }
        for (const auto& [sid, candidate] : remote_participants_) {
            (void)sid;
            if (auto match = find_in_participant(candidate); match.first) {
                return match;
            }
        }
    }

    if (!participant_identity.empty()) {
        std::shared_ptr<Participant> matched_participant;
        if (local_participant_ && local_participant_->identity() == participant_identity) {
            matched_participant = local_participant_;
        } else {
            for (const auto& [sid, candidate] : remote_participants_) {
                (void)sid;
                if (candidate && candidate->identity() == participant_identity) {
                    matched_participant = candidate;
                    break;
                }
            }
        }
        if (matched_participant) {
            std::shared_ptr<TrackPublication> matched_pub =
                track_sid.empty() ? nullptr : matched_participant->get_publication(track_sid);
            return {std::move(matched_participant), std::move(matched_pub)};
        }
    }

    return {nullptr, nullptr};
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
    auto operation_admission = AdmitOperation(OperationKind::Connect, "ConnectAsync");
    auto self = shared_from_this();
    std::chrono::steady_clock::time_point connect_accepted_at;
    const std::string attempt_url = url;
    const std::string attempt_token = token;
    const SignalOptions attempt_options = opts;
    uint64_t generation = 0;
    bool notify_connected = true;
    bool restart_connect = false;
    std::shared_ptr<SignalClient> attempt_signal;
    std::shared_ptr<ConnectAttemptInstrumentation> instrumentation_hooks;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    {
        std::lock_guard lock(room_mutex_);
        if (retired()) {
            throw OperationError(OperationKind::Connect, OperationErrorCode::SessionClosed,
                                 "connect_start", "room is retired");
        }
        if (connection_state_ != ConnectionState::Disconnected ||
            installed_session_generation_ != 0) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::InvalidState,
                                 "connect_start",
                                 "room is not disconnected");
        }
        // AttemptReconnect() 复用 ConnectAsync 做全量重建。若其间收到了
        // 服务端终态 LeaveRequest，绝不能在这里重置 reconnect_disabled_ 并
        // 重新加入房间；显式的新 Connect 调用则允许开始一个新会话。
        restart_connect = reconnect_active_;
        if (restart_connect && reconnect_disabled_) {
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
            full_reconnect_requested_ = false;
            reconnect_disabled_ = false;
            server_disconnect_finalizing_ = false;
            ResetSubscriptionSessionLocked(attempt_options.auto_subscribe);
        }
        generation = session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        connect_accepted_at = std::chrono::steady_clock::now();
        operation_timeouts_ = attempt_options.timeouts;
        require_media_connection_ = attempt_options.create_webrtc_pc;
        instrumentation_hooks = connect_attempt_instrumentation_;
        telemetry_owner = session_telemetry_.lock();
    }
    if (telemetry_owner) {
        telemetry_owner->RecordRoomConnectAccepted(generation, connect_accepted_at);
    }
    TelemetryOperationSpan connect_operation(
        restart_connect ? nullptr : std::move(telemetry_owner),
        telemetry::OperationKind::Connect);
    diagnostic::Event connect_event;
    connect_event.kind = diagnostic::EventKind::RoomConnectStarted;
    connect_event.context.room_generation = generation;
    connect_event.context.has_room_generation = true;
    connect_event.context.operation_id.Assign(connect_operation.id().empty()
        ? "room_connect_" + std::to_string(generation) : connect_operation.id());
    connect_event.stage = diagnostic::Stage::ConnectingRoom;
    EmitDiagnostic(connect_event);

    try {
        auto conn_res = co_await SignalClient::Connect(
            attempt_url, attempt_token, attempt_options, std::nullopt,
            [self, generation](const SignalEvent& event) {
            self->HandleSignalEvent(event, generation);
        });
        attempt_signal = conn_res.client;

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

        if (instrumentation_hooks && instrumentation_hooks->before_join_commit) {
            co_await instrumentation_hooks->before_join_commit(generation);
        }

        DataTrackCleanupBatch cross_room_cleanup;
        {
            std::lock_guard lock(room_mutex_);
            if (generation != session_generation_.load(std::memory_order_acquire) ||
                connection_state_ != ConnectionState::Connecting ||
                installed_session_generation_ != 0) {
                throw OperationError(OperationKind::Connect,
                                     OperationErrorCode::Cancelled,
                                     "join_commit",
                                     "connect operation was cancelled");
            }
        signal_client_ = attempt_signal;
        installed_session_generation_ = generation;
        join_response_ = join_res;
        if (e2ee_manager_ && e2ee_manager_->enabled()) {
            const auto& trailer = join_res->sif_trailer();
            e2ee_manager_->PrepareMediaSession({trailer.begin(), trailer.end()});
        }
        const std::string joined_room_sid = join_res->has_room()
            ? join_res->room().sid()
            : std::string{};
        if (restart_connect) {
            const bool room_changed = (subscription_room_sid_ != joined_room_sid);
            if (room_changed) {
                ResetSubscriptionSessionLocked(attempt_options.auto_subscribe);
                // This Connect is still owned by the active recovery transaction.
                // Default/new-room intents must wait for its final commit.
                subscription_recovery_barrier_ = true;
                // Reconnect records belong to the previous room only; discard cleanly
                // after releasing room_mutex_.
                TakeDataTrackCleanupLocked(/*clear_reconnect_records=*/true, cross_room_cleanup);
            } else {
                session_auto_subscribe_ = attempt_options.auto_subscribe;
                subscription_recovery_barrier_ = true;
                for (auto& [key, intent] : subscription_intents_) {
                    intent.sent_subscribed = session_auto_subscribe_;
                    intent.subscription_dirty =
                        (intent.subscribed != session_auto_subscribe_);
                    if (!intent.subscribed) {
                        intent.settings_dirty = false;
                        intent.accepted_at = {};
                    } else {
                        intent.accepted_at = connect_accepted_at;
                    }
                    if (!intent.subscription_dirty && !intent.settings_dirty) {
                        CancelPendingSubscriptionUpdateLocked(key);
                    } else {
                        QueueSubscriptionUpdateLocked(key);
                    }
                }
            }
        }
        subscription_room_sid_ = joined_room_sid;
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
            [self, generation](const proto::SignalRequest& req) {
                std::shared_ptr<SignalClient> signal;
                {
                    std::lock_guard lock(self->room_mutex_);
                    if (self->installed_session_generation_ != generation) return;
                    signal = self->signal_client_;
                }
                if (signal) signal->Send(req);
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
        local_participant_->set_kind(ParticipantKindFromProto(join_res->participant().kind()));
        local_participant_->set_attributes(std::map<std::string, std::string>(
            join_res->participant().attributes().begin(),
            join_res->participant().attributes().end()));

        // 绑定 LocalParticipant 发布 DataChannel 数据包 Handler
        local_participant_->SetPublishDataHandler(
            [self, generation](const std::vector<uint8_t>& payload, bool reliable,
                   const std::vector<std::string>& destination_identities, const std::string& topic) {
                self->PublishData(payload, reliable, destination_identities, topic, generation);
            }
        );

        // 绑定 LocalParticipant 发布 Native Track Handler
        local_participant_->SetAsyncPublishTrackHandler(
            [self, generation](std::shared_ptr<Track> track,
                   const proto::SignalRequest& request)
                -> asio::awaitable<std::shared_ptr<TrackPublication>> {
                co_return co_await self->PublishLocalTrackAsync(std::move(track), request, generation);
            }
        );

        local_participant_->SetAsyncPublishTracksBatchHandler(
            [self](std::vector<LocalParticipant::BatchTrackItem> items)
                -> asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> {
                co_return co_await self->PublishLocalTracksBatchAsync(std::move(items));
            }
        );

        BindLocalUnpublishHandler();
        BindLocalDataTrackHandlersLocked(generation);

        // 绑定 LocalParticipant 发送 RPC 请求 Handler
        local_participant_->SetSendRpcHandler(
            [self](const RpcPacket& packet) -> asio::awaitable<std::string> {
                co_return co_await self->SendRpcRequest(packet);
            }
        );
        }

        FinalizeDataTrackCleanup(
            std::move(cross_room_cleanup),
            OperationErrorCode::Cancelled,
            "reconnect switched room");

        if (attempt_options.create_webrtc_pc) {
            // Pin configuration while native creation runs outside room_mutex_.
            auto native_policy_admission = AcquireMediaPublishPolicy();
            // Creation can synchronously invoke WebRTC callbacks. Keep it outside
            // room_mutex_, and retain local ownership until the protected commit.
            struct NativeAttempt {
                std::shared_ptr<void> encryption_policy;
                std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
                std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
                std::vector<std::shared_ptr<webrtc::DataChannelObserver>> dc_observers;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
                webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable;
                webrtc::scoped_refptr<webrtc::DataChannelInterface> lossy;
                webrtc::scoped_refptr<webrtc::DataChannelInterface> data_track;
                ~NativeAttempt() {
                    if (reliable) { reliable->UnregisterObserver(); reliable->Close(); }
                    if (lossy) { lossy->UnregisterObserver(); lossy->Close(); }
                    if (data_track) { data_track->UnregisterObserver(); data_track->Close(); }
                    if (publisher) publisher->Close();
                    if (subscriber && subscriber != publisher) subscriber->Close();
                }
            } native;
            {
                std::lock_guard lock(room_mutex_);
                if (e2ee_manager_) native.encryption_policy = e2ee_manager_->AcquireMediaPublishPolicy();
            }
            if (WebRTCManager::Instance().Initialize()) {
                webrtc::PeerConnectionInterface::RTCConfiguration config;
                config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
                config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
                config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
                {
                    std::lock_guard lock(room_mutex_);
                    config.tcp_candidate_policy = disable_ice_tcp_for_inspection_
                        ? webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled
                        : webrtc::PeerConnectionInterface::kTcpCandidatePolicyEnabled;
                }
#if defined(COHAVORA_E2EE_MEDIA_GUARD)
                {
                    std::lock_guard lock(room_mutex_);
                    if ((e2ee_manager_ && e2ee_manager_->enabled()) ||
                        (connect_attempt_instrumentation_ && connect_attempt_instrumentation_->require_native_media_guard_without_cryptors)) {
                        auto crypto = webrtc::CryptoOptions::NoGcm();
                        crypto.sframe.require_frame_encryption = true;
                        config.crypto_options = crypto;
                    }
                }
#endif
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

                native.publisher_observer = CreatePeerConnectionObserver(0, generation);
                native.subscriber_observer = CreatePeerConnectionObserver(1, generation);

                webrtc::PeerConnectionDependencies pub_deps(native.publisher_observer.get());
                auto pub_res = WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(config, std::move(pub_deps));
                if (pub_res.ok()) {
                    native.publisher = pub_res.MoveValue();
                    // An explicit audio-only allow-list cannot answer a video
                    // capability section. The data transport must still join
                    // so publish admission can reject video without side effects.
                    const bool video_enabled = join_res->enabled_publish_codecs().empty() ||
                        std::any_of(join_res->enabled_publish_codecs().begin(),
                                    join_res->enabled_publish_codecs().end(), [](const auto& codec) {
                            std::string mime = codec.mime();
                            std::transform(mime.begin(), mime.end(), mime.begin(),
                                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                            return mime.rfind("video/", 0) == 0;
                        });
                    InitializePeerConnectionCapabilities(
                        native.publisher.get(), attempt_signal->is_single_pc_mode_active(), video_enabled);

                    webrtc::DataChannelInit rel_init;
                    rel_init.ordered = true;
                    native.reliable = native.publisher->CreateDataChannel("_reliable", &rel_init);
                    if (native.reliable) {
                        auto obs = CreateDataChannelObserver(
                            RoomDataChannelRole::Reliable, generation, native.reliable.get());
                        native.reliable->RegisterObserver(obs.get());
                        native.dc_observers.push_back(obs);
                    }

                    webrtc::DataChannelInit lossy_init;
                    lossy_init.ordered = false;
                    lossy_init.maxRetransmits = 0;
                    native.lossy = native.publisher->CreateDataChannel("_lossy", &lossy_init);
                    if (native.lossy) {
                        auto obs = CreateDataChannelObserver(
                            RoomDataChannelRole::Lossy, generation, native.lossy.get());
                        native.lossy->RegisterObserver(obs.get());
                        native.dc_observers.push_back(obs);
                    }

                    webrtc::DataChannelInit dt_init;
                    dt_init.ordered = false;
                    dt_init.maxRetransmits = 0;
                    native.data_track = native.publisher->CreateDataChannel("_data_track", &dt_init);
                    if (native.data_track) {
                        auto obs = CreateDataChannelObserver(
                            RoomDataChannelRole::DataTrack, generation, native.data_track.get());
                        native.data_track->RegisterObserver(obs.get());
                        native.dc_observers.push_back(obs);
                    }
                } else {
                    throw OperationError(OperationKind::Connect,
                                         OperationErrorCode::PeerConnectionCreateFailed,
                                         "create_publisher_pc",
                                         pub_res.error().message());
                }

                if (attempt_signal->is_single_pc_mode_active()) {
                    native.subscriber = native.publisher;
                    Log("WEBRTC", "SINGLE_PC", "Single PC mode enabled; subscriber_pc_ shares publisher_pc_");
                } else {
                    webrtc::PeerConnectionDependencies sub_deps(native.subscriber_observer.get());
                    auto sub_res = WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(config, std::move(sub_deps));
                    if (sub_res.ok()) {
                        native.subscriber = sub_res.MoveValue();
                        Log("WEBRTC", "SUB_PC_CREATED", "Subscriber PeerConnection created");
                    } else {
                        Log("ERROR", "SUB_PC_FAIL",
                            secure_log::OpaqueSummary("create_subscriber_pc"));
                        throw OperationError(OperationKind::Connect,
                                             OperationErrorCode::PeerConnectionCreateFailed,
                                             "create_subscriber_pc",
                                             sub_res.error().message());
                    }
                }

            } else {
                throw OperationError(OperationKind::Connect,
                                     OperationErrorCode::PeerConnectionCreateFailed,
                                     "initialize_webrtc",
                                     "WebRTCManager initialization failed");
            }
            std::lock_guard lock(room_mutex_);
            if (!IsNativeGenerationCurrentLocked(generation)) {
                throw OperationError(OperationKind::Connect, OperationErrorCode::Cancelled,
                                     "native_commit", "connect was replaced during native creation");
            }
            publisher_observer_ = std::move(native.publisher_observer);
            subscriber_observer_ = std::move(native.subscriber_observer);
            publisher_pc_ = std::move(native.publisher);
            subscriber_pc_ = std::move(native.subscriber);
            native_e2ee_policy_ = std::move(native.encryption_policy);
            reliable_dc_ = std::move(native.reliable);
            lossy_dc_ = std::move(native.lossy);
            data_track_dc_ = std::move(native.data_track);
            data_channel_observers_.insert(data_channel_observers_.end(),
                native.dc_observers.begin(), native.dc_observers.end());
            const bool data_publish_only_pc =
                join_res->participant().has_permission() &&
                join_res->participant().permission().can_publish_data() &&
                !join_res->participant().permission().can_subscribe();
            primary_pc_type_ = (attempt_signal->is_single_pc_mode_active() ||
                                data_publish_only_pc ||
                                attempt_options.eager_publish_data_channel)
                ? 0 : (join_res->subscriber_primary() ? 1 : 0);
        }

        UpdateParticipants(join_res->other_participants(), generation);
        {
            std::lock_guard lock(room_mutex_);
            if (IsNativeGenerationCurrentLocked(generation)) {
                PruneSubscriptionIntentsLocked();
            }
        }

    if (attempt_signal) {
        attempt_signal->SetEventReady();
        proto::SignalRequest perm_req;
        auto* perm = perm_req.mutable_subscription_permission();
        perm->set_all_participants(true);
        attempt_signal->Send(perm_req);
        Log("SIGNAL", "SUB_PERM", "Sent SubscriptionPermission to LiveKit (all_participants=true)");

        // JoinResponse determines which transport must be established eagerly.
        // subscriber-primary is lazy unless the server explicitly requests
        // fast_publish, the caller requests eager_publish_data_channel, or the
        // grant is data-publish-only (e.g. an Agent transcription publisher).
        const bool data_publish_only =
            join_res->participant().has_permission() &&
            join_res->participant().permission().can_publish_data() &&
            !join_res->participant().permission().can_subscribe();
        if (attempt_options.create_webrtc_pc &&
            (attempt_signal->is_single_pc_mode_active() ||
             !join_res->subscriber_primary() ||
             join_res->fast_publish() ||
             attempt_options.eager_publish_data_channel ||
             data_publish_only)) {
            co_await NegotiatePublisherAsync(attempt_options.timeouts.negotiation, generation);
        }
    }

    if (attempt_options.create_webrtc_pc) {
        co_await WaitForPrimaryPeerConnection(attempt_options.timeouts.peer_connection, generation);
    }

    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connecting ||
            installed_session_generation_ != generation) {
            throw OperationError(OperationKind::Connect,
                                 OperationErrorCode::Cancelled,
                                 "connect_commit",
                                 "connect operation was cancelled before commit");
        }
        connection_state_ = ConnectionState::Connected;
        notify_connected = !suppress_next_connected_event_;
        suppress_next_connected_event_ = false;
        EnqueueRosterLocked();
        ScheduleSubscriptionDrainLocked();
    }

    if (notify_connected) {
        LifecycleListenerDelivery delivery;
        delivery.kind = LifecycleListenerEventKind::Connected;
        delivery.generation = generation;
        delivery.required_state = ConnectionState::Connected;
        delivery.listeners = GetListenersSnapshot();
        DeliverLifecycleListenerEvent(std::move(delivery));
    }

    // Deliver initially discovered remote data tracks from JoinResponse after OnConnected.
    {
        std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<RemoteDataTrack>>>
            initial_data_tracks;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard lock(room_mutex_);
            if (IsNativeGenerationCurrentLocked(generation) &&
                connection_state_ == ConnectionState::Connected) {
                listeners_snapshot = listeners_;
                for (const auto& [sid, participant] : remote_participants_) {
                    (void)sid;
                    if (!participant) continue;
                    for (const auto& [dt_sid, dt] : participant->data_tracks()) {
                        (void)dt_sid;
                        if (dt) initial_data_tracks.emplace_back(participant, dt);
                    }
                }
            }
        }
        for (const auto& [p, dt] : initial_data_tracks) {
            for (const auto& listener : listeners_snapshot) {
                DeliverListener({generation}, listener, [&](RoomListener& target) {
                    target.OnDataTrackPublished(p, dt);
                }, true);
            }
        }
    }

    // The Room is externally connected before any delta accumulated during the
    // handshake is delivered. This prevents participant/track callbacks from
    // racing ahead of OnConnected while still preserving real post-Join deltas.
    FlushDeferredRoomMessages(generation);

        connect_operation.Finish(telemetry::OperationOutcome::Success);
        connect_event.kind = diagnostic::EventKind::RoomConnectTerminal;
        connect_event.outcome = diagnostic::Outcome::Success;
        connect_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connect_accepted_at).count();
        EmitDiagnostic(connect_event);
        co_return;
    } catch (...) {
        const auto operation_outcome = TelemetryOutcome(std::current_exception());
        std::shared_ptr<SignalClient> signal;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
        std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
        std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
        std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
        std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers;
        std::vector<RemoteTrackSinkBinding> track_sinks;
        std::deque<QueuedParticipantEvent> retired_events;
        PendingOperationCleanup pending;
        bool cleanup_shared_state = false;
        {
            std::lock_guard lock(room_mutex_);
            const bool is_current =
                generation == session_generation_.load(std::memory_order_acquire);
            const bool owns_installed_session =
                installed_session_generation_ == generation;
            cleanup_shared_state = is_current || owns_installed_session;
            if (is_current) {
                session_generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            if (owns_installed_session) {
                installed_session_generation_ = 0;
                signal = std::move(signal_client_);
                join_response_.reset();
                enabled_publish_codecs_.clear();
                RetireAllMembershipsLocked(retired_events);
                local_participant_.reset();
                pending_local_unpublishes_.clear();
                ClearRemotePublicationMediaBindingsLocked();
                remote_participants_.clear();
                remote_participant_joined_at_ms_.clear();
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
                if (data_track_dc_) {
                    data_channels.push_back(data_track_dc_);
                    data_track_dc_ = nullptr;
                }
                for (auto& dc : remote_data_channels_) {
                    if (dc) data_channels.push_back(dc);
                }
                remote_data_channels_.clear();
                data_channel_observers = std::move(data_channel_observers_);
                track_sinks = std::move(remote_track_sinks_);
                processed_remote_track_ids_.clear();
                pending_track_queue_.clear();
            }
            if (cleanup_shared_state) {
                deferred_room_messages_.clear();
                suppress_next_connected_event_ = false;
                connection_state_ = ConnectionState::Disconnected;
                pending = TakePendingOperationsLocked();
                if (!restart_connect) ClearSubscriptionSessionLocked();
            }
        }
        retired_events.clear();
        FailPendingOperations(std::move(pending),
                              OperationErrorCode::Cancelled,
                              "connect_rollback",
                              "connect transaction rolled back");
        if (signal) {
            signal->Close();
        } else if (attempt_signal) {
            // A stale attempt still owns the SignalClient that was never
            // installed (or was already detached by Disconnect). It must be
            // terminated even though it may no longer mutate Room state.
            attempt_signal->Close();
        }
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
        connect_operation.Finish(operation_outcome);
        connect_event.kind = diagnostic::EventKind::RoomConnectTerminal;
        connect_event.outcome = DiagnosticOutcome(operation_outcome);
        connect_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connect_accepted_at).count();
        ProjectOperationError(std::current_exception(), connect_event);
        EmitDiagnostic(connect_event);
        throw;
    }
}

void Room::Disconnect() {
    const auto disconnect_accepted_at = std::chrono::steady_clock::now();
    std::deque<QueuedParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    PendingOperationCleanup pending;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    uint64_t disconnected_generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected &&
            installed_session_generation_ == 0 &&
            !reconnect_active_) {
            retired_events.swap(participant_events_);
            return; // The lock is destroyed before the local payload queue.
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = RoomDisconnectReason::UserLeave;
        reconnect_disabled_ = true;
        reconnect_active_ = false;
        disconnected_generation =
            session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        installed_session_generation_ = 0;
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
        remote_participant_joined_at_ms_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        if (data_track_dc_) {
            data_channels.push_back(data_track_dc_);
            data_track_dc_ = nullptr;
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
        ClearSubscriptionSessionLocked();
        pending = TakePendingOperationsLocked();
        telemetry_owner = session_telemetry_.lock();
    }

    const auto telemetry_operation_id = telemetry_owner
        ? telemetry_owner->StartOperation(
              telemetry::OperationKind::Disconnect,
              "disconnect",
              disconnect_accepted_at)
        : std::string{};
    TelemetryOperationSpan disconnect_operation(
        telemetry_owner,
        telemetry::OperationKind::Disconnect,
        telemetry_operation_id,
        true);

    retired_events.clear();

    FailPendingOperations(std::move(pending),
                          OperationErrorCode::Cancelled,
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

    LifecycleListenerDelivery delivery;
    delivery.kind = LifecycleListenerEventKind::Disconnected;
    delivery.generation = disconnected_generation;
    delivery.required_state = ConnectionState::Disconnected;
    delivery.reason = RoomDisconnectReason::UserLeave;
    delivery.detail = "Client Initiated Disconnect";
    delivery.listeners = std::move(listeners_snapshot);
    DeliverLifecycleListenerEvent(std::move(delivery));
    disconnect_operation.Finish(telemetry::OperationOutcome::Success);
}

asio::awaitable<void> Room::DisconnectAsync() {
    const auto disconnect_accepted_at = std::chrono::steady_clock::now();
    std::deque<QueuedParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    PendingOperationCleanup pending;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    uint64_t disconnected_generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected &&
            installed_session_generation_ == 0 &&
            !reconnect_active_) {
            retired_events.swap(participant_events_);
            co_return; // Payloads outlive the lock, including this early exit.
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = RoomDisconnectReason::UserLeave;
        reconnect_disabled_ = true;
        reconnect_active_ = false;
        disconnected_generation =
            session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        installed_session_generation_ = 0;
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
        remote_participant_joined_at_ms_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        if (data_track_dc_) {
            data_channels.push_back(data_track_dc_);
            data_track_dc_ = nullptr;
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
        ClearSubscriptionSessionLocked();
        pending = TakePendingOperationsLocked();
        telemetry_owner = session_telemetry_.lock();
    }

    const auto telemetry_operation_id = telemetry_owner
        ? telemetry_owner->StartOperation(
              telemetry::OperationKind::Disconnect,
              "disconnect",
              disconnect_accepted_at)
        : std::string{};
    TelemetryOperationSpan disconnect_operation(
        telemetry_owner,
        telemetry::OperationKind::Disconnect,
        telemetry_operation_id,
        true);

    retired_events.clear();

    FailPendingOperations(std::move(pending),
                          OperationErrorCode::Cancelled,
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

    LifecycleListenerDelivery delivery;
    delivery.kind = LifecycleListenerEventKind::Disconnected;
    delivery.generation = disconnected_generation;
    delivery.required_state = ConnectionState::Disconnected;
    delivery.reason = RoomDisconnectReason::UserLeave;
    delivery.detail = "Client Initiated Disconnect";
    delivery.listeners = std::move(listeners_snapshot);
    DeliverLifecycleListenerEvent(std::move(delivery));
    disconnect_operation.Finish(telemetry::OperationOutcome::Success);
}

void Room::BeginServerDisconnect(
    RoomDisconnectReason reason,
    std::string detail,
    uint64_t event_generation) {
    bool should_finalize = false;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation) ||
            connection_state_ == ConnectionState::Disconnected ||
            server_disconnect_finalizing_) {
            return;
        }
        disconnect_reason_ = reason;
        reconnect_disabled_ = true;
        server_disconnect_finalizing_ = true;
        telemetry_owner = session_telemetry_.lock();
        should_finalize = true;
    }

    if (!should_finalize) return;

    Log("SIGNAL", "LEAVE_DISCONNECT",
        "[Room] Server requested leave: reason=" + std::string(ToString(reason)) +
        ", detail=" + detail);
    const auto telemetry_operation_id = telemetry_owner
        ? telemetry_owner->StartOperation(
              telemetry::OperationKind::Disconnect, "server_disconnect")
        : std::string{};
    livekit::safe_co_spawn(executor_,
        [self = shared_from_this(), reason, detail = std::move(detail), event_generation,
         telemetry_owner = std::move(telemetry_owner),
         telemetry_operation_id]()
            -> asio::awaitable<void> {
            co_await self->FinalizeServerDisconnectAsync(
                reason, detail, event_generation,
                std::move(telemetry_owner), telemetry_operation_id);
        });
}

asio::awaitable<void> Room::FinalizeServerDisconnectAsync(
    RoomDisconnectReason reason,
    std::string detail,
    uint64_t event_generation,
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner,
    std::string telemetry_operation_id) {
    TelemetryOperationSpan disconnect_operation(
        std::move(telemetry_owner),
        telemetry::OperationKind::Disconnect,
        std::move(telemetry_operation_id),
        true);
    std::deque<QueuedParticipantEvent> retired_events;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::shared_ptr<SignalClient> signal;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    std::shared_ptr<webrtc::PeerConnectionObserver> publisher_observer;
    std::shared_ptr<webrtc::PeerConnectionObserver> subscriber_observer;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels;
    std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    PendingOperationCleanup pending;
    uint64_t disconnected_generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) co_return;
        if (connection_state_ == ConnectionState::Disconnected &&
            installed_session_generation_ == 0) {
            server_disconnect_finalizing_ = false;
            retired_events.swap(participant_events_);
            co_return;
        }
        connection_state_ = ConnectionState::Disconnected;
        disconnect_reason_ = reason;
        reconnect_attempts_ = 0;
        reconnect_active_ = false;
        server_disconnect_finalizing_ = false;
        disconnected_generation =
            session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        installed_session_generation_ = 0;
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
        remote_participant_joined_at_ms_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        if (data_track_dc_) {
            data_channels.push_back(data_track_dc_);
            data_track_dc_ = nullptr;
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
        ClearSubscriptionSessionLocked();
        suppress_next_connected_event_ = false;
        pending = TakePendingOperationsLocked();
    }

    retired_events.clear();

    FailPendingOperations(std::move(pending),
                          OperationErrorCode::SessionClosed,
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
    LifecycleListenerDelivery delivery;
    delivery.kind = LifecycleListenerEventKind::Disconnected;
    delivery.generation = disconnected_generation;
    delivery.required_state = ConnectionState::Disconnected;
    delivery.reason = reason;
    delivery.detail = std::move(detail);
    delivery.listeners = std::move(listeners_snapshot);
    DeliverLifecycleListenerEvent(std::move(delivery));
    disconnect_operation.Finish(telemetry::OperationOutcome::Success);
}


void Room::OnIceConnected() {
    OnIceConnected(session_generation_.load(std::memory_order_acquire));
}

void Room::OnIceConnected(uint64_t generation) {
    BeforeNativeEventCommit(generation);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        for (const auto& kv : remote_participants_) {
            if (kv.second) {
                for (const auto& pub_kv : kv.second->tracks()) {
                    if (!pub_kv.second || !pub_kv.second->track() ||
                        pub_kv.second->track()->kind() != TrackKind::Video) {
                        continue;
                    }
                    const auto key = MakeSubscriptionIntentKeyLocked(
                        kv.first, kv.second->identity(), pub_kv.first);
                    auto& intent = EnsureSubscriptionIntentLocked(key);
                    if (intent.subscribed) {
                        if (remote_media_plan_active_) intent.settings_dirty = true;
                        QueueSubscriptionUpdateLocked(key);
                    }
                }
            }
        }
    }
}

void Room::OnPeerConnectionStateChanged(
    int pc_type,
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
    OnPeerConnectionStateChanged(pc_type, state, session_generation_.load(std::memory_order_acquire));
}

void Room::OnPeerConnectionStateChanged(int pc_type,
    webrtc::PeerConnectionInterface::PeerConnectionState state, uint64_t generation) {
    BeforeNativeEventCommit(generation);
    std::vector<std::shared_ptr<AwaitableState<void>>> success;
    std::vector<std::shared_ptr<AwaitableState<void>>> failure;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
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

void Room::OnDataChannelStateChanged(
    bool reliable,
    webrtc::DataChannelInterface::DataState state,
    uint64_t generation,
    const webrtc::DataChannelInterface* channel) {
    if (!reliable || !channel) return;
    BeforeNativeEventCommit(generation);

    std::vector<std::shared_ptr<AwaitableState<void>>> success;
    std::vector<std::shared_ptr<AwaitableState<void>>> failure;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            reliable_dc_.get() != channel) {
            return;
        }
        for (const auto& waiter : pending_reliable_dc_waits_) {
            if (waiter.generation != generation) continue;
            if (state == webrtc::DataChannelInterface::kOpen) {
                success.push_back(waiter.completion);
            } else if (state == webrtc::DataChannelInterface::kClosing ||
                       state == webrtc::DataChannelInterface::kClosed) {
                failure.push_back(waiter.completion);
            }
        }
    }
    for (const auto& completion : success) CompleteAwaitable(completion);
    for (const auto& completion : failure) {
        FailAwaitable(completion, std::make_exception_ptr(OperationError(
            OperationKind::Reconnect,
            OperationErrorCode::DataChannelUnavailable,
            "reliable_data_channel_state",
            "reliable data channel closed before becoming ready",
            true)));
    }
}

asio::awaitable<void> Room::WaitForReliableDataChannel(
    std::chrono::milliseconds timeout,
    uint64_t generation) {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) {
            throw OperationError(OperationKind::Reconnect,
                                 OperationErrorCode::Cancelled,
                                 "wait_reliable_data_channel",
                                 "session was replaced while waiting for data transport");
        }
        channel = reliable_dc_;
        if (!channel) {
            throw OperationError(OperationKind::Reconnect,
                                 OperationErrorCode::DataChannelUnavailable,
                                 "wait_reliable_data_channel",
                                 "reliable data channel is missing",
                                 true);
        }
        pending_reliable_dc_waits_.push_back({generation, completion});
    }

    OnDataChannelStateChanged(true, channel->state(), generation, channel.get());

    try {
        co_await WaitAwaitable<void>(completion, timeout,
                                     OperationKind::Reconnect,
                                     OperationErrorCode::DataChannelUnavailable,
                                     "wait_reliable_data_channel");
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        pending_reliable_dc_waits_.erase(
            std::remove_if(pending_reliable_dc_waits_.begin(),
                           pending_reliable_dc_waits_.end(),
                [&](const PendingDataChannelWait& item) {
                    return item.completion == completion;
                }),
            pending_reliable_dc_waits_.end());
        throw;
    }

    std::lock_guard lock(room_mutex_);
    pending_reliable_dc_waits_.erase(
        std::remove_if(pending_reliable_dc_waits_.begin(),
                       pending_reliable_dc_waits_.end(),
            [&](const PendingDataChannelWait& item) {
                return item.completion == completion;
            }),
        pending_reliable_dc_waits_.end());
}

Room::PendingOperationCleanup Room::TakePendingOperationsLocked() {
    // Retire only this bundle's streams, under the same lock as DC commits.
    for (auto& [id, entry] : active_text_readers_) entry.reader->OnStreamClose("session closed", {});
    for (auto& [id, entry] : active_byte_readers_) entry.reader->OnStreamClose("session closed", {});
    active_text_readers_.clear();
    active_byte_readers_.clear();
    incoming_stream_deadlines_.clear();
    if (incoming_stream_cleanup_timer_) {
        std::error_code ignored;
        incoming_stream_cleanup_timer_->cancel(ignored);
        incoming_stream_cleanup_timer_.reset();
    }
    incoming_data_streams_ = std::make_unique<IncomingDataStreamAssembler>();
    incoming_reader_budget_ = std::make_shared<DataStreamReaderBudget>();
    PendingOperationCleanup pending;
    {
        std::lock_guard lock(pending_rpc_mutex_);
        for (auto& [id, call] : pending_rpc_calls_) {
            call->finished = true;
            if (call->completion_cb) pending.rpc_completions.push_back(std::move(call->completion_cb));
        }
        pending_rpc_calls_.clear();
    }
    for (const auto& waiter : pending_pc_waits_) {
        pending.void_states.push_back(waiter.completion);
    }
    pending_pc_waits_.clear();
    for (const auto& waiter : pending_reliable_dc_waits_) {
        pending.void_states.push_back(waiter.completion);
    }
    pending_reliable_dc_waits_.clear();
    TakeDataTrackCleanupLocked(!reconnect_active_, pending.data_tracks);
    pending.void_states.insert(pending.void_states.end(),
                               negotiation_waiters_.begin(),
                               negotiation_waiters_.end());
    negotiation_waiters_.clear();
    if (publisher_sdp_trace_) publisher_sdp_trace_->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::Stopped);
    if (subscriber_sdp_trace_) subscriber_sdp_trace_->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::Stopped);
    publisher_sdp_trace_.reset();
    subscriber_sdp_trace_.reset();
    negotiation_state_ = NegotiationState::Idle;
    for (const auto& [cid, state] : pending_track_publishes_) {
        pending.publish_states.push_back(state);
    }
    pending_track_publishes_.clear();
    published_sender_track_ids_.clear();
    if (!reconnect_active_) {
        media_publication_policies_.clear();
        if (!publisher_pc_ && !subscriber_pc_) native_e2ee_policy_.reset();
    }
    return pending;
}

void Room::FailPendingOperations(PendingOperationCleanup pending,
                                 OperationErrorCode code,
                                 const std::string& stage,
                                 const std::string& message) {
    RpcPacket rpc_failure;
    rpc_failure.has_error = true;
    rpc_failure.error_code = static_cast<int>(RpcErrorCode::NETWORK_ERROR);
    rpc_failure.error_message = "RPC session retired";
    for (auto& complete : pending.rpc_completions) complete(rpc_failure);
    for (const auto& state : pending.void_states) {
        FailAwaitable(state, std::make_exception_ptr(OperationError(
            OperationKind::Connect, code, stage, message, true)));
    }
    for (const auto& state : pending.publish_states) {
        FailAwaitable(state, std::make_exception_ptr(OperationError(
            OperationKind::PublishTrack, code, stage, message, true)));
    }
    FinalizeDataTrackCleanup(std::move(pending.data_tracks), code, message);
}

void Room::CancelPendingOperations(OperationErrorCode code,
                                   const std::string& stage,
                                   const std::string& message) {
    PendingOperationCleanup pending;
    {
        std::lock_guard lock(room_mutex_);
        pending = TakePendingOperationsLocked();
    }
    FailPendingOperations(std::move(pending), code, stage, message);
}

void Room::FlushDeferredRoomMessages(uint64_t generation) {
    std::vector<std::shared_ptr<proto::SignalResponse>> messages;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            installed_session_generation_ != generation ||
            connection_state_ != ConnectionState::Connected) {
            return;
        }
        messages.swap(deferred_room_messages_);
    }
    for (auto& message : messages) {
        HandleSignalMessage(std::move(message), generation);
    }
}

void Room::OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    OnLocalIceCandidate(sdp, sdp_mid, sdp_mline_index, pc_type, session_generation_.load(std::memory_order_acquire));
}

void Room::OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type, uint64_t generation) {
    BeforeNativeEventCommit(generation);
    SendTrickleCandidate(sdp, sdp_mid, sdp_mline_index, pc_type, generation);
}

void Room::OnRemoteDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    OnRemoteDataChannel(data_channel, session_generation_.load(std::memory_order_acquire));
}

void Room::OnRemoteDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, uint64_t generation) {
    if (!data_channel) return;
    const std::string label = data_channel->label();
    RoomDataChannelRole role = RoomDataChannelRole::Lossy;
    if (label == "_reliable" || label == "reliable") {
        role = RoomDataChannelRole::Reliable;
    } else if (label == "_data_track") {
        role = RoomDataChannelRole::DataTrack;
    }
    auto obs = CreateDataChannelObserver(role, generation, data_channel.get());
    // Until the commit below, registration belongs solely to this callback.
    data_channel->RegisterObserver(obs.get());
    BeforeNativeEventCommit(generation);
    {
        std::lock_guard lock(room_mutex_);
        if (IsNativeGenerationCurrentLocked(generation)) {
            data_channel_observers_.push_back(obs);
            remote_data_channels_.push_back(data_channel);
            return;
        }
    }
    data_channel->UnregisterObserver();
    data_channel->Close();
}

void Room::OnRenegotiationNeeded(int pc_type) {
    OnRenegotiationNeeded(pc_type, session_generation_.load(std::memory_order_acquire));
}

void Room::OnRenegotiationNeeded(int pc_type, uint64_t generation) {
    if (pc_type != 0) return; // 只有 Publisher PC 需要由 Client 发送 Offer
    BeforeNativeEventCommit(generation);
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        pub_pc = publisher_pc_;
        if (negotiation_state_ != NegotiationState::Idle) {
            return; // 正在协商中，不产生多余的重协商风暴
        }
    }
    if (pub_pc && pub_pc->signaling_state() == webrtc::PeerConnectionInterface::SignalingState::kStable) {
        NegotiatePublisher(generation);
    }
}

void Room::HandleSignalEvent(const SignalEvent& event, uint64_t event_generation) {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    std::shared_ptr<ConnectAttemptInstrumentation> instrumentation_hooks;
    bool stale_event_at_admission = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) {
            stale_event_at_admission = true;
        } else {
            instrumentation_hooks = connect_attempt_instrumentation_;
        }
    }
    if (stale_event_at_admission) {
        Log("SIGNAL", "STALE_EVENT", "Ignoring signaling event from a replaced session");
        return;
    }

    // Instrumentation pauses here after admission and before the mutation boundary. The
    // production path is a no-op and still revalidates under room_mutex_.
    if (instrumentation_hooks && instrumentation_hooks->before_signal_event_commit) {
        instrumentation_hooks->before_signal_event_commit(event_generation);
    }

    if (event.type == SignalEvent::Close) {
        bool stale_event = false;
        bool should_reconnect = false;
        bool connect_failed = false;
        bool server_disconnect_finalizing = false;
        uint64_t reconnect_generation = 0;
        std::optional<RemoteMediaRecoveryRequest> media_recovery_request;
        RemoteMediaRecoveryHandler media_recovery_handler;
        std::shared_ptr<telemetry::SessionTelemetry> reconnect_telemetry;
        std::string reconnect_episode_id;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        PendingOperationCleanup connect_pending;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) {
                stale_event = true;
            } else {
                server_disconnect_finalizing = server_disconnect_finalizing_;
                if (server_disconnect_finalizing) {
                    // LeaveRequest cleanup owns the final notification. A following
                    // websocket close is expected and must not start a reconnect.
                } else if (!reconnect_disabled_ &&
                           connection_state_ == ConnectionState::Connected &&
                           reconnect_attempts_ < kMaxReconnectAttempts) {
                    connection_state_ = ConnectionState::Reconnecting;
                    reconnect_attempts_++;
                    PauseSubscriptionSendingLocked();
                    if (remote_media_plan_active_ &&
                        remote_media_recovery_handler_) {
                        media_recovery_request = BeginRemoteMediaRecoveryLocked();
                        media_recovery_handler = remote_media_recovery_handler_;
                    }
                    reconnect_telemetry = session_telemetry_.lock();
                    if (reconnect_telemetry) {
                        reconnect_episode_id = reconnect_telemetry->StartOperation(
                            telemetry::OperationKind::ReconnectEpisode,
                            "reconnect_episode");
                    }
                    should_reconnect = true;
                    reconnect_generation = session_generation_.load(std::memory_order_acquire);
                } else if (connection_state_ == ConnectionState::Connecting) {
                    connection_state_ = ConnectionState::Disconnected;
                    session_generation_.fetch_add(1, std::memory_order_acq_rel);
                    connect_pending = TakePendingOperationsLocked();
                    connect_failed = true;
                }
                listeners_snapshot = listeners_;
            }
        }

        if (stale_event) {
            Log("SIGNAL", "STALE_EVENT", "Ignoring signaling event from a replaced session");
            return;
        }

        if (server_disconnect_finalizing) {
            return;
        } else if (connect_failed) {
            FailPendingOperations(
                std::move(connect_pending),
                OperationErrorCode::SessionClosed,
                "signal_close_during_connect",
                event.close_reason.empty() ? "signal closed during connect" : event.close_reason);
        } else if (should_reconnect) {
            LifecycleListenerDelivery delivery;
            delivery.kind = LifecycleListenerEventKind::Reconnecting;
            delivery.generation = reconnect_generation;
            delivery.required_state = ConnectionState::Reconnecting;
            delivery.listeners = listeners_snapshot;
            DeliverLifecycleListenerEvent(std::move(delivery));
            if (media_recovery_request && media_recovery_handler) {
                try {
                    media_recovery_handler(*media_recovery_request);
                } catch (...) {
                    Log("WARNING", "MEDIA_RECOVERY_HANDLER",
                        "Remote media recovery handler failed; waiting for timeout");
                }
            }
            livekit::safe_co_spawn(executor_,
                [self = shared_from_this(), reconnect_generation,
                 reconnect_telemetry = std::move(reconnect_telemetry),
                 reconnect_episode_id = std::move(reconnect_episode_id)]()
                    -> asio::awaitable<void> {
                co_await self->AttemptReconnect(
                    reconnect_generation,
                    reconnect_telemetry,
                    reconnect_episode_id);
            });
        } else {
            bool notify_disconnect = false;
            uint64_t disconnected_generation = 0;
            {
                std::lock_guard lock(room_mutex_);
                if (IsSignalGenerationCurrentLocked(event_generation) &&
                    connection_state_ != ConnectionState::Disconnected &&
                    connection_state_ != ConnectionState::Reconnecting) {
                    connection_state_ = ConnectionState::Disconnected;
                    disconnected_generation =
                        session_generation_.load(std::memory_order_acquire);
                    notify_disconnect = true;
                }
            }
            if (notify_disconnect) {
                LifecycleListenerDelivery delivery;
                delivery.kind = LifecycleListenerEventKind::Disconnected;
                delivery.generation = disconnected_generation;
                delivery.required_state = ConnectionState::Disconnected;
                delivery.reason = RoomDisconnectReason::NetworkError;
                delivery.detail = event.close_reason;
                delivery.listeners = std::move(listeners_snapshot);
                DeliverLifecycleListenerEvent(std::move(delivery));
            }
        }
    } else if (event.type == SignalEvent::Message) {
        HandleSignalMessage(event.message, event_generation);
    }
}

bool Room::IsSignalGenerationCurrentLocked(uint64_t event_generation) const {
    return !retired() && (event_generation == 0 ||
        event_generation == session_generation_.load(std::memory_order_acquire));
}

void Room::HandleSignalMessage(
    std::shared_ptr<proto::SignalResponse> msg,
    uint64_t event_generation) {
    if (!msg) return;

    // Internal control messages must always reach their transaction waiters.
    // User-visible room updates stay buffered until Connect commits.
    const bool internal_control = msg->has_answer() || msg->has_offer() ||
        msg->has_trickle() || msg->has_track_published() ||
        msg->has_media_sections_requirement() || msg->has_reconnect() ||
        msg->has_leave() || msg->has_refresh_token() ||
        msg->has_pong() || msg->has_pong_resp() ||
        msg->has_publish_data_track_response() ||
        msg->has_unpublish_data_track_response() ||
        msg->has_data_track_subscriber_handles() ||
        msg->has_request_response();
    bool stale_message_at_admission = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) {
            stale_message_at_admission = true;
        } else if (!internal_control &&
                   connection_state_ == ConnectionState::Connecting) {
            deferred_room_messages_.push_back(std::move(msg));
            return;
        }
    }
    if (stale_message_at_admission) {
        Log("SIGNAL", "STALE_EVENT", "Ignoring signaling message from a replaced session");
        return;
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
        else if (msg->has_publish_data_track_response()) type_tag = "PUB_DATA_TRACK_RESP";
        else if (msg->has_unpublish_data_track_response()) type_tag = "UNPUB_DATA_TRACK_RESP";
        else if (msg->has_data_track_subscriber_handles()) type_tag = "DATA_TRACK_SUB_HANDLES";
        else if (msg->has_track_subscribed())        type_tag = "TRACK_SUBSCRIBED";
        else if (msg->has_media_sections_requirement()) type_tag = "MEDIA_SECTIONS_REQ";
        else if (msg->has_pong() || msg->has_pong_resp()) type_tag = "PONG";
        else if (msg->has_refresh_token())           type_tag = "REFRESH_TOKEN";
        else if (msg->has_room_moved())              type_tag = "ROOM_MOVED";

        const auto category =
            msg->has_pong() || msg->has_pong_resp() ||
            msg->has_speakers_changed() || msg->has_connection_quality() ||
            msg->has_stream_state_update()
                ? diagnostic::SignalCategory::Periodic
                : msg->has_track_published() || msg->has_track_unpublished() ||
                    msg->has_track_subscribed() || msg->has_subscribed_quality_update()
                    ? diagnostic::SignalCategory::Media
                    : type_tag == "UNKNOWN"
                        ? diagnostic::SignalCategory::Unknown
                        : diagnostic::SignalCategory::Control;
        const auto count = signal_message_counts_[static_cast<std::size_t>(
            category)].fetch_add(1, std::memory_order_relaxed) + 1;
        const bool window_sample = count % 16 == 0 && count % 64 != 0 &&
            diagnostic::DiagnosticWindowActive();
        if (count == 1 || count % 64 == 0 || window_sample) {
            diagnostic::Event summary;
            summary.kind = diagnostic::EventKind::SignalMessageSummary;
            summary.context.room_generation = event_generation;
            summary.context.has_room_generation = true;
            summary.signal_category = category;
            summary.signal_message_count = count;
            summary.window_sample = window_sample;
            EmitDiagnostic(summary);
        }

        if (type_tag == "UNKNOWN") {
            Log("SIGNAL", "RAW_MSG", "[Signal] Unknown server message received (Case=" +
                std::to_string(msg->message_case()) + ", detail=[omitted])");
        } else if (type_tag != "PONG") {
            Log("SIGNAL", "RAW_MSG", "[Signal] Server message received: " + type_tag);
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
            BeginServerDisconnect(reason, detail, event_generation);
        } else if (leave.action() == proto::LeaveRequest_Action_RECONNECT) {
            bool start_reconnect = false;
            {
                std::lock_guard lock(room_mutex_);
                if (IsSignalGenerationCurrentLocked(event_generation) &&
                    !reconnect_disabled_ &&
                    (connection_state_ == ConnectionState::Connected ||
                     connection_state_ == ConnectionState::Reconnecting)) {
                    full_reconnect_requested_ = true;
                    start_reconnect = connection_state_ == ConnectionState::Connected;
                }
            }
            if (start_reconnect) {
                asio::post(executor_, [self = shared_from_this(), event_generation] {
                    self->HandleSignalEvent(
                        SignalEvent{SignalEvent::Close, nullptr, "server requested full reconnect"},
                        event_generation);
                });
            }
        }
        return;
    }

    if (msg->has_update()) {
        Log("SIGNAL", "PARTICIPANT_UPDATE", "ParticipantUpdate received (updated participants: " + std::to_string(msg->update().participants_size()) + ")");
        UpdateParticipants(msg->update().participants(), event_generation);
    } else if (msg->has_mute()) {
        Log("SIGNAL", "MUTE_UPDATE", "Track mute update received");
        UpdateTrackMute(msg->mute(), event_generation);
    } else if (msg->has_speakers_changed()) {
        HandleActiveSpeakerUpdate(msg->speakers_changed(), event_generation);
    } else if (msg->has_offer()) {
        HandleOfferSignal(msg->offer(), event_generation);
    } else if (msg->has_answer()) {
        HandleAnswerSignal(msg->answer(), event_generation);
    } else if (msg->has_trickle()) {
        HandleTrickleSignal(msg->trickle(), event_generation);
    } else if (msg->has_track_published()) {
        const auto& tp = msg->track_published();
        Log("SIGNAL", "TRACK_PUB_ACK", "TrackPublished ACK received");
        std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>> pending;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) return;
            auto it = pending_track_publishes_.find(tp.cid());
            if (it != pending_track_publishes_.end()) pending = it->second;
        }
        if (pending) CompleteAwaitable(pending, tp);
    } else if (msg->has_publish_data_track_response()) {
        HandlePublishDataTrackResponse(msg->publish_data_track_response(), event_generation);
    } else if (msg->has_unpublish_data_track_response()) {
        HandleUnpublishDataTrackResponse(msg->unpublish_data_track_response(), event_generation);
    } else if (msg->has_data_track_subscriber_handles()) {
        HandleDataTrackSubscriberHandles(msg->data_track_subscriber_handles(), event_generation);
    } else if (msg->has_request_response()) {
        (void)HandleDataTrackRequestResponse(msg->request_response(), event_generation);
    } else if (msg->has_subscription_response()) {
        const auto& sr = msg->subscription_response();
        std::string err_str;
        switch (sr.err()) {
            case proto::SE_TRACK_NOTFOUND: err_str = "SE_TRACK_NOTFOUND (2)"; break;
            case proto::SE_CODEC_UNSUPPORTED: err_str = "SE_CODEC_UNSUPPORTED (1)"; break;
            default: err_str = "SE_UNKNOWN - subscription failed"; break;
        }
        Log("SIGNAL", "SUB_RESP", "SubscriptionResponse received: Err=" + err_str);
        UpdateTrackSubscriptionError(sr, event_generation);
    } else if (msg->has_subscription_permission_update()) {
        Log("SIGNAL", "SUB_PERM_UPDATE", "SubscriptionPermissionUpdate received (Allowed: " + std::string(msg->subscription_permission_update().allowed() ? "YES" : "NO") + ")");
        UpdateTrackSubscriptionPermission(
            msg->subscription_permission_update(), event_generation);
    } else if (msg->has_stream_state_update()) {
        Log("SIGNAL", "STREAM_STATE", "StreamStateUpdate received");
        UpdateTrackStreamStates(msg->stream_state_update(), event_generation);
    } else if (msg->has_room_update()) {
        Log("SIGNAL", "ROOM_UPDATE", "RoomUpdate received");
        UpdateRoomInfo(msg->room_update().room(), event_generation);
    } else if (msg->has_connection_quality()) {
        Log("SIGNAL", "CONN_QUALITY", "ConnectionQualityUpdate received");
        UpdateConnectionQuality(msg->connection_quality(), event_generation);
    } else if (msg->has_subscribed_quality_update()) {
        const auto& squ = msg->subscribed_quality_update();
        std::string track_sid = squ.track_sid();
        Log("SIGNAL", "QUALITY_UPDATE", "SFU Dynacast quality request received");

        std::map<std::string, std::map<livekit::proto::VideoQuality, bool>> codec_quality_map;
        std::map<livekit::proto::VideoQuality, bool> fallback_quality_states;

        for (const auto& sc : squ.subscribed_codecs()) {
            std::string c_lower = sc.codec();
            std::transform(c_lower.begin(), c_lower.end(), c_lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            // SFU sends MIME names (video/vp9), WebRTC uses codec names (VP9).
            if (c_lower.starts_with("video/")) c_lower.erase(0, 6);
            for (const auto& q : sc.qualities()) {
                codec_quality_map[c_lower][q.quality()] = q.enabled();
                fallback_quality_states[q.quality()] = q.enabled();
            }
        }
        for (const auto& q : squ.subscribed_qualities()) {
            fallback_quality_states[q.quality()] = q.enabled();
        }

        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> quality_track;
        std::shared_ptr<TrackPublication> quality_publication;
        std::shared_ptr<Track> quality_local_track;
        std::vector<std::string> quality_sender_track_ids;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) return;
            pub_pc = publisher_pc_;
            quality_publication = local_participant_ ? local_participant_->get_publication(track_sid) : nullptr;
            quality_local_track = quality_publication ? quality_publication->track() : nullptr;
            if (quality_local_track) {
                quality_track = quality_local_track->rtc_track();
                const auto owned = published_sender_track_ids_.find(
                    quality_local_track.get());
                if (owned != published_sender_track_ids_.end()) {
                    quality_sender_track_ids = owned->second;
                }
            }
        }

        // Flutter dispatches a quality update to its publication's local track.
        // A screen update must never switch the camera's encodings off.
        if (pub_pc && quality_track) {
            const auto still_current = [&] {
                std::lock_guard lock(room_mutex_);
                const auto current = local_participant_ ? local_participant_->get_publication(track_sid) : nullptr;
                return IsSignalGenerationCurrentLocked(event_generation) && publisher_pc_ == pub_pc &&
                    current == quality_publication && current && current->track() == quality_local_track;
            };
            struct QualityUpdateResult {
                bool applied;
                int error_type;
                std::string active_summary;
            };
            std::vector<QualityUpdateResult> results;
            auto* signaling = WebRTCManager::Instance().signaling_thread();
            if (!signaling || signaling->IsQuitting()) return;
            signaling->BlockingCall([&] {
                if (!still_current()) return;
                // GetParameters refreshes the sender's transaction ID. Keep
                // read/modify/write in one signaling task so a stats snapshot
                // cannot invalidate it between separate proxy calls.
                for (const auto& sender : pub_pc->GetSenders()) {
                    if (!sender || !sender->track() ||
                        quality_track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
                        continue;
                    }
                    const auto& sender_track = sender->track();
                    const bool owns_sender = sender_track == quality_track ||
                        std::find(quality_sender_track_ids.begin(),
                                  quality_sender_track_ids.end(),
                                  sender_track->id()) !=
                            quality_sender_track_ids.end();
                    if (!owns_sender) {
                        continue;
                    }

                    webrtc::RtpParameters parameters = sender->GetParameters();
                    if (parameters.encodings.empty()) continue;

                    // Match specific codec qualities if available.
                    const auto* target_qualities = &fallback_quality_states;
                    if (!parameters.codecs.empty()) {
                        std::string s_codec = parameters.codecs[0].name;
                        std::transform(s_codec.begin(), s_codec.end(), s_codec.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        const auto it = codec_quality_map.find(s_codec);
                        if (it != codec_quality_map.end()) target_qualities = &it->second;
                    }

                    bool params_changed = false;
                    const bool single_svc_encoding =
                        parameters.encodings.size() == 1 &&
                        parameters.encodings.front().scalability_mode.has_value();
                    if (single_svc_encoding && !target_qualities->empty()) {
                        const bool any_layer_enabled = std::any_of(
                            target_qualities->begin(), target_qualities->end(),
                            [](const auto& quality) { return quality.second; });
                        auto& encoding = parameters.encodings.front();
                        if (encoding.active != any_layer_enabled) {
                            encoding.active = any_layer_enabled;
                            params_changed = true;
                        }
                    }
                    for (auto& enc : parameters.encodings) {
                        if (single_svc_encoding) break;
                        // Flutter treats an omitted RID as q/LOW; a lone q
                        // layer must not also receive the HIGH setting.
                        const auto quality = enc.rid.empty() || enc.rid == "q" ? proto::VideoQuality::LOW
                            : enc.rid == "h" ? proto::VideoQuality::MEDIUM : proto::VideoQuality::HIGH;
                        const auto requested = target_qualities->find(quality);
                        if (requested != target_qualities->end() && enc.active != requested->second) {
                            enc.active = requested->second;
                            params_changed = true;
                        }
                    }
                    if (!params_changed) continue;

                    std::string active_summary;
                    for (const auto& enc : parameters.encodings) {
                        active_summary += (enc.rid.empty() ? "single" : enc.rid) + ":" + (enc.active ? "ON " : "OFF ");
                    }
                    const auto status = sender->SetParameters(parameters);
                    results.push_back({status.ok(), static_cast<int>(status.type()), std::move(active_summary)});
                }
            });
            if (!still_current()) return;
            // Only plain results cross back into the room executor. Keep user
            // logging callbacks outside both the room lock and signaling task.
            for (const auto& result : results) {
                if (result.applied) {
                    Log("DYNACAST", "LAYER_UPDATE", "Dynacast adjustment applied [" + result.active_summary + "]");
                } else {
                    Log("DYNACAST", "LAYER_UPDATE_FAILED", "Dynacast adjustment failed: code=" +
                        std::to_string(result.error_type) + ", detail=[omitted]");
                }
            }
        }
    } else if (msg->has_media_sections_requirement()) {
        const auto& msr = msg->media_sections_requirement();
        Log("SIGNAL", "MEDIA_SEC_REQ", "MediaSectionsRequirement received: audio=" + std::to_string(msr.num_audios()) + ", video=" + std::to_string(msr.num_videos()));
        HandleMediaSectionsRequirement(msr, event_generation);
    }
}

void Room::UpdateParticipants(
    const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants,
    uint64_t event_generation) {
    std::vector<std::shared_ptr<RemoteParticipant>> newly_connected;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<TrackPublication>>> newly_published_tracks;
    std::set<std::string> encryption_metadata_ready;
    bool encryption_resume = false;
    std::vector<std::pair<std::shared_ptr<RemoteTrackPublication>, uint64_t>> encryption_rejected_bindings;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<TrackPublication>>> unpublished_tracks;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::shared_ptr<RemoteDataTrack>>>
        newly_published_data_tracks;
    std::vector<std::pair<std::shared_ptr<RemoteParticipant>, std::string>>
        unpublished_data_tracks;
    DataTrackCleanupBatch retired_data_track_cleanup;
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
    std::shared_ptr<SignalClient> signal_snapshot;
    std::vector<TrackKey> removed_track_keys;
    std::vector<std::string> removed_video_telemetry_keys;
    std::vector<std::string> removed_audio_telemetry_keys;
    std::vector<std::shared_ptr<Track>> removed_tracks;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
        signal_snapshot = signal_client_;
        const bool emit_events = (connection_state_ != ConnectionState::Connecting);
        if (emit_events) {
            listeners_snapshot = listeners_;
        }

        const auto retire_remote_data_track_locked =
            [&](const std::shared_ptr<RemoteParticipant>& remote, const std::string& dt_sid) {
                remote->remove_data_track(dt_sid);
                const auto dt_it = remote_data_tracks_.find(dt_sid);
                if (dt_it != remote_data_tracks_.end()) {
                    auto entry = dt_it->second;
                    remote_data_tracks_.erase(dt_it);
                    if (entry) {
                        entry->published = false;
                        if (entry->assigned_sub_handle != 0) {
                            data_track_sub_handles_.erase(entry->assigned_sub_handle);
                            entry->assigned_sub_handle = 0;
                        }
                        if (auto live = entry->track.lock()) {
                            retired_data_track_cleanup.remote_tracks.push_back(std::move(live));
                        }
                        for (auto& pending_sub : entry->pending_subscribes) {
                            if (pending_sub) {
                                retired_data_track_cleanup.subscribe_states.push_back(
                                    std::move(pending_sub));
                            }
                        }
                        entry->pending_subscribes.clear();
                        for (const auto& weak_stream : entry->active_streams) {
                            if (auto stream = weak_stream.lock()) {
                                retired_data_track_cleanup.streams.push_back(std::move(stream));
                            }
                        }
                        entry->active_streams.clear();
                    }
                }
                if (emit_events) {
                    auto evt = MakeParticipantEventLocked(
                        ParticipantEventKind::DataTrackUnpublished, remote, false);
                    evt.data_track_sid = dt_sid;
                    EnqueueParticipantEventLocked(std::move(evt));
                }
                unpublished_data_tracks.emplace_back(remote, dt_sid);
            };

        const auto retire_remote = [&](auto it) {
            const auto remote = it->second;
            for (const auto& [dt_sid, dt] : remote->data_tracks()) {
                (void)dt;
                retire_remote_data_track_locked(remote, dt_sid);
            }
            for (const auto& [sid, publication] : remote->tracks()) {
                const auto intent_key = MakeSubscriptionIntentKeyLocked(
                    remote->sid(), remote->identity(), sid);
                subscription_intents_.erase(intent_key);
                CancelPendingSubscriptionUpdateLocked(intent_key);
                if (publication) {
                    if (const auto track = publication->track()) {
                        removed_tracks.push_back(track);
                        if (track->kind() == TrackKind::Video) {
                            removed_video_telemetry_keys.push_back(
                                RemoteVideoTelemetryKey(remote->sid(), sid));
                        } else if (track->kind() == TrackKind::Audio) {
                            removed_audio_telemetry_keys.push_back(
                                RemoteAudioTelemetryKey(remote->sid(), sid));
                        }
                    }
                    const auto track_membership = track_memberships_.find(publication.get());
                    if (track_membership != track_memberships_.end()) {
                        removed_track_keys.push_back(track_membership->second->key);
                    }
                }
                if (const auto remote_publication =
                        std::dynamic_pointer_cast<RemoteTrackPublication>(publication)) {
                    const auto current =
                        current_remote_binding_serials_.find(remote_publication.get());
                    if (current != current_remote_binding_serials_.end()) {
                        if (const auto media_binding = FindMediaBindingLocked(current->second)) {
                            media_binding->active.store(false, std::memory_order_release);
                        }
                    }
                    current_remote_binding_serials_.erase(remote_publication.get());
                    remote_publication->ClearMediaBinding();
                }
            }
            auto departure = MakeParticipantEventLocked(
                ParticipantEventKind::Departure, remote, false);
            disconnected.push_back(remote);
            if (RetireIncomingReadersForParticipantLocked(
                    remote, kDataStreamSenderDisconnected) != 0) {
                ScheduleIncomingStreamCleanupLocked(event_generation);
            }
            RetireParticipantLocked(remote);
            remote_participant_joined_at_ms_.erase(it->first);
            remote_participants_.erase(it);
            if (emit_events) {
                EnqueueParticipantEventLocked(std::move(departure));
            }
        };

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
                local_participant_->set_kind(ParticipantKindFromProto(p_info.kind()));
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
                if (emit_events) {
                    EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                        ParticipantEventKind::Upsert, local_participant_, true));
                }
                continue;
            }

            auto it = remote_participants_.find(p_info.sid());
            if (p_info.state() == proto::ParticipantInfo::DISCONNECTED) {
                if (it != remote_participants_.end()) {
                    retire_remote(it);
                }
            } else {
                const std::int64_t joined_at_ms = p_info.joined_at_ms() > 0
                    ? p_info.joined_at_ms() : p_info.joined_at() * 1000;
                if (!p_info.identity().empty()) {
                    bool stale_instance = false;
                    for (const auto& [sid, existing] : remote_participants_) {
                        if (sid == p_info.sid() || !existing ||
                            existing->identity() != p_info.identity()) continue;
                        const auto joined = remote_participant_joined_at_ms_.find(sid);
                        if (joined != remote_participant_joined_at_ms_.end() &&
                            joined->second > 0 && joined_at_ms > 0 &&
                            joined_at_ms < joined->second) {
                            stale_instance = true;
                            break;
                        }
                    }
                    if (stale_instance) continue;
                    for (auto previous = remote_participants_.begin();
                         previous != remote_participants_.end();) {
                        if (previous->first != p_info.sid() && previous->second &&
                            previous->second->identity() == p_info.identity()) {
                            retire_remote(previous++);
                        } else {
                            ++previous;
                        }
                    }
                }
                if (joined_at_ms > 0) {
                    auto& recorded = remote_participant_joined_at_ms_[p_info.sid()];
                    recorded = std::max(recorded, joined_at_ms);
                }
                std::shared_ptr<RemoteParticipant> remote;
                if (it == remote_participants_.end()) {
                    remote = std::make_shared<RemoteParticipant>(p_info.sid(), p_info.identity());
                    remote->set_name(p_info.name());
                    remote->set_metadata(p_info.metadata());
                    remote->set_kind(ParticipantKindFromProto(p_info.kind()));
                    remote->set_attributes(new_attrs);
                    remote->set_permission(new_perm);
                    remote_participants_[p_info.sid()] = remote;
                    newly_connected.push_back(remote);
                } else {
                    remote = it->second;
                    remote->set_kind(ParticipantKindFromProto(p_info.kind()));
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
                    // Signaling declaration only; never an authentication verdict.
                    const auto encryption = t_info.encryption() == proto::Encryption::GCM
                        ? TrackEncryption::Gcm : t_info.encryption() == proto::Encryption::NONE
                        ? TrackEncryption::None : t_info.encryption() == proto::Encryption::CUSTOM
                        ? TrackEncryption::Custom : TrackEncryption::Unknown;
                    current_track_sids.insert(t_info.sid());
                    auto pub = remote->get_publication(t_info.sid());
                    if (!pub) {
                        TrackKind kind = (t_info.type() == proto::TrackType::AUDIO) ? TrackKind::Audio : TrackKind::Video;
                        auto remote_track = std::make_shared<Track>(t_info.sid(), t_info.name(), kind,
                            TrackSourceFromProto(t_info.source()));
                        remote_track->set_muted(t_info.muted());
                        if (kind == TrackKind::Audio && audio_output_muted_) {
                            remote_track->set_output_muted(true);
                        }
                        // RemoteParticipant owns the actual controllable
                        // publication. Do not create a parallel controller by
                        // SID: subscription/quality settings must target this
                        // exact object and its eventual sink binding.
                        pub = CreateRemoteTrackPublication(remote_track,
                                                           p_info.sid(),
                                                           p_info.identity(),
                                                           t_info.sid(),
                                                           t_info.name(),
                                                           t_info.type());
                        pub->set_encryption(encryption);
                        pub->set_source_video_info(t_info.width(), t_info.height(),
                            PublishedVideoLayersFromProto(t_info));
                        remote->add_publication(pub);
                        const auto intent_key = MakeSubscriptionIntentKeyLocked(
                            remote->sid(), remote->identity(), t_info.sid());
                        EnsureSubscriptionIntentLocked(intent_key);
                        QueueSubscriptionUpdateLocked(intent_key);
                        newly_published_tracks.push_back({remote, pub});
                        Log("TRACK", "NEW_TRACK", "Remote track published: kind=" +
                            std::string(kind == TrackKind::Video ? "video" : "audio") +
                            ", muted=" + (t_info.muted() ? "true" : "false"));
                    } else {
                        // Metadata can arrive after an early RTC binding. Its
                        pub->set_encryption(encryption);
                        pub->set_source_video_info(t_info.width(), t_info.height(),
                            PublishedVideoLayersFromProto(t_info));
                        // source is projected to the same track read by render.
                        if (pub->track()) pub->track()->set_source(TrackSourceFromProto(t_info.source()));
                        // 检测静音/画面开关状态变化
                        if (pub->track() && pub->track()->muted() != t_info.muted()) {
                            pub->track()->set_muted(t_info.muted());
                            changed_mute_events.push_back({remote, pub, t_info.muted()});
                            if (emit_events) {
                                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                                    ParticipantEventKind::TrackMuted,
                                    remote,
                                    pub,
                                    false));
                            }
                            Log("TRACK", "MUTE_CHANGED", "Remote track state changed to: " +
                                std::string(t_info.muted() ? "muted/off" : "on"));
                        }
                    }
                    if (e2ee_manager_ && e2ee_manager_->enabled()) {
                        encryption_metadata_ready.insert(remote->sid());
                        if (encryption != TrackEncryption::Gcm) {
                            pub->set_subscription_error(TrackPublication::SubscriptionError::EncryptionRequired);
                            if (auto remote_pub = std::dynamic_pointer_cast<RemoteTrackPublication>(pub)) {
                                const auto current = current_remote_binding_serials_.find(remote_pub.get());
                                if (current != current_remote_binding_serials_.end()) {
                                    if (auto binding = FindMediaBindingLocked(current->second)) binding->active.store(false);
                                    encryption_rejected_bindings.emplace_back(remote_pub, current->second);
                                }
                            }
                            if (emit_events) {
                                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                                    ParticipantEventKind::TrackSubscriptionError, remote, pub, false));
                            }
                        } else if (pub->subscription_error() == TrackPublication::SubscriptionError::EncryptionRequired) {
                            encryption_resume = true;
                            pub->set_subscription_error(TrackPublication::SubscriptionError::None);
                            if (emit_events) {
                                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                                    ParticipantEventKind::TrackSubscriptionError, remote, pub, false));
                            }
                        }
                    }
                }

                // 检测被远端取消发布的 Track (Unpublished)
                auto existing_tracks = remote->tracks();
                for (const auto& [sid, pub] : existing_tracks) {
                    if (current_track_sids.find(sid) == current_track_sids.end()) {
                        const auto intent_key = MakeSubscriptionIntentKeyLocked(
                            remote->sid(), remote->identity(), sid);
                        subscription_intents_.erase(intent_key);
                        CancelPendingSubscriptionUpdateLocked(intent_key);
                        if (pub && pub->track()) removed_tracks.push_back(pub->track());
                        if (pub && pub->track() &&
                            pub->track()->kind() == TrackKind::Video) {
                            removed_video_telemetry_keys.push_back(
                                RemoteVideoTelemetryKey(remote->sid(), sid));
                        } else if (pub && pub->track() &&
                                   pub->track()->kind() == TrackKind::Audio) {
                            removed_audio_telemetry_keys.push_back(
                                RemoteAudioTelemetryKey(remote->sid(), sid));
                        }
                        if (const auto remote_publication =
                                std::dynamic_pointer_cast<RemoteTrackPublication>(pub)) {
                            const auto current =
                                current_remote_binding_serials_.find(remote_publication.get());
                            if (current != current_remote_binding_serials_.end()) {
                                if (const auto media_binding = FindMediaBindingLocked(current->second)) {
                                    media_binding->active.store(false, std::memory_order_release);
                                }
                            }
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
                        if (emit_events) {
                            EnqueueParticipantEventLocked(std::move(unavailable));
                        }
                        unpublished_tracks.push_back({remote, pub});
                        Log("TRACK", "UNPUBLISHED", "Remote track unpublished");
                    }
                }

                // 同步解析远端参会人发布的 2026 Dedicated DataTracks
                std::set<std::string> current_data_track_sids;
                for (int dt_idx = 0; dt_idx < p_info.data_tracks_size(); ++dt_idx) {
                    const auto& dt_proto = p_info.data_tracks(dt_idx);
                    if (dt_proto.sid().empty() || dt_proto.pub_handle() == 0 ||
                        dt_proto.pub_handle() > 0xFFFFu) {
                        continue;
                    }
                    if (dt_proto.encryption() != proto::Encryption::NONE &&
                        dt_proto.encryption() != proto::Encryption::GCM) {
                        Log("DATA_TRACK", "UNSUPPORTED_E2EE",
                            "Skipping remote data track with unsupported encryption type");
                        continue;
                    }
                    current_data_track_sids.insert(dt_proto.sid());
                    if (remote_data_tracks_.find(dt_proto.sid()) != remote_data_tracks_.end()) {
                        continue;
                    }
                    auto entry = std::make_shared<RemoteDataTrackEntry>();
                    entry->info.pub_handle = static_cast<uint16_t>(dt_proto.pub_handle());
                    entry->info.sid = dt_proto.sid();
                    entry->info.name = dt_proto.name();
                    entry->info.uses_e2ee = (dt_proto.encryption() == proto::Encryption::GCM);
                    entry->publisher_sid = remote->sid();
                    entry->publisher_identity = remote->identity();
                    auto dt_obj = BuildRemoteDataTrackObjectLocked(entry, event_generation);
                    entry->track = dt_obj;
                    remote_data_tracks_[entry->info.sid] = entry;
                    remote->add_data_track(dt_obj);

                    if (emit_events) {
                        auto dt_evt = MakeParticipantEventLocked(
                            ParticipantEventKind::DataTrackPublished, remote, false);
                        dt_evt.data_track = dt_obj;
                        dt_evt.data_track_sid = entry->info.sid;
                        EnqueueParticipantEventLocked(std::move(dt_evt));
                    }
                    newly_published_data_tracks.emplace_back(remote, std::move(dt_obj));
                }

                for (const auto& [existing_dt_sid, existing_dt] : remote->data_tracks()) {
                    (void)existing_dt;
                    if (current_data_track_sids.find(existing_dt_sid) == current_data_track_sids.end()) {
                        retire_remote_data_track_locked(remote, existing_dt_sid);
                    }
                }

                if (emit_events) {
                    EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                        ParticipantEventKind::Upsert, remote, false));
                }
            }
        }
    }

    if (const auto telemetry_owner = session_telemetry_.lock()) {
        for (const auto& key : removed_video_telemetry_keys) {
            telemetry_owner->SetRemoteVideoExpected(
                key,
                false,
                telemetry::MediaExpectationReason::PublicationEnded);
        }
        for (const auto& key : removed_audio_telemetry_keys) {
            telemetry_owner->SetRemoteAudioExpected(
                key,
                false,
                telemetry::MediaExpectationReason::PublicationEnded);
        }
        for (const auto& change : changed_mute_events) {
            if (!change.publication || !change.publication->track()) {
                continue;
            }
            const auto snapshot = change.publication->SnapshotState();
            const auto track = change.publication->track();
            SetRemoteMediaExpected(
                telemetry_owner,
                track->kind(),
                change.participant->sid(), change.publication->sid(),
                IsRemoteMediaExpected(
                    snapshot, IsRemotePublicationSubscribed(change.publication)),
                change.muted
                    ? telemetry::MediaExpectationReason::Muted
                    : telemetry::MediaExpectationReason::Unmuted);
        }
    }
    DetachRemoteTrackSinks(TakeRemoteTrackSinksForTrackKeys(removed_track_keys));
    RemoveRemoteMediaTrackReferences(removed_tracks);

    for (const auto& [publication, serial] : encryption_rejected_bindings)
        DetachRemotePublicationMedia(publication.get(), true, serial);
    for (const auto& sid : encryption_metadata_ready) FlushPendingTracks(sid, event_generation);
    if (encryption_resume) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher, subscriber;
        {
            std::lock_guard lock(room_mutex_);
            if (IsNativeGenerationCurrentLocked(event_generation)) {
                publisher = publisher_pc_; subscriber = subscriber_pc_;
            }
        }
        if (subscriber) ReconcileRemoteReceivers(subscriber, event_generation);
        if (publisher && publisher != subscriber) ReconcileRemoteReceivers(publisher, event_generation);
    }

    for (const auto& p : newly_connected) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnParticipantConnected(p);
            }, true);
        }
        FlushPendingTracks(p->sid(), event_generation);
    }

    for (const auto& [p, pub] : newly_published_tracks) {
        FlushPendingTracks(p->sid(), event_generation);
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnTrackPublished(p, pub);
            }, true);
            if (pub && pub->track() && pub->track()->rtc_track()) {
                DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                    target.OnTrackSubscribed(pub->track(), pub, p);
                }, true);
            }
        }
    }

    // In single-PC mode the SFU's MediaSectionsRequirement is authoritative.
    // Offering here races that request before its recvonly sections exist.

    for (const auto& evt : changed_mute_events) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnTrackMuted(evt.participant, evt.publication, evt.muted);
            }, true);
        }
    }

    for (const auto& [p, pub] : unpublished_tracks) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnTrackUnpublished(p, pub);
            }, true);
            if (pub && pub->track()) {
                DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                    target.OnTrackUnsubscribed(pub->track(), pub, p);
                }, true);
            }
        }
    }

    for (const auto& evt : changed_attributes_events) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnParticipantAttributesChanged(evt.attrs, evt.participant);
            }, true);
        }
    }

    for (const auto& evt : changed_permissions_events) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnParticipantPermissionsChanged(evt.old_perm, evt.new_perm, evt.participant);
            }, true);
        }
    }

    for (const auto& evt : changed_metadata_events) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnParticipantMetadataChanged(evt.participant, evt.old_metadata, evt.new_metadata);
            }, true);
        }
    }

    for (const auto& [p, dt] : newly_published_data_tracks) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnDataTrackPublished(p, dt);
            }, true);
        }
    }

    // Deliver normal EOS to streams whose remote data tracks were unpublished or departed.
    for (const auto& live : retired_data_track_cleanup.remote_tracks) {
        if (live) live->MarkUnpublishedInternal();
    }
    for (const auto& pending_sub : retired_data_track_cleanup.subscribe_states) {
        CompleteAwaitable(pending_sub,
            DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
                SubscribeDataTrackErrorCode::Unpublished,
                "Remote data track was unpublished before subscription completed",
            }));
    }
    for (const auto& stream : retired_data_track_cleanup.streams) {
        if (stream) stream->PushEos(std::nullopt);
    }
    for (const auto& [p, dt_sid] : unpublished_data_tracks) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnDataTrackUnpublished(p, dt_sid);
            }, true);
        }
    }

    for (const auto& p : disconnected) {
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnParticipantDisconnected(p);
            }, true);
        }
    }
}

void Room::UpdateParticipants(
    const proto::ParticipantUpdate& update,
    uint64_t event_generation) {
    UpdateParticipants(update.participants(), event_generation);
}

void Room::UpdateTrackMute(
    const proto::MuteTrackRequest& mute,
    uint64_t event_generation) {
    std::shared_ptr<Participant> target_participant;
    std::shared_ptr<TrackPublication> target_pub;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
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
        if (target_participant != local_participant_ && target_pub->track()) {
            const auto snapshot = target_pub->SnapshotState();
            if (const auto telemetry_owner = session_telemetry_.lock()) {
                SetRemoteMediaExpected(
                    telemetry_owner,
                    snapshot.kind,
                    target_participant->sid(), target_pub->sid(),
                    IsRemoteMediaExpected(
                        snapshot, IsRemotePublicationSubscribed(target_pub)),
                    mute.muted()
                        ? telemetry::MediaExpectationReason::Muted
                        : telemetry::MediaExpectationReason::Unmuted);
            }
        }
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnTrackMuted(target_participant, target_pub, mute.muted());
            }, true);
        }
    }
}

void Room::InjectParticipantUpdate(const proto::ParticipantUpdate& update) {
    UpdateParticipants(update.participants());
}

void Room::InjectSignalMessage(const proto::SignalResponse& message) {
    HandleSignalMessage(std::make_shared<proto::SignalResponse>(message));
}

std::shared_ptr<void> Room::AcquireMediaPublishPolicy() {
    std::lock_guard lock(room_mutex_);
    auto manager_lease = e2ee_manager_ ? e2ee_manager_->AcquireMediaPublishPolicy() : nullptr;
    auto self = shared_from_this();
    media_publish_admissions_.fetch_add(1);
    return std::shared_ptr<void>(nullptr, [self, manager_lease](void*) {
        self->media_publish_admissions_.fetch_sub(1);
    });
}

std::optional<MediaEncryptionStatus> Room::ReadMediaEncryptionStatus(uint64_t expected_generation) const {
    std::lock_guard lock(room_mutex_);
    if (!IsNativeGenerationCurrentLocked(expected_generation) ||
        connection_state_ != ConnectionState::Connected || !e2ee_manager_) return std::nullopt;
    return e2ee_manager_->ReadMediaStatus(expected_generation);
}

void Room::PostMediaEncryptionState(std::weak_ptr<E2eeManager> manager,
    std::shared_ptr<const E2eeManager::MediaObservation> binding, EncryptionState state) {
    if (!binding) return;
    callback_gate_->Post([weak = weak_from_this(), manager, binding = std::move(binding), state] {
        auto self = weak.lock();
        if (!self) return;
        ListenerDeliveryContext context{};
        context.generation = binding->generation;
        context.require_installed_owner = true;
        context.media_encryption_binding = binding;
        context.e2ee_owner = manager.lock();
        if (!context.e2ee_owner) return;
        MediaEncryptionEvent event;
        event.participant_identity = binding->identity;
        event.generation = binding->generation;
        event.receiving = binding->receiving;
        event.video = binding->video;
        event.state = state;
        if (binding->receiving) event.track_sid = binding->track;
        else event.native_track_id = binding->track;
        std::vector<std::shared_ptr<RoomListener>> listeners;
        {
            std::lock_guard lock(self->room_mutex_);
            if (self->e2ee_manager_ != context.e2ee_owner ||
                !self->IsNativeGenerationCurrentLocked(binding->generation)) return;
            listeners = self->listeners_;
        }
        for (const auto& listener : listeners) {
            self->DeliverListener(context, listener, [&](RoomListener& target) {
                target.OnMediaEncryptionStateChanged(event);
            });
        }
    });
}

bool Room::RecoverE2eeSharedKey(const std::shared_ptr<MeetingSecretHandle>& secret,
                              uint64_t expected_generation) {
    std::lock_guard lock(room_mutex_);
    if (!secret) return false;
    if (!IsNativeGenerationCurrentLocked(expected_generation) ||
        connection_state_ != ConnectionState::Connected || !e2ee_manager_) {
        secret->Revoke();
        return false;
    }
    return e2ee_manager_->InstallRecoveryKey(secret);
}

void Room::EnableE2ee(const E2eeOptions& options) {
#if defined(COHAVORA_E2EE_MEDIA_GUARD)
    constexpr bool backendAvailable = true;
#else
    constexpr bool backendAvailable = false;
#endif
    // The packaged SDK alone lacks the pre-OnTrack media guard and the
    // conditional key-epoch repair. Reject before allocating a manager or
    // connecting, rather than exposing a partially protected Required room.
    if (!backendAvailable && options.encryption_type != EncryptionType::NONE) {
        throw OperationError(OperationKind::Connect, OperationErrorCode::EncryptionFailed,
            "e2ee_backend_unavailable", "this build does not include the required E2EE backend");
    }
    std::lock_guard lock(room_mutex_);
    if (publisher_pc_ || subscriber_pc_ || native_e2ee_policy_.use_count() != 0 ||
        media_publish_admissions_.load() != 0 || !published_sender_track_ids_.empty() ||
        !media_publication_policies_.empty() ||
        (e2ee_manager_ && e2ee_manager_->HasActiveMediaBindings())) {
        throw std::logic_error("retire media publications before replacing encryption policy");
    }
    e2ee_manager_ = std::make_shared<E2eeManager>(options);
    std::weak_ptr<E2eeManager> manager = e2ee_manager_;
    e2ee_manager_->SetMediaStateChangedHandler([weak = weak_from_this(), manager](
        std::shared_ptr<const E2eeManager::MediaObservation> binding, EncryptionState state) {
        if (auto self = weak.lock()) self->PostMediaEncryptionState(manager, std::move(binding), state);
    });
    e2ee_manager_->SetStateChangedHandler([weak = weak_from_this(), manager](
        const std::string& identity, EncryptionState state) {
        if (auto self = weak.lock()) {
            std::vector<std::shared_ptr<RoomListener>> snapshot;
            ListenerDeliveryContext context{};
            {
                std::lock_guard lock(self->room_mutex_);
                context.e2ee_owner = manager.lock();
                if (!context.e2ee_owner || context.e2ee_owner != self->e2ee_manager_) return;
                // E2EE configuration is Room-global and can be enabled before
                // Connect. Bind each emitted event (not the configuration) to
                // the admitted session; replacement invalidates later delivery.
                context.generation = self->session_generation_.load(std::memory_order_acquire);
                snapshot = self->listeners_;
            }
            for (const auto& listener : snapshot) {
                self->DeliverListener(context, listener, [&](RoomListener& target) {
                    target.OnE2eeStateChanged(identity, "", state);
                });
            }
        }
    });
}

void Room::InjectActiveSpeakerUpdate(const proto::SpeakersChanged& update) {
    HandleActiveSpeakerUpdate(update);
}

void Room::HandleActiveSpeakerUpdate(
    const proto::SpeakersChanged& update,
    uint64_t event_generation) {
    std::vector<std::shared_ptr<Participant>> active_speakers;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    ParticipantEvent value_event;
    value_event.kind = ParticipantEventKind::ActiveSpeakers;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
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
        DeliverListener({event_generation}, listener, [&](RoomListener& target) {
            target.OnActiveSpeakersChanged(active_speakers);
        }, true);
    }
}

void Room::SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type, uint64_t generation) {
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

    std::shared_ptr<SignalClient> signal;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        signal = signal_client_;
    }
    if (signal) signal->Send(req);
}

void Room::HandleOfferSignal(
    const proto::SessionDescription& offer,
    uint64_t event_generation) {
    // ⚠ 最早期日志 - 确认此函数被调用
    Log("SIGNAL", "OFFER_CALLED",
        secure_log::SdpSummary("remote_offer_received", offer.sdp()));
    LogSdpNegotiationDetails(
        *this, "SDP_OFFER_DETAIL", "subscriber_offer_received", offer.sdp());
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    std::shared_ptr<SignalClient> client;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        client = signal_client_;
        pc = (offer.type() == "offer" && subscriber_pc_) ? subscriber_pc_ : publisher_pc_;
        trace = SdpTraceLocked(pc == subscriber_pc_ ? diagnostic::SdpRole::Subscriber
            : diagnostic::SdpRole::Publisher, event_generation, true);
        trace->Record(diagnostic::SdpAction::ReceiveOffer, diagnostic::SdpPhase::Completed);
        
        if (client && client->is_single_pc_mode_active()) {
            trace->Finish(diagnostic::Outcome::Failure, diagnostic::SdpReason::UnexpectedMessage);
            Log("WARNING", "UNEXPECTED_OFFER", "Ignoring server Offer in Single PC mode");
            return;
        }
    }

    if (!pc || !client) {
        trace->Finish(diagnostic::Outcome::Failure, diagnostic::SdpReason::Stopped);
        Log("ERROR", "SDP_ERR", "HandleOfferSignal: PeerConnection or SignalClient is null");
        return;
    }

    Log("SIGNAL", "SDP_OFFER_RECV",
        secure_log::SdpSummary("subscriber_offer_received", offer.sdp()));
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pc, offer.type(), offer.sdp(), executor_,
        [self, client, pc, event_generation, trace](const std::string& set_remote_err) {
            {
                std::lock_guard lock(self->room_mutex_);
                if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                    trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                        diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                    trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                    return;
                }
            }
            if (!set_remote_err.empty()) {
                self->Log("ERROR", "SET_REMOTE_ERR",
                          secure_log::OpaqueSummary("set_remote_offer"));
                return;
            }

            self->Log("SIGNAL", "OFFER_APPLIED", "Server Offer applied. Generating SDP Answer...");
            self->ReconcileRemoteReceivers(pc, event_generation);
            WebRTCManager::Instance().CreateAnswer(pc, self->executor_,
                [self, client, pc, event_generation, trace](
                    const std::string& sdp,
                    const std::string& create_ans_err) {
                    {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                            trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                                diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                            trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                            return;
                        }
                    }
                    if (!create_ans_err.empty()) {
                        self->Log("ERROR", "CREATE_ANS_ERR",
                                  secure_log::OpaqueSummary("create_answer"));
                        return;
                    }

                    WebRTCManager::Instance().SetLocalDescription(pc, "answer", sdp, self->executor_,
                        [self, client, pc, sdp, event_generation, trace](
                            const std::string& set_local_err) {
                            {
                                std::lock_guard lock(self->room_mutex_);
                                if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                                    trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                                        diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                                    trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                                    return;
                                }
                            }
                            if (!set_local_err.empty()) {
                                self->Log("ERROR", "SET_LOCAL_ANS_ERR",
                                          secure_log::OpaqueSummary("set_local_answer"));
                                return;
                            }

                            std::vector<PendingIceCandidate> pending_cands;
                            {
                                std::lock_guard lock(self->room_mutex_);
                                if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                                    trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                                        diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                                    trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                                    return;
                                }
                                pending_cands =
                                    std::move(self->pending_sub_ice_candidates_);
                            }

                            proto::SignalRequest req;
                            auto* answer_msg = req.mutable_answer();
                            answer_msg->set_type("answer");
                            answer_msg->set_sdp(sdp);
                            client->Send(req, trace);
                            LogSdpNegotiationDetails(
                                *self, "SDP_ANSWER_DETAIL", "subscriber_answer_prepared", sdp);

                            // 重放暂存的 Subscriber 早期 ICE 候选
                            if (!pending_cands.empty()) {
                                self->Log("SIGNAL", "ICE_FLUSH", "Replaying " + std::to_string(pending_cands.size()) + " buffered early Subscriber candidates...");
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
                                          std::string("Early candidate replay result=") +
                                              (res.second ? "success" : "failure"));
                                }
                            }

                            // 扫描 Subscriber Transceiver 下的 Receiver Track，确保后加入用户的轨道被及时挂载；
                            // 不得在此无条件 set_enabled(true)，轨道启用状态由 Room/Track 静音与扬声器静音状态统一控制。
                            auto transceivers = pc->GetTransceivers();
                            for (const auto& t : transceivers) {
                                if (t && t->receiver() && t->receiver()->track()) {
                                    auto r_track = t->receiver()->track();
                                    self->PostRemoteTrack(t->receiver(), r_track, event_generation);
                                }
                            }
                        }, self->callback_gate_, trace);
                }, self->callback_gate_, trace);
        }, callback_gate_, trace);
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

void Room::HandleAnswerSignal(
    const proto::SessionDescription& answer,
    uint64_t event_generation) {
    Log("SIGNAL", "SDP_ANSWER_RECV",
        secure_log::SdpSummary("remote_answer_received", answer.sdp()));
    LogSdpNegotiationDetails(
        *this, "SDP_ANSWER_DETAIL", "remote_answer_received", answer.sdp());
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    bool pub_negotiating = false;
    bool sub_negotiating = false;
    bool single_pc = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
        pub_negotiating = (negotiation_state_ != NegotiationState::Idle);
        sub_negotiating = subscriber_negotiating_;
        single_pc = signal_client_ && signal_client_->is_single_pc_mode_active();
    }

    if (single_pc) {
        Log("SIGNAL", "SDP_ANS_ROUTING", "Single PC mode: routing Answer to publisher_pc_");
        auto self = shared_from_this();
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) return;
            trace = SdpTraceLocked(diagnostic::SdpRole::Publisher, event_generation);
            trace->Record(diagnostic::SdpAction::ReceiveAnswer, diagnostic::SdpPhase::Completed);
        }
        TracePublisherSdpFormats(answer.sdp(), true, event_generation, trace);
        WebRTCManager::Instance().SetRemoteDescription(pub_pc, "answer", answer.sdp(), executor_,
            [self, pub_pc, event_generation, trace](const std::string& err) {
                {
                    std::lock_guard lock(self->room_mutex_);
                    if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                        trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                            diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                        trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                        return;
                    }
                    if (err.empty()) trace->Finish(diagnostic::Outcome::Success);
                }
                if (!err.empty()) {
                    self->Log("ERROR", "PUB_REMOTE_ERR",
                              secure_log::OpaqueSummary("set_publisher_remote_answer"));
                    self->CompleteNegotiation(err, event_generation);
                } else {
                    self->TracePublisherSenderFormats(pub_pc, event_generation, trace);
                    self->Log("SIGNAL", "PUB_STABLE", "Publisher PC negotiation completed (Answer applied)");
                    self->ReconcileRemoteReceivers(pub_pc, event_generation);
                    std::vector<PendingIceCandidate> pending_cands;
                    bool need_retry = false;
                    {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                            trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                                diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                            trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                            return;
                        }
                        self->subscriber_negotiating_ = false;
                        if (self->negotiation_state_ == NegotiationState::PendingRetry) {
                            self->negotiation_state_ = NegotiationState::InProgress;
                            need_retry = true;
                        }
                        pending_cands = std::move(self->pending_pub_ice_candidates_);
                    }
                    if (need_retry) {
                        self->Log("SIGNAL", "NEG_RETRY", "Pending negotiation detected; starting another attempt");
                        self->ExecuteNegotiatePublisher(event_generation);
                    } else {
                        self->CompleteNegotiation("", event_generation);
                    }
                    
                    // 单 PC 模式下，本地主动发起的 recvonly transceiver 不会触发 OnTrack，需手动提取
                    auto transceivers = pub_pc->GetTransceivers();
                    self->Log("SIGNAL", "TRANS_SCAN", "Single PC Answer negotiation completed; scanning " + std::to_string(transceivers.size()) + " transceivers");
                    for (const auto& t : transceivers) {
                        if (t && (t->direction() == webrtc::RtpTransceiverDirection::kRecvOnly ||
                            (t->current_direction().has_value() && *t->current_direction() == webrtc::RtpTransceiverDirection::kRecvOnly))) {
                            if (t->receiver() && t->receiver()->track()) {
                                self->PostRemoteTrack(t->receiver(), t->receiver()->track(), event_generation);
                            }
                        }
                    }

                    if (!pending_cands.empty()) {
                        self->Log("SIGNAL", "ICE_FLUSH", "Replaying " + std::to_string(pending_cands.size()) + " buffered early Publisher candidates...");
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
                                      std::string("Early candidate replay result=") +
                                          (res.second ? "success" : "failure"));
                        }
                    }
                }
            }, callback_gate_, trace);
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
        Log("SIGNAL", "SUB_ANS_RECV", "Subscriber SDP Answer received (" + std::to_string(answer.sdp().length()) + " bytes, m-lines=" + std::to_string(answer_mlines.size()) + "); applying to Subscriber PC...");
        auto self = shared_from_this();
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) return;
            trace = SdpTraceLocked(diagnostic::SdpRole::Subscriber, event_generation);
            trace->Record(diagnostic::SdpAction::ReceiveAnswer, diagnostic::SdpPhase::Completed);
        }
        WebRTCManager::Instance().SetRemoteDescription(sub_pc, answer.type(), answer.sdp(), executor_,
            [self, sub_pc, answer_sdp = answer.sdp(), event_generation, trace](const std::string& err) {
                std::vector<PendingIceCandidate> pending_cands;
                {
                    std::lock_guard lock(self->room_mutex_);
                    if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                        trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                            diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                        trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                        return;
                    }
                    if (err.empty()) trace->Finish(diagnostic::Outcome::Success);
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
                    self->Log("WEBRTC", "SUB_STABLE", "Subscriber RemoteDescription (Answer) applied; signaling state is STABLE");
                    self->ReconcileRemoteReceivers(sub_pc, event_generation);

                    // 在锁外部安全重放早期候选，彻底避免死锁
                    if (!pending_cands.empty()) {
                        self->Log("SIGNAL", "ICE_FLUSH", "Replaying " + std::to_string(pending_cands.size()) + " buffered early Subscriber candidates...");
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
                                          std::string("Early candidate replay result=") +
                                              (res.second ? "success" : "failure"));
                        }
                    }
                }
            }, callback_gate_, trace);
        return;
    }

    // Publisher Answer
    Log("SIGNAL", "SDP_ANSWER_RECV", "Remote Publisher SDP Answer received (" + std::to_string(answer.sdp().length()) + " bytes, m-lines=" + std::to_string(answer_mlines.size()) + ")");
    auto self = shared_from_this();
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        trace = SdpTraceLocked(diagnostic::SdpRole::Publisher, event_generation);
        trace->Record(diagnostic::SdpAction::ReceiveAnswer, diagnostic::SdpPhase::Completed);
    }
    TracePublisherSdpFormats(answer.sdp(), true, event_generation, trace);
    WebRTCManager::Instance().SetRemoteDescription(pub_pc, answer.type(), answer.sdp(), executor_,
        [self, pub_pc, event_generation, trace](const std::string& err) {
            bool need_retry = false;
            std::vector<PendingIceCandidate> pending_cands;
            {
                std::lock_guard lock(self->room_mutex_);
                if (!self->IsSignalGenerationCurrentLocked(event_generation)) {
                    trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                        diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                    trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                    return;
                }
                if (err.empty()) trace->Finish(diagnostic::Outcome::Success);
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
                self->Log("ERROR", "ANS_ERR",
                          secure_log::OpaqueSummary("set_publisher_remote_answer"));
                self->CompleteNegotiation(err, event_generation);
            } else {
                self->TracePublisherSenderFormats(pub_pc, event_generation, trace);
                self->Log("WEBRTC", "PUB_STABLE", "Publisher RemoteDescription applied; signaling state is STABLE");

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
                self->ExecuteNegotiatePublisher(event_generation);
            } else if (err.empty()) {
                self->CompleteNegotiation("", event_generation);
            }
        }, callback_gate_, trace);
}

void Room::HandleTrickleSignal(
    const proto::TrickleRequest& trickle,
    uint64_t event_generation) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        pub_pc = publisher_pc_;
        sub_pc = subscriber_pc_;
    }

    try {
        auto json_cand = nlohmann::json::parse(trickle.candidateinit());
        std::string sdp = json_cand.value("candidate", "");
        std::string sdp_mid = json_cand.value("sdpMid", "");
        if (sdp_mid.empty()) sdp_mid = json_cand.value("sdp_mid", "");
        int sdp_mline_index = json_cand.value("sdpMLineIndex", 0);

        Log("SIGNAL", "TRICKLE_RECV", "Server ICE candidate received: target=" +
            std::string(trickle.target() == proto::SignalTarget::PUBLISHER ? "PUBLISHER" : "SUBSCRIBER") +
            ", detail=[omitted]");

        auto self = shared_from_this();
        WebRTCManager::Instance().signaling_thread()->BlockingCall([self, pub_pc, sub_pc, trickle_target = trickle.target(), sdp_mid, sdp_mline_index, sdp, event_generation]() {
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
                if (!self->IsSignalGenerationCurrentLocked(event_generation)) return;
                is_single = self->signal_client_ && self->signal_client_->is_single_pc_mode_active();
            }

            if (is_single) {
                if (pub_pc) {
                    bool ok = pub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) return;
                        self->pending_pub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_PUB_QUEUE",
                                  "Publisher PC not ready; early ICE candidate buffered: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_PUB_ADD",
                                  "Added ICE candidate to Publisher PC: success (Single PC)");
                    }
                }
                return;
            }

            if (trickle_target == proto::SignalTarget::SUBSCRIBER) {
                if (sub_pc) {
                    bool ok = sub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) return;
                        self->pending_sub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_SUB_QUEUE",
                                  "Subscriber PC not ready; early ICE candidate buffered: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD",
                                  "Added ICE candidate to Subscriber PC: success");
                    }
                }
            } else {
                if (pub_pc) {
                    bool ok = pub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) return;
                        self->pending_pub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                    }
                }
                // 同时尝试添加至 sub_pc，防止服务端 target 缺省为 0 导致 Subscriber 缺少候选
                if (sub_pc) {
                    bool ok = sub_pc->AddIceCandidate(cand.get());
                    if (!ok) {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(event_generation)) return;
                        self->pending_sub_ice_candidates_.push_back({sdp_mid, sdp_mline_index, sdp});
                        self->Log("SIGNAL", "ICE_SUB_QUEUE",
                                  "Subscriber PC not ready; early ICE candidate buffered: detail=[omitted]");
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD",
                                  "Attempted to add ICE candidate to Subscriber PC: success");
                    }
                }
            }
        });
    } catch (...) {
        diagnostic::Event issue;
        issue.kind = diagnostic::EventKind::SignalIssue;
        issue.context.room_generation = event_generation;
        issue.context.has_room_generation = true;
        issue.error_layer = diagnostic::ErrorLayer::Parse;
        issue.error_code = diagnostic::ErrorCode::InvalidResponse;
        EmitDiagnostic(issue);
    }
}

// The SFU sends this only for the client-offer/single-PC flow. The counts are
// additional media sections required by the server, not desired totals.
void Room::HandleMediaSectionsRequirement(
    const proto::MediaSectionsRequirement& req,
    uint64_t event_generation) {
    const uint32_t num_audios = req.num_audios();
    const uint32_t num_videos = req.num_videos();
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    bool single_pc = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        single_pc = signal_client_ && signal_client_->is_single_pc_mode_active();
        publisher = publisher_pc_;
    }

    if (!single_pc) {
        Log("SIGNAL", "MEDIA_SEC_SKIP",
            "Ignoring MediaSectionsRequirement in dual-PC mode; the server initiates Subscriber SDP");
        return;
    }
    if (!publisher) {
        Log("SIGNAL", "MEDIA_SEC_SKIP", "MediaSectionsRequirement has no active Publisher PC");
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

    Log("SIGNAL", "MEDIA_SEC_NEGOTIATE",
        "Server requested downstream negotiation: added_audio=" + std::to_string(num_audios) +
        ", added_video=" + std::to_string(num_videos));
    // Match rtc_session.rs::handle_media_sections_requirement: zero additional
    // sections still requests an offer to bind/rebind tracks on existing MIDs.
    // NegotiatePublisher coalesces repeated requirements received while an
    // offer is in flight, so no server request is dropped.
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        NegotiatePublisher(event_generation);
    }
}

proto::SyncState Room::BuildSyncState(SubscriptionSyncSnapshot* snapshot) const {
    proto::SyncState state;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sync_pc;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> lossy;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> remote_channels;
    std::shared_ptr<LocalParticipant> local;
    bool single_pc = false;
    bool auto_subscribe = true;
    std::map<std::string, std::vector<std::string>> subscription_exceptions;
    {
        std::lock_guard lock(room_mutex_);
        if (snapshot) {
            snapshot->logical_session = subscription_session_generation_;
            snapshot->revisions.clear();
        }
        single_pc = signal_client_ && signal_client_->is_single_pc_mode_active();
        auto_subscribe = session_auto_subscribe_;
        sync_pc = single_pc ? publisher_pc_ : subscriber_pc_;
        reliable = reliable_dc_;
        lossy = lossy_dc_;
        remote_channels = remote_data_channels_;
        local = local_participant_;
        for (const auto& [key, intent] : subscription_intents_) {
            if (snapshot) snapshot->revisions[key] = intent.revision;
            if (intent.subscribed == auto_subscribe) continue;
            const auto participant = remote_participants_.find(key.participant_sid);
            if (participant == remote_participants_.end() || !participant->second ||
                participant->second->identity() != key.participant_identity ||
                !participant->second->get_publication(key.track_sid)) {
                continue;
            }
            subscription_exceptions[key.participant_sid].push_back(key.track_sid);
        }
    }

    auto* subscription = state.mutable_subscription();
    subscription->set_subscribe(!auto_subscribe);
    for (const auto& [participant_sid, track_sids] : subscription_exceptions) {
        auto* participant_tracks = subscription->add_participant_tracks();
        participant_tracks->set_participant_sid(participant_sid);
        for (const auto& track_sid : track_sids) {
            subscription->add_track_sids(track_sid);
            participant_tracks->add_track_sids(track_sid);
        }
    }

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
    {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> dt_dc;
        {
            std::lock_guard lock(room_mutex_);
            dt_dc = data_track_dc_;
            for (const auto& [pub_handle, entry] : local_data_tracks_) {
                (void)pub_handle;
                if (!entry || entry->unpublished) continue;
                const auto live = entry->track.lock();
                if (!live || !live->IsPublished()) continue;
                auto* resp = state.add_publish_data_tracks();
                auto* info = resp->mutable_info();
                const auto snapshot = live->info();
                info->set_pub_handle(entry->pub_handle);
                info->set_sid(snapshot.sid);
                info->set_name(snapshot.name);
                info->set_encryption(snapshot.uses_e2ee
                    ? proto::Encryption::GCM
                    : proto::Encryption::NONE);
            }
        }
        add_data_channel(dt_dc, proto::SignalTarget::PUBLISHER);
    }
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

asio::awaitable<void> Room::WaitForRemoteMediaRecoveryHandshake(
    uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(owner_generation) ||
                installed_session_generation_ != owner_generation ||
                !remote_media_recovery_pending_) {
                co_return;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::vector<std::pair<std::shared_ptr<RemoteTrackPublication>, uint64_t>>
                detach;
            {
                std::lock_guard lock(room_mutex_);
                if (IsSignalGenerationCurrentLocked(owner_generation) &&
                    installed_session_generation_ == owner_generation &&
                    remote_media_recovery_pending_) {
                    for (const auto& [participant_sid, participant] :
                         remote_participants_) {
                        if (!participant) continue;
                        for (const auto& [track_sid, base_publication] :
                             participant->tracks()) {
                            const auto publication =
                                std::dynamic_pointer_cast<RemoteTrackPublication>(
                                    base_publication);
                            if (!publication || !publication->track() ||
                                publication->track()->kind() != TrackKind::Video) {
                                continue;
                            }
                            const auto key = MakeSubscriptionIntentKeyLocked(
                                participant_sid, participant->identity(), track_sid);
                            auto& intent = EnsureSubscriptionIntentLocked(key);
                            const bool subscription_changed = intent.subscribed;
                            const bool settings_changed = intent.enabled ||
                                intent.quality != proto::VideoQuality::OFF ||
                                intent.width != 0 || intent.height != 0 ||
                                intent.max_fps != 0 || intent.priority != 0;
                            if (!subscription_changed && !settings_changed) continue;
                            intent.subscribed = false;
                            intent.enabled = false;
                            intent.quality = proto::VideoQuality::OFF;
                            intent.width = intent.height = 0;
                            intent.max_fps = intent.priority = 0;
                            intent.revision = next_subscription_revision_++;
                            intent.subscription_dirty =
                                intent.subscription_dirty ||
                                !intent.sent_subscribed.has_value() ||
                                *intent.sent_subscribed;
                            intent.settings_dirty = true;
                            intent.accepted_at = {};
                            QueueSubscriptionUpdateLocked(key);

                            RemotePublicationControlRequest control;
                            control.kind =
                                RemotePublicationControlRequest::Kind::Subscription;
                            control.subscribed = false;
                            control.enabled = false;
                            control.quality = proto::VideoQuality::OFF;
                            control.width = control.height = control.priority = 0;
                            publication->CommitControl(control);
                            if (publication->track()) {
                                SetRemoteMediaExpected(
                                    session_telemetry_.lock(), publication->track()->kind(),
                                    key.participant_sid, publication->sid(), false,
                                    telemetry::MediaExpectationReason::Unsubscribed);
                            }
                            const auto binding =
                                current_remote_binding_serials_.find(publication.get());
                            if (binding != current_remote_binding_serials_.end()) {
                                if (const auto media =
                                        FindMediaBindingLocked(binding->second)) {
                                    media->active.store(false,
                                        std::memory_order_release);
                                }
                                detach.push_back({publication, binding->second});
                            }
                        }
                    }
                    CompleteRemoteMediaRecoveryLocked();
                }
            }
            for (const auto& [publication, binding] : detach) {
                DetachRemotePublicationMedia(
                    publication.get(), /*notify_listener=*/true, binding);
            }
            Log("WARNING", "MEDIA_RECOVERY_TIMEOUT",
                "Remote media plan recovery timed out; video demand is held empty");
            co_return;
        }
        asio::steady_timer timer(executor_, std::chrono::milliseconds(5));
        std::error_code error;
        co_await timer.async_wait(
            asio::redirect_error(asio::use_awaitable, error));
        if (error && error != asio::error::operation_aborted) co_return;
    }
}

asio::awaitable<void> Room::AttemptReconnect(
    uint64_t owner_generation,
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner,
    std::string reconnect_episode_id) {
    auto operation_admission = callback_gate_->Enter();
    TelemetryOperationSpan reconnect_episode(
        telemetry_owner, telemetry::OperationKind::ReconnectEpisode,
        std::move(reconnect_episode_id), true);
    // The episode was accepted by HandleSignalEvent before this coroutine was
    // scheduled. Even a retirement-time rejection must close that old span.
    if (!operation_admission) co_return;
    std::string reconnect_url;
    std::string reconnect_token;
    SignalOptions reconnect_options;
    uint64_t reconnect_generation = owner_generation;
    {
        std::lock_guard lock(room_mutex_);
        if (reconnect_generation == 0) {
            reconnect_generation = session_generation_.load(std::memory_order_acquire);
        }
        if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
            installed_session_generation_ != reconnect_generation ||
            connection_state_ != ConnectionState::Reconnecting ||
            reconnect_active_ || reconnect_disabled_) {
            co_return;
        }
        reconnect_active_ = true;
        if (signal_client_) {
            reconnect_url = signal_client_->url();
            reconnect_token = signal_client_->token();
            reconnect_options = signal_client_->options();
        }
    }

    const auto episode_started_at = std::chrono::steady_clock::now();
    diagnostic::Event episode_event;
    episode_event.kind = diagnostic::EventKind::ReconnectEpisodeStarted;
    episode_event.context.room_generation = reconnect_generation;
    episode_event.context.has_room_generation = true;
    episode_event.context.operation_id.Assign(reconnect_episode.id().empty()
        ? "reconnect_" + std::to_string(reconnect_generation) + "_" +
            std::to_string(episode_started_at.time_since_epoch().count())
        : reconnect_episode.id());
    EmitDiagnostic(episode_event);
    reconnect_episode.OnTerminal(
        [weak = weak_from_this(), episode_event, episode_started_at](
            telemetry::OperationOutcome outcome) mutable {
            if (const auto room = weak.lock()) {
                episode_event.kind = diagnostic::EventKind::ReconnectEpisodeTerminal;
                episode_event.outcome = DiagnosticOutcome(outcome);
                episode_event.duration_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - episode_started_at).count();
                if (outcome == telemetry::OperationOutcome::Failure ||
                    outcome == telemetry::OperationOutcome::Timeout) {
                    episode_event.error_layer = diagnostic::ErrorLayer::Native;
                    episode_event.error_code = diagnostic::ErrorCode::ReconnectExhausted;
                }
                room->EmitDiagnostic(episode_event);
            }
        });

    RecordPublishedTracks();

    std::uint32_t attempts = 0;
    bool full_restart = false;
    {
        std::lock_guard lock(room_mutex_);
        full_restart = full_reconnect_requested_ ||
            (join_response_ && join_response_->has_client_configuration() &&
             join_response_->client_configuration().resume_connection() ==
                 proto::ClientConfigSetting::DISABLED);
        full_reconnect_requested_ = false;
    }
    std::string last_error = "reconnect attempts exhausted";
    const auto reconnect_deadline = std::chrono::steady_clock::now() +
        operation_timeouts_.reconnect_total;

    while (attempts < (std::numeric_limits<std::uint32_t>::max)() &&
           std::chrono::steady_clock::now() < reconnect_deadline) {
        attempts++;
        auto delay = kBaseReconnectDelay * (1 << (std::min)(attempts - 1, 4u));
        if (delay > kMaxReconnectDelay) delay = kMaxReconnectDelay;
        // Deterministic operation id based jitter avoids synchronized reconnect storms.
        delay += std::chrono::milliseconds(
            static_cast<int>(operation_sequence_.fetch_add(1, std::memory_order_relaxed) % 75));

        delay = (std::min)(delay,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                reconnect_deadline - std::chrono::steady_clock::now()));
        if (delay <= std::chrono::milliseconds::zero()) break;

        asio::steady_timer timer(executor_, delay);
        std::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                (installed_session_generation_ != 0 &&
                 installed_session_generation_ != reconnect_generation)) {
                co_return;
            }
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
        TelemetryOperationSpan reconnect_attempt(
            telemetry_owner, telemetry::OperationKind::ReconnectAttempt);
        const auto attempt_started_at = std::chrono::steady_clock::now();
        auto attempt_mode = std::make_shared<diagnostic::Stage>(
            full_restart ? diagnostic::Stage::FullRestart : diagnostic::Stage::Resume);
        auto attempt_error = std::make_shared<diagnostic::Event>();
        diagnostic::Event attempt_event;
        attempt_event.kind = diagnostic::EventKind::ReconnectAttemptStarted;
        attempt_event.context.room_generation = reconnect_generation;
        attempt_event.context.has_room_generation = true;
        attempt_event.context.operation_id.Assign(reconnect_attempt.id().empty()
            ? "reconnect_attempt_" + std::to_string(reconnect_generation) + "_" +
                std::to_string(attempts) : reconnect_attempt.id());
        attempt_event.context.parent_operation_id = episode_event.context.operation_id;
        attempt_event.attempt = attempts;
        attempt_event.stage = *attempt_mode;
        EmitDiagnostic(attempt_event);
        reconnect_attempt.OnTerminal(
            [weak = weak_from_this(), attempt_event, attempt_mode, attempt_error,
             attempt_started_at](telemetry::OperationOutcome outcome) mutable {
                if (const auto room = weak.lock()) {
                    attempt_event.kind = diagnostic::EventKind::ReconnectAttemptTerminal;
                    attempt_event.stage = *attempt_mode;
                    attempt_event.outcome = DiagnosticOutcome(outcome);
                    if (outcome != telemetry::OperationOutcome::Success) {
                        attempt_event.error_layer = diagnostic::ErrorLayer::Native;
                        attempt_event.error_code = attempt_error->error_code ==
                            diagnostic::ErrorCode::None
                            ? diagnostic::ErrorCode::Cancelled
                            : attempt_error->error_code;
                        attempt_event.retryable = attempt_error->retryable;
                    }
                    attempt_event.duration_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - attempt_started_at).count();
                    room->EmitDiagnostic(attempt_event);
                }
            });

        if (!full_restart) {
            try {
                std::shared_ptr<SignalClient> signal;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation) {
                        co_return;
                    }
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

                co_await WaitForRemoteMediaRecoveryHandshake(
                    reconnect_generation,
                    std::min(attempt_deadline,
                        std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(250)));

                SubscriptionSyncSnapshot subscription_snapshot;
                proto::SignalRequest sync_request;
                *sync_request.mutable_sync_state() =
                    BuildSyncState(&subscription_snapshot);
                std::shared_ptr<ConnectAttemptInstrumentation> instrumentation_hooks;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation ||
                        signal_client_ != signal ||
                        subscription_snapshot.logical_session !=
                            subscription_session_generation_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "sync_state_admission",
                                             "reconnect was replaced before SyncState");
                    }
                    instrumentation_hooks = connect_attempt_instrumentation_;
                }
                if (instrumentation_hooks && instrumentation_hooks->before_subscription_sync_send) {
                    co_await instrumentation_hooks->before_subscription_sync_send(
                        sync_request.sync_state());
                }
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation ||
                        signal_client_ != signal ||
                        subscription_snapshot.logical_session !=
                            subscription_session_generation_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "sync_state_send",
                                             "reconnect was replaced before SyncState send");
                    }
                }
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

                LifecycleListenerDelivery reconnected_delivery;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation ||
                        connection_state_ != ConnectionState::Reconnecting ||
                        reconnect_disabled_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "resume_commit",
                                             "reconnect was cancelled before commit");
                    }
                    connection_state_ = ConnectionState::Connected;
                    reconnect_attempts_ = 0;
                    reconnect_active_ = false;
                    FinishSubscriptionRecoveryLocked(subscription_snapshot, signal);
                    EnqueueRosterLocked();
                    ScheduleSubscriptionDrainLocked();
                    reconnected_delivery.kind = LifecycleListenerEventKind::Reconnected;
                    reconnected_delivery.generation = reconnect_generation;
                    reconnected_delivery.required_state = ConnectionState::Connected;
                    reconnected_delivery.listeners = listeners_;
                }
                DeliverLifecycleListenerEvent(std::move(reconnected_delivery));
                reconnect_attempt.Finish(telemetry::OperationOutcome::Success);
                reconnect_episode.Finish(telemetry::OperationOutcome::Success);
                co_return;
            } catch (const std::exception& error) {
                ProjectOperationError(std::current_exception(), *attempt_error);
                last_error = error.what();
                Log("WARNING", "RESUME_FAILED",
                    secure_log::ExceptionSummary("resume_reconnect") +
                        "; switching to full restart");
                full_restart = true;
                *attempt_mode = diagnostic::Stage::FullRestart;
                auto mode_event = attempt_event;
                mode_event.kind = diagnostic::EventKind::ReconnectModeChanged;
                mode_event.stage = diagnostic::Stage::FullRestart;
                mode_event.error_layer = attempt_error->error_layer;
                mode_event.error_code = attempt_error->error_code;
                mode_event.retryable = attempt_error->retryable;
                EmitDiagnostic(mode_event);
            }
        }

        if (full_restart) {
            uint64_t restart_connect_generation = 0;
            try {
                std::deque<QueuedParticipantEvent> retired_events;
                std::shared_ptr<SignalClient> old_signal;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> old_publisher;
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> old_subscriber;
                std::shared_ptr<webrtc::PeerConnectionObserver> old_publisher_observer;
                std::shared_ptr<webrtc::PeerConnectionObserver> old_subscriber_observer;
                std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> old_data_channels;
                std::vector<std::shared_ptr<webrtc::DataChannelObserver>> old_data_channel_observers;
                std::vector<RemoteTrackSinkBinding> old_track_sinks;
                std::uint64_t retired_media_generation = 0;
                PendingOperationCleanup old_pending;
                LifecycleListenerDelivery reconnected_delivery;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        (installed_session_generation_ != 0 &&
                         installed_session_generation_ != reconnect_generation)) {
                        co_return;
                    }
                    if (reconnect_disabled_) {
                        reconnect_active_ = false;
                        co_return;
                    }
                    retired_media_generation = installed_session_generation_;
                    if (const auto telemetry_owner = session_telemetry_.lock()) {
                        std::map<std::string, std::string> recovery_identities;
                        for (const auto& [participant_sid, participant] : remote_participants_) {
                            if (!participant) continue;
                            for (const auto& [track_sid, publication] : participant->tracks()) {
                                if (!publication) continue;
                                const auto identity = RemoteMediaRecoveryIdentity(
                                    *participant, *publication);
                                if (publication->SnapshotState().kind == TrackKind::Video) {
                                    recovery_identities.emplace(
                                        RemoteVideoTelemetryKey(participant_sid, track_sid), identity);
                                    recovery_identities.emplace(
                                        RemoteVideoRenderTelemetryKey(participant_sid, track_sid), identity);
                                } else if (publication->SnapshotState().kind == TrackKind::Audio) {
                                    recovery_identities.emplace(
                                        RemoteAudioTelemetryKey(participant_sid, track_sid), identity);
                                }
                            }
                        }
                        // Enqueue before old sinks emit BindingEnded. This is
                        // transport replacement, not a logical unsubscribe.
                        telemetry_owner->RetireRemoteMediaGeneration(
                            retired_media_generation, std::chrono::steady_clock::now(),
                            std::move(recovery_identities));
                    }
                    old_signal = std::move(signal_client_);
                    installed_session_generation_ = 0;
                    old_publisher = std::move(publisher_pc_);
                    old_subscriber = std::move(subscriber_pc_);
                    old_publisher_observer = std::move(publisher_observer_);
                    old_subscriber_observer = std::move(subscriber_observer_);
                    PrepareDataTracksForFullRestartLocked();
                    RetireAllMembershipsLocked(retired_events);
                    local_participant_.reset();
                    pending_local_unpublishes_.clear();
                    ClearRemotePublicationMediaBindingsLocked();
                    remote_participants_.clear();
                    remote_participant_joined_at_ms_.clear();
                    if (reliable_dc_) {
                        old_data_channels.push_back(reliable_dc_);
                        reliable_dc_ = nullptr;
                    }
                    if (lossy_dc_) {
                        old_data_channels.push_back(lossy_dc_);
                        lossy_dc_ = nullptr;
                    }
                    if (data_track_dc_) {
                        old_data_channels.push_back(data_track_dc_);
                        data_track_dc_ = nullptr;
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
                    reconnect_generation =
                        session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
                    old_pending = TakePendingOperationsLocked();
                }
                retired_events.clear();
                FailPendingOperations(std::move(old_pending),
                                      OperationErrorCode::Cancelled,
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

                restart_connect_generation = reconnect_generation + 1;
                co_await ConnectAsync(reconnect_url, reconnect_token, reconnect_options);

                std::shared_ptr<LocalParticipant> local;
                std::vector<std::shared_ptr<RoomListener>> listeners;
                std::optional<RemoteMediaRecoveryRequest> media_recovery_request;
                RemoteMediaRecoveryHandler media_recovery_handler;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(restart_connect_generation) ||
                        installed_session_generation_ != restart_connect_generation ||
                        connection_state_ != ConnectionState::Connected ||
                        !reconnect_active_ ||
                        reconnect_disabled_) {
                        throw OperationError(OperationKind::Reconnect,
                                             OperationErrorCode::Cancelled,
                                             "full_restart_commit",
                                             "reconnect was cancelled by server leave");
                    }
                    reconnect_generation = restart_connect_generation;
                    local = local_participant_;
                    listeners = listeners_;
                    if (remote_media_plan_active_ &&
                        remote_media_recovery_handler_) {
                        media_recovery_request = BeginRemoteMediaRecoveryLocked();
                        media_recovery_handler = remote_media_recovery_handler_;
                    }
                }
                if (!local) {
                    throw OperationError(OperationKind::Reconnect,
                                         OperationErrorCode::StateUncertain,
                                         "full_restart_republish",
                                         "local participant is missing after full restart");
                }
                if (media_recovery_request && media_recovery_handler) {
                    try {
                        media_recovery_handler(*media_recovery_request);
                    } catch (...) {
                        Log("WARNING", "MEDIA_RECOVERY_HANDLER",
                            "Remote media full-restart handler failed; waiting for timeout");
                    }
                    co_await WaitForRemoteMediaRecoveryHandshake(
                        reconnect_generation,
                        std::min(attempt_deadline,
                            std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(250)));
                }

                co_await RepublishLocalTracks(reconnect_generation);

                bool wait_for_reliable_data_channel = false;
                bool negotiate_lazy_publisher = false;
                std::function<void(uint64_t)> before_data_channel_wait;
                std::function<asio::awaitable<void>(std::chrono::milliseconds, uint64_t)>
                    negotiate_publisher_override;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation ||
                        connection_state_ != ConnectionState::Connected ||
                        !reconnect_active_ || reconnect_disabled_) {
                        co_return;
                    }
                    wait_for_reliable_data_channel = require_media_connection_;
                    // Subscriber-primary dual-PC joins may leave the publisher
                    // lazy. With no tracks to republish, explicitly negotiate
                    // its SCTP transport before waiting for the data channel.
                    negotiate_lazy_publisher = signal_client_ &&
                        !signal_client_->is_single_pc_mode_active() &&
                        join_response_ && join_response_->subscriber_primary() &&
                        !join_response_->fast_publish() && local->tracks().empty();
                    if (stream_delivery_instrumentation_) {
                        before_data_channel_wait =
                            stream_delivery_instrumentation_->before_full_restart_data_channel_wait;
                        negotiate_publisher_override =
                            stream_delivery_instrumentation_->negotiate_full_restart_publisher;
                    }
                }
                if (before_data_channel_wait) {
                    before_data_channel_wait(reconnect_generation);
                    wait_for_reliable_data_channel = true;
                }
                if (wait_for_reliable_data_channel) {
                    const auto remaining_data_budget = [&] {
                        const auto remaining =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                attempt_deadline - std::chrono::steady_clock::now());
                        if (remaining <= std::chrono::milliseconds::zero()) {
                            throw OperationError(OperationKind::Reconnect,
                                                 OperationErrorCode::DataChannelUnavailable,
                                                 "wait_reliable_data_channel",
                                                 "reconnect attempt timed out before data transport readiness",
                                                 true);
                        }
                        return remaining;
                    };
                    if (negotiate_lazy_publisher) {
                        const auto negotiation_budget = std::min(
                            reconnect_options.timeouts.negotiation,
                            remaining_data_budget());
                        if (negotiate_publisher_override) {
                            co_await negotiate_publisher_override(
                                negotiation_budget, reconnect_generation);
                        } else {
                            co_await NegotiatePublisherAsync(
                                negotiation_budget, reconnect_generation);
                        }
                    }
                    co_await WaitForReliableDataChannel(
                        std::min(reconnect_options.timeouts.peer_connection,
                                 remaining_data_budget()),
                        reconnect_generation);
                }

                co_await RecoverDataTracksAfterFullRestartAsync(reconnect_generation);

                {
                    std::lock_guard lock(room_mutex_);
                    if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
                        installed_session_generation_ != reconnect_generation ||
                        connection_state_ != ConnectionState::Connected ||
                        !reconnect_active_ || reconnect_disabled_) {
                        co_return;
                    }
                    reconnect_attempts_ = 0;
                    reconnect_active_ = false;
                    subscription_recovery_barrier_ = false;
                    ScheduleSubscriptionDrainLocked();
                    if (local_participant_) {
                        EnqueueParticipantEventLocked(MakeParticipantEventLocked(
                            ParticipantEventKind::Upsert,
                            local_participant_,
                            true));
                    }
                    reconnected_delivery.kind = LifecycleListenerEventKind::Reconnected;
                    reconnected_delivery.generation = reconnect_generation;
                    reconnected_delivery.required_state = ConnectionState::Connected;
                    reconnected_delivery.listeners = listeners;
                }
                DeliverLifecycleListenerEvent(std::move(reconnected_delivery));
                reconnect_attempt.Finish(telemetry::OperationOutcome::Success);
                reconnect_episode.Finish(telemetry::OperationOutcome::Success);
                co_return;
            } catch (const std::exception& error) {
                const auto operation_outcome = TelemetryOutcome(std::current_exception());
                ProjectOperationError(std::current_exception(), *attempt_error);
                last_error = error.what();
                Log("WARNING", "FULL_RESTART_FAILED",
                    secure_log::ExceptionSummary("full_restart"));
                std::lock_guard lock(room_mutex_);
                const uint64_t current_generation =
                    session_generation_.load(std::memory_order_acquire);
                const uint64_t failed_connect_cleanup_generation =
                    restart_connect_generation + 1;
                if (restart_connect_generation != 0 &&
                    current_generation == restart_connect_generation &&
                    installed_session_generation_ == restart_connect_generation &&
                    connection_state_ == ConnectionState::Connected &&
                    !reconnect_disabled_ && reconnect_active_) {
                    reconnect_generation = restart_connect_generation;
                    connection_state_ = ConnectionState::Reconnecting;
                } else if (restart_connect_generation != 0 &&
                    current_generation == failed_connect_cleanup_generation &&
                    connection_state_ == ConnectionState::Disconnected &&
                    installed_session_generation_ == 0 &&
                    !reconnect_disabled_ && reconnect_active_) {
                    reconnect_generation = failed_connect_cleanup_generation;
                    connection_state_ = ConnectionState::Reconnecting;
                } else if (restart_connect_generation == 0 &&
                           current_generation == reconnect_generation &&
                           connection_state_ == ConnectionState::Disconnected &&
                           installed_session_generation_ == 0 &&
                           !reconnect_disabled_ && reconnect_active_) {
                    connection_state_ = ConnectionState::Reconnecting;
                } else if (connection_state_ != ConnectionState::Reconnecting ||
                           !IsSignalGenerationCurrentLocked(reconnect_generation)) {
                    co_return;
                }
                suppress_next_connected_event_ = false;
                reconnect_attempt.Finish(operation_outcome);
            }
        }
    }

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
            (installed_session_generation_ != 0 &&
             installed_session_generation_ != reconnect_generation)) {
            co_return;
        }
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
    std::vector<std::shared_ptr<webrtc::DataChannelObserver>> data_channel_observers;
    std::vector<RemoteTrackSinkBinding> track_sinks;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    std::deque<QueuedParticipantEvent> retired_events;
    PendingOperationCleanup pending;
    uint64_t disconnected_generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(reconnect_generation) ||
            (installed_session_generation_ != 0 &&
             installed_session_generation_ != reconnect_generation)) {
            co_return;
        }
        signal = std::move(signal_client_);
        installed_session_generation_ = 0;
        publisher = std::move(publisher_pc_);
        subscriber = std::move(subscriber_pc_);
        publisher_observer = std::move(publisher_observer_);
        subscriber_observer = std::move(subscriber_observer_);
        RetireAllMembershipsLocked(retired_events);
        local_participant_.reset();
        pending_local_unpublishes_.clear();
        ClearRemotePublicationMediaBindingsLocked();
        remote_participants_.clear();
        remote_participant_joined_at_ms_.clear();
        if (reliable_dc_) {
            data_channels.push_back(reliable_dc_);
            reliable_dc_ = nullptr;
        }
        if (lossy_dc_) {
            data_channels.push_back(lossy_dc_);
            lossy_dc_ = nullptr;
        }
        if (data_track_dc_) {
            data_channels.push_back(data_track_dc_);
            data_track_dc_ = nullptr;
        }
        for (auto& dc : remote_data_channels_) {
            if (dc) data_channels.push_back(dc);
        }
        remote_data_channels_.clear();
        data_channel_observers = std::move(data_channel_observers_);
        track_sinks = std::move(remote_track_sinks_);
        processed_remote_track_ids_.clear();
        pending_track_queue_.clear();
        ClearSubscriptionSessionLocked();
        connection_state_ = ConnectionState::Disconnected;
        reconnect_active_ = false;
        suppress_next_connected_event_ = false;
        listeners_snapshot = listeners_;
        disconnected_generation =
            session_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        pending = TakePendingOperationsLocked();
    }
    retired_events.clear();
    FailPendingOperations(std::move(pending),
                          OperationErrorCode::ReconnectExhausted,
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
    LifecycleListenerDelivery delivery;
    delivery.kind = LifecycleListenerEventKind::Disconnected;
    delivery.generation = disconnected_generation;
    delivery.required_state = ConnectionState::Disconnected;
    delivery.reason = RoomDisconnectReason::NetworkError;
    delivery.detail = "Reconnect failed: " + last_error;
    delivery.listeners = std::move(listeners_snapshot);
    DeliverLifecycleListenerEvent(std::move(delivery));
    reconnect_episode.Finish(
        std::chrono::steady_clock::now() >= reconnect_deadline
            ? telemetry::OperationOutcome::Timeout
            : telemetry::OperationOutcome::Failure);
}

void Room::UpdateRoomInfo(const proto::Room& room, uint64_t event_generation) {
    RoomInfo updated;
    std::string old_metadata;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    bool metadata_changed = false;
    bool room_changed = false;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
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
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnRoomMetadataChanged(updated, old_metadata, updated.metadata);
            });
        }
    }
    for (const auto& listener : listeners_snapshot) {
        DeliverListener({event_generation}, listener, [&](RoomListener& target) {
            target.OnRoomUpdated(updated);
        });
    }
}

void Room::UpdateConnectionQuality(
    const proto::ConnectionQualityUpdate& update,
    uint64_t event_generation) {
    struct Change {
        std::shared_ptr<Participant> participant;
        ConnectionQuality quality = ConnectionQuality::Unknown;
        float score = 0.0f;
    };

    std::vector<Change> changes;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
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
                    "Ignoring connection quality update for an unknown participant: " + quality_update.participant_sid());
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
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnConnectionQualityChanged(change.participant,
                                                     change.quality,
                                                     change.score);
            }, true);
        }
    }
}

void Room::UpdateTrackStreamStates(
    const proto::StreamStateUpdate& update,
    uint64_t event_generation) {
    struct Change {
        std::shared_ptr<Participant> participant;
        std::shared_ptr<TrackPublication> publication;
        TrackPublication::StreamState state;
    };

    std::vector<Change> changes;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
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
                    "Ignoring stream state update for an unknown participant or track: participant=" +
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
        if (change.publication && change.publication->track() &&
            change.participant != local_participant_) {
            const auto snapshot = change.publication->SnapshotState();
            if (const auto telemetry_owner = session_telemetry_.lock()) {
                SetRemoteMediaExpected(
                    telemetry_owner,
                    snapshot.kind,
                    change.participant->sid(), change.publication->sid(),
                    IsRemoteMediaExpected(
                        snapshot, IsRemotePublicationSubscribed(change.publication)),
                    change.state == TrackPublication::StreamState::Paused
                        ? telemetry::MediaExpectationReason::StreamPaused
                        : telemetry::MediaExpectationReason::StreamActive);
            }
        }
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({event_generation}, listener, [&](RoomListener& target) {
                target.OnTrackStreamStateChanged(change.participant,
                                                    change.publication,
                                                    change.state);
            }, true);
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
    uint64_t generation) {

    std::vector<PublishedTrackRecord> records;
    std::shared_ptr<LocalParticipant> local;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) co_return;
        records = published_track_records_;
        local = local_participant_;
    }

    if (!local) co_return;

    std::vector<PublishedTrackRecord> valid_records;
    std::vector<std::shared_ptr<Track>> tracks_to_publish;
    for (auto& record : records) {
        if (record.track) {
            valid_records.push_back(record);
            tracks_to_publish.push_back(record.track);
        }
    }
    if (tracks_to_publish.empty()) co_return;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            !reconnect_active_ || reconnect_disabled_) co_return;
        for (const auto& track : tracks_to_publish) {
            reconnect_republish_tracks_.insert(track.get());
        }
    }

    std::vector<std::shared_ptr<TrackPublication>> new_pubs;
    try {
        new_pubs = co_await local->PublishTracksBatchAsync(std::move(tracks_to_publish));
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        for (const auto& record : valid_records) {
            reconnect_republish_tracks_.erase(record.track.get());
        }
        throw;
    }
    for (size_t i = 0; i < new_pubs.size() && i < valid_records.size(); ++i) {
        DeliverRepublishedTrack(generation, valid_records[i].previous_sid, std::move(new_pubs[i]));
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

    // Signaling-only mode is deliberately supported by headless callers.
    // A successful reconnect transaction ends at the signaling SyncState ACK in
    // this mode; there is no media transport whose ICE state can be restarted.
    if (!require_media_connection_) {
        co_return;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration new_config;
    new_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    new_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    new_config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
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
        new_config.tcp_candidate_policy = disable_ice_tcp_for_inspection_
            ? webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled
            : webrtc::PeerConnectionInterface::kTcpCandidatePolicyEnabled;
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
                       bool publisher_role,
                       bool subscriber_role) {
    for (const auto& candidate_pair : stats.candidate_pairs) {
        if (!candidate_pair.current_pair) {
            continue;
        }
        if (publisher_role) {
            room_report.publisher_rtt_ms =
                candidate_pair.current_round_trip_time * 1000.0;
            room_report.available_outgoing_bitrate =
                candidate_pair.available_outgoing_bitrate;
        }
        if (subscriber_role) {
            room_report.subscriber_rtt_ms =
                candidate_pair.current_round_trip_time * 1000.0;
        }
    }

    for (const auto& outbound : stats.outbound_rtp) {
        room_report.total_bytes_sent += outbound.bytes_sent;
    }
    for (const auto& inbound : stats.inbound_rtp) {
        room_report.total_bytes_received += inbound.bytes_received;
    }
    room_report.reports.push_back(stats);
}

void CountStatsResult(RoomStatsReport& room_report,
                      RtcStatsCollectionStatus status) {
    switch (status) {
    case RtcStatsCollectionStatus::Success:
        ++room_report.successful_peer_connection_count;
        break;
    case RtcStatsCollectionStatus::Timeout:
        ++room_report.timed_out_peer_connection_count;
        break;
    case RtcStatsCollectionStatus::Rejected:
    case RtcStatsCollectionStatus::Failed:
        ++room_report.rejected_peer_connection_count;
        break;
    }
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

    const auto collect = [&room_report](
        const webrtc::scoped_refptr<webrtc::PeerConnectionInterface>& peer,
        bool publisher_role,
        bool subscriber_role) {
        if (!peer) return;
        ++room_report.actual_peer_connection_count;
        auto state = std::make_shared<RtcStatsState>();
        if (RequestRtcStats(peer, state)) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->cv.wait_for(lock, std::chrono::milliseconds(1500), [&]() { return state->done; })) {
                const auto status = state->status;
                const auto report = state->report;
                lock.unlock();
                CountStatsResult(room_report, status);
                if (status == RtcStatsCollectionStatus::Success) {
                    AppendStatsReport(
                        room_report, report, publisher_role, subscriber_role);
                }
            } else {
                state->done = true;
                state->timed_out = true;
                state->status = RtcStatsCollectionStatus::Timeout;
                lock.unlock();
                CountStatsResult(room_report, RtcStatsCollectionStatus::Timeout);
            }
        } else {
            CountStatsResult(room_report, RtcStatsCollectionStatus::Rejected);
        }
    };

    if (pub_pc && pub_pc.get() == sub_pc.get()) {
        collect(pub_pc, true, true);
    } else {
        collect(pub_pc, true, false);
        collect(sub_pc, false, true);
    }

    return room_report;
}

asio::awaitable<RoomStatsReport> Room::GetStats(
    std::function<void()> late_completion) {
    RoomStatsReport room_report;
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) co_return room_report;
    room_report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> subscriber;
    {
        std::lock_guard lock(room_mutex_);
        publisher = publisher_pc_;
        subscriber = subscriber_pc_;
    }

    const auto append = [&room_report](RtcStatsCollectionResult result,
                                      bool publisher_role,
                                      bool subscriber_role) {
        CountStatsResult(room_report, result.status);
        if (result.status == RtcStatsCollectionStatus::Success && result.report) {
            AppendStatsReport(
                room_report, *result.report, publisher_role, subscriber_role);
        }
    };

    if (publisher && publisher.get() == subscriber.get()) {
        room_report.actual_peer_connection_count = 1;
        append(co_await CollectRtcStatsDetailed(
                   publisher, executor_, std::chrono::milliseconds(1500),
                   std::move(late_completion), executor_lifetime_),
               true, true);
    } else if (publisher && subscriber) {
        room_report.actual_peer_connection_count = 2;
        using namespace asio::experimental::awaitable_operators;
        auto [publisher_result, subscriber_result] = co_await (
            CollectRtcStatsDetailed(
                publisher, executor_, std::chrono::milliseconds(1500), late_completion, executor_lifetime_) &&
            CollectRtcStatsDetailed(
                subscriber, executor_, std::chrono::milliseconds(1500), late_completion, executor_lifetime_));
        append(std::move(publisher_result), true, false);
        append(std::move(subscriber_result), false, true);
    } else if (publisher) {
        room_report.actual_peer_connection_count = 1;
        append(co_await CollectRtcStatsDetailed(
                   publisher, executor_, std::chrono::milliseconds(1500),
                   std::move(late_completion), executor_lifetime_),
               true, false);
    } else if (subscriber) {
        room_report.actual_peer_connection_count = 1;
        append(co_await CollectRtcStatsDetailed(
                   subscriber, executor_, std::chrono::milliseconds(1500),
                   std::move(late_completion), executor_lifetime_),
               false, true);
    }
    co_return room_report;
}

PublisherMediaObjectCounts Room::GetPublisherMediaObjectCounts() const {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    {
        std::lock_guard lock(room_mutex_);
        publisher = publisher_pc_;
    }
    PublisherMediaObjectCounts result;
    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!publisher || !signaling) return result;
    signaling->BlockingCall([&] {
        result.transceivers = publisher->GetTransceivers().size();
        for (const auto& sender : publisher->GetSenders()) {
            if (!sender) continue;
            if (sender->track()) ++result.senders_with_track;
            else ++result.senders_without_track;
        }
    });
    return result;
}

void Room::SetParticipantVolume(const std::string& identity_or_sid, double volume) {
    std::string changed_identity;
    std::vector<std::pair<std::shared_ptr<Participant>,
                          std::shared_ptr<TrackPublication>>> changed;
    {
        std::lock_guard lock(room_mutex_);
        for (auto& [sid, p] : remote_participants_) {
            if (p->identity() == identity_or_sid || p->sid() == identity_or_sid) {
                for (auto& [tsid, pub] : p->tracks()) {
                    if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                        pub->track()->set_volume(volume);
                        changed_identity = p->identity();
                        changed.emplace_back(p, pub);
                    }
                }
            }
        }
    }
    if (const auto telemetry_owner = session_telemetry_.lock()) {
        for (const auto& [participant, publication] : changed) {
            const auto snapshot = publication->SnapshotState();
            SetRemoteMediaExpected(
                telemetry_owner, TrackKind::Audio,
                participant->sid(), publication->sid(),
                IsRemoteMediaExpected(
                    snapshot, IsRemotePublicationSubscribed(publication)),
                volume > 0.001
                    ? telemetry::MediaExpectationReason::VolumeAudible
                    : telemetry::MediaExpectationReason::VolumeZero);
        }
    }
    if (!changed_identity.empty()) {
        Log("MEDIA", "VOLUME_SET", "Set remote participant volume to " +
            std::to_string(static_cast<int>(volume * 100)) + "%");
    }
}

void Room::SetParticipantMuted(const std::string& identity_or_sid, bool muted) {
    std::string changed_identity;
    std::vector<std::pair<std::shared_ptr<Participant>,
                          std::shared_ptr<TrackPublication>>> changed;
    {
        std::lock_guard lock(room_mutex_);
        for (auto& [sid, p] : remote_participants_) {
            if (p->identity() == identity_or_sid || p->sid() == identity_or_sid) {
                for (auto& [tsid, pub] : p->tracks()) {
                    if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                        pub->track()->set_playout_muted(muted);
                        changed_identity = p->identity();
                        changed.emplace_back(p, pub);
                    }
                }
            }
        }
    }
    if (const auto telemetry_owner = session_telemetry_.lock()) {
        for (const auto& [participant, publication] : changed) {
            const auto snapshot = publication->SnapshotState();
            SetRemoteMediaExpected(
                telemetry_owner, TrackKind::Audio,
                participant->sid(), publication->sid(),
                IsRemoteMediaExpected(
                    snapshot, IsRemotePublicationSubscribed(publication)),
                muted ? telemetry::MediaExpectationReason::Muted
                      : telemetry::MediaExpectationReason::Unmuted);
        }
    }
    if (!changed_identity.empty()) {
        Log("MEDIA", "MUTE_SET", "Set remote participant local mute=" +
            std::string(muted ? "true" : "false"));
    }
}

void Room::SetAudioOutputMuted(bool muted) {
    std::vector<std::pair<std::shared_ptr<Participant>,
                          std::shared_ptr<TrackPublication>>> changed;
    {
        std::lock_guard lock(room_mutex_);
        audio_output_muted_ = muted;
        for (auto& [sid, p] : remote_participants_) {
            for (auto& [tsid, pub] : p->tracks()) {
                if (pub && pub->track() && pub->track()->kind() == TrackKind::Audio) {
                    pub->track()->set_output_muted(muted);
                    changed.emplace_back(p, pub);
                }
            }
        }
    }
    if (const auto telemetry_owner = session_telemetry_.lock()) {
        for (const auto& [participant, publication] : changed) {
            const auto snapshot = publication->SnapshotState();
            SetRemoteMediaExpected(
                telemetry_owner, TrackKind::Audio,
                participant->sid(), publication->sid(),
                IsRemoteMediaExpected(
                    snapshot, IsRemotePublicationSubscribed(publication)),
                muted ? telemetry::MediaExpectationReason::OutputMuted
                      : telemetry::MediaExpectationReason::OutputUnmuted);
        }
    }
    Log("MEDIA", "SPEAKER_MUTE", "Set room audio output mute=" + std::string(muted ? "true" : "false"));
}

bool Room::IsAudioOutputMuted() const {
    std::lock_guard lock(room_mutex_);
    return audio_output_muted_;
}

void Room::SimulateScenario(SimulateScenarioType scenario) {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    auto self = shared_from_this();
    livekit::safe_co_spawn(executor_, [self, scenario]() -> asio::awaitable<void> {
        co_await self->SimulateScenarioAsync(scenario);
    });
}

asio::awaitable<void> Room::SimulateScenarioAsync(SimulateScenarioType scenario) {
    auto operation_admission = AdmitOperation(OperationKind::SendData, "SimulateScenarioAsync");
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
        Log("SIMULATE", "SIGNAL_RECONNECT", "Disconnecting signaling to simulate soft reconnect / resume");
        HandleSignalEvent(SignalEvent{SignalEvent::Close, nullptr, "simulated signal reconnect"});
        break;
    }
    case SimulateScenarioType::FullReconnect: {
        Log("SIMULATE", "FULL_RECONNECT", "Simulating hard reconnect / full restart");
        {
            std::lock_guard lock(room_mutex_);
            if (connection_state_ == ConnectionState::Connected && !reconnect_disabled_) {
                full_reconnect_requested_ = true;
            }
        }
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_leave_request_full_reconnect(true);
            try {
                co_await signal->SendAsync(req);
            } catch (...) {
                Log("WARNING", "SIMULATE_SEND_FAILED",
                    secure_log::ExceptionSummary("simulate_full_reconnect"));
            }
        }
        HandleSignalEvent(SignalEvent{SignalEvent::Close, nullptr, "simulated full reconnect"});
        break;
    }
    case SimulateScenarioType::SpeakerUpdate: {
        Log("SIMULATE", "SPEAKER_UPDATE", "Sending SimulateScenario.speaker_update = 3");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_speaker_update(3);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::NodeFailure: {
        Log("SIMULATE", "NODE_FAILURE", "Sending SimulateScenario.node_failure = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_node_failure(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::Migration: {
        Log("SIMULATE", "MIGRATION", "Sending SimulateScenario.migration = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_migration(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::ServerLeave: {
        Log("SIMULATE", "SERVER_LEAVE", "Sending SimulateScenario.server_leave = true");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_server_leave(true);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::SwitchCandidate: {
        Log("SIMULATE", "SWITCH_CANDIDATE", "Sending SimulateScenario.switch_candidate_protocol = TCP");
        if (signal) {
            proto::SignalRequest req;
            auto* sim = req.mutable_simulate();
            sim->set_switch_candidate_protocol(proto::CandidateProtocol::TCP);
            signal->Send(req);
        }
        break;
    }
    case SimulateScenarioType::E2eeKeyRatchet: {
        Log("SIMULATE", "E2EE_RATCHET", "Triggering E2EE key ratchet");
        if (e2ee) {
            e2ee->RatchetKey();
            Log("SIMULATE", "E2EE_RATCHET", "E2EE key ratcheted");
        } else {
            Log("SIMULATE", "E2EE_RATCHET", "E2EE manager is disabled; ignoring key ratchet");
        }
        break;
    }
    case SimulateScenarioType::ParticipantName: {
        std::string new_name = "Simulated " + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 10000);
        Log("SIMULATE", "PARTICIPANT_NAME", "Simulating local participant rename: " + new_name);
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
        Log("SIMULATE", "PARTICIPANT_METADATA", "Simulating local participant metadata update: " + new_meta);
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
        Log("SIMULATE", "CLEAR", "Sending SimulateScenario.subscriber_bandwidth = 0 to clear bandwidth restrictions");
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
