#include "participant.h"
#include "livekit_rtc.pb.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <random>

namespace livekit {

static std::string GenerateUuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << std::hex << dis(gen) << dis(gen);
    return ss.str().substr(0, 16);
}

static int64_t CurrentEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void LocalParticipant::PublishTrack(std::shared_ptr<Track> track) {
    if (!track) return;

    proto::SignalRequest req;
    auto* add_track = req.mutable_add_track();
    add_track->set_cid(track->name()); 
    add_track->set_name(track->name());
    
    if (track->kind() == TrackKind::Audio) {
        add_track->set_type(proto::TrackType::AUDIO);
    } else if (track->kind() == TrackKind::Video) {
        add_track->set_type(proto::TrackType::VIDEO);
    }

    auto pub = std::make_shared<TrackPublication>(track, track->name(), track->name());
    add_publication(pub);

    if (send_handler_) {
        send_handler_(req);
    }
}

void LocalParticipant::SetMuted(const std::string& track_sid, bool muted) {
    auto pub = get_publication(track_sid);
    if (pub && pub->track()) {
        pub->track()->set_muted(muted);
    }

    proto::SignalRequest req;
    auto* mute_req = req.mutable_mute();
    mute_req->set_sid(track_sid);
    mute_req->set_muted(muted);

    if (send_handler_) {
        send_handler_(req);
    }
}

void LocalParticipant::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                                    const std::vector<std::string>& destination_identities, const std::string& topic) {
    if (publish_data_handler_) {
        publish_data_handler_(payload, reliable, destination_identities, topic);
    } else {
        std::cout << "LocalParticipant::PublishData: warning, publish_data_handler_ is not set" << std::endl;
    }
}

ChatMessage LocalParticipant::SendChatMessage(const std::string& text, const std::vector<std::string>& destination_identities) {
    ChatMessage msg;
    msg.id = "chat_" + GenerateUuid();
    msg.timestamp = CurrentEpochMs();
    msg.message = text;
    msg.sender_identity = identity();
    msg.destination_identities = destination_identities;

    std::string encoded = msg.Encode();
    std::vector<uint8_t> payload(encoded.begin(), encoded.end());

    PublishData(payload, /*reliable=*/true, destination_identities, /*topic=*/"lk.chat");
    return msg;
}

ChatMessage LocalParticipant::EditChatMessage(const std::string& edit_text, const std::string& original_message_id) {
    ChatMessage msg;
    msg.id = original_message_id;
    msg.timestamp = CurrentEpochMs(); // 可以保留原始时间
    msg.edit_timestamp = CurrentEpochMs();
    msg.message = edit_text;
    msg.sender_identity = identity();

    std::string encoded = msg.Encode();
    std::vector<uint8_t> payload(encoded.begin(), encoded.end());

    PublishData(payload, /*reliable=*/true, {}, /*topic=*/"lk.chat");
    return msg;
}

void LocalParticipant::registerRpcMethod(const std::string& method_name, RpcHandler handler) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    rpc_handlers_[method_name] = std::move(handler);
}

void LocalParticipant::unregisterRpcMethod(const std::string& method_name) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    rpc_handlers_.erase(method_name);
}

RpcHandler LocalParticipant::getRpcHandler(const std::string& method_name) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    auto it = rpc_handlers_.find(method_name);
    if (it != rpc_handlers_.end()) {
        return it->second;
    }
    return nullptr;
}

asio::awaitable<std::string> LocalParticipant::performRpc(const std::string& destination_identity,
                                                        const std::string& method,
                                                        const std::string& payload,
                                                        double response_timeout_sec) {
    if (!send_rpc_handler_) {
        throw RpcError(RpcErrorCode::NETWORK_ERROR, "RPC send handler is not configured");
    }

    RpcPacket packet;
    packet.type = RpcPacketType::Request;
    packet.request_id = "rpc_" + GenerateUuid();
    packet.method = method;
    packet.payload = payload;
    packet.caller_identity = identity();
    packet.destination_identity = destination_identity;
    packet.timeout_sec = response_timeout_sec;

    co_return co_await send_rpc_handler_(packet);
}

} // namespace livekit
