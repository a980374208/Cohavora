#include "src/core/publication_catalog.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

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

void TestSubscriptionFailureBeforeMediaAndRetry() {
    using Error = livekit::TrackPublication::SubscriptionError;
    livekit::PublicationCatalog catalog(75);
    const auto values = Values(9, 1, 1);
    Require(catalog.Apply(Upsert(values)) == livekit::CatalogApplyResult::Applied,
            "failure catalog setup failed");
    auto failure = TrackEvent(livekit::ParticipantEventKind::TrackSubscriptionError, values);
    failure.publication.subscription_error = Error::CodecUnsupported;
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::Applied,
            "first subscription failure without a media binding was lost");
    auto* publication = catalog.Find(values.track->key);
    Require(publication && publication->subscription_error == Error::CodecUnsupported &&
                publication->subscription_allowed && !publication->media_available,
            "codec error was conflated with permission or media availability");
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::NoChange,
            "duplicate subscription failure advanced the catalog");

    // Metadata refreshes retain the frozen error carried by native snapshots.
    auto roster = Upsert(values);
    roster.participant.publications.front().state.subscription_error = Error::CodecUnsupported;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::NoChange,
            "roster refresh changed a persistent subscription failure");

    failure.publication.subscription_error = Error::TrackNotFound;
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::Applied &&
                catalog.Find(values.track->key)->subscription_error == Error::TrackNotFound,
            "track-not-found subscription error was not distinguished");
    failure.publication.subscription_error = Error::None;
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::Applied,
            "explicit retry did not clear the publication error");
    Require(!catalog.Find(values.track->key)->media_available,
            "retry falsely claimed that remote media was already bound");
    Require(catalog.Apply(TrackEvent(livekit::ParticipantEventKind::TrackAvailable, values)) ==
                livekit::CatalogApplyResult::Applied,
            "successful media after retry was rejected");

    failure.publication.subscription_error = Error::Unknown;
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::Applied &&
                !catalog.Find(values.track->key)->media_available,
            "failure after media availability did not revoke availability");

    values.track->active.store(false, std::memory_order_release);
    const auto successor = Values(9, 1, 2);
    Require(catalog.Apply(Upsert(successor)) == livekit::CatalogApplyResult::Applied,
            "replacement publication was rejected");
    Require(catalog.Apply(failure) == livekit::CatalogApplyResult::RejectedStale &&
                catalog.Find(successor.track->key)->subscription_error == Error::None,
            "late failure polluted the replacement publication lifetime");
}

void TestSubscriptionErrorSnapshotCopy() {
    using Error = livekit::TrackPublication::SubscriptionError;
    livekit::TrackPublication publication(nullptr, "TR_ERROR", "camera");
    publication.set_subscription_error(Error::CodecUnsupported);
    auto frozen = publication.SnapshotState();
    livekit::TrackPublication copied(publication);
    publication.set_subscription_error(Error::TrackNotFound);
    livekit::TrackPublication assigned(nullptr, "TR_OTHER", "other");
    assigned = publication;
    Require(frozen.subscription_error == Error::CodecUnsupported &&
                copied.subscription_error() == Error::CodecUnsupported &&
                assigned.SnapshotState().subscription_error == Error::TrackNotFound,
            "subscription errors were lost or mutated across frozen snapshots/copies");
}

void TestSourceDimensionsSnapshotCopyAndUnknown() {
    livekit::TrackPublication publication(nullptr, "TR_SOURCE", "camera");
    const auto unknown = publication.SnapshotState();
    Require(unknown.source_width == 0 && unknown.source_height == 0,
            "a publication without source metadata claimed known dimensions");

    publication.set_source_dimensions(1920, 1080);
    const auto frozen = publication.SnapshotState();
    livekit::TrackPublication copied(publication);
    publication.set_source_dimensions(3840, 2160);
    livekit::TrackPublication assigned(nullptr, "TR_OTHER", "other");
    assigned = publication;
    Require(frozen.source_width == 1920 && frozen.source_height == 1080 &&
                copied.SnapshotState().source_width == 1920 &&
                copied.SnapshotState().source_height == 1080 &&
                assigned.SnapshotState().source_width == 3840 &&
                assigned.SnapshotState().source_height == 2160,
            "source dimensions were lost or mutated across frozen snapshots/copies");

    for (const auto dimensions : {std::pair<uint32_t, uint32_t>{0, 1080},
                                  std::pair<uint32_t, uint32_t>{1920, 0},
                                  std::pair<uint32_t, uint32_t>{0, 0}}) {
        publication.set_source_dimensions(dimensions.first, dimensions.second);
        const auto state = publication.SnapshotState();
        Require(state.source_width == 0 && state.source_height == 0,
                "partial source dimensions were not normalized to unknown");
    }
}

void TestSourceDimensionsFollowMetadataAndGeneration() {
    livekit::PublicationCatalog catalog(76);
    const auto values = Values(10, 1, 1);
    auto roster = Upsert(values);
    auto& state = roster.participant.publications.front().state;
    state.source_width = 1920;
    state.source_height = 1080;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied,
            "known source dimensions were not admitted from the roster");
    const auto* publication = catalog.Find(values.track->key);
    Require(publication && publication->source_width == 1920 &&
                publication->source_height == 1080 && !publication->media_available,
            "source dimensions were lost or falsely presented as bound media");

    const auto first_revision = catalog.snapshot().catalog_revision;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::NoChange,
            "unchanged source metadata advanced the catalog revision");
    state.source_width = 3840;
    state.source_height = 2160;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied &&
                catalog.snapshot().catalog_revision == first_revision + 1 &&
                catalog.Find(values.track->key)->source_width == 3840 &&
                catalog.Find(values.track->key)->source_height == 2160,
            "source-only metadata changes did not advance the demand inputs");

    state.source_width = state.source_height = 0;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied &&
                catalog.Find(values.track->key)->source_width == 0 &&
                catalog.Find(values.track->key)->source_height == 0,
            "missing refreshed metadata retained obsolete source dimensions");
    state.source_width = 1280;
    state.source_height = 720;
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied,
            "source metadata could not recover after an unknown refresh");

    const auto next_room = Values(11, 1, 1);
    Require(catalog.Apply(Upsert(next_room)) == livekit::CatalogApplyResult::Applied &&
                !catalog.Find(values.track->key) &&
                catalog.Find(next_room.track->key)->source_width == 0 &&
                catalog.Find(next_room.track->key)->source_height == 0,
            "a successor generation inherited the previous source dimensions");
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::RejectedStale &&
                catalog.Find(next_room.track->key)->source_width == 0,
            "late source metadata polluted the successor publication");
}

void TestPublishedVideoLayersSnapshotAndRefresh() {
    using Quality = livekit::PublishedVideoQuality;
    const livekit::PublishedVideoLayer low{Quality::Low, 320, 180, "q"};
    const livekit::PublishedVideoLayer medium{Quality::Medium, 640, 360, "h"};
    const livekit::PublishedVideoLayer high{Quality::High, 2560, 1440, "f"};
    const std::vector<livekit::PublishedVideoLayer> expected{low, medium, high};
    livekit::TrackPublication publication(nullptr, "TR_LAYERS", "camera");
    publication.set_source_video_info(2560, 1440,
        {high, low, medium, low, {Quality::High, 0, 1440, "f"},
         {Quality::Low, 320, 0, "q"},
         {static_cast<Quality>(99), 1280, 720, "unknown"}});
    const auto frozen = publication.SnapshotState();
    livekit::TrackPublication copied(publication);
    Require(frozen.source_width == 2560 && frozen.source_height == 1440 &&
                frozen.published_video_layers == expected &&
                copied.SnapshotState().published_video_layers == expected,
            "layer metadata was invalid, duplicated, or lost across a snapshot/copy");

    // Two-layer screen sources can advertise their highest layer as h/Medium.
    // Do not infer quality or invent an absent RID from the layer's dimensions.
    const std::vector<livekit::PublishedVideoLayer> screen_layers{
        {Quality::Low, 1280, 720, ""}, {Quality::Medium, 2560, 1440, "h"}};
    publication.set_source_video_info(2560, 1440, screen_layers);
    livekit::TrackPublication assigned(nullptr, "TR_OTHER", "other");
    assigned = publication;
    Require(assigned.SnapshotState().published_video_layers == screen_layers &&
                frozen.published_video_layers == expected &&
                copied.SnapshotState().published_video_layers == expected,
            "a refresh mutated frozen layers or reassigned the advertised quality/RID");

    publication.set_source_video_info(0, 1440, screen_layers);
    const auto unknown_source = publication.SnapshotState();
    Require(unknown_source.source_width == 0 && unknown_source.source_height == 0 &&
                unknown_source.published_video_layers == screen_layers,
            "known layer metadata depended on complete original source dimensions");
    publication.set_source_dimensions(1920, 1080);
    Require(publication.SnapshotState().published_video_layers.empty(),
            "a dimension-only refresh mixed fresh source dimensions with stale layers");
    publication.set_source_video_info(2560, 1440, screen_layers);
    publication.set_source_video_info(0, 0, {});
    Require(publication.SnapshotState().published_video_layers.empty(),
            "missing layer metadata retained the previous declaration");
}

void TestPublishedVideoLayersCatalogRevisionAndGeneration() {
    using Quality = livekit::PublishedVideoQuality;
    livekit::PublicationCatalog catalog(77);
    const auto values = Values(12, 1, 1);
    auto roster = Upsert(values);
    auto& layers = roster.participant.publications.front().state.published_video_layers;
    layers = {{Quality::Medium, 640, 360, "h"}, {Quality::Low, 320, 180, "q"}};
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied,
            "layer metadata without media or source dimensions was not admitted");
    const auto* publication = catalog.Find(values.track->key);
    Require(publication && !publication->media_available &&
                publication->published_video_layers.size() == 2 &&
                publication->published_video_layers.front().quality == Quality::Low,
            "catalog layer projection was lost or falsely presented as bound media");

    const auto first_revision = catalog.snapshot().catalog_revision;
    layers = {{Quality::Low, 320, 180, "q"}, {Quality::Medium, 640, 360, "h"},
              {Quality::Low, 320, 180, "q"}, {Quality::High, 0, 720, "f"}};
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::NoChange &&
                catalog.snapshot().catalog_revision == first_revision,
            "reordered, duplicate, or invalid layers advanced the catalog revision");
    layers = {{Quality::Low, 320, 180, "q"}, {Quality::Medium, 1280, 720, "h"}};
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied &&
                catalog.snapshot().catalog_revision == first_revision + 1 &&
                catalog.Find(values.track->key)->published_video_layers.back().height == 720,
            "a layer-only refresh did not advance demand inputs");

    layers.clear();
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied &&
                catalog.Find(values.track->key)->published_video_layers.empty(),
            "missing refreshed layer metadata retained the obsolete declaration");
    layers = {{Quality::High, 2560, 1440, "f"}};
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::Applied,
            "layer metadata could not recover after a missing refresh");

    const auto next_room = Values(13, 1, 1);
    Require(catalog.Apply(Upsert(next_room)) == livekit::CatalogApplyResult::Applied &&
                !catalog.Find(values.track->key) &&
                catalog.Find(next_room.track->key)->published_video_layers.empty(),
            "a successor generation inherited the previous layers");
    Require(catalog.Apply(roster) == livekit::CatalogApplyResult::RejectedStale &&
                catalog.Find(next_room.track->key)->published_video_layers.empty(),
            "late layer metadata polluted the successor publication");
}

} // namespace

int main() {
    TestUnsubscribedPublicationIsDiscoverable();
    TestMediaUnavailablePreservesPublication();
    TestOldKeysCannotAffectSuccessors();
    TestRetiredCatalogRejectsNewDemandInputs();
    TestSubscriptionFailureBeforeMediaAndRetry();
    TestSubscriptionErrorSnapshotCopy();
    TestSourceDimensionsSnapshotCopyAndUnknown();
    TestSourceDimensionsFollowMetadataAndGeneration();
    TestPublishedVideoLayersSnapshotAndRefresh();
    TestPublishedVideoLayersCatalogRevisionAndGeneration();
    std::cout << "publication catalog contract tests passed" << std::endl;
    return 0;
}
