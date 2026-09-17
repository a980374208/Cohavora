#include "participant.h"
#include "telemetry.h"
#include "local_video_track.h"
#include "remote_track_publication.h"
#include "video_source.h"
#include "livekit_rtc.pb.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <random>

namespace livekit {

Participant::Participant(const Participant& other) {
    std::lock_guard<std::mutex> lock(other.state_mutex_);
    sid_ = other.sid_;
    identity_ = other.identity_;
    name_ = other.name_;
    metadata_ = other.metadata_;
    speaking_ = other.speaking_;
    audio_level_ = other.audio_level_;
    connection_quality_ = other.connection_quality_;
    connection_quality_score_ = other.connection_quality_score_;
    tracks_ = other.tracks_;
    attributes_ = other.attributes_;
    permission_ = other.permission_;
}

Participant& Participant::operator=(const Participant& other) {
    if (this == &other) return *this;

    std::string sid;
    std::string identity;
    std::string name;
    std::string metadata;
    bool speaking = false;
    float audio_level = 0.0f;
    ConnectionQuality connection_quality = ConnectionQuality::Unknown;
    float connection_quality_score = 0.0f;
    std::map<std::string, std::shared_ptr<TrackPublication>> tracks;
    std::map<std::string, std::string> attributes;
    ParticipantPermission permission;
    {
        std::lock_guard<std::mutex> lock(other.state_mutex_);
        sid = other.sid_;
        identity = other.identity_;
        name = other.name_;
        metadata = other.metadata_;
        speaking = other.speaking_;
        audio_level = other.audio_level_;
        connection_quality = other.connection_quality_;
        connection_quality_score = other.connection_quality_score_;
        tracks = other.tracks_;
        attributes = other.attributes_;
        permission = other.permission_;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sid_ = std::move(sid);
        identity_ = std::move(identity);
        name_ = std::move(name);
        metadata_ = std::move(metadata);
        speaking_ = speaking;
        audio_level_ = audio_level;
        connection_quality_ = connection_quality;
        connection_quality_score_ = connection_quality_score;
        tracks_ = std::move(tracks);
        attributes_ = std::move(attributes);
        permission_ = permission;
    }
    return *this;
}

std::string Participant::sid() const { std::lock_guard<std::mutex> lock(state_mutex_); return sid_; }
std::string Participant::identity() const { std::lock_guard<std::mutex> lock(state_mutex_); return identity_; }
std::string Participant::name() const { std::lock_guard<std::mutex> lock(state_mutex_); return name_; }
std::string Participant::metadata() const { std::lock_guard<std::mutex> lock(state_mutex_); return metadata_; }
bool Participant::is_speaking() const { std::lock_guard<std::mutex> lock(state_mutex_); return speaking_; }
float Participant::audio_level() const { std::lock_guard<std::mutex> lock(state_mutex_); return audio_level_; }
ConnectionQuality Participant::connection_quality() const { std::lock_guard<std::mutex> lock(state_mutex_); return connection_quality_; }
float Participant::connection_quality_score() const { std::lock_guard<std::mutex> lock(state_mutex_); return connection_quality_score_; }

void Participant::set_name(const std::string& name) { std::lock_guard<std::mutex> lock(state_mutex_); name_ = name; }
void Participant::set_metadata(const std::string& metadata) { std::lock_guard<std::mutex> lock(state_mutex_); metadata_ = metadata; }
void Participant::set_sid(const std::string& sid) { std::lock_guard<std::mutex> lock(state_mutex_); sid_ = sid; }
void Participant::set_speaking(bool speaking) { std::lock_guard<std::mutex> lock(state_mutex_); speaking_ = speaking; }
void Participant::set_audio_level(float level) { std::lock_guard<std::mutex> lock(state_mutex_); audio_level_ = level; }
void Participant::set_connection_quality(ConnectionQuality quality, float score) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    connection_quality_ = quality;
    connection_quality_score_ = score;
}

std::map<std::string, std::string> Participant::attributes() const { std::lock_guard<std::mutex> lock(state_mutex_); return attributes_; }
std::string Participant::get_attribute(const std::string& key) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = attributes_.find(key);
    return it != attributes_.end() ? it->second : std::string();
}
void Participant::set_attributes(const std::map<std::string, std::string>& attrs) { std::lock_guard<std::mutex> lock(state_mutex_); attributes_ = attrs; }
void Participant::set_attribute(const std::string& key, const std::string& val) { std::lock_guard<std::mutex> lock(state_mutex_); attributes_[key] = val; }
ParticipantPermission Participant::permission() const { std::lock_guard<std::mutex> lock(state_mutex_); return permission_; }
void Participant::set_permission(const ParticipantPermission& perm) { std::lock_guard<std::mutex> lock(state_mutex_); permission_ = perm; }
std::map<std::string, std::shared_ptr<TrackPublication>> Participant::tracks() const { std::lock_guard<std::mutex> lock(state_mutex_); return tracks_; }
void Participant::add_publication(std::shared_ptr<TrackPublication> pub) {
    if (!pub) return;
    const auto publication_sid = pub->sid();
    std::lock_guard<std::mutex> lock(state_mutex_);
    tracks_[publication_sid] = std::move(pub);
}
std::shared_ptr<TrackPublication> Participant::get_publication(const std::string& sid) {
    return static_cast<const Participant&>(*this).get_publication(sid);
}
std::shared_ptr<TrackPublication> Participant::get_publication(const std::string& sid) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = tracks_.find(sid);
    return it != tracks_.end() ? it->second : nullptr;
}
void Participant::remove_publication(const std::string& sid) { std::lock_guard<std::mutex> lock(state_mutex_); tracks_.erase(sid); }

ParticipantStateSnapshot Participant::SnapshotState() const {
    ParticipantStateSnapshot snapshot;
    std::lock_guard<std::mutex> lock(state_mutex_);
    snapshot.sid = sid_;
    snapshot.identity = identity_;
    snapshot.name = name_;
    snapshot.metadata = metadata_;
    snapshot.speaking = speaking_;
    snapshot.audio_level = audio_level_;
    snapshot.connection_quality = connection_quality_;
    snapshot.connection_quality_score = connection_quality_score_;
    snapshot.attributes = attributes_;
    snapshot.permission = permission_;
    snapshot.publications.reserve(tracks_.size());
    for (const auto& [sid, publication] : tracks_) {
        if (publication) snapshot.publications.push_back(publication->SnapshotState());
    }
    return snapshot;
}

std::shared_ptr<RemoteTrackPublication> RemoteParticipant::get_remote_publication(
    const std::string& sid) const {
    return std::dynamic_pointer_cast<RemoteTrackPublication>(get_publication(sid));
}

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

static proto::SignalRequest BuildAddTrackRequest(const std::shared_ptr<Track>& track,
                                                 const std::string& identity) {
    proto::SignalRequest req;
    auto* add_track = req.mutable_add_track();
    add_track->set_cid(track->name()); 
    add_track->set_name(track->name());
    add_track->set_muted(track->muted());
    std::string stream_id = "livekit_stream_" + (identity.empty() ? "local" : identity);
    add_track->set_stream(stream_id);
    
    if (track->kind() == TrackKind::Audio) {
        add_track->set_type(proto::TrackType::AUDIO);
        if (track->source() == TrackSource::ScreenShareAudio) {
            add_track->set_source(proto::TrackSource::SCREEN_SHARE_AUDIO);
        } else {
            add_track->set_source(proto::TrackSource::MICROPHONE);
        }
    } else if (track->kind() == TrackKind::Video) {
        add_track->set_type(proto::TrackType::VIDEO);
        if (track->source() == TrackSource::ScreenShareVideo) {
            add_track->set_source(proto::TrackSource::SCREEN_SHARE);
        } else {
            add_track->set_source(proto::TrackSource::CAMERA);
        }
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

            // GAP-03: Set BackupCodecPolicy
            proto::BackupCodecPolicy proto_policy = proto::BackupCodecPolicy::PREFER_REGRESSION;
            if (pub_opts.backup_codec_policy == BackupCodecPolicy::Simulcast) {
                proto_policy = proto::BackupCodecPolicy::SIMULCAST;
            } else if (pub_opts.backup_codec_policy == BackupCodecPolicy::Regression) {
                proto_policy = proto::BackupCodecPolicy::REGRESSION;
            }
            add_track->set_backup_codec_policy(proto_policy);

            if (pub_opts.simulcast && !pub_opts.simulcast_codecs.empty()) {
                for (size_t c_idx = 0; c_idx < pub_opts.simulcast_codecs.size(); ++c_idx) {
                    const auto& spec = pub_opts.simulcast_codecs[c_idx];
                    auto* sim_codec = add_track->add_simulcast_codecs();
                    sim_codec->set_codec(spec.codec);
                    std::string codec_cid = spec.cid.empty() ? (c_idx == 0 ? add_track->cid() : add_track->cid() + "_backup") : spec.cid;
                    sim_codec->set_cid(codec_cid);
                    sim_codec->set_video_layer_mode(proto::VideoLayer::ONE_SPATIAL_LAYER_PER_STREAM);

                    // Order layers ascending (q:0, h:1, f:2)
                    std::vector<VideoLayerSetting> ordered_layers = spec.layers;
                    std::sort(ordered_layers.begin(), ordered_layers.end(), [](const VideoLayerSetting& a, const VideoLayerSetting& b) {
                        auto get_idx = [](const std::string& r) {
                            if (r == "q") return 0;
                            if (r == "h") return 1;
                            return 2;
                        };
                        return get_idx(a.rid) < get_idx(b.rid);
                    });

                    for (const auto& layer_setting : ordered_layers) {
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

                        sim_layer->set_quality(q);
                        sim_layer->set_width(layer_setting.width);
                        sim_layer->set_height(layer_setting.height);
                        sim_layer->set_bitrate(layer_setting.max_bitrate_bps);
                        sim_layer->set_rid(layer_setting.rid);
                        sim_layer->set_spatial_layer(spatial_idx);

                        // Populate top-level layers for the primary codec
                        if (c_idx == 0) {
                            auto* layer = add_track->add_layers();
                            layer->set_quality(q);
                            layer->set_width(layer_setting.width);
                            layer->set_height(layer_setting.height);
                            layer->set_bitrate(layer_setting.max_bitrate_bps);
                            layer->set_rid(layer_setting.rid);
                            layer->set_spatial_layer(spatial_idx);
                        }
                    }
                }
                std::cout << "[SIMULCAST SIGNAL] Serialized " << add_track->simulcast_codecs_size() 
                          << " codecs (Primary=" << (add_track->simulcast_codecs_size() > 0 ? add_track->simulcast_codecs(0).codec() : "") 
                          << ", Policy=" << add_track->backup_codec_policy() << ") into AddTrackRequest (cid=" << add_track->cid() << ")\n";
            } else if (pub_opts.simulcast && !pub_opts.layers.empty()) {
                auto* sim_codec = add_track->add_simulcast_codecs();
                sim_codec->set_codec(pub_opts.video_codec);
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
            } else {
                auto* layer = add_track->add_layers();
                layer->set_quality(proto::VideoQuality::HIGH);
                layer->set_width(w);
                layer->set_height(h);
                layer->set_bitrate(2500000);
            }
        }
    }

    return req;
}

void LocalParticipant::PublishTrack(std::shared_ptr<Track> track) {
    if (!track) return;
    if (async_publish_track_handler_) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "legacy_publish",
                             "Room participants must use PublishTrackAsync");
    }
    if (!permission().can_publish) {
        std::cerr << "[LocalParticipant] Permission denied: cannot publish track (can_publish is false).\n";
        return;
    }
    Telemetry::Instance().RecordPublishStart();

    auto req = BuildAddTrackRequest(track, identity());
    auto pub = std::make_shared<TrackPublication>(track, track->name(), track->name());
    add_publication(pub);

    if (publish_track_handler_) {
        publish_track_handler_(track);
    }

    if (send_handler_) {
        send_handler_(req);
    }
}

asio::awaitable<std::shared_ptr<TrackPublication>> LocalParticipant::PublishTrackAsync(
    std::shared_ptr<Track> track) {
    if (!track) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "validate",
                             "track is null");
    }
    if (!permission().can_publish) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::PermissionDenied,
                             "validate_permission",
                             "participant is not allowed to publish tracks");
    }
    if (!async_publish_track_handler_) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "validate_session",
                             "participant is not attached to an active Room");
    }

    Telemetry::Instance().RecordPublishStart();
    auto req = BuildAddTrackRequest(track, identity());
    co_return co_await async_publish_track_handler_(std::move(track), req);
}

asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> LocalParticipant::PublishTracksBatchAsync(
    std::vector<std::shared_ptr<Track>> tracks) {
    if (tracks.empty()) {
        co_return std::vector<std::shared_ptr<TrackPublication>>{};
    }
    for (const auto& track : tracks) {
        if (!track) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "validate",
                                 "track is null");
        }
    }
    if (!permission().can_publish) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::PermissionDenied,
                             "validate_permission",
                             "participant is not allowed to publish tracks");
    }
    if (!async_publish_tracks_batch_handler_) {
        // Fallback to sequential publishing if batch handler is not attached
        std::vector<std::shared_ptr<TrackPublication>> publications;
        publications.reserve(tracks.size());
        for (auto& track : tracks) {
            publications.push_back(co_await PublishTrackAsync(std::move(track)));
        }
        co_return publications;
    }

    std::vector<BatchTrackItem> items;
    items.reserve(tracks.size());
    for (auto& track : tracks) {
        Telemetry::Instance().RecordPublishStart();
        auto req = std::make_shared<proto::SignalRequest>(BuildAddTrackRequest(track, identity()));
        items.push_back({std::move(track), std::move(req)});
    }
    co_return co_await async_publish_tracks_batch_handler_(std::move(items));
}

asio::awaitable<std::shared_ptr<TrackPublication>> LocalParticipant::UnpublishTrackAsync(
    std::string track_sid) {
    if (track_sid.empty()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "validate",
                             "track SID is empty");
    }
    if (!async_unpublish_track_handler_) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "validate_session",
                             "participant is not attached to an active Room");
    }
    co_return co_await async_unpublish_track_handler_(std::move(track_sid));
}

void LocalParticipant::SetMuted(const std::string& track_sid, bool muted) {
    std::string actual_sid = track_sid;
    std::vector<std::shared_ptr<Track>> target_tracks;
    if (actual_sid.empty()) {
        const auto publications = tracks();
        for (const auto& [sid, pub] : publications) {
            const auto track = pub ? pub->track() : nullptr;
            if (track) {
                target_tracks.push_back(track);
                if (!sid.empty() && sid.find("TR_") == 0) {
                    actual_sid = sid;
                    break;
                }
            }
        }
    } else {
        auto pub = get_publication(actual_sid);
        if (auto track = pub ? pub->track() : nullptr) {
            target_tracks.push_back(std::move(track));
        }
    }

    for (const auto& track : target_tracks) track->set_muted(muted);

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
    if (!permission().can_publish_data) {
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
    std::string metadata;
    std::string identity;
    std::map<std::string, std::string> merged_attributes;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!permission_.can_update_metadata) {
            std::cerr << "[LocalParticipant] Permission denied: cannot update metadata/attributes (can_update_metadata is false).\n";
            return;
        }
        for (const auto& kv : attributes) attributes_[kv.first] = kv.second;
        metadata = metadata_;
        identity = identity_;
        merged_attributes = attributes_;
    }

    if (send_handler_) {
        proto::SignalRequest req;
        auto* update_meta = req.mutable_update_metadata();
        update_meta->set_metadata(metadata);
        update_meta->set_name(identity);
        auto* pb_attrs = update_meta->mutable_attributes();
        for (const auto& kv : merged_attributes) {
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
