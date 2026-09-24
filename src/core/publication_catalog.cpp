#include "publication_catalog.h"

#include <algorithm>

namespace livekit {
namespace {

uint64_t NativeGeneration(const ParticipantEvent& event) {
    if (event.native_room_generation != 0) return event.native_room_generation;
    if (event.participant.key.native_room_generation != 0) {
        return event.participant.key.native_room_generation;
    }
    return event.track_key.participant.native_room_generation;
}

bool SamePublicationState(const RemotePublicationInfo& left,
                          const RemotePublicationInfo& right) {
    return left.key == right.key && left.name == right.name &&
        left.kind == right.kind && left.source == right.source &&
        left.muted == right.muted && left.stream_state == right.stream_state &&
        left.subscription_allowed == right.subscription_allowed &&
        left.media_available == right.media_available &&
        left.source_width == right.source_width &&
        left.source_height == right.source_height;
}

bool SameParticipantState(const PublicationCatalogParticipant& left,
                          const PublicationCatalogParticipant& right) {
    if (left.key != right.key || left.display_name != right.display_name ||
        left.speaking != right.speaking || left.audio_level != right.audio_level ||
        left.publications.size() != right.publications.size()) {
        return false;
    }
    for (std::size_t index = 0; index != left.publications.size(); ++index) {
        if (!SamePublicationState(left.publications[index], right.publications[index])) {
            return false;
        }
    }
    return true;
}

} // namespace

PublicationCatalog::PublicationCatalog(uint64_t coordinator_session) {
    snapshot_.coordinator_session = coordinator_session;
}

bool PublicationCatalog::Consumes(ParticipantEventKind kind) {
    switch (kind) {
    case ParticipantEventKind::Upsert:
    case ParticipantEventKind::Departure:
    case ParticipantEventKind::TrackAvailable:
    case ParticipantEventKind::TrackUnavailable:
    case ParticipantEventKind::TrackMuted:
    case ParticipantEventKind::TrackStreamState:
    case ParticipantEventKind::TrackSubscriptionPermission:
        return true;
    default:
        return false;
    }
}

CatalogApplyResult PublicationCatalog::Apply(const ParticipantEvent& event) {
    if (!accepting_) return CatalogApplyResult::RejectedRetired;
    if (!Consumes(event.kind) || event.participant.is_local) {
        return CatalogApplyResult::NoChange;
    }

    const uint64_t native_generation = NativeGeneration(event);
    if (native_generation == 0) return CatalogApplyResult::RejectedInvalid;
    if (snapshot_.native_room_generation != 0 &&
        native_generation < snapshot_.native_room_generation) {
        return CatalogApplyResult::RejectedStale;
    }
    if (event.kind == ParticipantEventKind::Upsert &&
        !IsParticipantTicketActive(event.participant.ticket, event.participant.key)) {
        return CatalogApplyResult::RejectedStale;
    }
    if (snapshot_.native_room_generation != 0 &&
        native_generation > snapshot_.native_room_generation &&
        event.kind != ParticipantEventKind::Upsert) {
        return CatalogApplyResult::RejectedStale;
    }
    bool changed = InstallGeneration(native_generation);

    if (event.kind == ParticipantEventKind::Departure) {
        const auto size = snapshot_.participants.size();
        snapshot_.participants.erase(std::remove_if(
            snapshot_.participants.begin(), snapshot_.participants.end(),
            [&](const auto& participant) { return participant.key == event.participant.key; }),
            snapshot_.participants.end());
        changed = changed || snapshot_.participants.size() != size;
    } else if (event.kind == ParticipantEventKind::Upsert) {
        PublicationCatalogParticipant projected;
        projected.key = event.participant.key;
        projected.ticket = event.participant.ticket;
        projected.display_name = event.participant.state.name.empty()
            ? event.participant.key.identity : event.participant.state.name;
        projected.speaking = event.participant.state.speaking;
        projected.audio_level = event.participant.state.audio_level;
        projected.publications.reserve(event.participant.publications.size());
        for (const auto& publication : event.participant.publications) {
            if (!IsTrackTicketActive(publication.ticket, publication.key) ||
                publication.key.participant != event.participant.key) {
                continue;
            }
            bool media_available = false;
            if (const auto* current = Find(publication.key)) {
                media_available = current->media_available;
            }
            projected.publications.push_back(Project(publication, media_available));
        }
        auto* existing = FindParticipant(event.participant.key);
        if (!existing) {
            const auto before = snapshot_.participants.size();
            snapshot_.participants.erase(std::remove_if(
                snapshot_.participants.begin(), snapshot_.participants.end(),
                [&](const auto& participant) {
                    if (participant.key == event.participant.key) return false;
                    return participant.key.sid == event.participant.key.sid ||
                        (!event.participant.key.identity.empty() &&
                         participant.key.identity == event.participant.key.identity);
                }), snapshot_.participants.end());
            changed = changed || snapshot_.participants.size() != before;
            snapshot_.participants.push_back(std::move(projected));
            changed = true;
        } else if (!SameParticipantState(*existing, projected)) {
            *existing = std::move(projected);
            changed = true;
        } else {
            *existing = std::move(projected);
        }
    } else {
        auto* participant = FindParticipant(event.participant.key);
        if (!participant) return CatalogApplyResult::RejectedStale;
        auto* publication = FindPublication(event.track_key);
        if (event.kind == ParticipantEventKind::TrackAvailable) {
            if (!IsTrackTicketActive(event.track_ticket, event.track_key)) {
                return CatalogApplyResult::RejectedStale;
            }
            const auto projected = Project(event, true);
            if (!publication) {
                participant->publications.push_back(projected);
                changed = true;
            } else if (!SamePublicationState(*publication, projected)) {
                *publication = projected;
                changed = true;
            } else {
                publication->ticket = event.track_ticket;
            }
        } else if (publication) {
            if (event.kind == ParticipantEventKind::TrackUnavailable) {
                if (publication->media_available) {
                    publication->media_available = false;
                    changed = true;
                }
            } else {
                if (!IsTrackTicketActive(event.track_ticket, event.track_key)) {
                    return CatalogApplyResult::RejectedStale;
                }
                const bool media_available = publication->media_available;
                const auto projected = Project(event, media_available);
                if (!SamePublicationState(*publication, projected)) {
                    *publication = projected;
                    changed = true;
                } else {
                    publication->ticket = event.track_ticket;
                }
            }
        } else {
            return CatalogApplyResult::RejectedStale;
        }
    }

    if (!changed) return CatalogApplyResult::NoChange;
    Changed();
    return CatalogApplyResult::Applied;
}

void PublicationCatalog::Retire() {
    if (!accepting_) return;
    accepting_ = false;
    snapshot_.participants.clear();
    Changed();
}

const RemotePublicationInfo* PublicationCatalog::Find(const TrackKey& key) const {
    const auto* participant = FindParticipant(key.participant);
    if (!participant) return nullptr;
    const auto found = std::find_if(participant->publications.begin(),
        participant->publications.end(),
        [&](const auto& publication) { return publication.key == key; });
    return found == participant->publications.end() ? nullptr : &*found;
}

PublicationCatalogParticipant* PublicationCatalog::FindParticipant(
    const ParticipantKey& key) {
    const auto found = std::find_if(snapshot_.participants.begin(),
        snapshot_.participants.end(),
        [&](const auto& participant) { return participant.key == key; });
    return found == snapshot_.participants.end() ? nullptr : &*found;
}

const PublicationCatalogParticipant* PublicationCatalog::FindParticipant(
    const ParticipantKey& key) const {
    const auto found = std::find_if(snapshot_.participants.begin(),
        snapshot_.participants.end(),
        [&](const auto& participant) { return participant.key == key; });
    return found == snapshot_.participants.end() ? nullptr : &*found;
}

RemotePublicationInfo* PublicationCatalog::FindPublication(const TrackKey& key) {
    auto* participant = FindParticipant(key.participant);
    if (!participant) return nullptr;
    const auto found = std::find_if(participant->publications.begin(),
        participant->publications.end(),
        [&](const auto& publication) { return publication.key == key; });
    return found == participant->publications.end() ? nullptr : &*found;
}

RemotePublicationInfo PublicationCatalog::Project(
    const PublicationSnapshotEvent& publication,
    bool media_available) {
    RemotePublicationInfo result;
    result.key = publication.key;
    result.ticket = publication.ticket;
    result.name = publication.state.name;
    result.kind = publication.state.kind;
    result.source = publication.state.source;
    result.muted = publication.state.muted;
    result.stream_state = publication.state.stream_state;
    result.subscription_allowed = publication.state.subscription_allowed;
    result.media_available = media_available;
    return result;
}

RemotePublicationInfo PublicationCatalog::Project(const ParticipantEvent& event,
                                                  bool media_available) {
    PublicationSnapshotEvent publication;
    publication.key = event.track_key;
    publication.ticket = event.track_ticket;
    publication.state = event.publication;
    return Project(publication, media_available);
}

bool PublicationCatalog::InstallGeneration(uint64_t native_room_generation) {
    if (snapshot_.native_room_generation == native_room_generation) return false;
    snapshot_.native_room_generation = native_room_generation;
    snapshot_.participants.clear();
    return true;
}

void PublicationCatalog::Changed() {
    ++snapshot_.catalog_revision;
    if (snapshot_.catalog_revision == 0) ++snapshot_.catalog_revision;
}

} // namespace livekit
