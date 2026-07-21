#include "room.h"
#include "webrtc_manager.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include "api/jsep.h"

namespace livekit {

namespace {

class RoomPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    RoomPeerConnectionObserver(std::shared_ptr<Room> room, int pc_type)
        : room_(room), pc_type_(pc_type) {}

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {}
    
    void OnRenegotiationNeeded() override {
        if (auto room = room_.lock()) {
            asio::post(room->executor(), [room, type = pc_type_]() {
                room->OnRenegotiationNeeded(type);
            });
        }
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        std::string sdp_mid = candidate->sdp_mid();
        int sdp_mline_index = candidate->sdp_mline_index();

        if (auto room = room_.lock()) {
            asio::post(room->executor(), [room, sdp, sdp_mid, sdp_mline_index, type = pc_type_]() {
                room->OnLocalIceCandidate(sdp, sdp_mid, sdp_mline_index, type);
            });
        }
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        if (auto room = room_.lock()) {
            auto receiver = transceiver->receiver();
            auto track = receiver->track();
            asio::post(room->executor(), [room, receiver, track]() {
                room->OnRemoteTrackAdded(receiver, track);
            });
        }
    }

private:
    std::weak_ptr<Room> room_;
    int pc_type_; // 0 = Publisher, 1 = Subscriber
};

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

} // namespace

Room::Room(asio::any_io_executor executor)
    : executor_(executor) {
    CrashHandler::InstallSignalHandlers();
}

Room::~Room() {
    Disconnect();
}

ConnectionState Room::connection_state() const {
    std::lock_guard<std::mutex> lock(room_mutex_);
    return connection_state_;
}

std::shared_ptr<LocalParticipant> Room::local_participant() const {
    std::lock_guard<std::mutex> lock(room_mutex_);
    return local_participant_;
}

std::map<std::string, std::shared_ptr<RemoteParticipant>> Room::remote_participants() const {
    std::lock_guard<std::mutex> lock(room_mutex_);
    return remote_participants_;
}

std::vector<std::shared_ptr<RoomListener>> Room::GetListenersSnapshot() const {
    std::lock_guard<std::mutex> lock(room_mutex_);
    return listeners_;
}

void Room::AddListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    if (listener) {
        listeners_.push_back(listener);
    }
}

void Room::RemoveListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

asio::awaitable<bool> Room::Connect(const std::string& url, const std::string& token, const SignalOptions& opts) {
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (connection_state_ != ConnectionState::Disconnected) {
            co_return false;
        }
        connection_state_ = ConnectionState::Connecting;
    }

    auto self = shared_from_this();
    auto conn_res = co_await SignalClient::Connect(url, token, opts, std::nullopt, [self](const SignalEvent& event) {
        self->HandleSignalEvent(event);
    });

    if (conn_res.error || !conn_res.join_response) {
        std::lock_guard<std::mutex> lock(room_mutex_);
        connection_state_ = ConnectionState::Disconnected;
        co_return false;
    }

    signal_client_ = conn_res.client;
    auto join_res = conn_res.join_response;

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        local_participant_ = std::make_shared<LocalParticipant>(
            join_res->participant().sid(),
            join_res->participant().identity(),
            [self](const proto::SignalRequest& req) {
                if (self->signal_client_) {
                    self->signal_client_->Send(req);
                }
            }
        );

        // 绑定 LocalParticipant 发布 DataChannel 数据包 Handler
        local_participant_->SetPublishDataHandler(
            [self](const std::vector<uint8_t>& payload, bool reliable,
                   const std::vector<std::string>& destination_identities, const std::string& topic) {
                self->PublishData(payload, reliable, destination_identities, topic);
            }
        );

        UpdateParticipants(join_res->other_participants());
        connection_state_ = ConnectionState::Connected;
    }

    auto listeners_snapshot = GetListenersSnapshot();
    for (const auto& listener : listeners_snapshot) {
        listener->OnConnected();
    }

    co_return true;
}

void Room::Disconnect() {
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) return;
        connection_state_ = ConnectionState::Disconnected;
        listeners_snapshot = listeners_;
    }

    if (signal_client_) {
        signal_client_->Close();
        signal_client_.reset();
    }

    publisher_pc_ = nullptr;
    subscriber_pc_ = nullptr;
    reliable_dc_ = nullptr;
    lossy_dc_ = nullptr;

    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected("Client Initiated Disconnect");
    }
}

void Room::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                       const std::vector<std::string>& destination_identities, const std::string& topic) {
    proto::DataPacket packet;
    packet.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);

    auto* user_packet = packet.mutable_user();
    user_packet->set_topic(topic);
    user_packet->set_payload(payload.data(), payload.size());
    for (const auto& dest : destination_identities) {
        user_packet->add_destination_identities(dest);
    }

    std::vector<uint8_t> data(packet.ByteSizeLong());
    packet.SerializeToArray(data.data(), static_cast<int>(data.size()));

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        dc = reliable ? reliable_dc_ : lossy_dc_;
    }

    if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
        std::string payload_str(data.begin(), data.end());
        webrtc::DataBuffer buffer(webrtc::CopyOnWriteBuffer(payload_str.data(), payload_str.size()), /*binary=*/true);
        dc->Send(buffer);
    } else {
        // 若底层 DataChannel 暂未建立物理 Socket，直接进行安全解包分发
        OnIncomingDataPacket(payload, local_participant_ ? local_participant_->identity() : "", topic);
    }
}

void Room::SetDataChannelBufferedAmountLowThreshold(uint64_t threshold, bool reliable) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    if (reliable) {
        reliable_buffered_low_threshold_ = threshold;
    } else {
        lossy_buffered_low_threshold_ = threshold;
    }
}

uint64_t Room::GetDataChannelBufferedAmount(bool reliable) const {
    std::lock_guard<std::mutex> lock(room_mutex_);
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
    std::string text_payload(payload.begin(), payload.end());
    
    // 解析结构化 Chat 消息 (Topic 为 lk.chat 或无指定 Topic 时尝试解析)
    if (topic == "lk.chat" || topic.empty()) {
        auto chat_opt = ChatMessage::Decode(text_payload, participant_sid);
        if (chat_opt.has_value()) {
            auto listeners_snapshot = GetListenersSnapshot();
            std::shared_ptr<Participant> p;
            {
                std::lock_guard<std::mutex> lock(room_mutex_);
                auto it = remote_participants_.find(participant_sid);
                if (it != remote_participants_.end()) {
                    p = it->second;
                } else if (local_participant_ && local_participant_->identity() == participant_sid) {
                    p = local_participant_;
                }
            }
            for (const auto& listener : listeners_snapshot) {
                listener->OnChatMessage(chat_opt.value(), p);
            }
        }
    }
}

void Room::OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    SendTrickleCandidate(sdp, sdp_mid, sdp_mline_index, pc_type);
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
}

void Room::OnRenegotiationNeeded(int pc_type) {
}

void Room::HandleSignalEvent(const SignalEvent& event) {
    if (event.type == SignalEvent::Close) {
        bool should_reconnect = false;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            if (connection_state_ == ConnectionState::Connected && reconnect_attempts_ < kMaxReconnectAttempts) {
                connection_state_ = ConnectionState::Reconnecting;
                reconnect_attempts_++;
                should_reconnect = true;
            }
            listeners_snapshot = listeners_;
        }

        if (should_reconnect) {
            for (const auto& listener : listeners_snapshot) {
                listener->OnReconnecting();
            }
            livekit::safe_co_spawn(executor_, [self = shared_from_this()]() -> asio::awaitable<void> {
                co_await self->AttemptReconnect();
            });
        } else {
            bool notify_disconnect = false;
            {
                std::lock_guard<std::mutex> lock(room_mutex_);
                if (connection_state_ != ConnectionState::Disconnected && connection_state_ != ConnectionState::Reconnecting) {
                    connection_state_ = ConnectionState::Disconnected;
                    notify_disconnect = true;
                }
            }
            if (notify_disconnect) {
                for (const auto& listener : listeners_snapshot) {
                    listener->OnDisconnected(event.close_reason);
                }
            }
        }
    } else if (event.type == SignalEvent::Message) {
        HandleSignalMessage(event.message);
    }
}

void Room::HandleSignalMessage(std::shared_ptr<proto::SignalResponse> msg) {
    if (!msg) return;

    if (msg->has_update()) {
        UpdateParticipants(msg->update().participants());
    } else if (msg->has_mute()) {
        UpdateTrackMute(msg->mute());
    } else if (msg->has_offer()) {
        HandleOfferSignal(msg->offer());
    } else if (msg->has_answer()) {
        HandleAnswerSignal(msg->answer());
    } else if (msg->has_trickle()) {
        HandleTrickleSignal(msg->trickle());
    }
}

void Room::UpdateParticipants(const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants) {
    std::vector<std::shared_ptr<RemoteParticipant>> newly_connected;
    std::vector<std::shared_ptr<RemoteParticipant>> disconnected;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        listeners_snapshot = listeners_;

        for (int i = 0; i < participants.size(); ++i) {
            const auto& p_info = participants.Get(i);
            
            if (local_participant_ && p_info.sid() == local_participant_->sid()) {
                local_participant_->set_metadata(p_info.metadata());
                continue;
            }

            auto it = remote_participants_.find(p_info.sid());
            if (p_info.state() == proto::ParticipantInfo::DISCONNECTED) {
                if (it != remote_participants_.end()) {
                    disconnected.push_back(it->second);
                    remote_participants_.erase(it);
                }
            } else {
                if (it == remote_participants_.end()) {
                    auto remote = std::make_shared<RemoteParticipant>(p_info.sid(), p_info.identity());
                    remote->set_metadata(p_info.metadata());
                    remote_participants_[p_info.sid()] = remote;
                    newly_connected.push_back(remote);
                } else {
                    it->second->set_metadata(p_info.metadata());
                }
            }
        }
    }

    for (const auto& p : newly_connected) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnParticipantConnected(p);
        }
    }

    for (const auto& p : disconnected) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnParticipantDisconnected(p);
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
        std::lock_guard<std::mutex> lock(room_mutex_);
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
    }

    if (target_participant && target_pub) {
        if (target_pub->track()) {
            target_pub->track()->set_muted(mute.muted());
        }
        for (const auto& listener : listeners_snapshot) {
            listener->OnTrackMuted(target_participant, target_pub, mute.muted());
        }
    }
}

void Room::SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    proto::SignalRequest req;
    auto* trickle = req.mutable_trickle();
    
    nlohmann::json candidate_json;
    candidate_json["candidate"] = sdp;
    candidate_json["sdpMid"] = sdp_mid;
    candidate_json["sdpMLineIndex"] = sdp_mline_index;
    
    trickle->set_candidateinit(candidate_json.dump());
    trickle->set_target(pc_type == 0 ? proto::SignalTarget::PUBLISHER : proto::SignalTarget::SUBSCRIBER);

    if (signal_client_) {
        signal_client_->Send(req);
    }
}

void Room::HandleOfferSignal(const proto::SessionDescription& offer) {
    std::shared_ptr<SignalClient> client;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        client = signal_client_;
        sub_pc = subscriber_pc_;
    }

    if (!client || !sub_pc) return;

    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(sub_pc, offer.type(), offer.sdp(), executor_,
        [self, client, sub_pc](const std::string& set_remote_err) {
            if (!set_remote_err.empty()) return;
        });
}

void Room::HandleAnswerSignal(const proto::SessionDescription& answer) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        pub_pc = publisher_pc_;
    }

    if (!pub_pc) return;

    WebRTCManager::Instance().SetRemoteDescription(pub_pc, answer.type(), answer.sdp(), executor_,
        [](const std::string& err) {
            if (!err.empty()) {
                std::cerr << "Room: SetRemoteDescription answer error: " << err << std::endl;
            }
        });
}

void Room::HandleTrickleSignal(const proto::TrickleRequest& trickle) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        pc = (trickle.target() == proto::SignalTarget::PUBLISHER) ? publisher_pc_ : subscriber_pc_;
    }

    if (!pc) return;

    try {
        auto json_cand = nlohmann::json::parse(trickle.candidateinit());
        std::string sdp = json_cand.value("candidate", "");
        std::string sdp_mid = json_cand.value("sdpMid", "");
        int sdp_mline_index = json_cand.value("sdpMLineIndex", 0);

        WebRTCManager::Instance().signaling_thread()->BlockingCall([pc, sdp_mid, sdp_mline_index, sdp]() {
            webrtc::SdpParseError err;
            std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &err));
            if (cand) {
                pc->AddIceCandidate(cand.get());
            }
        });
    } catch (...) {
        std::cerr << "Room: Failed to parse trickle candidate JSON" << std::endl;
    }
}

asio::awaitable<void> Room::AttemptReconnect() {
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        reconnect_active_ = true;
    }

    RecordPublishedTracks();

    int attempts = 0;
    while (attempts < kMaxReconnectAttempts) {
        attempts++;
        auto delay = kBaseReconnectDelay * (1 << (attempts - 1));
        if (delay > kMaxReconnectDelay) delay = kMaxReconnectDelay;

        asio::steady_timer timer(executor_, delay);
        std::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (!signal_client_) break;
    }

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        connection_state_ = ConnectionState::Disconnected;
        reconnect_active_ = false;
    }

    auto listeners_snapshot = GetListenersSnapshot();
    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected("Reconnect Max Retries Exceeded");
    }
}

void Room::RecordPublishedTracks() {
    std::lock_guard<std::mutex> lock(room_mutex_);
    published_track_records_.clear();

    if (!local_participant_) return;

    for (const auto& kv : local_participant_->tracks()) {
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
        std::lock_guard<std::mutex> lock(room_mutex_);
        records = published_track_records_;
        local = local_participant_;
        listeners_snapshot = listeners_;
    }

    if (!local) co_return;

    for (auto& record : records) {
        if (!record.track) continue;

        local->PublishTrack(record.track);

        auto new_pub = local->get_publication(record.track->name());
        for (const auto& listener : listeners_snapshot) {
            listener->OnLocalTrackRepublished(record.previous_sid, new_pub);
        }
    }
}

asio::awaitable<void> Room::RestartIceConnections(
    std::shared_ptr<proto::ReconnectResponse> reconnect_response) {

    if (!reconnect_response) co_return;

    webrtc::PeerConnectionInterface::RTCConfiguration new_config;
    new_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
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
    std::shared_ptr<SignalClient> client;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        pub_pc = publisher_pc_;
        client = signal_client_;
    }

    if (!pub_pc || !client) co_return;

    WebRTCManager::Instance().signaling_thread()->BlockingCall([pub_pc, &new_config]() {
        pub_pc->SetConfiguration(new_config);
    });

    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(pub_pc, executor_,
        [self, client, pub_pc](const std::string& sdp, const std::string& error) {
            if (!error.empty()) {
                std::cerr << "Room: ICE restart CreateOffer failed: " << error << std::endl;
                return;
            }

            WebRTCManager::Instance().SetLocalDescription(pub_pc, "offer", sdp,
                self->executor_,
                [self, client, sdp, pub_pc](const std::string& set_err) {
                    if (!set_err.empty()) return;
                    proto::SignalRequest req;
                    auto* offer_msg = req.mutable_offer();
                    offer_msg->set_type("offer");
                    offer_msg->set_sdp(sdp);
                    client->Send(req);
                });
        });
}

} // namespace livekit
