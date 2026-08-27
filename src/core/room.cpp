#include "room.h"
#include "webrtc_manager.h"
#include "stats_collector.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "rtc_audio_source.h"
#include "rtc_video_source.h"
#include "telemetry.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include "api/jsep.h"
#include "api/video/video_sink_interface.h"

namespace livekit {

namespace {

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
        }
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        std::string sdp_mid = candidate->sdp_mid();
        int sdp_mline_index = candidate->sdp_mline_index();

        if (auto room = room_.lock()) {
            room->Log("SIGNAL", "LOCAL_ICE", "收集到本地 ICE 候选 (" + std::string(pc_type_ == 0 ? "Publisher" : "Subscriber") + "): mid=" + sdp_mid);
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
        h(cat, tag, msg);
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

asio::awaitable<bool> Room::Connect(const std::string& url, const std::string& token, const SignalOptions& opts) {
    {
        std::lock_guard lock(room_mutex_);
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
        std::lock_guard lock(room_mutex_);
        connection_state_ = ConnectionState::Disconnected;
        co_return false;
    }

    signal_client_ = conn_res.client;
    auto join_res = conn_res.join_response;

    {
        std::lock_guard lock(room_mutex_);
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

        // 绑定 LocalParticipant 发布 Native Track Handler
        local_participant_->SetPublishTrackHandler(
            [self](std::shared_ptr<Track> track) {
                self->AddTrackToPublisher(track);
            }
        );

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
                        Log("ERROR", "SUB_PC_FAIL", "Subscriber PeerConnection 创建失败: " + std::string(sub_res.error().message()));
                    }
                }

                // [FIX] 预分配 recvonly 轨道：无论单双 PC，提前为接收方向注册 m=audio 和 m=video，防止初始 SDP 丢失接收能力
                if (subscriber_pc_) {
                    WebRTCManager::Instance().signaling_thread()->BlockingCall([pc = subscriber_pc_]() {
                        webrtc::RtpTransceiverInit init;
                        init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
                        auto err_audio = pc->AddTransceiver(webrtc::MediaType::AUDIO, init);
                        auto err_video = pc->AddTransceiver(webrtc::MediaType::VIDEO, init);
                        if (!err_video.ok()) {
                            std::cerr << "[WEBRTC] Pre-allocate video transceiver failed: " << err_video.error().message() << std::endl;
                        } else {
                            std::cout << "[WEBRTC] Pre-allocated Audio/Video recvonly transceivers successfully.\n";
                        }
                    });
                }
            }
        }

        UpdateParticipants(join_res->other_participants());
        connection_state_ = ConnectionState::Connected;
    }

    if (signal_client_) {
        signal_client_->SetEventReady();
        proto::SignalRequest perm_req;
        auto* perm = perm_req.mutable_subscription_permission();
        perm->set_all_participants(true);
        signal_client_->Send(perm_req);
        Log("SIGNAL", "SUB_PERM", "已向 LiveKit 发送全员订阅权限 SubscriptionPermission (all_participants=true)");

        // [FIX] 单 PC 模式下，入会成功后主动发起初始 Offer 协商，确保 WebRTC 媒体通道与 recvonly 轨道立即激活
        if (signal_client_->is_single_pc_mode_active()) {
            NegotiatePublisher();
        }
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
        std::lock_guard lock(room_mutex_);
        if (connection_state_ == ConnectionState::Disconnected) return;
        connection_state_ = ConnectionState::Disconnected;
        listeners_snapshot = listeners_;
    }

    // 1. 关闭信令连接
    if (signal_client_) {
        signal_client_->Close();
        signal_client_.reset();
    }

    // 2. 显式关闭 PeerConnection 网络传输
    if (publisher_pc_) {
        publisher_pc_->Close();
    }
    if (subscriber_pc_ && subscriber_pc_ != publisher_pc_) {
        subscriber_pc_->Close();
    }

    // 3. 释放 PeerConnection 与 DataChannel
    publisher_pc_ = nullptr;
    subscriber_pc_ = nullptr;
    reliable_dc_ = nullptr;
    lossy_dc_ = nullptr;
    remote_data_channels_.clear();
    data_channel_observers_.clear();
    remote_track_sinks_.clear();

    // 4. 释放 Observer
    publisher_observer_.reset();
    subscriber_observer_.reset();

    {
        std::lock_guard mlock(remote_media_mutex_);
        remote_video_tracks_.clear();
        remote_audio_tracks_.clear();
    }

    for (const auto& listener : listeners_snapshot) {
        listener->OnDisconnected("Client Initiated Disconnect");
    }
}

void Room::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                       const std::vector<std::string>& destination_identities, const std::string& topic) {
    static constexpr size_t kMaxChunkSize = 15000;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    {
        std::lock_guard lock(room_mutex_);
        dc = reliable ? reliable_dc_ : lossy_dc_;
    }

    if (payload.size() > kMaxChunkSize) {
        // === DataStream Chunking 大包切片分发逻辑 ===
        std::string stream_id = "ds_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

        // 1. 发送 Header 帧
        proto::DataPacket header_pkt;
        header_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant_) {
            header_pkt.set_participant_identity(local_participant_->identity());
            header_pkt.set_participant_sid(local_participant_->sid());
        }
        auto* header = header_pkt.mutable_stream_header();
        header->set_stream_id(stream_id);
        header->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        header->set_topic(topic);
        header->set_total_length(payload.size());

        std::vector<uint8_t> header_bytes(header_pkt.ByteSizeLong());
        header_pkt.SerializeToArray(header_bytes.data(), static_cast<int>(header_bytes.size()));

        if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
            webrtc::DataBuffer buf(webrtc::CopyOnWriteBuffer(header_bytes.data(), header_bytes.size()), true);
            dc->Send(buf);
        } else {
            OnIncomingDataPacket(header_bytes, local_participant_ ? local_participant_->identity() : "", topic);
        }

        // 2. 切片发送 Chunks
        size_t total_chunks = (payload.size() + kMaxChunkSize - 1) / kMaxChunkSize;
        for (size_t i = 0; i < total_chunks; ++i) {
            size_t offset = i * kMaxChunkSize;
            size_t chunk_len = std::min(kMaxChunkSize, payload.size() - offset);

            proto::DataPacket chunk_pkt;
            chunk_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
            if (local_participant_) {
                chunk_pkt.set_participant_identity(local_participant_->identity());
                chunk_pkt.set_participant_sid(local_participant_->sid());
            }
            auto* chunk = chunk_pkt.mutable_stream_chunk();
            chunk->set_stream_id(stream_id);
            chunk->set_chunk_index(i);
            chunk->set_content(payload.data() + offset, chunk_len);

            std::vector<uint8_t> chunk_bytes(chunk_pkt.ByteSizeLong());
            chunk_pkt.SerializeToArray(chunk_bytes.data(), static_cast<int>(chunk_bytes.size()));

            if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
                while (dc->buffered_amount() > 256 * 1024) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                webrtc::DataBuffer buf(webrtc::CopyOnWriteBuffer(chunk_bytes.data(), chunk_bytes.size()), true);
                dc->Send(buf);
            } else {
                OnIncomingDataPacket(chunk_bytes, local_participant_ ? local_participant_->identity() : "", topic);
            }
        }

        // 3. 发送 Trailer 结束帧
        proto::DataPacket trailer_pkt;
        trailer_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant_) {
            trailer_pkt.set_participant_identity(local_participant_->identity());
            trailer_pkt.set_participant_sid(local_participant_->sid());
        }
        auto* trailer = trailer_pkt.mutable_stream_trailer();
        trailer->set_stream_id(stream_id);

        std::vector<uint8_t> trailer_bytes(trailer_pkt.ByteSizeLong());
        trailer_pkt.SerializeToArray(trailer_bytes.data(), static_cast<int>(trailer_bytes.size()));

        if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
            webrtc::DataBuffer buf(webrtc::CopyOnWriteBuffer(trailer_bytes.data(), trailer_bytes.size()), true);
            dc->Send(buf);
        } else {
            OnIncomingDataPacket(trailer_bytes, local_participant_ ? local_participant_->identity() : "", topic);
        }
        return;
    }

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

    if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
        std::string payload_str(data.begin(), data.end());
        webrtc::DataBuffer buffer(webrtc::CopyOnWriteBuffer(payload_str.data(), payload_str.size()), /*binary=*/true);
        dc->Send(buffer);
    } else {
        // 若底层 DataChannel 暂未建立物理 Socket，直接进行安全解包分发
        OnIncomingDataPacket(data, local_participant_ ? local_participant_->identity() : "", topic);
    }
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

void Room::CleanupStaleDataStreams() {
    std::lock_guard lock(incoming_streams_mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = incoming_streams_.begin(); it != incoming_streams_.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->start_time).count() > 30) {
            std::cout << "[DATA_STREAM] Cleaned up stale un-assembled stream '" << it->first << "'\n";
            it = incoming_streams_.erase(it);
        } else {
            ++it;
        }
    }
}

void Room::OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic) {
    CleanupStaleDataStreams();

    std::vector<uint8_t> real_payload = payload;
    std::string real_topic = topic;
    std::string real_sender_sid = participant_sid;
    std::string sender_identity;
    bool is_protobuf_packet = false;

    // 尝试反序列化 Protobuf DataPacket
    proto::DataPacket data_pkt;
    if (data_pkt.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        is_protobuf_packet = true;
        if (!data_pkt.participant_identity().empty()) {
            sender_identity = data_pkt.participant_identity();
        }
        if (!data_pkt.participant_sid().empty()) {
            real_sender_sid = data_pkt.participant_sid();
        }

        if (data_pkt.has_stream_header()) {
            const auto& header = data_pkt.stream_header();
            auto tracker = std::make_shared<IncomingDataStreamTracker>();
            tracker->stream_id = header.stream_id();
            tracker->topic = header.topic();
            tracker->total_length = header.total_length();
            tracker->sender_identity = sender_identity;
            tracker->sender_sid = real_sender_sid;
            tracker->start_time = std::chrono::steady_clock::now();

            std::lock_guard lock(incoming_streams_mutex_);
            incoming_streams_[header.stream_id()] = tracker;
            return;
        } else if (data_pkt.has_stream_chunk()) {
            const auto& chunk = data_pkt.stream_chunk();
            std::shared_ptr<IncomingDataStreamTracker> tracker;
            {
                std::lock_guard lock(incoming_streams_mutex_);
                auto it = incoming_streams_.find(chunk.stream_id());
                if (it != incoming_streams_.end()) {
                    tracker = it->second;
                }
            }

            if (tracker) {
                const std::string& content = chunk.content();
                std::vector<uint8_t> chunk_vec(content.begin(), content.end());

                std::lock_guard lock(incoming_streams_mutex_);
                if (tracker->chunks.find(chunk.chunk_index()) == tracker->chunks.end()) {
                    tracker->chunks[chunk.chunk_index()] = chunk_vec;
                    tracker->current_received_bytes += chunk_vec.size();
                }

                bool is_complete = (tracker->total_length > 0 && tracker->current_received_bytes >= tracker->total_length);
                if (is_complete) {
                    std::vector<uint8_t> assembled_payload;
                    assembled_payload.reserve(tracker->total_length);
                    for (const auto& kv : tracker->chunks) {
                        assembled_payload.insert(assembled_payload.end(), kv.second.begin(), kv.second.end());
                    }

                    real_payload = assembled_payload;
                    real_topic = tracker->topic;
                    if (!tracker->sender_identity.empty()) sender_identity = tracker->sender_identity;
                    if (!tracker->sender_sid.empty()) real_sender_sid = tracker->sender_sid;

                    incoming_streams_.erase(chunk.stream_id());
                    // 接收完成，无缝还原 real_payload 并下发到业务层！
                } else {
                    return; // 暂未接收完毕，等待后续分片
                }
            } else {
                return;
            }
        } else if (data_pkt.has_stream_trailer()) {
            const auto& trailer = data_pkt.stream_trailer();
            std::shared_ptr<IncomingDataStreamTracker> tracker;
            {
                std::lock_guard lock(incoming_streams_mutex_);
                auto it = incoming_streams_.find(trailer.stream_id());
                if (it != incoming_streams_.end()) {
                    tracker = it->second;
                    incoming_streams_.erase(it);
                }
            }
            if (tracker && tracker->current_received_bytes > 0) {
                std::vector<uint8_t> assembled_payload;
                for (const auto& kv : tracker->chunks) {
                    assembled_payload.insert(assembled_payload.end(), kv.second.begin(), kv.second.end());
                }
                real_payload = assembled_payload;
                real_topic = tracker->topic;
                if (!tracker->sender_identity.empty()) sender_identity = tracker->sender_identity;
                if (!tracker->sender_sid.empty()) real_sender_sid = tracker->sender_sid;
            } else {
                return;
            }
        } else if (data_pkt.has_user()) {
            const auto& user_pkt = data_pkt.user();
            const std::string& p_bytes = user_pkt.payload();
            real_payload.assign(p_bytes.begin(), p_bytes.end());
            real_topic = user_pkt.topic();
        } else if (data_pkt.has_chat_message()) {
            const auto& pb_chat = data_pkt.chat_message();
            real_payload.assign(pb_chat.message().begin(), pb_chat.message().end());
            real_topic = "lk.chat";
        }
    }

    std::string text_payload(real_payload.begin(), real_payload.end());

    // 1. 优先校验解包 LiveKit RPC 报文 (Topic 为 lk.rpc)
    if (real_topic == "lk.rpc") {
        auto rpc_pkt_opt = RpcPacket::Decode(text_payload);
        if (rpc_pkt_opt.has_value()) {
            OnIncomingRpcPacket(rpc_pkt_opt.value());
            return;
        }
    }

    auto listeners_snapshot = GetListenersSnapshot();
    std::shared_ptr<RemoteParticipant> remote_p;
    {
        std::lock_guard lock(room_mutex_);
        if (!real_sender_sid.empty()) {
            auto it = remote_participants_.find(real_sender_sid);
            if (it != remote_participants_.end()) {
                remote_p = it->second;
            }
        }
        if (!remote_p && !sender_identity.empty()) {
            for (const auto& kv : remote_participants_) {
                if (kv.second && (kv.second->identity() == sender_identity || kv.second->sid() == sender_identity)) {
                    remote_p = kv.second;
                    break;
                }
            }
        }
    }

    // 2. 解析并派发结构化 Chat 消息
    bool chat_dispatched = false;
    if (is_protobuf_packet && data_pkt.has_chat_message()) {
        const auto& pb_chat = data_pkt.chat_message();
        ChatMessage chat;
        chat.id = pb_chat.id();
        chat.timestamp = pb_chat.timestamp();
        chat.edit_timestamp = pb_chat.edit_timestamp();
        chat.message = pb_chat.message();
        chat.sender_identity = !sender_identity.empty() ? sender_identity : (remote_p ? remote_p->identity() : real_sender_sid);
        std::shared_ptr<Participant> p = remote_p ? remote_p : (local_participant_ && (local_participant_->sid() == real_sender_sid || local_participant_->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_participant_) : nullptr);
        for (const auto& listener : listeners_snapshot) {
            listener->OnChatMessage(chat, p);
        }
        chat_dispatched = true;
    } else if (real_topic == "lk.chat" || real_topic == "lk-chat-topic" || real_topic.empty()) {
        auto chat_opt = ChatMessage::Decode(text_payload, !sender_identity.empty() ? sender_identity : real_sender_sid);
        if (chat_opt.has_value()) {
            std::shared_ptr<Participant> p = remote_p ? remote_p : (local_participant_ && (local_participant_->sid() == real_sender_sid || local_participant_->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_participant_) : nullptr);
            for (const auto& listener : listeners_snapshot) {
                listener->OnChatMessage(chat_opt.value(), p);
            }
            chat_dispatched = true;
        }
    }

    // 3. 派发原始 OnDataReceived 事件给 RoomListener
    // 如果是 LiveKit 原生 Protobuf 包装包（如 DataPacket / ChatMessage / RPC），已在前面派发给对应结构化回调，避免在基础通道中重复输出乱码
    if (!is_protobuf_packet && !chat_dispatched && real_topic != "lk.chat" && real_topic != "lk-chat-topic" && real_topic != "lk.rpc") {
        for (const auto& listener : listeners_snapshot) {
            listener->OnDataReceived(real_payload, remote_p, real_topic);
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

        callback_(frame);
    }

private:
    std::function<void(const AudioFrame&)> callback_;
};

class NativeVideoTrackSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit NativeVideoTrackSink(std::function<void(const VideoFrame&, const VideoCaptureOptions&)> callback)
        : callback_(std::move(callback)) {}

    void OnFrame(const webrtc::VideoFrame& rtc_frame) override {
        if (!callback_) return;

        int width = rtc_frame.width();
        int height = rtc_frame.height();
        auto buffer = rtc_frame.video_frame_buffer();
        if (!buffer || width <= 0 || height <= 0) return;

        auto i420_buffer = buffer->ToI420();
        if (!i420_buffer) return;

        VideoFrame frame = VideoFrame::create(width, height, VideoBufferType::RGBA);
        uint8_t* dst_rgba = frame.data();
        if (!dst_rgba) return;

        const uint8_t* y_src = i420_buffer->DataY();
        const uint8_t* u_src = i420_buffer->DataU();
        const uint8_t* v_src = i420_buffer->DataV();
        const int y_stride = i420_buffer->StrideY();
        const int u_stride = i420_buffer->StrideU();
        const int v_stride = i420_buffer->StrideV();

        auto clamp8 = [](int val) -> uint8_t {
            return static_cast<uint8_t>(val < 0 ? 0 : (val > 255 ? 255 : val));
        };

        for (int y = 0; y < height; ++y) {
            const uint8_t* py = y_src + y * y_stride;
            const uint8_t* pu = u_src + (y / 2) * u_stride;
            const uint8_t* pv = v_src + (y / 2) * v_stride;
            uint8_t* pdst = dst_rgba + y * width * 4;

            for (int x = 0; x < width; ++x) {
                int y_val = py[x] - 16;
                int u_val = pu[x / 2] - 128;
                int v_val = pv[x / 2] - 128;

                int r = clamp8((298 * y_val + 409 * v_val + 128) >> 8);
                int g = clamp8((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
                int b = clamp8((298 * y_val + 516 * u_val + 128) >> 8);

                pdst[x * 4 + 0] = static_cast<uint8_t>(r);
                pdst[x * 4 + 1] = static_cast<uint8_t>(g);
                pdst[x * 4 + 2] = static_cast<uint8_t>(b);
                pdst[x * 4 + 3] = 255;
            }
        }

        VideoCaptureOptions options;
        options.timestamp_us = rtc_frame.timestamp_us();
        if (rtc_frame.rotation() == webrtc::kVideoRotation_90) options.rotation = VideoRotation::VIDEO_ROTATION_90;
        else if (rtc_frame.rotation() == webrtc::kVideoRotation_180) options.rotation = VideoRotation::VIDEO_ROTATION_180;
        else if (rtc_frame.rotation() == webrtc::kVideoRotation_270) options.rotation = VideoRotation::VIDEO_ROTATION_270;

        callback_(frame, options);
    }

private:
    std::function<void(const VideoFrame&, const VideoCaptureOptions&)> callback_;
};

} // namespace

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
                        std::cerr << "[SIMULCAST TRANSCEIVER] AddTransceiver failed: " << transceiver_res.error().message() << "\n";
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
            std::cerr << "[SIMULCAST SET_PARAMS] SetParameters failed: " << status.message() << "\n";
        }
    }
}

void Room::NegotiatePublisher() {
    bool should_start = false;
    {
        std::lock_guard lock(room_mutex_);
        if (negotiation_state_ == NegotiationState::Idle) {
            negotiation_state_ = NegotiationState::InProgress;
            should_start = true;
        } else if (negotiation_state_ == NegotiationState::InProgress) {
            negotiation_state_ = NegotiationState::PendingRetry;
        }
    }
    
    if (should_start) {
        ExecuteNegotiatePublisher();
    }
}

void Room::OnNegotiationFailed() {
    bool need_retry = false;
    {
        std::lock_guard lock(room_mutex_);
        if (negotiation_state_ == NegotiationState::PendingRetry) {
            negotiation_state_ = NegotiationState::InProgress;
            need_retry = true;
        } else {
            negotiation_state_ = NegotiationState::Idle;
            subscriber_negotiating_ = false;
        }
    }
    if (need_retry) {
        ExecuteNegotiatePublisher();
    }
}

void Room::ExecuteNegotiatePublisher() {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::shared_ptr<SignalClient> client;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        client = signal_client_;
        if (!pub_pc || !client) {
            negotiation_state_ = NegotiationState::Idle;
            return;
        }
        
        if (pub_pc->signaling_state() != webrtc::PeerConnectionInterface::SignalingState::kStable) {
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

    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(pub_pc, executor_,
        [self, client, pub_pc](const std::string& sdp, const std::string& err) {
            if (!err.empty()) {
                std::cerr << "[WebRTC] CreateOffer error: " << err << std::endl;
                self->Log("ERROR", "OFFER_FAIL", "CreateOffer error: " + err);
                self->OnNegotiationFailed();
                return;
            }

            WebRTCManager::Instance().SetLocalDescription(pub_pc, "offer", sdp, self->executor_,
                [self, client, pub_pc, sdp](const std::string& set_local_err) {
                    if (!set_local_err.empty()) {
                        std::cerr << "[WebRTC] SetLocalDescription offer error: " << set_local_err << std::endl;
                        self->Log("ERROR", "LOCAL_DESC_FAIL", "SetLocalDescription offer error: " + set_local_err);
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

                    client->Send(req);
                    std::cout << "[WebRTC] -> Sent publisher SDP Offer to LiveKit server with mid_to_track_id mapping!" << std::endl;
                    self->Log("SIGNAL", "SDP_OFFER_SENT", "已向 LiveKit 服务端发送 Publisher SDP Offer (" + std::to_string(sdp.length()) + " 字节):\n" + sdp);
                });
        });
}

void Room::SendPublishOffer() {
    NegotiatePublisher();
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    if (!track) return;
    
    std::string track_id = track->id();
    {
        std::lock_guard lock(room_mutex_);
        if (processed_remote_track_ids_.find(track_id) != processed_remote_track_ids_.end()) {
            return; // 已经处理过这个 track
        }
        processed_remote_track_ids_.insert(track_id);
    }

    std::cout << "[TRACK ATTACHED] Attached sink to remote WebRTC " << track->kind() << " track (id: " << track_id << ")" << std::endl;
    Log("WEBRTC", "TRACK_ATTACHED", "WebRTC 下行 Track 挂载: Kind=" + std::string(track->kind()) + ", ID=" + track_id);

    if (track->kind() == "audio") {
        auto audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto sink = std::make_shared<NativeAudioTrackSink>([this, track_id, has_logged](const AudioFrame& frame) {
            std::vector<std::shared_ptr<Track>> targets;
            {
                std::lock_guard lock(remote_media_mutex_);
                for (auto it = remote_audio_tracks_.begin(); it != remote_audio_tracks_.end();) {
                    if (auto t = it->lock()) {
                        targets.push_back(t);
                        ++it;
                    } else {
                        it = remote_audio_tracks_.erase(it);
                    }
                }
            }

            if (targets.empty()) {
                std::lock_guard lock(room_mutex_);
                for (const auto& kv : remote_participants_) {
                    if (kv.second) {
                        for (const auto& pub_kv : kv.second->tracks()) {
                            if (pub_kv.second && pub_kv.second->track() && pub_kv.second->track()->kind() == TrackKind::Audio) {
                                targets.push_back(pub_kv.second->track());
                            }
                        }
                    }
                }
                if (!targets.empty()) {
                    std::lock_guard mlock(remote_media_mutex_);
                    for (const auto& t : targets) {
                        remote_audio_tracks_.push_back(t);
                    }
                }
            }

            for (const auto& t : targets) {
                t->notifyAudioFrame(frame);
            }

            if (!has_logged->exchange(true)) {
                std::cout << "[RECV AUDIO] Started receiving audio PCM stream, sample_rate=" << frame.sampleRate() << "Hz, channels=" << frame.numChannels() << std::endl;
                Log("WEBRTC", "AUDIO_STREAM", "开始接收远端音频流");
            }
        });
        audio_track->AddSink(sink.get());
        std::lock_guard lock(room_mutex_);
        remote_track_sinks_.push_back(sink);
    } else if (track->kind() == "video") {
        auto video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto frame_counter = std::make_shared<std::atomic<uint64_t>>(0);
        auto sink = std::make_shared<NativeVideoTrackSink>([this, track_id, has_logged, frame_counter](const VideoFrame& frame, const VideoCaptureOptions& options) {
            std::vector<std::shared_ptr<Track>> targets;
            {
                std::lock_guard lock(remote_media_mutex_);
                for (auto it = remote_video_tracks_.begin(); it != remote_video_tracks_.end();) {
                    if (auto t = it->lock()) {
                        targets.push_back(t);
                        ++it;
                    } else {
                        it = remote_video_tracks_.erase(it);
                    }
                }
            }

            if (targets.empty()) {
                std::lock_guard lock(room_mutex_);
                for (const auto& kv : remote_participants_) {
                    if (kv.second) {
                        for (const auto& pub_kv : kv.second->tracks()) {
                            if (pub_kv.second && pub_kv.second->track() && pub_kv.second->track()->kind() == TrackKind::Video) {
                                targets.push_back(pub_kv.second->track());
                            }
                        }
                        if (targets.empty()) {
                            auto r_track = std::make_shared<Track>(track_id, "camera_video", TrackKind::Video);
                            auto pub = std::make_shared<TrackPublication>(r_track, track_id, "camera_video");
                            kv.second->add_publication(pub);
                            targets.push_back(r_track);
                        }
                    }
                }
                if (!targets.empty()) {
                    std::lock_guard mlock(remote_media_mutex_);
                    for (const auto& t : targets) {
                        remote_video_tracks_.push_back(t);
                    }
                }
            }

            for (const auto& t : targets) {
                t->notifyVideoFrame(frame, options);
            }

            uint64_t cnt = frame_counter->fetch_add(1);
            if (!has_logged->exchange(true) || cnt % 120 == 0) {
                std::cout << "[RECV VIDEO] Receiving video stream, frame #" << cnt << ", resolution=" << frame.width() << "x" << frame.height() << std::endl;
                Log("WEBRTC", "VIDEO_FRAME", "WebRTC 解码器输出第 " + std::to_string(cnt) + " 帧 (" + std::to_string(frame.width()) + "x" + std::to_string(frame.height()) + ")");
                Telemetry::Instance().OnFirstRemoteFrameReceived("video");
            }
        });
        video_track->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
        std::lock_guard lock(room_mutex_);
        remote_track_sinks_.push_back(sink);

        // 主动通知 SFU 激活所有远端视频 Track 质量
        if (signal_client_) {
            for (const auto& kv : remote_participants_) {
                if (kv.second) {
                    for (const auto& pub_kv : kv.second->tracks()) {
                        if (pub_kv.second && pub_kv.second->track() && pub_kv.second->track()->kind() == TrackKind::Video) {
                            signal_client_->SendUpdateTrackSettings(pub_kv.first, false, proto::VideoQuality::HIGH, 1280, 720, 30, 0);
                            signal_client_->SendUpdateSubscription({pub_kv.first}, true, kv.first);
                            Log("SIGNAL", "TRACK_ACTIVE", "已向 SFU 激活下行视频流: Track SID=" + pub_kv.first + " (Participant: " + kv.second->identity() + ")");
                        }
                    }
                }
            }
        }
    }
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

void Room::HandleSignalEvent(const SignalEvent& event) {
    if (event.type == SignalEvent::Close) {
        bool should_reconnect = false;
        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard lock(room_mutex_);
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
                std::lock_guard lock(room_mutex_);
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
            Log("SIGNAL", "RAW_MSG", "[Signal] 收到服务端未识别消息 (Case=" + std::to_string(msg->message_case()) + "): " + msg->ShortDebugString());
        } else if (type_tag != "PONG") {
            Log("SIGNAL", "RAW_MSG", "[Signal] 收到服务端消息: " + type_tag);
        }
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
        std::lock_guard lock(room_mutex_);
        if (local_participant_) {
            auto pub = local_participant_->get_publication(tp.cid());
            if (pub) {
                if (pub->track()) {
                    pub->track()->set_sid(tp.track().sid());
                }
                local_participant_->add_publication(std::make_shared<TrackPublication>(pub->track(), tp.track().sid(), pub->name()));
            }
        }
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
    } else if (msg->has_stream_state_update()) {
        Log("SIGNAL", "STREAM_STATE", "收到服务端 StreamStateUpdate 状态更新");
    } else if (msg->has_room_update()) {
        Log("SIGNAL", "ROOM_UPDATE", "收到服务端 RoomUpdate 房间信息变更");
    } else if (msg->has_subscribed_quality_update()) {
        Log("SIGNAL", "QUALITY_UPDATE", "收到 SFU Dynacast 质量调控需求: Track SID=" + msg->subscribed_quality_update().track_sid());
        const auto& squ = msg->subscribed_quality_update();
        std::map<livekit::proto::VideoQuality, bool> quality_states;

        for (const auto& sc : squ.subscribed_codecs()) {
            for (const auto& q : sc.qualities()) {
                quality_states[q.quality()] = q.enabled();
            }
        }
        for (const auto& q : squ.subscribed_qualities()) {
            quality_states[q.quality()] = q.enabled();
        }

        // Dynacast 闭环
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

                bool params_changed = false;
                for (const auto& [quality, enabled] : quality_states) {
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
                    sender->SetParameters(parameters);
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

    struct MuteChange {
        std::shared_ptr<Participant> participant;
        std::shared_ptr<TrackPublication> publication;
        bool muted;
    };
    std::vector<MuteChange> changed_mute_events;

    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard lock(room_mutex_);
        listeners_snapshot = listeners_;

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
                local_participant_->set_metadata(p_info.metadata());

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
                continue;
            }

            auto it = remote_participants_.find(p_info.sid());
            if (p_info.state() == proto::ParticipantInfo::DISCONNECTED) {
                if (it != remote_participants_.end()) {
                    disconnected.push_back(it->second);
                    remote_participants_.erase(it);
                }
            } else {
                std::shared_ptr<RemoteParticipant> remote;
                if (it == remote_participants_.end()) {
                    remote = std::make_shared<RemoteParticipant>(p_info.sid(), p_info.identity());
                    remote->set_metadata(p_info.metadata());
                    remote->set_attributes(new_attrs);
                    remote->set_permission(new_perm);
                    remote_participants_[p_info.sid()] = remote;
                    newly_connected.push_back(remote);
                } else {
                    remote = it->second;
                    remote->set_metadata(p_info.metadata());

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
                        pub = std::make_shared<TrackPublication>(remote_track, t_info.sid(), t_info.name());
                        remote->add_publication(pub);
                        {
                            std::lock_guard mlock(remote_media_mutex_);
                            if (kind == TrackKind::Video) {
                                remote_video_tracks_.push_back(remote_track);
                            } else {
                                remote_audio_tracks_.push_back(remote_track);
                            }
                        }
                        newly_published_tracks.push_back({remote, pub});
                        Log("TRACK", "NEW_TRACK", "参会人 [" + remote->identity() + "] 发布新 Track: " + t_info.name() + " (" + (kind == TrackKind::Video ? "VIDEO" : "AUDIO") + ", SID: " + t_info.sid() + ", Muted: " + (t_info.muted() ? "true" : "false") + ")");
                    } else {
                        // 检测静音/画面开关状态变化
                        if (pub->track() && pub->track()->muted() != t_info.muted()) {
                            pub->track()->set_muted(t_info.muted());
                            changed_mute_events.push_back({remote, pub, t_info.muted()});
                            Log("TRACK", "MUTE_CHANGED", "参会人 [" + remote->identity() + "] Track [" + t_info.sid() + "] 状态变更为: " + (t_info.muted() ? "静音/关闭" : "开启"));
                        }
                    }
                }

                // 检测被远端取消发布的 Track (Unpublished)
                auto existing_tracks = remote->tracks();
                for (const auto& [sid, pub] : existing_tracks) {
                    if (current_track_sids.find(sid) == current_track_sids.end()) {
                        remote->remove_publication(sid);
                        unpublished_tracks.push_back({remote, pub});
                        Log("TRACK", "UNPUBLISHED", "参会人 [" + remote->identity() + "] 取消发布 Track: " + sid);
                    }
                }
            }
        }
    }

    for (const auto& p : newly_connected) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnParticipantConnected(p);
        }
    }

    bool has_new_video_pub = false;
    for (const auto& [p, pub] : newly_published_tracks) {
        if (signal_client_) {
            if (pub->track() && pub->track()->kind() == TrackKind::Video) {
                signal_client_->SendUpdateTrackSettings(pub->sid(), false, proto::VideoQuality::HIGH, 1280, 720, 30, 0);
                has_new_video_pub = true;
            }
        }
        for (const auto& listener : listeners_snapshot) {
            listener->OnTrackPublished(p, pub);
            if (pub && pub->track()) {
                listener->OnTrackSubscribed(pub->track(), pub, p);
            }
        }
    }

    // 会议中新增远端视频轨时，确保 Single PC 下扫描并重新激活 Transceiver Receiver
    if (has_new_video_pub && signal_client_ && signal_client_->is_single_pc_mode_active()) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
        {
            std::lock_guard lock(room_mutex_);
            pub_pc = publisher_pc_;
        }
        if (pub_pc) {
            auto transceivers = pub_pc->GetTransceivers();
            for (const auto& t : transceivers) {
                if (t->receiver() && t->receiver()->track()) {
                    std::weak_ptr<Room> weak_this = shared_from_this();
                    asio::post(executor_, [weak_this, receiver = t->receiver(), track = t->receiver()->track()]() {
                        if (auto room = weak_this.lock()) {
                            room->OnRemoteTrackAdded(receiver, track);
                        }
                    });
                }
            }
        }
    }

    for (const auto& evt : changed_mute_events) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnTrackMuted(evt.participant, evt.publication, evt.muted);
        }
    }

    for (const auto& [p, pub] : unpublished_tracks) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnTrackUnpublished(p, pub);
            if (pub && pub->track()) {
                listener->OnTrackUnsubscribed(pub->track(), pub, p);
            }
        }
    }

    for (const auto& evt : changed_attributes_events) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnParticipantAttributesChanged(evt.attrs, evt.participant);
        }
    }

    for (const auto& evt : changed_permissions_events) {
        for (const auto& listener : listeners_snapshot) {
            listener->OnParticipantPermissionsChanged(evt.old_perm, evt.new_perm, evt.participant);
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

void Room::UpdateParticipantsForTesting(const proto::ParticipantUpdate& update) {
    UpdateParticipants(update.participants());
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
                }
            }
        }
    }

    std::sort(active_speakers.begin(), active_speakers.end(), [](const std::shared_ptr<Participant>& a, const std::shared_ptr<Participant>& b) {
        return a->audio_level() > b->audio_level();
    });

    for (const auto& listener : listeners_snapshot) {
        listener->OnActiveSpeakersChanged(active_speakers);
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
    Log("SIGNAL", "OFFER_CALLED", "HandleOfferSignal 被调用! type=" + offer.type() + ", SDP size=" + std::to_string(offer.sdp().size()) + " bytes");
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
    Log("SIGNAL", "SDP_OFFER_RECV", "收到服务端下发的 SDP Offer (" + std::to_string(offer.sdp().length()) + " 字节):\n" + offer.sdp());
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pc, offer.type(), offer.sdp(), executor_,
        [self, client, pc](const std::string& set_remote_err) {
            if (!set_remote_err.empty()) {
                std::cerr << "Room: SetRemoteDescription offer error: " << set_remote_err << std::endl;
                self->Log("ERROR", "SET_REMOTE_ERR", "Subscriber SetRemoteDescription 失败: " + set_remote_err);
                return;
            }

            std::cout << "[WebRTC] SetRemoteDescription offer succeeded. Generating SDP Answer..." << std::endl;
            self->Log("SIGNAL", "OFFER_APPLIED", "服务端 Offer 设置成功，正在生成 SDP Answer...");
            WebRTCManager::Instance().CreateAnswer(pc, self->executor_,
                [self, client, pc](const std::string& sdp, const std::string& create_ans_err) {
                    if (!create_ans_err.empty()) {
                        std::cerr << "Room: CreateAnswer error: " << create_ans_err << std::endl;
                        self->Log("ERROR", "CREATE_ANS_ERR", "Subscriber CreateAnswer 失败: " + create_ans_err);
                        return;
                    }

                    std::cout << "[WebRTC] CreateAnswer succeeded. Setting LocalDescription..." << std::endl;
                    WebRTCManager::Instance().SetLocalDescription(pc, "answer", sdp, self->executor_,
                        [self, client, sdp](const std::string& set_local_err) {
                            if (!set_local_err.empty()) {
                                std::cerr << "Room: SetLocalDescription answer error: " << set_local_err << std::endl;
                                self->Log("ERROR", "SET_LOCAL_ANS_ERR", "Subscriber SetLocalDescription answer 失败: " + set_local_err);
                                return;
                            }

                            proto::SignalRequest req;
                            auto* answer_msg = req.mutable_answer();
                            answer_msg->set_type("answer");
                            answer_msg->set_sdp(sdp);
                            client->Send(req);
                            std::cout << "[WebRTC] -> Successfully created and sent SDP Answer back to LiveKit Server!" << std::endl;
                            self->Log("SIGNAL", "SDP_ANSWER_SENT", "已生成并向 LiveKit 服务端发送 Subscriber SDP Answer (" + std::to_string(sdp.length()) + " 字节):\n" + sdp);
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
    Log("SIGNAL", "SDP_ANSWER_RECV", "收到服务端下发的 SDP Answer (" + std::to_string(answer.sdp().length()) + " 字节):\n" + answer.sdp());
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
        WebRTCManager::Instance().SetRemoteDescription(pub_pc, "answer", answer.sdp(), executor_,
            [this, pub_pc](const std::string& err) {
                if (!err.empty()) {
                    Log("ERROR", "PUB_REMOTE_ERR", "Publisher SetRemoteDescription (Answer) 失败: " + err);
                    OnNegotiationFailed();
                } else {
                    Log("SIGNAL", "PUB_STABLE", "Publisher PC 协商完成 (Answer 应用成功)");
                    std::vector<PendingIceCandidate> pending_cands;
                    bool need_retry = false;
                    {
                        std::lock_guard lock(room_mutex_);
                        subscriber_negotiating_ = false;
                        if (negotiation_state_ == NegotiationState::PendingRetry) {
                            negotiation_state_ = NegotiationState::InProgress;
                            need_retry = true;
                        } else {
                            negotiation_state_ = NegotiationState::Idle;
                        }
                        if (reconnect_active_) reconnect_active_ = false;
                        pending_cands = std::move(pending_pub_ice_candidates_);
                    }
                    if (need_retry) {
                        Log("SIGNAL", "NEG_RETRY", "检测到挂起的协商请求，开始新一轮重试");
                        ExecuteNegotiatePublisher();
                    }
                    
                    // 单 PC 模式下，本地主动发起的 recvonly transceiver 不会触发 OnTrack，需手动提取
                    auto transceivers = pub_pc->GetTransceivers();
                    Log("SIGNAL", "TRANS_SCAN", "Single PC Answer 协商成功，扫描 " + std::to_string(transceivers.size()) + " 个 transceivers");
                    for (const auto& t : transceivers) {
                        if (t && (t->direction() == webrtc::RtpTransceiverDirection::kRecvOnly ||
                            (t->current_direction().has_value() && *t->current_direction() == webrtc::RtpTransceiverDirection::kRecvOnly))) {
                            if (t->receiver() && t->receiver()->track()) {
                                std::weak_ptr<Room> weak_this = shared_from_this();
                                asio::post(executor_, [weak_this, receiver = t->receiver(), track = t->receiver()->track()]() {
                                    if (auto room = weak_this.lock()) {
                                        room->OnRemoteTrackAdded(receiver, track);
                                    }
                                });
                            }
                        }
                    }

                    if (!pending_cands.empty()) {
                        Log("SIGNAL", "ICE_FLUSH", "开始重放暂存的 " + std::to_string(pending_cands.size()) + " 个 Publisher 早期候选...");
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
                            Log("SIGNAL", "ICE_PUB_REPLAY", "重放早期候选 mid=" + res.first + ", 结果=" + (res.second ? "成功" : "失败"));
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

    std::string ans_m_summary = "";
    for (const auto& m : answer_mlines) ans_m_summary += m + " ";
    std::string pub_m_summary = "";
    for (const auto& m : pub_mlines) pub_m_summary += m + " ";
    std::string sub_m_summary = "";
    for (const auto& m : sub_mlines) sub_m_summary += m + " ";

    Log("SIGNAL", "SDP_ANS_ROUTING", "Answer 诊断: Ans=[" + ans_m_summary + "], Pub=[" + pub_m_summary + "], Sub=[" + sub_m_summary + "], PubNeg=" + std::to_string(pub_negotiating) + ", SubNeg=" + std::to_string(sub_negotiating));

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
                    self->Log("ERROR", "SUB_ANS_ERR", "Subscriber SetRemoteDescription Answer 失败: " + err);
                    auto ans_m = ExtractSdpMLines(answer_sdp);
                    std::string m_str = "";
                    for (const auto& m : ans_m) m_str += m + " ";
                    self->Log("ERROR", "SUB_ANS_LINES", "失败 Answer 的 m-lines: [" + m_str + "]");
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
                            self->Log("SIGNAL", "ICE_SUB_REPLAY", "重放早期候选 mid=" + res.first + ", 结果=" + (res.second ? "成功" : "失败"));
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
                std::cerr << "Room: SetRemoteDescription answer error: " << err << std::endl;
                self->Log("ERROR", "ANS_ERR", "Publisher SetRemoteDescription 失败: " + err);
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

        Log("SIGNAL", "TRICKLE_RECV", "收到服务端 ICE 候选: target=" + std::string(trickle.target() == proto::SignalTarget::PUBLISHER ? "PUBLISHER" : "SUBSCRIBER") + ", mid=" + sdp_mid);

        auto self = shared_from_this();
        WebRTCManager::Instance().signaling_thread()->BlockingCall([self, pub_pc, sub_pc, trickle_target = trickle.target(), sdp_mid, sdp_mline_index, sdp]() {
            webrtc::SdpParseError err;
            std::unique_ptr<webrtc::IceCandidateInterface> cand(webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &err));
            if (!cand) {
                self->Log("ERROR", "ICE_PARSE_FAIL", "ICE 候选解析失败: " + err.description);
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
                        self->Log("SIGNAL", "ICE_PUB_QUEUE", "Publisher PC 暂未就绪，已暂存早期 ICE 候选: mid=" + sdp_mid);
                    } else {
                        self->Log("SIGNAL", "ICE_PUB_ADD", "向 Publisher PC 添加 ICE 候选: mid=" + sdp_mid + ", 结果=成功 (Single PC)");
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
                        self->Log("SIGNAL", "ICE_SUB_QUEUE", "Subscriber PC 暂未就绪，已暂存早期 ICE 候选: mid=" + sdp_mid);
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD", "向 Subscriber PC 添加 ICE 候选: mid=" + sdp_mid + ", 结果=成功");
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
                        self->Log("SIGNAL", "ICE_SUB_QUEUE", "Subscriber PC 暂未就绪，已暂存早期 ICE 候选: mid=" + sdp_mid);
                    } else {
                        self->Log("SIGNAL", "ICE_SUB_ADD", "向 Subscriber PC 尝试添加 ICE 候选: mid=" + sdp_mid + ", 结果=成功");
                    }
                }
            }
        });
    } catch (...) {
        std::cerr << "Room: Failed to parse trickle candidate JSON" << std::endl;
    }
}

// ──────────────────────────────────────────────────────────────────────
//  Subscriber PC 客户端发起协商（处理 MediaSectionsRequirement）
// ──────────────────────────────────────────────────────────────────────
void Room::HandleMediaSectionsRequirement(const proto::MediaSectionsRequirement& req) {
    uint32_t num_audios = req.num_audios();
    uint32_t num_videos = req.num_videos();

    bool should_negotiate = false;
    {
        std::lock_guard lock(room_mutex_);

        // 关键修复 1：如果需要的音视频 section 数量与当前已协商/就绪的数量完全相同，直接跳过，绝不重复协商！
        if (current_sub_audios_ >= num_audios && current_sub_videos_ >= num_videos) {
            Log("SIGNAL", "MEDIA_SEC_SKIP", "下行 MediaSections 已满足要求 ("
                + std::to_string(num_audios) + "A, " + std::to_string(num_videos) + "V)，跳过重复协商");
            return;
        }
        if (subscriber_negotiating_) {
            Log("SIGNAL", "MEDIA_SEC_SKIP", "Subscriber 协商已在进行中，跳过");
            return;
        }
        subscriber_negotiating_ = true;
        current_sub_audios_ = num_audios;
        current_sub_videos_ = num_videos;
        should_negotiate = true;
    }
    if (should_negotiate) {
        NegotiateSubscriber(num_audios, num_videos);
    }
}

void Room::NegotiateSubscriber(uint32_t num_audios, uint32_t num_videos) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sub_pc;
    std::shared_ptr<SignalClient> client;
    {
        std::lock_guard lock(room_mutex_);
        sub_pc = subscriber_pc_;
        client = signal_client_;
        subscriber_negotiating_ = true;
    }

    if (!sub_pc || !client) {
        Log("ERROR", "SUB_NEG_ERR", "NegotiateSubscriber: subscriber_pc_ 或 signal_client_ 为空");
        std::lock_guard lock(room_mutex_);
        subscriber_negotiating_ = false;
        return;
    }

    uint32_t target_audios = std::max(num_audios, 1u);
    uint32_t target_videos = std::max(num_videos, 1u);

    Log("SIGNAL", "SUB_NEG_START", "开始 Subscriber PC 协商: 需要 " + std::to_string(target_audios) + " 音频 + " + std::to_string(target_videos) + " 视频 recvonly transceiver");

    // 1. 同步在 signaling thread 上添加 recvonly transceiver (先 audio 后 video)
    WebRTCManager::Instance().signaling_thread()->BlockingCall([sub_pc, target_audios, target_videos]() {
        // 统计已有的 audio/video transceiver 数量（避免重复添加）
        auto existing_transceivers = sub_pc->GetTransceivers();
        uint32_t existing_audios = 0, existing_videos = 0;
        for (const auto& t : existing_transceivers) {
            if (t->media_type() == webrtc::MediaType::AUDIO) existing_audios++;
            else if (t->media_type() == webrtc::MediaType::VIDEO) existing_videos++;
        }

        // 补充不足的 audio transceiver
        for (uint32_t i = existing_audios; i < target_audios; ++i) {
            webrtc::RtpTransceiverInit init;
            init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
            sub_pc->AddTransceiver(webrtc::MediaType::AUDIO, init);
        }

        // 补充不足的 video transceiver
        for (uint32_t i = existing_videos; i < target_videos; ++i) {
            webrtc::RtpTransceiverInit init;
            init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
            sub_pc->AddTransceiver(webrtc::MediaType::VIDEO, init);
        }
    });

    Log("SIGNAL", "SUB_TRANS_ADDED", "已就绪 " + std::to_string(target_videos) + " video + " + std::to_string(target_audios) + " audio recvonly transceiver，准备 CreateOffer...");

    // 2. 创建 Offer (单 PC 模式下，直接调用 NegotiatePublisher)
    if (client->is_single_pc_mode_active()) {
        NegotiatePublisher();
        return;
    }

    // 3. (双 PC 模式下，保留原有的 Subscriber Offer 发送逻辑)
    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(sub_pc, executor_,
        [self, sub_pc, client](const std::string& sdp, const std::string& err) {
            if (!err.empty()) {
                self->Log("ERROR", "SUB_OFFER_ERR", "Subscriber CreateOffer 失败: " + err);
                std::lock_guard lock(self->room_mutex_);
                self->subscriber_negotiating_ = false;
                return;
            }

            self->Log("SIGNAL", "SUB_OFFER_CREATED", "Subscriber SDP Offer 已创建 (" + std::to_string(sdp.size()) + " 字节), 设置 LocalDescription...");

            // 设置 Local Description
            WebRTCManager::Instance().SetLocalDescription(sub_pc, "offer", sdp, self->executor_,
                [self, sub_pc, client, sdp](const std::string& set_err) {
                    if (!set_err.empty()) {
                        self->Log("ERROR", "SUB_LOCAL_ERR", "Subscriber SetLocalDescription 失败: " + set_err);
                        std::lock_guard lock(self->room_mutex_);
                        self->subscriber_negotiating_ = false;
                        return;
                    }

                    // 发送 Subscriber Offer 到服务端（使用 SignalRequest.offer 字段）
                    proto::SignalRequest req;
                    auto* offer_msg = req.mutable_offer();
                    offer_msg->set_type("offer");
                    offer_msg->set_sdp(sdp);
                    client->Send(req);
                    self->Log("SIGNAL", "SUB_OFFER_SENT", "已向 SFU 发送 Subscriber SDP Offer (" + std::to_string(sdp.size()) + " 字节)");
                });
        });
}

asio::awaitable<void> Room::AttemptReconnect() {
    {
        std::lock_guard lock(room_mutex_);
        reconnect_active_ = true;
    }

    RecordPublishedTracks();

    int attempts = 0;
    bool reconnected = false;
    while (attempts < kMaxReconnectAttempts) {
        attempts++;
        auto delay = kBaseReconnectDelay * (1 << (attempts - 1));
        if (delay > kMaxReconnectDelay) delay = kMaxReconnectDelay;

        asio::steady_timer timer(executor_, delay);
        std::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (!signal_client_) break;

        auto restart_res = co_await signal_client_->Restart();
        if (!restart_res.error && restart_res.reconnect_response) {
            co_await RepublishLocalTracks(restart_res.reconnect_response);
            co_await RestartIceConnections(restart_res.reconnect_response);
            signal_client_->SetReconnected();
            signal_client_->SetEventReady();

            {
                std::lock_guard lock(room_mutex_);
                connection_state_ = ConnectionState::Connected;
                reconnect_attempts_ = 0;
                reconnect_active_ = false;
            }

            auto listeners_snapshot = GetListenersSnapshot();
            for (const auto& listener : listeners_snapshot) {
                listener->OnReconnected();
            }
            reconnected = true;
            break;
        }
    }

    if (!reconnected) {
        {
            std::lock_guard lock(room_mutex_);
            connection_state_ = ConnectionState::Disconnected;
            reconnect_active_ = false;
        }

        auto listeners_snapshot = GetListenersSnapshot();
        for (const auto& listener : listeners_snapshot) {
            listener->OnDisconnected("Reconnect Max Retries Exceeded");
        }
    }
}

void Room::RecordPublishedTracks() {
    std::lock_guard lock(room_mutex_);
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
        std::lock_guard lock(room_mutex_);
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
        std::lock_guard lock(room_mutex_);
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
        webrtc::PeerConnectionInterface* pub_ptr = pub_pc.get();
        auto state = std::make_shared<RtcStatsState>();
        auto pub_cb = RtcStatsCollectorBridge::Create(state);
        webrtc::RTCStatsCollectorCallback* raw_cb = pub_cb.get();

        WebRTCManager::Instance().signaling_thread()->PostTask([pub_ptr, raw_cb]() {
            pub_ptr->GetStats(raw_cb);
        });

        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock, std::chrono::milliseconds(1500), [&]() { return state->done; })) {
            const auto& r = state->report;
            for (const auto& cp : r.candidate_pairs) {
                if (cp.current_pair) {
                    room_report.publisher_rtt_ms = cp.current_round_trip_time * 1000.0;
                    room_report.available_outgoing_bitrate = cp.available_outgoing_bitrate;
                }
            }
            for (const auto& out : r.outbound_rtp) {
                room_report.total_bytes_sent += out.bytes_sent;
            }
            room_report.reports.push_back(r);
        }
    }

    if (sub_pc) {
        webrtc::PeerConnectionInterface* sub_ptr = sub_pc.get();
        auto state = std::make_shared<RtcStatsState>();
        auto sub_cb = RtcStatsCollectorBridge::Create(state);
        webrtc::RTCStatsCollectorCallback* raw_cb = sub_cb.get();

        WebRTCManager::Instance().signaling_thread()->PostTask([sub_ptr, raw_cb]() {
            sub_ptr->GetStats(raw_cb);
        });

        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock, std::chrono::milliseconds(1500), [&]() { return state->done; })) {
            const auto& r = state->report;
            for (const auto& cp : r.candidate_pairs) {
                if (cp.current_pair) {
                    room_report.subscriber_rtt_ms = cp.current_round_trip_time * 1000.0;
                }
            }
            for (const auto& in : r.inbound_rtp) {
                room_report.total_bytes_received += in.bytes_received;
            }
            room_report.reports.push_back(r);
        }
    }

    return room_report;
}

asio::awaitable<RoomStatsReport> Room::GetStats() {
    co_return GetStatsSync();
}

} // namespace livekit
