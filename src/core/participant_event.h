#pragma once

#include "participant.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace livekit {

class TextStreamReader;
class ByteStreamReader;

struct ParticipantKey {
    uint64_t native_room_generation = 0;
    uint64_t incarnation = 0;
    std::string sid;
    std::string identity;

    bool operator==(const ParticipantKey& other) const {
        return native_room_generation == other.native_room_generation &&
            incarnation == other.incarnation && sid == other.sid && identity == other.identity;
    }
    bool operator!=(const ParticipantKey& other) const { return !(*this == other); }
};

struct MembershipState {
    explicit MembershipState(ParticipantKey value) : key(std::move(value)) {}
    const ParticipantKey key;
    std::atomic<bool> active{true};
};

using ParticipantTicket = std::weak_ptr<const MembershipState>;

struct TrackKey {
    ParticipantKey participant;
    uint64_t publication_incarnation = 0;
    std::string publication_sid;

    bool operator==(const TrackKey& other) const {
        return participant == other.participant &&
            publication_incarnation == other.publication_incarnation &&
            publication_sid == other.publication_sid;
    }
    bool operator!=(const TrackKey& other) const { return !(*this == other); }
};

struct TrackMembershipState {
    explicit TrackMembershipState(TrackKey value) : key(std::move(value)) {}
    const TrackKey key;
    std::atomic<bool> active{true};
};

using TrackTicket = std::weak_ptr<const TrackMembershipState>;

inline bool IsParticipantTicketActive(const ParticipantTicket& ticket,
                                      const ParticipantKey& expected) {
    const auto state = ticket.lock();
    return state && state->key == expected && state->active.load(std::memory_order_acquire);
}

inline bool IsTrackTicketActive(const TrackTicket& ticket, const TrackKey& expected) {
    const auto state = ticket.lock();
    return state && state->key == expected && state->active.load(std::memory_order_acquire);
}

enum class ParticipantEventKind {
    Upsert,
    Departure,
    TrackAvailable,
    TrackUnavailable,
    TrackMuted,
    TrackStreamState,
    TrackSubscriptionPermission,
    ConnectionQuality,
    ActiveSpeakers,
    DataReceived,
    TextStreamOpened,
    ByteStreamOpened,
};

struct ParticipantSnapshotEvent {
    ParticipantKey key;
    ParticipantTicket ticket;
    ParticipantStateSnapshot state;
    bool is_local = false;
};

struct ActiveSpeakerInfo {
    ParticipantKey key;
    ParticipantTicket ticket;
    std::string sid;
    std::string identity;
    bool is_local = false;
    bool speaking = false;
    float audio_level = 0.0f;
};

enum class SenderOrigin {
    Server,
    Remote,
    Local,
    Unresolved,
};

struct SenderContext {
    SenderOrigin origin = SenderOrigin::Unresolved;
    std::string transport_sid;
    std::string transport_identity;
    ParticipantKey key;
    ParticipantTicket ticket;
    std::string display_name;
};

struct ParticipantEvent {
    ParticipantEventKind kind = ParticipantEventKind::Upsert;
    uint64_t native_room_generation = 0;
    uint64_t event_sequence = 0;
    ParticipantSnapshotEvent participant;
    TrackKey track_key;
    TrackTicket track_ticket;
    TrackPublication::StateSnapshot publication;
    std::vector<ActiveSpeakerInfo> speakers;
    SenderContext sender;
    std::vector<uint8_t> data;
    std::string topic;
    std::shared_ptr<TextStreamReader> text_reader;
    std::shared_ptr<ByteStreamReader> byte_reader;
};

} // namespace livekit
