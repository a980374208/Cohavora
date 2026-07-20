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
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    
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

asio::awaitable<bool> Room::Connect(const std::string& url, const std::string& token, const SignalOptions& opts) {
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (connection_state_ != ConnectionState::Disconnected) {
            co_return false;
        }
        connection_state_ = ConnectionState::Connecting;
    }

    auto self = shared_from_this();
    auto event_handler = [self](const SignalEvent& ev) {
        self->HandleSignalEvent(ev);
    };

    std::cout << "Room::Connect: calling SignalClient::Connect" << std::endl;
    auto conn_res = co_await SignalClient::Connect(url, token, opts, std::nullopt, event_handler);
    if (conn_res.error) {
        std::cerr << "Room::Connect failed: " << conn_res.error.message() << std::endl;
        std::lock_guard<std::mutex> lock(room_mutex_);
        connection_state_ = ConnectionState::Disconnected;
        co_return false;
    }

    auto join_resp = conn_res.join_response;
    std::string local_sid = join_resp->participant().sid();
    std::string local_identity = join_resp->participant().identity();

    auto send_handler = [self](const proto::SignalRequest& req) {
        std::shared_ptr<SignalClient> client;
        {
            std::lock_guard<std::mutex> lock(self->room_mutex_);
            client = self->signal_client_;
        }
        if (client) {
            client->Send(req);
        }
    };

    auto local_part = std::make_shared<LocalParticipant>(local_sid, local_identity, send_handler);
    local_part->set_metadata(join_resp->participant().metadata());

    std::map<std::string, std::shared_ptr<RemoteParticipant>> remote_parts;
    for (int i = 0; i < join_resp->other_participants_size(); ++i) {
        const auto& pinfo = join_resp->other_participants(i);
        auto remote_p = std::make_shared<RemoteParticipant>(pinfo.sid(), pinfo.identity());
        remote_p->set_metadata(pinfo.metadata());
        remote_parts[pinfo.sid()] = remote_p;
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;

    if (opts.create_webrtc_pc) {
        if (!WebRTCManager::Instance().Initialize()) {
            std::cerr << "Room::Connect: Failed to initialize WebRTCManager" << std::endl;
            std::lock_guard<std::mutex> lock(room_mutex_);
            connection_state_ = ConnectionState::Disconnected;
            co_return false;
        }

        auto factory = WebRTCManager::Instance().factory();
        webrtc::PeerConnectionInterface::RTCConfiguration config_pub;
        config_pub.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

        webrtc::PeerConnectionInterface::RTCConfiguration config_sub;
        config_sub.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

        for (int i = 0; i < join_resp->ice_servers_size(); ++i) {
            const auto& ice_srv = join_resp->ice_servers(i);
            webrtc::PeerConnectionInterface::IceServer server;
            for (int j = 0; j < ice_srv.urls_size(); ++j) {
                server.urls.push_back(ice_srv.urls(j));
            }
            server.username = ice_srv.username();
            server.password = ice_srv.credential();
            config_pub.servers.push_back(server);
            config_sub.servers.push_back(server);
        }

        std::unique_ptr<webrtc::PeerConnectionObserver> pub_obs = std::make_unique<RoomPeerConnectionObserver>(self, 0);
        std::unique_ptr<webrtc::PeerConnectionObserver> sub_obs = std::make_unique<RoomPeerConnectionObserver>(self, 1);
        std::cout << "[DEBUG_PC] Observers created. pub_obs=" << pub_obs.get() 
                  << ", sub_obs=" << sub_obs.get() << ". Starting BlockingCall 1..." << std::endl;

        webrtc::PeerConnectionInterface* temp_pub_pc = nullptr;
        WebRTCManager::Instance().signaling_thread()->BlockingCall([factory, config_pub, &pub_obs, &temp_pub_pc]() {
            std::cout << "[DEBUG_PC] Inside BlockingCall 1. Creating publisher PC..." << std::endl;
            webrtc::PeerConnectionDependencies pub_deps(pub_obs.get());
            auto pub_pc_or_err = factory->CreatePeerConnectionOrError(config_pub, std::move(pub_deps));
            if (pub_pc_or_err.ok()) {
                std::cout << "[DEBUG_PC] Publisher PC created successfully." << std::endl;
                temp_pub_pc = pub_pc_or_err.MoveValue().release();
            } else {
                std::cerr << "Room: Failed to create publisher PC: " << pub_pc_or_err.error().message() << std::endl;
            }
        });
        std::cout << "[DEBUG_PC] BlockingCall 1 completed." << std::endl;

        if (!temp_pub_pc) {
            std::cerr << "Room::Connect: Failed to create Publisher PeerConnection" << std::endl;
            Disconnect();
            co_return false;
        }

        pub_pc = webrtc::scoped_refptr<webrtc::PeerConnectionInterface>(temp_pub_pc);
        
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            publisher_observer_ = std::move(pub_obs);
            subscriber_observer_ = std::move(sub_obs);
        }
    }

    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        signal_client_ = conn_res.client;
        local_participant_ = local_part;
        remote_participants_ = remote_parts;
        publisher_pc_ = pub_pc;
        subscriber_pc_ = sub_pc;
        connection_state_ = ConnectionState::Connected;
        reconnect_attempts_ = 0;
        reconnect_active_ = false;
        listeners_snapshot = listeners_;
    }

    conn_res.client->SetEventReady();
    std::cout << "Room::Connect: Connected to room: " << join_resp->room().name() << std::endl;

    for (const auto& listener : listeners_snapshot) {
        listener->OnConnected();
    }

    co_return true;
}

void Room::Disconnect() {
    std::shared_ptr<SignalClient> client_to_close;
    std::shared_ptr<LocalParticipant> local_to_clear;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) return;

        connection_state_ = ConnectionState::Disconnected;
        reconnect_active_ = false;

        client_to_close = std::move(signal_client_);
        local_to_clear = std::move(local_participant_);
        remote_participants_.clear();
        published_track_records_.clear();
        listeners_snapshot = listeners_;

        publisher_pc_ = nullptr;
        subscriber_pc_ = nullptr;
    }

    publisher_observer_.reset();
    subscriber_observer_.reset();
    WebRTCManager::Instance().Deinitialize();

    if (client_to_close) {
        client_to_close->Close();
    }

    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected("user disconnected");
    }
}

void Room::AddListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    if (listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
        listeners_.push_back(listener);
    }
}

void Room::RemoveListener(std::shared_ptr<RoomListener> listener) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    auto it = std::remove(listeners_.begin(), listeners_.end(), listener);
    if (it != listeners_.end()) {
        listeners_.erase(it, listeners_.end());
    }
}

void Room::HandleSignalEvent(const SignalEvent& event) {
    if (event.type == SignalEvent::Close) {
        bool should_reconnect = false;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            std::cout << "Room::HandleSignalEvent: SignalClient closed, reason=" << event.close_reason << std::endl;
            if (connection_state_ == ConnectionState::Connected && !reconnect_active_) {
                connection_state_ = ConnectionState::Reconnecting;
                reconnect_active_ = true;
                should_reconnect = true;
                RecordPublishedTracks();
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
    if (msg->has_update()) {
        UpdateParticipants(msg->update());
    } else if (msg->has_mute()) {
        UpdateTrackMute(msg->mute());
    } else if (msg->has_leave()) {
        std::cout << "Room::HandleSignalMessage: Server requested leave" << std::endl;
        Disconnect();
    } else if (msg->has_reconnect()) {
        std::cout << "Room: Reconnect signal received" << std::endl;
    } else if (msg->has_offer()) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            pc = subscriber_pc_;
        }
        if (pc) HandleOfferSignal(msg->offer());
    } else if (msg->has_answer()) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            pc = publisher_pc_;
        }
        if (pc) HandleAnswerSignal(msg->answer());
    } else if (msg->has_trickle()) {
        HandleTrickleSignal(msg->trickle());
    }
}

void Room::UpdateParticipants(const proto::ParticipantUpdate& update) {
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    struct Notification {
        enum Type { Connected, Disconnected } type;
        std::shared_ptr<RemoteParticipant> participant;
    };
    std::vector<Notification> notifications;

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        listeners_snapshot = listeners_;

        for (int i = 0; i < update.participants_size(); ++i) {
            const auto& pinfo = update.participants(i);
            if (pinfo.state() == proto::ParticipantInfo::DISCONNECTED) {
                auto it = remote_participants_.find(pinfo.sid());
                if (it != remote_participants_.end()) {
                    notifications.push_back({Notification::Disconnected, it->second});
                    remote_participants_.erase(it);
                }
            } else {
                auto it = remote_participants_.find(pinfo.sid());
                if (it == remote_participants_.end()) {
                    auto p = std::make_shared<RemoteParticipant>(pinfo.sid(), pinfo.identity());
                    p->set_metadata(pinfo.metadata());
                    remote_participants_[pinfo.sid()] = p;
                    notifications.push_back({Notification::Connected, p});
                } else {
                    it->second->set_metadata(pinfo.metadata());
                }
            }
        }
    }

    for (const auto& n : notifications) {
        for (const auto& listener : listeners_snapshot) {
            if (n.type == Notification::Connected) {
                listener->OnParticipantConnected(n.participant);
            } else {
                listener->OnParticipantDisconnected(n.participant);
            }
        }
    }
}

void Room::UpdateTrackMute(const proto::MuteTrackRequest& mute) {
    std::shared_ptr<Participant> target_p;
    std::shared_ptr<TrackPublication> target_pub;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    std::string track_sid = mute.sid();
    bool is_muted = mute.muted();

    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        listeners_snapshot = listeners_;

        if (local_participant_) {
            auto pub = local_participant_->get_publication(track_sid);
            if (pub) {
                target_p = local_participant_;
                target_pub = pub;
            }
        }

        if (!target_p) {
            for (const auto& pair : remote_participants_) {
                auto pub = pair.second->get_publication(track_sid);
                if (pub) {
                    target_p = pair.second;
                    target_pub = pub;
                    break;
                }
            }
        }
    }

    if (target_p && target_pub) {
        if (target_pub->track()) {
            target_pub->track()->set_muted(is_muted);
        }
        for (const auto& listener : listeners_snapshot) {
            listener->OnTrackMuted(target_p, target_pub, is_muted);
        }
    }
}

void Room::OnLocalIceCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    if (connection_state() != ConnectionState::Connected) return;
    SendTrickleCandidate(sdp, sdp_mid, sdp_mline_index, pc_type);
}

void Room::SendTrickleCandidate(const std::string& sdp, const std::string& sdp_mid, int sdp_mline_index, int pc_type) {
    nlohmann::json j;
    j["candidate"] = sdp;
    j["sdpMid"] = sdp_mid;
    j["sdpMLineIndex"] = sdp_mline_index;
    std::string cand_json = j.dump();

    proto::SignalRequest req;
    auto* trickle = req.mutable_trickle();
    trickle->set_candidateinit(cand_json);
    trickle->set_target(pc_type == 0 ? proto::SignalTarget::PUBLISHER : proto::SignalTarget::SUBSCRIBER);

    std::shared_ptr<SignalClient> client;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        client = signal_client_;
    }
    if (client) {
        client->Send(req);
    }
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    std::cout << "Room::OnRemoteTrackAdded: Remote track arrived: " << track->id() << ", kind: " << track->kind() << std::endl;
}

void Room::OnRenegotiationNeeded(int pc_type) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (pc_type == 0) {
            pc = publisher_pc_;
        }
    }
    if (pc_type == 0 && pc) { 
        std::cout << "Room::OnRenegotiationNeeded: Publisher PC renegotiation triggered" << std::endl;
        auto self = shared_from_this();
        
        WebRTCManager::Instance().CreateOffer(pc, executor_, [self, pc](const std::string& sdp, const std::string& error) {
            if (!error.empty()) {
                std::cerr << "Room: CreateOffer failed: " << error << std::endl;
                return;
            }
            
            WebRTCManager::Instance().SetLocalDescription(pc, "offer", sdp, self->executor_, [self, sdp](const std::string& set_err) {
                if (!set_err.empty()) {
                    std::cerr << "Room: SetLocalDescription failed: " << set_err << std::endl;
                    return;
                }
                
                proto::SignalRequest req;
                auto* offer_msg = req.mutable_offer();
                offer_msg->set_type("offer");
                offer_msg->set_sdp(sdp);
                
                std::shared_ptr<SignalClient> client;
                {
                    std::lock_guard<std::mutex> lock(self->room_mutex_);
                    client = self->signal_client_;
                }
                if (client) {
                    client->Send(req);
                }
            });
        });
    }
}

void Room::HandleOfferSignal(const proto::SessionDescription& offer) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        pc = subscriber_pc_;
    }
    if (!pc) return;
    std::cout << "Room: Received Offer from server, setting remote description..." << std::endl;
    
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pc, "offer", offer.sdp(), executor_, [self, pc](const std::string& err) {
        if (!err.empty()) {
            std::cerr << "Room: SetRemoteDescription (Offer) failed: " << err << std::endl;
            return;
        }
        
        WebRTCManager::Instance().CreateOffer(pc, self->executor_, [self, pc](const std::string& answer_sdp, const std::string& create_err) {
            if (!create_err.empty()) {
                std::cerr << "Room: CreateAnswer failed: " << create_err << std::endl;
                return;
            }
            
            WebRTCManager::Instance().SetLocalDescription(pc, "answer", answer_sdp, self->executor_, [self, answer_sdp, pc](const std::string& set_err) {
                if (!set_err.empty()) {
                    std::cerr << "Room: SetLocalDescription (Answer) failed: " << set_err << std::endl;
                    return;
                }
                
                proto::SignalRequest req;
                auto* answer_msg = req.mutable_answer();
                answer_msg->set_type("answer");
                answer_msg->set_sdp(answer_sdp);
                
                std::shared_ptr<SignalClient> client;
                {
                    std::lock_guard<std::mutex> lock(self->room_mutex_);
                    client = self->signal_client_;
                }
                if (client) {
                    client->Send(req);
                }
            });
        });
    });
}

void Room::HandleAnswerSignal(const proto::SessionDescription& answer) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        pc = publisher_pc_;
    }
    if (!pc) return;
    std::cout << "Room: Received Answer from server, setting remote description..." << std::endl;
    
    WebRTCManager::Instance().SetRemoteDescription(pc, "answer", answer.sdp(), executor_, [](const std::string& err) {
        if (!err.empty()) {
            std::cerr << "Room: SetRemoteDescription (Answer) failed: " << err << std::endl;
        } else {
            std::cout << "Room: Answer successfully applied on Publisher PC." << std::endl;
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
        auto j = nlohmann::json::parse(trickle.candidateinit());
        std::string candidate_str = j.at("candidate").get<std::string>();
        std::string sdp_mid = j.at("sdpMid").get<std::string>();
        int sdp_mline_index = j.at("sdpMLineIndex").get<int>();

        webrtc::PeerConnectionInterface* pc_raw = pc.get();
        WebRTCManager::Instance().signaling_thread()->PostTask([pc_raw, candidate_str, sdp_mid, sdp_mline_index]() {
            webrtc::SdpParseError err;
            std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
                webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate_str, &err));
            
            if (!ice_candidate) {
                std::cerr << "Room: Failed to parse trickle candidate: " << err.description << std::endl;
                return;
            }
            
            if (!pc_raw->AddIceCandidate(ice_candidate.get())) {
                std::cerr << "Room: Failed to add IceCandidate to PC" << std::endl;
            }
        });
    } catch (const std::exception& e) {
        std::cerr << "Room: Failed to parse candidate JSON: " << e.what() << std::endl;
    }
}

void Room::RecordPublishedTracks() {
    published_track_records_.clear();
    if (local_participant_) {
        for (const auto& [sid, pub] : local_participant_->tracks()) {
            if (pub && pub->track()) {
                published_track_records_.push_back({pub->track(), sid});
            }
        }
    }
}

asio::awaitable<void> Room::AttemptReconnect() {
    auto self = shared_from_this();

    for (int attempt = 0; attempt < kMaxReconnectAttempts; ++attempt) {
        auto delay = std::min(
            kBaseReconnectDelay * (1 << std::min(attempt, 4)),
            kMaxReconnectDelay);
        asio::steady_timer timer(executor_);
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);

        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            if (connection_state_ != ConnectionState::Reconnecting) {
                co_return;
            }
        }

        std::shared_ptr<SignalClient> client;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            client = signal_client_;
        }
        if (!client) co_return;

        auto restart_result = co_await client->Restart();
        if (restart_result.error) {
            std::cout << "Room: Reconnect attempt " << (attempt + 1)
                      << "/" << kMaxReconnectAttempts
                      << " failed: " << restart_result.error.message() << std::endl;
            continue;
        }

        client->SetReconnected();
        client->SetEventReady();

        co_await RestartIceConnections(restart_result.reconnect_response);
        co_await RepublishLocalTracks(restart_result.reconnect_response);

        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            connection_state_ = ConnectionState::Connected;
            reconnect_attempts_ = 0;
            reconnect_active_ = false;
            listeners_snapshot = listeners_;
        }

        for (const auto& listener : listeners_snapshot) {
            listener->OnReconnected();
        }
        co_return;
    }

    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        connection_state_ = ConnectionState::Disconnected;
        reconnect_active_ = false;
        listeners_snapshot = listeners_;
    }
    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected("reconnect failed after "
                                  + std::to_string(kMaxReconnectAttempts) + " attempts");
    }
}

asio::awaitable<void> Room::RepublishLocalTracks(
    std::shared_ptr<proto::ReconnectResponse> reconnect_response) {

    std::vector<PublishedTrackRecord> records;
    std::shared_ptr<LocalParticipant> local;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        records = std::move(published_track_records_);
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
