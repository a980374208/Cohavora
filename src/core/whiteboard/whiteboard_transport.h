#pragma once

#include "whiteboard_protocol.h"
#include "src/core/room.h"

#include <memory>
#include <string_view>
#include <vector>

namespace livekit::whiteboard {

// Dedicated bounded adapter for whiteboard packets. It intentionally bypasses
// chat framing and never performs Room::PublishData's local loopback.
class RoomTransport final {
public:
    explicit RoomTransport(std::weak_ptr<Room> room) : room_(std::move(room)) {}
    bool send(std::string_view topic, std::string_view payload,
              const std::vector<std::string> &destinationIdentities) const {
        if (!isWhiteboardTopic(topic) || payload.empty() || payload.size() > MaxWireBytes) return false;
        const auto room = room_.lock();
        const auto highWater = topic == AssetTopic ? 192 * 1024 : 256 * 1024;
        if (!room || room->GetDataChannelBufferedAmount(true) > highWater) return false;
        proto::DataPacket packet;
        auto *user = packet.mutable_user();
        user->set_topic(topic.data(), topic.size());
        user->set_payload(payload.data(), payload.size());
        for (const auto &identity : destinationIdentities) {
            if (!identity.empty()) user->add_destination_identities(identity);
        }
        return room->PublishDataPacket(packet, true);
    }

private:
    std::weak_ptr<Room> room_;
};

} // namespace livekit::whiteboard
