#include "src/core/publication_catalog.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

struct EventValues {
    std::shared_ptr<livekit::MembershipState> participant;
    std::shared_ptr<livekit::TrackMembershipState> track;
};

EventValues Values(uint64_t generation,
                   uint64_t participant_incarnation,
                   uint64_t publication_incarnation,
                   const char* publication_sid = "TR_CAMERA") {
    livekit::ParticipantKey participant_key{
        generation, participant_incarnation, "PA_REMOTE", "remote-user"};
    EventValues values;
    values.participant = std::make_shared<livekit::MembershipState>(participant_key);
    values.track = std::make_shared<livekit::TrackMembershipState>(livekit::TrackKey{
        participant_key, publication_incarnation, publication_sid});
    return values;
}

livekit::TrackPublication::StateSnapshot PublicationState(
    const char* sid,
    livekit::TrackSource source = livekit::TrackSource::Camera) {
    livekit::TrackPublication::StateSnapshot state;
    state.sid = sid;
    state.name = source == livekit::TrackSource::ScreenShareVideo ? "screen" : "camera";
    state.kind = livekit::TrackKind::Video;
    state.source = source;
    state.subscription_allowed = true;
    return state;
}

livekit::ParticipantEvent Upsert(const EventValues& values,
                                 bool include_publication = true) {
    livekit::ParticipantEvent event;
    event.kind = livekit::ParticipantEventKind::Upsert;
    event.native_room_generation = values.participant->key.native_room_generation;
    event.participant.key = values.participant->key;
    event.participant.ticket = values.participant;
    event.participant.state.sid = values.participant->key.sid;
    event.participant.state.identity = values.participant->key.identity;
    event.participant.state.name = "Remote User";
    if (include_publication) {
        event.participant.publications.push_back({
            values.track->key,
            values.track,
            PublicationState(values.track->key.publication_sid.c_str(),
                             livekit::TrackSource::ScreenShareVideo)});
    }
    return event;
}

livekit::ParticipantEvent TrackEvent(livekit::ParticipantEventKind kind,
                                     const EventValues& values) {
    auto event = Upsert(values, false);
    event.kind = kind;
    event.track_key = values.track->key;
    event.track_ticket = values.track;
    event.publication = PublicationState(values.track->key.publication_sid.c_str(),
                                         livekit::TrackSource::ScreenShareVideo);
    return event;
}

void TestUnsubscribedPublicationIsDiscoverable() {
    livekit::PublicationCatalog catalog(71);
    const auto values = Values(4, 10, 20, "TR_SCREEN");
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::Applied,
            "full roster did not populate the catalog");
    const auto* publication = catalog.Find(values.track->key);
    Require(publication, "unsubscribed publication was not discoverable");
    Require(publication->kind == livekit::TrackKind::Video &&
                publication->source == livekit::TrackSource::ScreenShareVideo,
            "publication kind/source projection was lost");
    Require(!publication->media_available,
            "metadata-only publication was presented as bound media");
    Require(catalog.snapshot().coordinator_session == 71 &&
                catalog.snapshot().native_room_generation == 4 &&
                catalog.snapshot().catalog_revision == 1,
            "catalog version tuple was not initialized");
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::NoChange &&
                catalog.snapshot().catalog_revision == 1,
            "idempotent roster advanced the catalog revision");
}

void TestMediaUnavailablePreservesPublication() {
    livekit::PublicationCatalog catalog(72);
    const auto values = Values(5, 11, 21);
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::Applied,
            "catalog setup failed");
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackAvailable, values)) ==
                livekit::CatalogApplyResult::Applied,
            "media binding was not marked available");
    Require(catalog.Find(values.track->key)->media_available,
            "available media was not observable");
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackUnavailable, values)) ==
                livekit::CatalogApplyResult::Applied,
            "media unavailability was not applied");
    const auto* publication = catalog.Find(values.track->key);
    Require(publication && !publication->media_available,
            "TrackUnavailable deleted the publication instead of its binding");
    Require(catalog.Apply(Upsert(values, false)) == livekit::CatalogApplyResult::Applied &&
                !catalog.Find(values.track->key),
            "full roster removal did not delete an unpublished track");
}

void TestOldKeysCannotAffectSuccessors() {
    livekit::PublicationCatalog catalog(73);
    auto old_values = Values(6, 12, 22);
    Require(catalog.Apply(Upsert(old_values)) == livekit::CatalogApplyResult::Applied,
            "old catalog setup failed");
    old_values.track->active.store(false, std::memory_order_release);

    const auto successor = Values(6, 13, 23);
    Require(catalog.Apply(Upsert(successor)) == livekit::CatalogApplyResult::Applied,
            "successor publication was not installed");
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackAvailable, successor)) ==
                livekit::CatalogApplyResult::Applied,
            "successor media did not become available");
    const auto revision = catalog.snapshot().catalog_revision;
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackUnavailable, old_values)) ==
                livekit::CatalogApplyResult::RejectedStale,
            "retired key was accepted");
    Require(catalog.snapshot().catalog_revision == revision &&
                catalog.Find(successor.track->key) &&
                catalog.Find(successor.track->key)->media_available,
            "retired key changed the successor publication");

    auto next_room = Values(7, 1, 1);
    Require(catalog.Apply(Upsert(next_room)) == livekit::CatalogApplyResult::Applied,
            "new native generation did not replace the catalog");
    Require(!catalog.Find(successor.track->key),
            "old room generation survived catalog replacement");
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackAvailable, successor)) ==
                livekit::CatalogApplyResult::RejectedStale,
            "old room generation event was accepted");
}

void TestRetiredCatalogRejectsNewDemandInputs() {
    livekit::PublicationCatalog catalog(74);
    const auto values = Values(8, 1, 1);
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::Applied,
            "catalog setup failed");
    catalog.Retire();
    Require(!catalog.accepting() && catalog.snapshot().participants.empty(),
            "retired catalog retained active state");
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::RejectedRetired,
            "retired catalog accepted new input");
}

} // namespace

int main() {
    TestUnsubscribedPublicationIsDiscoverable();
    TestMediaUnavailablePreservesPublication();
    TestOldKeysCannotAffectSuccessors();
    TestRetiredCatalogRejectsNewDemandInputs();
    std::cout << "publication catalog contract tests passed" << std::endl;
    return 0;
}
