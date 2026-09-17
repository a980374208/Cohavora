#pragma once

#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <asio.hpp>
#include "track.h"
#include "chat_message.h"
#include "rpc_types.h"
#include "operation.h"

// Forward declare generated protobuf messages
namespace livekit {

class RemoteTrackPublication;
namespace proto {
class SignalRequest;
}
}

namespace livekit {

// Client-facing connection quality. Keep this independent from the protobuf
// enum so business/UI layers do not need to include the signaling schema.
enum class ConnectionQuality {
    Unknown,
    Poor,
    Good,
    Excellent,
    Lost,
};

struct ParticipantPermission {
    bool can_subscribe = true;
    bool can_publish = true;
    bool can_publish_data = true;
    bool can_update_metadata = true;
    bool hidden = false;
};

struct ParticipantStateSnapshot {
    std::string sid;
    std::string identity;
    std::string name;
    std::string metadata;
    bool speaking = false;
    float audio_level = 0.0f;
    ConnectionQuality connection_quality = ConnectionQuality::Unknown;
    float connection_quality_score = 0.0f;
    std::map<std::string, std::string> attributes;
    ParticipantPermission permission;
    std::vector<TrackPublication::StateSnapshot> publications;
};

class Participant {
public:
    Participant(const std::string& sid, const std::string& identity)
        : sid_(sid), identity_(identity) {}
    Participant(const Participant& other);
    Participant& operator=(const Participant& other);
    virtual ~Participant() = default;

    std::string sid() const;
    std::string identity() const;
    std::string name() const;
    std::string metadata() const;
    bool is_speaking() const;
    float audio_level() const;
    ConnectionQuality connection_quality() const;
    float connection_quality_score() const;

    void set_name(const std::string& name);
    void set_metadata(const std::string& metadata);
    void set_sid(const std::string& sid);
    void set_speaking(bool speaking);
    void set_audio_level(float level);
    void set_connection_quality(ConnectionQuality quality, float score);

    std::map<std::string, std::string> attributes() const;
    std::string get_attribute(const std::string& key) const;
    void set_attributes(const std::map<std::string, std::string>& attrs);
    void set_attribute(const std::string& key, const std::string& val);

    ParticipantPermission permission() const;
    void set_permission(const ParticipantPermission& perm);

    std::map<std::string, std::shared_ptr<TrackPublication>> tracks() const;

    void add_publication(std::shared_ptr<TrackPublication> pub);

    std::shared_ptr<TrackPublication> get_publication(const std::string& sid);
    std::shared_ptr<TrackPublication> get_publication(const std::string& sid) const;

    void remove_publication(const std::string& sid);
    ParticipantStateSnapshot SnapshotState() const;

protected:
    mutable std::mutex state_mutex_;
    std::string sid_;
    std::string identity_;
    std::string name_;
    std::string metadata_;
    bool speaking_{false};
    float audio_level_{0.0f};
    ConnectionQuality connection_quality_{ConnectionQuality::Unknown};
    float connection_quality_score_{0.0f};
    std::map<std::string, std::shared_ptr<TrackPublication>> tracks_;
    std::map<std::string, std::string> attributes_;
    ParticipantPermission permission_;
};

class LocalParticipant : public Participant {
public:
    using SendSignalHandler = std::function<void(const proto::SignalRequest&)>;
    using PublishDataHandler = std::function<void(const std::vector<uint8_t>& payload, bool reliable, const std::vector<std::string>& destination_identities, const std::string& topic)>;
    using SendRpcHandler = std::function<asio::awaitable<std::string>(const RpcPacket& packet)>;

    LocalParticipant(const std::string& sid, const std::string& identity, SendSignalHandler send_handler)
        : Participant(sid, identity), send_handler_(send_handler) {}

    using PublishTrackHandler = std::function<void(std::shared_ptr<Track>)>;
    using AsyncPublishTrackHandler = std::function<asio::awaitable<std::shared_ptr<TrackPublication>>(
        std::shared_ptr<Track>, const proto::SignalRequest&)>;
    using AsyncUnpublishTrackHandler = std::function<asio::awaitable<std::shared_ptr<TrackPublication>>(
        std::string)>;
    struct BatchTrackItem {
        std::shared_ptr<Track> track;
        std::shared_ptr<proto::SignalRequest> request;
    };
    using AsyncPublishTracksBatchHandler = std::function<asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>>(
        std::vector<BatchTrackItem>)>;
    void SetPublishTrackHandler(PublishTrackHandler handler) {
        publish_track_handler_ = std::move(handler);
    }

    void SetAsyncPublishTrackHandler(AsyncPublishTrackHandler handler) {
        async_publish_track_handler_ = std::move(handler);
    }

    void SetAsyncUnpublishTrackHandler(AsyncUnpublishTrackHandler handler) {
        async_unpublish_track_handler_ = std::move(handler);
    }

    void SetAsyncPublishTracksBatchHandler(AsyncPublishTracksBatchHandler handler) {
        async_publish_tracks_batch_handler_ = std::move(handler);
    }

    void SetPublishDataHandler(PublishDataHandler handler) {
        publish_data_handler_ = std::move(handler);
    }

    void SetSendRpcHandler(SendRpcHandler handler) {
        send_rpc_handler_ = std::move(handler);
    }

    // Legacy synchronous helper kept for isolated/offline tests. A participant
    // attached to a Room must use PublishTrackAsync so success cannot be faked.
    void PublishTrack(std::shared_ptr<Track> track);
    asio::awaitable<std::shared_ptr<TrackPublication>> PublishTrackAsync(
        std::shared_ptr<Track> track);
    // Batch publishing allows multiple local tracks to be added and negotiated
    // in a single Offer/Answer roundtrip, reducing startup latency.
    asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> PublishTracksBatchAsync(
        std::vector<std::shared_ptr<Track>> tracks);
    // The Room owns sender removal and publisher renegotiation. This entry
    // point deliberately delegates there instead of mutating the publication
    // map optimistically.
    asio::awaitable<std::shared_ptr<TrackPublication>> UnpublishTrackAsync(
        std::string track_sid);

    // 模拟本地静音控制逻辑
    void SetMuted(const std::string& track_sid, bool muted);

    // 发布自定义 Raw Data
    void PublishData(const std::vector<uint8_t>& payload, bool reliable = true,
                     const std::vector<std::string>& destination_identities = {}, const std::string& topic = "");

    // 发送结构化 Chat 消息 (对齐 client-sdk-cpp / Rust SDK)
    ChatMessage SendChatMessage(const std::string& text, const std::vector<std::string>& destination_identities = {});

    // 编辑已有 Chat 消息 (对齐 client-sdk-cpp / Rust SDK)
    ChatMessage EditChatMessage(const std::string& edit_text, const std::string& original_message_id);

    // === 新增：设置与动态更新 Participant Attributes ===
    void SetAttributes(const std::map<std::string, std::string>& attributes);
    void SetAttribute(const std::string& key, const std::string& value);

    // === 新增：LiveKit RPC 远程过程调用 ===
    void registerRpcMethod(const std::string& method_name, RpcHandler handler);
    void unregisterRpcMethod(const std::string& method_name);
    RpcHandler getRpcHandler(const std::string& method_name);

    asio::awaitable<std::string> performRpc(const std::string& destination_identity,
                                            const std::string& method,
                                            const std::string& payload,
                                            double response_timeout_sec = 15.0);

private:
    SendSignalHandler send_handler_;
    PublishTrackHandler publish_track_handler_;
    AsyncPublishTrackHandler async_publish_track_handler_;
    AsyncPublishTracksBatchHandler async_publish_tracks_batch_handler_;
    AsyncUnpublishTrackHandler async_unpublish_track_handler_;
    PublishDataHandler publish_data_handler_;
    SendRpcHandler send_rpc_handler_;

    mutable std::mutex rpc_mutex_;
    std::unordered_map<std::string, RpcHandler> rpc_handlers_;
};

class RemoteParticipant : public Participant {
public:
    RemoteParticipant(const std::string& sid, const std::string& identity)
        : Participant(sid, identity) {}

    // The participant map is the only authoritative owner of a remote
    // publication. Callers that need remote-only controls must resolve it
    // from here instead of constructing a parallel controller by SID.
    std::shared_ptr<RemoteTrackPublication> get_remote_publication(
        const std::string& sid) const;
};

} // namespace livekit
