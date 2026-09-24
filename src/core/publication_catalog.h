#pragma once

#include "participant_event.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace livekit {

struct RemotePublicationInfo {
    TrackKey key;
    TrackTicket ticket;
    std::string name;
    TrackKind kind = TrackKind::Unknown;
    TrackSource source = TrackSource::Unknown;
    bool muted = false;
    TrackPublication::StreamState stream_state = TrackPublication::StreamState::Active;
    bool subscription_allowed = true;
    bool media_available = false;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
};

struct PublicationCatalogParticipant {
    ParticipantKey key;
    ParticipantTicket ticket;
    std::string display_name;
    bool speaking = false;
    float audio_level = 0.0f;
    std::vector<RemotePublicationInfo> publications;
};

struct PublicationCatalogSnapshot {
    uint64_t coordinator_session = 0;
    uint64_t native_room_generation = 0;
    uint64_t catalog_revision = 0;
    std::vector<PublicationCatalogParticipant> participants;
};

enum class CatalogApplyResult {
    Applied,
    NoChange,
    RejectedStale,
    RejectedInvalid,
    RejectedRetired,
};

class PublicationCatalog final {
public:
    explicit PublicationCatalog(uint64_t coordinator_session = 0);

    static bool Consumes(ParticipantEventKind kind);
    CatalogApplyResult Apply(const ParticipantEvent& event);
    void Retire();

    bool accepting() const noexcept { return accepting_; }
    const PublicationCatalogSnapshot& snapshot() const noexcept { return snapshot_; }
    const RemotePublicationInfo* Find(const TrackKey& key) const;

private:
    PublicationCatalogParticipant* FindParticipant(const ParticipantKey& key);
    const PublicationCatalogParticipant* FindParticipant(const ParticipantKey& key) const;
    RemotePublicationInfo* FindPublication(const TrackKey& key);
    static RemotePublicationInfo Project(const PublicationSnapshotEvent& publication,
                                         bool media_available);
    static RemotePublicationInfo Project(const ParticipantEvent& event,
                                         bool media_available);
    bool InstallGeneration(uint64_t native_room_generation);
    void Changed();

    PublicationCatalogSnapshot snapshot_;
    bool accepting_ = true;
};

} // namespace livekit
