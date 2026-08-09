#include "room.h"
#include "webrtc_manager.h"
#include "stats_collector.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "rtc_audio_source.h"
#include "rtc_video_source.h"
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

                publisher_observer_ = std::make_unique<RoomPeerConnectionObserver>(shared_from_this(), 0);
                subscriber_observer_ = std::make_unique<RoomPeerConnectionObserver>(shared_from_this(), 1);

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

                webrtc::PeerConnectionDependencies sub_deps(subscriber_observer_.get());
                auto sub_res = WebRTCManager::Instance().factory()->CreatePeerConnectionOrError(config, std::move(sub_deps));
                if (sub_res.ok()) {
                    subscriber_pc_ = sub_res.MoveValue();
                }
            }
        }

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
        std::lock_guard lock(room_mutex_);
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
    remote_data_channels_.clear();
    data_channel_observers_.clear();
    remote_track_sinks_.clear();

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
        std::lock_guard lock(room_mutex_);
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

        if (data_pkt.has_user()) {
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
        if (!buffer) return;

        auto i420_buffer = buffer->ToI420();
        VideoFrame frame = VideoFrame::create(width, height, VideoBufferType::I420);

        auto planes = frame.planeInfos();
        if (planes.size() >= 3) {
            uint8_t* y_dst = reinterpret_cast<uint8_t*>(planes[0].data_ptr);
            uint8_t* u_dst = reinterpret_cast<uint8_t*>(planes[1].data_ptr);
            uint8_t* v_dst = reinterpret_cast<uint8_t*>(planes[2].data_ptr);

            for (int r = 0; r < height; ++r) {
                std::memcpy(y_dst + r * planes[0].stride,
                            i420_buffer->DataY() + r * i420_buffer->StrideY(),
                            width);
            }
            int chroma_h = (height + 1) / 2;
            int chroma_w = (width + 1) / 2;
            for (int r = 0; r < chroma_h; ++r) {
                std::memcpy(u_dst + r * planes[1].stride,
                            i420_buffer->DataU() + r * i420_buffer->StrideU(),
                            chroma_w);
                std::memcpy(v_dst + r * planes[2].stride,
                            i420_buffer->DataV() + r * i420_buffer->StrideV(),
                            chroma_w);
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
                    std::cout << "[SIMULCAST WEBRTC] Configured AddTransceiver with " << init.send_encodings.size() << " send encodings:\n";
                    for (size_t idx = 0; idx < init.send_encodings.size(); ++idx) {
                        const auto& enc = init.send_encodings[idx];
                        std::cout << "  Encoding [" << idx << "]: rid='" << enc.rid
                                  << "', active=" << (enc.active ? "true" : "false")
                                  << ", scale_down=" << enc.scale_resolution_down_by.value_or(1.0)
                                  << ", max_bitrate=" << enc.max_bitrate_bps.value_or(0) << "bps\n";
                    }
                    auto result = p->pc->AddTransceiver(p->track, init);
                    if (result.ok()) {
                        std::cout << "[WebRTC] Successfully added video track '" << p->track->id()
                                  << "' with VP8 Simulcast (" << p->publish_opts.layers.size() << " layers)!" << std::endl;
                    } else {
                        std::cerr << "[WebRTC] AddTransceiver failed: " << result.error().message()
                                  << ", falling back to AddTrack..." << std::endl;
                        p->pc->AddTrack(p->track, { p->stream_id });
                    }
                } else {
                    p->pc->AddTrack(p->track, { p->stream_id });
                    std::cout << "[WebRTC] Added " << p->track->kind() << " track '" << p->track->id()
                              << "' to PeerConnection!" << std::endl;
                }
                p->room->NegotiatePublisher();
            }
            delete p;
        });
    }
}

void Room::NegotiatePublisher() {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::shared_ptr<SignalClient> client;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        client = signal_client_;
        if (!pub_pc || !client) return;

        if (publisher_negotiating_) {
            publisher_renegotiation_pending_ = true;
            return;
        }

        if (pub_pc->signaling_state() != webrtc::PeerConnectionInterface::SignalingState::kStable) {
            publisher_renegotiation_pending_ = true;
            return;
        }

        publisher_negotiating_ = true;
        publisher_renegotiation_pending_ = false;
    }

    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(pub_pc, executor_,
        [self, client, pub_pc](const std::string& sdp, const std::string& err) {
            if (!err.empty()) {
                std::cerr << "[WebRTC] CreateOffer error: " << err << std::endl;
                bool need_retry = false;
                {
                    std::lock_guard lock(self->room_mutex_);
                    self->publisher_negotiating_ = false;
                    if (self->publisher_renegotiation_pending_) {
                        self->publisher_renegotiation_pending_ = false;
                        need_retry = true;
                    }
                }
                if (need_retry) {
                    self->NegotiatePublisher();
                }
                return;
            }

            WebRTCManager::Instance().SetLocalDescription(pub_pc, "offer", sdp, self->executor_,
                [self, client, pub_pc, sdp](const std::string& set_local_err) {
                    if (!set_local_err.empty()) {
                        std::cerr << "[WebRTC] SetLocalDescription offer error: " << set_local_err << std::endl;
                        bool need_retry = false;
                        {
                            std::lock_guard lock(self->room_mutex_);
                            self->publisher_negotiating_ = false;
                            if (self->publisher_renegotiation_pending_) {
                                self->publisher_renegotiation_pending_ = false;
                                need_retry = true;
                            }
                        }
                        if (need_retry) {
                            self->NegotiatePublisher();
                        }
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
                            auto space_pos = line.find(' ');
                            if (space_pos != std::string::npos) {
                                std::string track_id = line.substr(space_pos + 1);
                                (*offer_msg->mutable_mid_to_track_id())[cur_mid] = track_id;
                            }
                        }
                    }

                    std::cout << "[SDP OFFER KEY ATTRIBUTES]:\n";
                    std::istringstream sdp_diag(sdp);
                    std::string diag_line;
                    while (std::getline(sdp_diag, diag_line)) {
                        if (diag_line.find("a=simulcast") != std::string::npos ||
                            diag_line.find("a=rid") != std::string::npos ||
                            diag_line.find("rtp-stream-id") != std::string::npos ||
                            diag_line.find("m=video") != std::string::npos) {
                            std::cout << "  " << diag_line << "\n";
                        }
                    }

                    client->Send(req);
                    std::cout << "[WebRTC] -> Sent publisher SDP Offer to LiveKit server with mid_to_track_id mapping!" << std::endl;
                });
        });
}

void Room::SendPublishOffer() {
    NegotiatePublisher();
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    if (!track) return;
    std::cout << "[TRACK ATTACHED] Attached sink to remote WebRTC " << track->kind() << " track (id: " << track->id() << ")" << std::endl;

    if (track->kind() == "audio") {
        auto audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto sink = std::make_shared<NativeAudioTrackSink>([this, track_id = track->id(), has_logged](const AudioFrame& frame) {
            std::string user_info = "Remote Participant";
            {
                std::lock_guard lock(room_mutex_);
                for (const auto& kv : remote_participants_) {
                    if (kv.second) {
                        user_info = kv.second->identity() + " (sid: " + kv.first + ")";
                        for (const auto& pub_kv : kv.second->tracks()) {
                            if (pub_kv.second && pub_kv.second->track()) {
                                pub_kv.second->track()->notifyAudioFrame(frame);
                            }
                        }
                    }
                }
            }
            if (!has_logged->exchange(true)) {
                std::cout << "[RECV AUDIO] Started receiving audio PCM stream from user: [" << user_info
                          << "], sample_rate=" << frame.sampleRate() << "Hz, channels=" << frame.numChannels() << std::endl;
            }
        });
        audio_track->AddSink(sink.get());
        std::lock_guard lock(room_mutex_);
        remote_track_sinks_.push_back(sink);
    } else if (track->kind() == "video") {
        auto video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto sink = std::make_shared<NativeVideoTrackSink>([this, track_id = track->id(), has_logged](const VideoFrame& frame, const VideoCaptureOptions& options) {
            std::string user_info = "Remote Participant";
            {
                std::lock_guard lock(room_mutex_);
                for (const auto& kv : remote_participants_) {
                    if (kv.second) {
                        user_info = kv.second->identity() + " (sid: " + kv.first + ")";
                        for (const auto& pub_kv : kv.second->tracks()) {
                            if (pub_kv.second && pub_kv.second->track()) {
                                pub_kv.second->track()->notifyVideoFrame(frame, options);
                            }
                        }
                    }
                }
            }
            if (!has_logged->exchange(true)) {
                std::cout << "[RECV VIDEO] Started receiving video stream from user: [" << user_info
                          << "], resolution=" << frame.width() << "x" << frame.height() << std::endl;
            }
        });
        video_track->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
        std::lock_guard lock(room_mutex_);
        remote_track_sinks_.push_back(sink);
    }
}

void Room::OnRenegotiationNeeded(int pc_type) {
    if (pc_type != 0) return; // 只有 Publisher PC 需要由 Client 发送 Offer
    NegotiatePublisher();
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
    } else if (msg->has_track_published()) {
        const auto& tp = msg->track_published();
        std::cout << "[SIGNAL RECV] TrackPublished ACK: cid='" << tp.cid()
                  << "', track_sid='" << tp.track().sid()
                  << "', type=" << tp.track().type()
                  << ", name='" << tp.track().name() << "'" << std::endl;
    }
}

void Room::UpdateParticipants(const google::protobuf::RepeatedPtrField<proto::ParticipantInfo>& participants) {
    std::vector<std::shared_ptr<RemoteParticipant>> newly_connected;
    std::vector<std::shared_ptr<RemoteParticipant>> disconnected;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;

    {
        std::lock_guard lock(room_mutex_);
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
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
        client = signal_client_;
        pc = (offer.type() == "offer" && subscriber_pc_) ? subscriber_pc_ : publisher_pc_;
        if (!pc && subscriber_pc_) pc = subscriber_pc_;
    }

    if (!client || !pc) {
        std::cerr << "Room::HandleOfferSignal: warning, PeerConnection or SignalClient is null" << std::endl;
        return;
    }

    std::cout << "[WebRTC] Received SDP Offer from server, setting RemoteDescription..." << std::endl;
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pc, offer.type(), offer.sdp(), executor_,
        [self, client, pc](const std::string& set_remote_err) {
            if (!set_remote_err.empty()) {
                std::cerr << "Room: SetRemoteDescription offer error: " << set_remote_err << std::endl;
                return;
            }

            std::cout << "[WebRTC] SetRemoteDescription offer succeeded. Generating SDP Answer..." << std::endl;
            WebRTCManager::Instance().CreateAnswer(pc, self->executor_,
                [self, client, pc](const std::string& sdp, const std::string& create_ans_err) {
                    if (!create_ans_err.empty()) {
                        std::cerr << "Room: CreateAnswer error: " << create_ans_err << std::endl;
                        return;
                    }

                    std::cout << "[WebRTC] CreateAnswer succeeded. Setting LocalDescription..." << std::endl;
                    WebRTCManager::Instance().SetLocalDescription(pc, "answer", sdp, self->executor_,
                        [self, client, sdp](const std::string& set_local_err) {
                            if (!set_local_err.empty()) {
                                std::cerr << "Room: SetLocalDescription answer error: " << set_local_err << std::endl;
                                return;
                            }

                            proto::SignalRequest req;
                            auto* answer_msg = req.mutable_answer();
                            answer_msg->set_type("answer");
                            answer_msg->set_sdp(sdp);
                            client->Send(req);
                            std::cout << "[WebRTC] -> Successfully created and sent SDP Answer back to LiveKit Server!" << std::endl;
                        });
                });
        });
}

void Room::HandleAnswerSignal(const proto::SessionDescription& answer) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
    }

    std::cout << "[SDP ANSWER] Received remote SDP Answer (length: " << answer.sdp().length() << " bytes):\n";
    std::istringstream answer_stream(answer.sdp());
    std::string ans_line;
    while (std::getline(answer_stream, ans_line)) {
        if (ans_line.find("m=video") != std::string::npos ||
            ans_line.find("a=simulcast") != std::string::npos ||
            ans_line.find("a=rid") != std::string::npos ||
            ans_line.find("a=recvonly") != std::string::npos ||
            ans_line.find("a=sendrecv") != std::string::npos ||
            ans_line.find("a=inactive") != std::string::npos) {
            std::cout << "  [SDP ANS] " << ans_line << "\n";
        }
    }
    auto self = shared_from_this();
    WebRTCManager::Instance().SetRemoteDescription(pub_pc, answer.type(), answer.sdp(), executor_,
        [self](const std::string& err) {
            bool need_retry = false;
            {
                std::lock_guard lock(self->room_mutex_);
                if (!err.empty()) {
                    std::cerr << "Room: SetRemoteDescription answer error: " << err << std::endl;
                } else {
                    std::cout << "[WebRTC] Publisher remote description applied successfully! PC signaling state is STABLE." << std::endl;
                }
                self->publisher_negotiating_ = false;
                if (self->publisher_renegotiation_pending_) {
                    self->publisher_renegotiation_pending_ = false;
                    need_retry = true;
                }
            }
            if (need_retry) {
                std::cout << "[WebRTC] Triggering queued renegotiation Offer..." << std::endl;
                self->NegotiatePublisher();
            }
        });
}

void Room::HandleTrickleSignal(const proto::TrickleRequest& trickle) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
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

asio::awaitable<RoomStatsReport> Room::GetStats() {
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
        auto pub_cb = RtcStatsCollectorBridge::Create();
        auto pub_future = pub_cb->get_future();
        pub_pc->GetStats(pub_cb.get());

        if (pub_future.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready) {
            auto r = pub_future.get();
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
        auto sub_cb = RtcStatsCollectorBridge::Create();
        auto sub_future = sub_cb->get_future();
        sub_pc->GetStats(sub_cb.get());

        if (sub_future.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready) {
            auto r = sub_future.get();
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

    co_return room_report;
}

} // namespace livekit
