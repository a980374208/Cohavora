#pragma once

#include "whiteboard_document.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace livekit::whiteboard {

inline constexpr std::string_view ControlTopic = "whiteboard.v1.control";
inline constexpr std::string_view OperationsTopic = "whiteboard.v1.ops";
inline constexpr std::string_view SyncTopic = "whiteboard.v1.sync";
inline constexpr std::string_view PresenceTopic = "whiteboard.v1.presence";
inline constexpr std::string_view AssetTopic = "whiteboard.v1.asset";
inline constexpr std::size_t MaxWireBytes = 12 * 1024;

enum class MessageKind {
    Discover,
    Descriptor,
    Propose,
    Commit,
    Reject,
    SyncRequest,
    Delta,
    SnapshotChunk,
    Heartbeat,
    AssetRequest,
    AssetChunk,
};

struct CommitEntry {
    std::uint64_t sequence = 0;
    bool control = false;
    Command command;
    bool locked = false;
    bool writersOpen = true;
    std::vector<std::string> writers;
};

struct Message {
    MessageKind kind = MessageKind::Discover;
    std::string documentId;
    std::string authorityIdentity;
    std::uint64_t sequence = 0;
    std::uint64_t baseSequence = 0;
    std::uint64_t sentAtMs = 0;
    bool locked = false;
    bool writersOpen = true;
    std::vector<std::string> writers;
    std::string requestId;
    std::string reason;
    CommitEntry entry;
    std::vector<CommitEntry> entries;
    std::string snapshotId;
    std::string snapshotHash;
    std::uint32_t chunkIndex = 0;
    std::uint32_t chunkCount = 0;
    std::string payload;
    std::string assetId;
    std::string assetMime;
    std::uint32_t assetWidth = 0;
    std::uint32_t assetHeight = 0;
    std::uint32_t assetBytes = 0;
};

bool isWhiteboardTopic(std::string_view topic);
std::string_view topicFor(MessageKind kind);
std::optional<std::string> encodeMessage(const Message &message);
std::optional<Message> decodeMessage(std::string_view topic, std::string_view bytes);
std::string contentHash(std::string_view bytes);

} // namespace livekit::whiteboard
