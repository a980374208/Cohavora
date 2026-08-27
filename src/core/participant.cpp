#include "participant.h"
#include "telemetry.h"
#include "local_video_track.h"
#include "video_source.h"
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
    if (!permission_.can_publish) {
        std::cerr << "[LocalParticipant] Permission denied: cannot publish track (can_publish is false).\n";
        return;
    }
    Telemetry::Instance().RecordPublishStart();

    proto::SignalRequest req;
    auto* add_track = req.mutable_add_track();
    add_track->set_cid(track->name()); 
    add_track->set_name(track->name());
    add_track->set_muted(false);
    std::string stream_id = "livekit_stream_" + (identity().empty() ? "local" : identity());
    add_track->set_stream(stream_id);
    
    if (track->kind() == TrackKind::Audio) {
        add_track->set_type(proto::TrackType::AUDIO);
        add_track->set_source(proto::TrackSource::MICROPHONE);
    } else if (track->kind() == TrackKind::Video) {
        add_track->set_type(proto::TrackType::VIDEO);
        add_track->set_source(proto::TrackSource::CAMERA);
        int w = 1280;
        int h = 720;
        auto vid_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
        if (vid_track && vid_track->source()) {
            if (vid_track->source()->width() > 0) w = vid_track->source()->width();
            if (vid_track->source()->height() > 0) h = vid_track->source()->height();
        }
        if (vid_track) {
            auto pub_opts = vid_track->publish_options();
            if (pub_opts.simulcast && !pub_opts.layers.empty()) {
                w = pub_opts.layers[0].width;
                h = pub_opts.layers[0].height;
            }
        }
        add_track->set_width(w);
        add_track->set_height(h);

        if (vid_track) {
            auto pub_opts = vid_track->publish_options();
            if (pub_opts.simulcast && !pub_opts.layers.empty()) {
                auto* sim_codec = add_track->add_simulcast_codecs();
                sim_codec->set_codec("vp8");
                sim_codec->set_cid(add_track->cid());
                sim_codec->set_video_layer_mode(proto::VideoLayer::ONE_SPATIAL_LAYER_PER_STREAM);

                // Order layers for AddTrackRequest Protobuf payload by spatial_layer ascending (q:0, h:1, f:2)
                std::vector<VideoLayerSetting> ordered_layers = pub_opts.layers;
                std::sort(ordered_layers.begin(), ordered_layers.end(), [](const VideoLayerSetting& a, const VideoLayerSetting& b) {
                    auto get_idx = [](const std::string& r) {
                        if (r == "q") return 0;
                        if (r == "h") return 1;
                        return 2;
                    };
                    return get_idx(a.rid) < get_idx(b.rid);
                });

                for (const auto& layer_setting : ordered_layers) {
                    auto* layer = add_track->add_layers();
                    auto* sim_layer = sim_codec->add_layers();

                    proto::VideoQuality q = proto::VideoQuality::HIGH;
                    int spatial_idx = 2;
                    if (layer_setting.rid == "q") {
                        q = proto::VideoQuality::LOW;
                        spatial_idx = 0;
                    } else if (layer_setting.rid == "h") {
                        q = proto::VideoQuality::MEDIUM;
                        spatial_idx = 1;
                    } else {
                        q = proto::VideoQuality::HIGH;
                        spatial_idx = 2;
                    }

                    layer->set_quality(q);
                    layer->set_width(layer_setting.width);
                    layer->set_height(layer_setting.height);
                    layer->set_bitrate(layer_setting.max_bitrate_bps);
                    layer->set_rid(layer_setting.rid);
                    layer->set_spatial_layer(spatial_idx);

                    sim_layer->set_quality(q);
                    sim_layer->set_width(layer_setting.width);
                    sim_layer->set_height(layer_setting.height);
                    sim_layer->set_bitrate(layer_setting.max_bitrate_bps);
                    sim_layer->set_rid(layer_setting.rid);
                    sim_layer->set_spatial_layer(spatial_idx);
                }
                std::cout << "[SIMULCAST SIGNAL] Serialized " << pub_opts.layers.size() << " layers into AddTrackRequest (cid=" << add_track->cid() << "):\n";
                for (int i = 0; i < add_track->layers_size(); ++i) {
                    const auto& l = add_track->layers(i);
                    std::cout << "  Layer [" << i << "]: rid='" << l.rid() << "', spatial=" << l.spatial_layer()
                              << ", quality=" << l.quality() << ", " << l.width() << "x" << l.height()
                              << " @" << l.bitrate() << "bps\n";
                }
            } else {
                auto* layer = add_track->add_layers();
                layer->set_quality(proto::VideoQuality::HIGH);
                layer->set_width(w);
                layer->set_height(h);
                layer->set_bitrate(2500000);
            }
        }
    }

    auto pub = std::make_shared<TrackPublication>(track, track->name(), track->name());
    add_publication(pub);

    if (publish_track_handler_) {
        publish_track_handler_(track);
    }

    if (send_handler_) {
        send_handler_(req);
    }
}

void LocalParticipant::SetMuted(const std::string& track_sid, bool muted) {
    std::string actual_sid = track_sid;
    if (actual_sid.empty()) {
        for (const auto& [sid, pub] : tracks_) {
            if (pub && pub->track()) {
                pub->track()->set_muted(muted);
                if (!sid.empty() && sid.find("TR_") == 0) {
                    actual_sid = sid;
                    break;
                }
            }
        }
    } else {
        auto pub = get_publication(actual_sid);
        if (pub && pub->track()) {
            pub->track()->set_muted(muted);
        }
    }

    if (actual_sid.empty()) {
        return;
    }

    proto::SignalRequest req;
    auto* mute_req = req.mutable_mute();
    mute_req->set_sid(actual_sid);
    mute_req->set_muted(muted);

    if (send_handler_) {
        send_handler_(req);
    }
}

void LocalParticipant::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                                    const std::vector<std::string>& destination_identities, const std::string& topic) {
    if (!permission_.can_publish_data) {
        std::cerr << "[LocalParticipant] Permission denied: cannot publish data (can_publish_data is false).\n";
        return;
    }
    if (publish_data_handler_) {
        publish_data_handler_(payload, reliable, destination_identities, topic);
    } else {
        std::cout << "LocalParticipant::PublishData: warning, publish_data_handler_ is not set" << std::endl;
    }
}

void LocalParticipant::SetAttributes(const std::map<std::string, std::string>& attributes) {
    if (!permission_.can_update_metadata) {
        std::cerr << "[LocalParticipant] Permission denied: cannot update metadata/attributes (can_update_metadata is false).\n";
        return;
    }

    for (const auto& kv : attributes) {
        attributes_[kv.first] = kv.second;
    }

    if (send_handler_) {
        proto::SignalRequest req;
        auto* update_meta = req.mutable_update_metadata();
        update_meta->set_metadata(metadata_);
        update_meta->set_name(identity_);
        auto* pb_attrs = update_meta->mutable_attributes();
        for (const auto& kv : attributes_) {
            (*pb_attrs)[kv.first] = kv.second;
        }
        send_handler_(req);
    }
}

void LocalParticipant::SetAttribute(const std::string& key, const std::string& value) {
    SetAttributes({{key, value}});
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
