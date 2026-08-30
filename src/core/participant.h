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
namespace proto {
class SignalRequest;
}
}

namespace livekit {

struct ParticipantPermission {
    bool can_subscribe = true;
    bool can_publish = true;
    bool can_publish_data = true;
    bool can_update_metadata = true;
    bool hidden = false;
};

class Participant {
public:
    Participant(const std::string& sid, const std::string& identity)
        : sid_(sid), identity_(identity) {}
    virtual ~Participant() = default;

    std::string sid() const { return sid_; }
    std::string identity() const { return identity_; }
    std::string metadata() const { return metadata_; }
    bool is_speaking() const { return speaking_; }
    float audio_level() const { return audio_level_; }

    void set_metadata(const std::string& metadata) { metadata_ = metadata; }
    void set_sid(const std::string& sid) { sid_ = sid; }
    void set_speaking(bool speaking) { speaking_ = speaking; }
    void set_audio_level(float level) { audio_level_ = level; }

    std::map<std::string, std::string> attributes() const { return attributes_; }
    std::string get_attribute(const std::string& key) const {
        auto it = attributes_.find(key);
        if (it != attributes_.end()) return it->second;
        return "";
    }
    void set_attributes(const std::map<std::string, std::string>& attrs) { attributes_ = attrs; }
    void set_attribute(const std::string& key, const std::string& val) { attributes_[key] = val; }

    ParticipantPermission permission() const { return permission_; }
    void set_permission(const ParticipantPermission& perm) { permission_ = perm; }

    std::map<std::string, std::shared_ptr<TrackPublication>> tracks() const { return tracks_; }

    void add_publication(std::shared_ptr<TrackPublication> pub) {
        tracks_[pub->sid()] = pub;
    }

    std::shared_ptr<TrackPublication> get_publication(const std::string& sid) {
        auto it = tracks_.find(sid);
        if (it != tracks_.end()) {
            return it->second;
        }
        return nullptr;
    }

    void remove_publication(const std::string& sid) {
        tracks_.erase(sid);
    }

protected:
    std::string sid_;
    std::string identity_;
    std::string metadata_;
    bool speaking_{false};
    float audio_level_{0.0f};
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
    void SetPublishTrackHandler(PublishTrackHandler handler) {
        publish_track_handler_ = std::move(handler);
    }

    void SetAsyncPublishTrackHandler(AsyncPublishTrackHandler handler) {
        async_publish_track_handler_ = std::move(handler);
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
    PublishDataHandler publish_data_handler_;
    SendRpcHandler send_rpc_handler_;

    mutable std::mutex rpc_mutex_;
    std::unordered_map<std::string, RpcHandler> rpc_handlers_;
};

class RemoteParticipant : public Participant {
public:
    RemoteParticipant(const std::string& sid, const std::string& identity)
        : Participant(sid, identity) {}
};

} // namespace livekit
