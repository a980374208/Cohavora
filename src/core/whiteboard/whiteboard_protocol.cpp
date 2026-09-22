#include "whiteboard_protocol.h"
#include "whiteboard_asset.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace livekit::whiteboard {
namespace {
using Json = nlohmann::json;

constexpr std::string_view names[] = {
    "discover", "descriptor", "propose", "commit", "reject",
    "sync_request", "delta", "snapshot_chunk", "heartbeat",
    "asset_request", "asset_chunk"};

bool identifier(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c == '.' || c == '@';
        });
}

bool wireIdentity(std::string_view value) {
    return !value.empty() && value.size() <= 256 &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
}

std::string counter(std::uint64_t value) { return std::to_string(value); }

std::uint64_t parseCounter(const Json &value, bool allowZero = true) {
    if (!value.is_string()) throw std::invalid_argument("counter type");
    const auto &text = value.get_ref<const std::string &>();
    if (text.empty() || text.size() > 20) throw std::invalid_argument("counter size");
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (!allowZero && result == 0)) throw std::invalid_argument("counter");
    return result;
}

Json pointJson(const Point &point) { return Json::array({point.x, point.y}); }

Json commandJson(const Command &command) {
    Json points = Json::array();
    for (const auto &point : command.object.points) points.push_back(pointJson(point));
    return Json{{"id", command.id}, {"actor", command.actor},
        {"context", {{"documentId", command.context.documentId}, {"pageId", command.context.pageId},
            {"pageEpoch", counter(command.context.pageEpoch)},
            {"interactionEpoch", counter(command.context.interactionEpoch)}}},
        {"kind", static_cast<int>(command.kind)}, {"target", command.target},
        {"assetId", command.assetId}, {"pageWidth", command.pageWidth},
        {"pageHeight", command.pageHeight},
        {"object", {{"id", command.object.id}, {"author", command.object.author},
            {"kind", static_cast<int>(command.object.kind)}, {"color", command.object.color},
            {"width", command.object.width}, {"fontSize", command.object.fontSize},
            {"points", std::move(points)}, {"text", command.object.text}}}};
}

Command parseCommand(const Json &value) {
    Command result;
    result.id = value.at("id").get<std::string>();
    result.actor = value.at("actor").get<std::string>();
    const auto &context = value.at("context");
    result.context.documentId = context.at("documentId").get<std::string>();
    result.context.pageId = context.at("pageId").get<std::string>();
    result.context.pageEpoch = parseCounter(context.at("pageEpoch"), false);
    result.context.interactionEpoch = parseCounter(context.at("interactionEpoch"), false);
    if (!value.at("kind").is_number_integer()) throw std::invalid_argument("command kind");
    const int kind = value.at("kind").get<int>();
    if (kind < 0 || kind > static_cast<int>(CommandKind::SetBackground)) throw std::invalid_argument("command kind");
    result.kind = static_cast<CommandKind>(kind);
    result.target = value.at("target").get<std::string>();
    result.assetId = value.at("assetId").get<std::string>();
    result.pageWidth = value.at("pageWidth").get<double>();
    result.pageHeight = value.at("pageHeight").get<double>();
    const auto &object = value.at("object");
    result.object.id = object.at("id").get<std::string>();
    result.object.author = object.at("author").get<std::string>();
    if (!object.at("kind").is_number_integer() || !object.at("color").is_number_unsigned())
        throw std::invalid_argument("object type");
    const int objectKind = object.at("kind").get<int>();
    if (objectKind < 0 || objectKind > static_cast<int>(ObjectKind::Text)) throw std::invalid_argument("object kind");
    result.object.kind = static_cast<ObjectKind>(objectKind);
    result.object.color = object.at("color").get<std::uint32_t>();
    result.object.width = object.at("width").get<double>();
    result.object.fontSize = object.at("fontSize").get<double>();
    result.object.text = object.at("text").get<std::string>();
    const auto &points = object.at("points");
    if (!points.is_array() || points.size() > Document::MaxStrokePoints) throw std::invalid_argument("points");
    for (const auto &point : points) {
        if (!point.is_array() || point.size() != 2) throw std::invalid_argument("point");
        result.object.points.push_back({point.at(0).get<double>(), point.at(1).get<double>()});
    }
    if (!identifier(result.id) || !identifier(result.actor) ||
        !identifier(result.context.documentId) || !identifier(result.context.pageId) ||
        result.target.size() > 128 || result.assetId.size() > 64 ||
        (!result.assetId.empty() && !validAssetId(result.assetId)) ||
        result.object.id.size() > 128 ||
        result.object.author.size() > 128 || result.object.text.size() > 8000)
        throw std::invalid_argument("command field");
    return result;
}

Json writersJson(const std::vector<std::string> &writers) {
    Json result = Json::array();
    for (const auto &writer : writers) result.push_back(writer);
    return result;
}

std::vector<std::string> parseWriters(const Json &value) {
    if (!value.is_array() || value.size() > 256) throw std::invalid_argument("writers");
    std::vector<std::string> result;
    std::unordered_set<std::string> unique;
    for (const auto &item : value) {
        const auto writer = item.get<std::string>();
        if (!wireIdentity(writer) || !unique.insert(writer).second) throw std::invalid_argument("writer");
        result.push_back(writer);
    }
    return result;
}

Json entryJson(const CommitEntry &entry) {
    Json value{{"seq", counter(entry.sequence)}, {"control", entry.control}};
    if (entry.control) {
        value["locked"] = entry.locked;
        value["writersOpen"] = entry.writersOpen;
        value["writers"] = writersJson(entry.writers);
    } else value["command"] = commandJson(entry.command);
    return value;
}

CommitEntry parseEntry(const Json &value) {
    CommitEntry result;
    result.sequence = parseCounter(value.at("seq"), false);
    result.control = value.at("control").get<bool>();
    if (result.control) {
        result.locked = value.at("locked").get<bool>();
        result.writersOpen = value.at("writersOpen").get<bool>();
        result.writers = parseWriters(value.at("writers"));
    } else result.command = parseCommand(value.at("command"));
    return result;
}

bool kindMatchesTopic(MessageKind kind, std::string_view topic) {
    return topicFor(kind) == topic;
}

std::string base64Encode(std::string_view bytes) {
    if (bytes.empty()) return {};
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()),
        reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    if (size < 0) return {};
    result.resize(static_cast<std::size_t>(size));
    return result;
}

std::optional<std::string> base64Decode(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() > 4 * ((AssetChunkBytes + 2) / 3))
        return std::nullopt;
    std::string result((encoded.size() / 4) * 3, '\0');
    const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(result.data()),
        reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<int>(encoded.size()));
    if (size < 0) return std::nullopt;
    std::size_t actual = static_cast<std::size_t>(size);
    if (!encoded.empty() && encoded.back() == '=') --actual;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --actual;
    if (actual == 0 || actual > AssetChunkBytes) return std::nullopt;
    result.resize(actual);
    return result;
}
} // namespace

bool isWhiteboardTopic(std::string_view topic) {
    return topic.size() > std::string_view("whiteboard.").size() &&
        topic.substr(0, std::string_view("whiteboard.").size()) == "whiteboard.";
}

std::string_view topicFor(MessageKind kind) {
    switch (kind) {
    case MessageKind::Propose:
    case MessageKind::Commit: return OperationsTopic;
    case MessageKind::SyncRequest:
    case MessageKind::Delta:
    case MessageKind::SnapshotChunk: return SyncTopic;
    case MessageKind::Heartbeat: return PresenceTopic;
    case MessageKind::AssetRequest:
    case MessageKind::AssetChunk: return AssetTopic;
    default: return ControlTopic;
    }
}

std::optional<std::string> encodeMessage(const Message &message) {
    const auto kind = static_cast<std::size_t>(message.kind);
    if (kind >= std::size(names) || !identifier(message.documentId) ||
        !wireIdentity(message.authorityIdentity)) return std::nullopt;
    Json value{{"v", 1}, {"kind", names[kind]}, {"documentId", message.documentId},
        {"authority", message.authorityIdentity}, {"seq", counter(message.sequence)}};
    switch (message.kind) {
    case MessageKind::Discover:
    case MessageKind::SyncRequest:
        value["baseSeq"] = counter(message.baseSequence);
        break;
    case MessageKind::Descriptor:
        value["locked"] = message.locked;
        value["writersOpen"] = message.writersOpen;
        value["writers"] = writersJson(message.writers);
        value["snapshotHash"] = message.snapshotHash;
        break;
    case MessageKind::Propose:
        value["requestId"] = message.requestId;
        value["command"] = commandJson(message.entry.command);
        break;
    case MessageKind::Commit:
        value["entry"] = entryJson(message.entry);
        break;
    case MessageKind::Reject:
        value["requestId"] = message.requestId;
        value["reason"] = message.reason.substr(0, 128);
        break;
    case MessageKind::Delta: {
        value["baseSeq"] = counter(message.baseSequence);
        Json entries = Json::array();
        for (const auto &entry : message.entries) entries.push_back(entryJson(entry));
        value["entries"] = std::move(entries);
        break;
    }
    case MessageKind::SnapshotChunk:
        value["baseSeq"] = counter(message.baseSequence);
        value["snapshotId"] = message.snapshotId;
        value["snapshotHash"] = message.snapshotHash;
        value["chunkIndex"] = message.chunkIndex;
        value["chunkCount"] = message.chunkCount;
        value["locked"] = message.locked;
        value["writersOpen"] = message.writersOpen;
        value["writers"] = writersJson(message.writers);
        value["payload"] = message.payload;
        break;
    case MessageKind::Heartbeat:
        value["sentAtMs"] = counter(message.sentAtMs);
        break;
    case MessageKind::AssetRequest:
        if (!validAssetId(message.assetId)) return std::nullopt;
        value["assetId"] = message.assetId;
        break;
    case MessageKind::AssetChunk:
        if (!validAssetId(message.assetId) || message.assetMime != "image/png" ||
            message.assetWidth == 0 || message.assetHeight == 0 ||
            message.assetWidth > MaxAssetDimension || message.assetHeight > MaxAssetDimension ||
            static_cast<std::uint64_t>(message.assetWidth) * message.assetHeight > MaxAssetPixels ||
            message.assetBytes == 0 || message.assetBytes > MaxAssetBytes ||
            message.chunkCount == 0 || message.chunkIndex >= message.chunkCount ||
            message.payload.empty() || message.payload.size() > AssetChunkBytes) return std::nullopt;
        value["assetId"] = message.assetId;
        value["mime"] = message.assetMime;
        value["width"] = message.assetWidth;
        value["height"] = message.assetHeight;
        value["bytes"] = message.assetBytes;
        value["chunkIndex"] = message.chunkIndex;
        value["chunkCount"] = message.chunkCount;
        value["payload"] = base64Encode(message.payload);
        break;
    }
    auto bytes = value.dump();
    if (bytes.size() > MaxWireBytes) return std::nullopt;
    return bytes;
}

std::optional<Message> decodeMessage(std::string_view topic, std::string_view bytes) {
    if (!isWhiteboardTopic(topic) || bytes.empty() || bytes.size() > MaxWireBytes) return std::nullopt;
    try {
        const auto value = Json::parse(bytes, [](int depth, Json::parse_event_t, Json &) {
            if (depth > 16) throw std::invalid_argument("depth");
            return true;
        });
        if (!value.is_object() || value.size() > 24 || value.at("v") != 1) return std::nullopt;
        const auto name = value.at("kind").get<std::string>();
        const auto found = std::find(names, names + std::size(names), name);
        if (found == names + std::size(names)) return std::nullopt;
        Message result;
        result.kind = static_cast<MessageKind>(found - names);
        if (!kindMatchesTopic(result.kind, topic)) return std::nullopt;
        result.documentId = value.at("documentId").get<std::string>();
        result.authorityIdentity = value.at("authority").get<std::string>();
        result.sequence = parseCounter(value.at("seq"));
        if (!identifier(result.documentId) || !wireIdentity(result.authorityIdentity)) return std::nullopt;
        switch (result.kind) {
        case MessageKind::Discover:
        case MessageKind::SyncRequest:
            result.baseSequence = parseCounter(value.at("baseSeq"));
            break;
        case MessageKind::Descriptor:
            result.locked = value.at("locked").get<bool>();
            result.writersOpen = value.at("writersOpen").get<bool>();
            result.writers = parseWriters(value.at("writers"));
            result.snapshotHash = value.at("snapshotHash").get<std::string>();
            break;
        case MessageKind::Propose:
            result.requestId = value.at("requestId").get<std::string>();
            result.entry.command = parseCommand(value.at("command"));
            if (!identifier(result.requestId)) return std::nullopt;
            break;
        case MessageKind::Commit:
            result.entry = parseEntry(value.at("entry"));
            break;
        case MessageKind::Reject:
            result.requestId = value.at("requestId").get<std::string>();
            result.reason = value.at("reason").get<std::string>();
            if (!identifier(result.requestId) || result.reason.size() > 128) return std::nullopt;
            break;
        case MessageKind::Delta:
            result.baseSequence = parseCounter(value.at("baseSeq"));
            if (!value.at("entries").is_array() || value.at("entries").size() > 64) return std::nullopt;
            for (const auto &entry : value.at("entries")) result.entries.push_back(parseEntry(entry));
            break;
        case MessageKind::SnapshotChunk:
            result.baseSequence = parseCounter(value.at("baseSeq"));
            result.snapshotId = value.at("snapshotId").get<std::string>();
            result.snapshotHash = value.at("snapshotHash").get<std::string>();
            result.chunkIndex = value.at("chunkIndex").get<std::uint32_t>();
            result.chunkCount = value.at("chunkCount").get<std::uint32_t>();
            result.locked = value.at("locked").get<bool>();
            result.writersOpen = value.at("writersOpen").get<bool>();
            result.writers = parseWriters(value.at("writers"));
            result.payload = value.at("payload").get<std::string>();
            if (!identifier(result.snapshotId) || result.snapshotHash.size() != 16 ||
                result.chunkCount == 0 || result.chunkCount > 1400 ||
                result.chunkIndex >= result.chunkCount || result.payload.size() > 7000) return std::nullopt;
            break;
        case MessageKind::Heartbeat:
            result.sentAtMs = parseCounter(value.at("sentAtMs"));
            break;
        case MessageKind::AssetRequest:
            result.assetId = value.at("assetId").get<std::string>();
            if (!validAssetId(result.assetId)) return std::nullopt;
            break;
        case MessageKind::AssetChunk: {
            result.assetId = value.at("assetId").get<std::string>();
            result.assetMime = value.at("mime").get<std::string>();
            result.assetWidth = value.at("width").get<std::uint32_t>();
            result.assetHeight = value.at("height").get<std::uint32_t>();
            result.assetBytes = value.at("bytes").get<std::uint32_t>();
            result.chunkIndex = value.at("chunkIndex").get<std::uint32_t>();
            result.chunkCount = value.at("chunkCount").get<std::uint32_t>();
            const auto decoded = base64Decode(value.at("payload").get<std::string>());
            if (!validAssetId(result.assetId) || result.assetMime != "image/png" ||
                result.assetWidth == 0 || result.assetHeight == 0 ||
                result.assetWidth > MaxAssetDimension || result.assetHeight > MaxAssetDimension ||
                static_cast<std::uint64_t>(result.assetWidth) * result.assetHeight > MaxAssetPixels ||
                result.assetBytes == 0 || result.assetBytes > MaxAssetBytes ||
                result.chunkCount == 0 ||
                result.chunkCount > (MaxAssetBytes + AssetChunkBytes - 1) / AssetChunkBytes ||
                result.chunkIndex >= result.chunkCount || !decoded) return std::nullopt;
            result.payload = std::move(*decoded);
            break;
        }
        }
        return result;
    } catch (const std::exception &) { return std::nullopt; }
}

std::string contentHash(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[static_cast<std::size_t>(i)] = digits[hash & 0xf];
        hash >>= 4;
    }
    return result;
}

} // namespace livekit::whiteboard
