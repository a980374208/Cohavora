#include "src/core/video_demand_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using Policy = livekit::VideoDemandPolicy;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

Policy::TimePoint At(std::chrono::milliseconds elapsed) {
    return Policy::TimePoint{} + elapsed;
}

livekit::ParticipantKey Participant(uint64_t generation, int index) {
    const auto suffix = std::to_string(index);
    return {generation, static_cast<uint64_t>(index + 1),
            "PA_" + suffix, "user-" + suffix};
}

livekit::RemotePublicationInfo Publication(
    const livekit::ParticipantKey& participant,
    uint64_t incarnation,
    std::string sid,
    livekit::TrackKind kind,
    livekit::TrackSource source,
    bool muted = false,
    bool allowed = true) {
    livekit::RemotePublicationInfo result;
    result.key = {participant, incarnation, std::move(sid)};
    result.name = result.key.publication_sid;
    result.kind = kind;
    result.source = source;
    result.muted = muted;
    result.subscription_allowed = allowed;
    return result;
}

livekit::PublicationCatalogSnapshot Catalog(uint64_t session,
                                            uint64_t generation,
                                            int video_count,
                                            int audio_count = 0) {
    livekit::PublicationCatalogSnapshot result;
    result.coordinator_session = session;
    result.native_room_generation = generation;
    result.catalog_revision = 1;
    const int participant_count = std::max(video_count, audio_count);
    for (int index = 0; index != participant_count; ++index) {
        livekit::PublicationCatalogParticipant participant;
        participant.key = Participant(generation, index);
        participant.display_name = participant.key.identity;
        if (index < video_count) {
            participant.publications.push_back(Publication(
                participant.key, 100 + index, "CAM_" + std::to_string(index),
                livekit::TrackKind::Video, livekit::TrackSource::Camera));
        }
        if (index < audio_count) {
            participant.publications.push_back(Publication(
                participant.key, 1000 + index, "MIC_" + std::to_string(index),
                livekit::TrackKind::Audio, livekit::TrackSource::Microphone));
        }
        result.participants.push_back(std::move(participant));
    }
    return result;
}

livekit::ViewportIntent Viewport(uint64_t session,
                                 uint64_t revision,
                                 livekit::VideoLayoutMode mode,
                                 uint32_t page = 0,
                                 uint32_t page_size = 9) {
    livekit::ViewportIntent result;
    result.coordinator_session = session;
    result.view_revision = revision;
    result.mode = mode;
    result.page = page;
    result.page_size = page_size;
    result.stage_rect = {0, 0, 1920, 1080};
    return result;
}

const livekit::TrackKey& CameraKey(
    const livekit::PublicationCatalogSnapshot& catalog,
    std::size_t participant) {
    return catalog.participants.at(participant).publications.front().key;
}

bool HasKey(const std::vector<livekit::TrackKey>& keys,
            const livekit::TrackKey& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

livekit::ActiveSpeakerInfo Speaker(
    const livekit::PublicationCatalogSnapshot& catalog,
    std::size_t participant) {
    livekit::ActiveSpeakerInfo result;
    result.key = catalog.participants.at(participant).key;
    result.sid = result.key.sid;
    result.identity = result.key.identity;
    result.speaking = true;
    return result;
}

void TestBoundedGridPaginationAndAudioIndependence() {
    constexpr uint64_t session = 81;
    for (const uint32_t page_size : {4u, 9u, 16u}) {
        Policy policy(session);
        auto catalog = Catalog(session, 11, 100, 100);
        Require(policy.UpdateCatalog(catalog), "100-person catalog was rejected");
        Require(policy.UpdateViewport(
                    Viewport(session, 1, livekit::VideoLayoutMode::Grid,
                             0, page_size), At(0ms)),
                "grid viewport was rejected");
        const auto& plan = policy.Reconcile(At(0ms));
        Require(plan.visible_seats.size() == page_size,
                "grid did not honor the requested page size");
        Require(plan.selected_video.size() == page_size &&
                    plan.selected_video.size() <= 16,
                "grid exceeded or underfilled the finite video budget");
        Require(plan.selected_audio.size() == 100,
                "video pagination incorrectly limited remote audio");
        Require(plan.page_count == (100 + page_size - 1) / page_size,
                "grid page count was incorrect");
        for (const auto& seat : plan.visible_seats) {
            Require(seat.quality == livekit::VideoQualityTier::P180 ||
                        seat.quality == livekit::VideoQualityTier::P360,
                    "grid requested quality above 360p");
        }

        auto last_page = Viewport(session, 2, livekit::VideoLayoutMode::Grid,
                                  999, page_size);
        policy.UpdateViewport(last_page, At(1ms));
        const auto& clamped = policy.Reconcile(At(1ms));
        Require(clamped.page + 1 == clamped.page_count,
                "out-of-range grid page was not clamped");
        Require(clamped.selected_video.size() <= page_size,
                "last page exceeded its page size");
    }

    for (const int count : {0, 1, 16, 17}) {
        Policy policy(session + count + 1);
        const auto catalog = Catalog(session + count + 1, 12, count);
        policy.UpdateCatalog(catalog);
        policy.UpdateViewport(Viewport(session + count + 1, 1,
                                      livekit::VideoLayoutMode::Grid, 0, 16),
                              At(0ms));
        const auto& plan = policy.Reconcile(At(0ms));
        Require(plan.selected_video.size() ==
                    static_cast<std::size_t>(std::min(count, 16)),
                "0/1/16/17 grid boundary selected the wrong number of tracks");
        Require(plan.selected_video.size() <= 16,
                "video budget exceeded 16 tracks");
    }

    Policy auto_policy(session + 200);
    const auto auto_catalog = Catalog(session + 200, 13, 2);
    auto_policy.UpdateCatalog(auto_catalog);
    auto auto_view = Viewport(
        session + 200, 1, livekit::VideoLayoutMode::Auto);
    auto_view.stage_rect = {0, 0, 320, 180};
    auto_policy.UpdateViewport(auto_view, At(0ms));
    const auto& dual = auto_policy.Reconcile(At(0ms));
    Require(dual.mode == livekit::VideoLayoutMode::Auto &&
                dual.visible_seats.size() == 2,
            "Auto mode did not use its two-candidate layout");
    for (const auto& seat : dual.visible_seats) {
        Require(seat.width <= 160 && seat.height <= 180 &&
                    seat.quality == livekit::VideoQualityTier::P180,
                "narrow dual layout requested dimensions outside its seat");
    }
}

void TestStablePageAnchorAndSourceOrder() {
    constexpr uint64_t session = 82;
    Policy policy(session);
    auto catalog = Catalog(session, 20, 17);
    policy.UpdateCatalog(catalog);
    policy.UpdateViewport(
        Viewport(session, 1, livekit::VideoLayoutMode::Grid, 1, 9), At(0ms));
    const auto first_key = policy.Reconcile(At(0ms)).visible_seats.front().key;
    Require(first_key == CameraKey(catalog, 9),
            "second grid page did not start at the stable ninth offset");

    catalog.participants.erase(catalog.participants.begin());
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto& anchored = policy.Reconcile(At(1ms));
    Require(!anchored.visible_seats.empty() &&
                anchored.visible_seats.front().key == first_key,
            "departure before the current page moved its page anchor");

    auto explicit_anchor = Viewport(
        session, 2, livekit::VideoLayoutMode::Grid, 0, 9);
    explicit_anchor.page_anchor = CameraKey(catalog, 5);
    policy.UpdateViewport(explicit_anchor, At(2ms));
    Require(policy.Reconcile(At(2ms)).visible_seats.front().key ==
                *explicit_anchor.page_anchor,
            "explicit page anchor was overwritten by page initialization");

    Policy source_policy(session + 1);
    auto mixed = Catalog(session + 1, 21, 0);
    livekit::PublicationCatalogParticipant participant;
    participant.key = Participant(21, 0);
    const auto screen = Publication(
        participant.key, 2, "SCREEN", livekit::TrackKind::Video,
        livekit::TrackSource::ScreenShareVideo);
    const auto camera = Publication(
        participant.key, 1, "CAMERA", livekit::TrackKind::Video,
        livekit::TrackSource::Camera);
    participant.publications = {screen, camera};
    mixed.participants.push_back(participant);
    source_policy.UpdateCatalog(mixed);
    source_policy.UpdateViewport(
        Viewport(session + 1, 1, livekit::VideoLayoutMode::Grid, 0, 4),
        At(0ms));
    const auto& plan = source_policy.Reconcile(At(0ms));
    Require(plan.visible_seats.size() == 2 &&
                plan.visible_seats[0].key == camera.key &&
                plan.visible_seats[1].key == screen.key,
            "camera and screen were not kept as distinct stable source entries");
}

void TestSharePriorityAndExplicitSelection() {
    constexpr uint64_t session = 83;
    Policy policy(session);
    auto catalog = Catalog(session, 30, 2);
    auto share_owner = Participant(30, 20);
    livekit::PublicationCatalogParticipant share_participant;
    share_participant.key = share_owner;
    auto share_one = Publication(share_owner, 1, "SHARE_ONE",
        livekit::TrackKind::Video, livekit::TrackSource::ScreenShareVideo);
    share_participant.publications.push_back(share_one);
    catalog.participants.push_back(share_participant);
    policy.UpdateCatalog(catalog);
    policy.UpdateViewport(
        Viewport(session, 1, livekit::VideoLayoutMode::Auto), At(0ms));
    const auto& initial = policy.Reconcile(At(0ms));
    Require(initial.focused && *initial.focused == share_one.key &&
                initial.reason == livekit::VideoDemandReason::ScreenShare,
            "active sharing did not take automatic focus priority");

    livekit::PublicationCatalogParticipant newer_share_participant;
    newer_share_participant.key = Participant(30, 19);
    auto share_two = Publication(newer_share_participant.key, 1, "SHARE_TWO",
        livekit::TrackKind::Video, livekit::TrackSource::ScreenShareVideo);
    newer_share_participant.publications.push_back(share_two);
    catalog.participants.insert(catalog.participants.begin(),
                                newer_share_participant);
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto& retained = policy.Reconcile(At(1ms));
    Require(retained.focused && *retained.focused == share_one.key,
            "a newly observed share stole the current automatic share focus");

    auto explicit_share = Viewport(session, 2, livekit::VideoLayoutMode::Auto);
    explicit_share.selected_share = share_two.key;
    policy.UpdateViewport(explicit_share, At(2ms));
    const auto& switched = policy.Reconcile(At(2ms));
    Require(switched.focused && *switched.focused == share_two.key,
            "explicit share selection did not switch focus immediately");
    Require(switched.visible_seats.front().priority >
                switched.visible_seats.back().priority,
            "focused share did not receive higher priority than thumbnails");
}

void TestPinPlaceholderAndRestore() {
    constexpr uint64_t session = 84;
    Policy policy(session);
    auto catalog = Catalog(session, 40, 12);
    catalog.participants[10].publications[0].muted = true;
    const auto pinned_key = CameraKey(catalog, 10);
    policy.UpdateCatalog(catalog);
    auto grid = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 9);
    policy.UpdateViewport(grid, At(0ms));
    const auto original_first = policy.Reconcile(At(0ms)).visible_seats.front().key;

    grid.view_revision = 2;
    grid.pinned = pinned_key;
    policy.UpdateViewport(grid, At(100ms));
    const auto& pinned = policy.Reconcile(At(100ms));
    Require(pinned.mode == livekit::VideoLayoutMode::Speaker &&
                pinned.focused && *pinned.focused == pinned_key,
            "Pin did not enter a focused layout immediately");
    Require(!pinned.visible_seats.empty() &&
                pinned.visible_seats.front().reason ==
                    livekit::VideoDemandReason::Muted &&
                !HasKey(pinned.selected_video, pinned_key),
            "muted Pin did not remain as a non-subscribed placeholder");

    grid.view_revision = 3;
    grid.pinned.reset();
    policy.UpdateViewport(grid, At(200ms));
    const auto& restored = policy.Reconcile(At(200ms));
    Require(restored.mode == livekit::VideoLayoutMode::Grid &&
                restored.page == 0 &&
                restored.visible_seats.front().key == original_first,
            "unpin did not restore the previous grid page");

    grid.view_revision = 4;
    grid.pinned = CameraKey(catalog, 11);
    policy.UpdateViewport(grid, At(300ms));
    policy.Reconcile(At(300ms));
    catalog.participants.pop_back();
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto& removed = policy.Reconcile(At(301ms));
    Require(!removed.focused && removed.mode == livekit::VideoLayoutMode::Grid,
            "deleted pinned publication remained focused");
}

void TestSpeakerHysteresisAndFocusedLayouts() {
    constexpr uint64_t session = 85;
    Policy policy(session);
    const auto catalog = Catalog(session, 50, 8);
    policy.UpdateCatalog(catalog);
    policy.UpdateViewport(
        Viewport(session, 1, livekit::VideoLayoutMode::Speaker), At(0ms));
    const auto first = CameraKey(catalog, 0);
    const auto second = CameraKey(catalog, 1);
    const auto third = CameraKey(catalog, 2);
    Require(policy.Reconcile(At(0ms)).focused == first,
            "speaker layout did not choose a stable fallback");

    policy.UpdateSpeakers({Speaker(catalog, 1)}, At(0ms));
    Require(policy.NextReconcileAt(At(0ms)) == At(1000ms),
            "speaker candidate did not expose its reconcile deadline");
    const auto& before_delay = policy.Reconcile(At(999ms));
    Require(!before_delay.stable_speaker && before_delay.focused == first,
            "speaker changed before the one-second candidate delay");
    const auto& candidate = policy.Reconcile(At(1000ms));
    Require(candidate.stable_speaker == second && candidate.focused == first,
            "speaker candidate or three-second focus residence was ignored");
    Require(policy.NextReconcileAt(At(1000ms)) == At(3000ms),
            "focus residence did not expose its reconcile deadline");
    Require(policy.Reconcile(At(2999ms)).focused == first,
            "focus changed before its minimum residence elapsed");
    Require(policy.Reconcile(At(3000ms)).focused == second,
            "stable speaker did not take focus after minimum residence");
    Require(!policy.NextReconcileAt(At(3000ms)),
            "settled speaker retained a redundant reconcile deadline");

    policy.UpdateSpeakers({Speaker(catalog, 2)}, At(3000ms));
    Require(policy.Reconcile(At(4000ms)).stable_speaker == third &&
                policy.plan().focused == second,
            "new stable speaker bypassed focus residence");
    Require(policy.Reconcile(At(6000ms)).focused == third,
            "speaker focus did not switch at the residence boundary");
    Require(policy.plan().visible_seats.size() == 5,
            "speaker layout did not enforce one main plus four side seats");
    Require(std::count(policy.plan().selected_video.begin(),
                       policy.plan().selected_video.end(), third) == 1,
            "speaker main track was duplicated in the sidebar selection");

    auto pin = Viewport(session, 2, livekit::VideoLayoutMode::Speaker);
    pin.pinned = first;
    policy.UpdateViewport(pin, At(6100ms));
    Require(policy.Reconcile(At(6100ms)).focused == first,
            "Pin did not bypass speaker focus residence");

    Policy pip_policy(session + 1);
    auto pip_catalog = Catalog(session + 1, 51, 8);
    pip_policy.UpdateCatalog(pip_catalog);
    pip_policy.UpdateViewport(
        Viewport(session + 1, 1,
                 livekit::VideoLayoutMode::PictureInPicture), At(0ms));
    const auto& pip = pip_policy.Reconcile(At(0ms));
    Require(pip.visible_seats.size() == 2 &&
                pip.visible_seats[0].role == livekit::VideoSeatRole::Main &&
                pip.visible_seats[1].role ==
                    livekit::VideoSeatRole::PictureInPicture,
            "PiP layout did not enforce one main plus one floating seat");
}

void TestGridSpeakerDoesNotMovePage() {
    constexpr uint64_t session = 86;
    Policy policy(session);
    const auto catalog = Catalog(session, 60, 20);
    policy.UpdateCatalog(catalog);
    policy.UpdateViewport(
        Viewport(session, 1, livekit::VideoLayoutMode::Grid, 1, 9), At(0ms));
    const auto before = policy.Reconcile(At(0ms)).selected_video;
    policy.UpdateSpeakers({Speaker(catalog, 0)}, At(0ms));
    const auto& after = policy.Reconcile(At(4000ms));
    Require(after.selected_video == before && after.page == 1 && !after.focused,
            "active speaker reordered or moved an explicit grid page");
    Require(after.stable_speaker == CameraKey(catalog, 0),
            "grid plan did not project the stable speaker state");
}

void TestVisibilityPermissionAndIdempotence() {
    constexpr uint64_t session = 87;
    Policy policy(session);
    auto catalog = Catalog(session, 70, 20, 20);
    catalog.participants[1].publications[0].subscription_allowed = false;
    policy.UpdateCatalog(catalog);
    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 4);
    policy.UpdateViewport(view, At(0ms));
    const auto& visible = policy.Reconcile(At(0ms));
    Require(visible.visible_seats.size() == 4 &&
                visible.selected_video.size() == 3 &&
                visible.visible_seats[1].reason ==
                    livekit::VideoDemandReason::PermissionDenied,
            "permission-denied publication consumed video budget");
    Require(visible.selected_audio.size() == 20,
            "allowed audio was not selected independently");
    const auto revision = visible.policy_revision;
    Require(policy.Reconcile(At(10ms)).policy_revision == revision,
            "identical reconciliation advanced policy revision");
    view.view_revision = 2;
    policy.UpdateViewport(view, At(20ms));
    Require(policy.Reconcile(At(20ms)).policy_revision == revision,
            "semantic duplicate viewport advanced policy revision");

    view.view_revision = 3;
    view.window_visible = false;
    policy.UpdateViewport(view, At(30ms));
    const auto& hidden = policy.Reconcile(At(30ms));
    Require(hidden.selected_video.empty() && hidden.visible_seats.empty() &&
                hidden.selected_audio.size() == 20 &&
                hidden.reason == livekit::VideoDemandReason::Hidden,
            "hidden window retained video or dropped audio demand");

    view.view_revision = 4;
    view.window_visible = true;
    view.stage_content = livekit::StageContent::Whiteboard;
    policy.UpdateViewport(view, At(40ms));
    const auto& whiteboard = policy.Reconcile(At(40ms));
    Require(whiteboard.selected_video.empty() &&
                whiteboard.selected_audio.size() == 20 &&
                whiteboard.reason == livekit::VideoDemandReason::Whiteboard,
            "whiteboard stage retained video or dropped audio demand");
}

void TestGenerationReplacementAndQualityCaps() {
    constexpr uint64_t session = 88;
    Policy policy(session);
    auto old_catalog = Catalog(session, 80, 3);
    const auto old_key = CameraKey(old_catalog, 0);
    policy.UpdateCatalog(old_catalog);
    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 4);
    view.pinned = old_key;
    policy.UpdateViewport(view, At(0ms));
    policy.Reconcile(At(0ms));

    auto new_catalog = Catalog(session, 81, 3);
    new_catalog.catalog_revision = old_catalog.catalog_revision + 1;
    policy.UpdateCatalog(new_catalog);
    const auto& replaced = policy.Reconcile(At(1ms));
    Require(!HasKey(replaced.selected_video, old_key) &&
                replaced.mode == livekit::VideoLayoutMode::Grid,
            "old-generation key survived catalog replacement");
    for (const auto& key : replaced.selected_video) {
        Require(key.participant.native_room_generation == 81,
                "plan mixed native room generations");
    }

    Policy share_policy(session + 1);
    auto share_catalog = Catalog(session + 1, 82, 1);
    auto share = Publication(share_catalog.participants[0].key, 999, "SCREEN",
        livekit::TrackKind::Video, livekit::TrackSource::ScreenShareVideo);
    share_catalog.participants[0].publications.push_back(share);
    share_policy.UpdateCatalog(share_catalog);
    auto share_view = Viewport(session + 1, 1, livekit::VideoLayoutMode::Auto);
    share_view.selected_share = share.key;
    share_policy.UpdateViewport(share_view, At(0ms));
    const auto& focused_share = share_policy.Reconcile(At(0ms));
    Require(focused_share.visible_seats.front().quality ==
                livekit::VideoQualityTier::P1080 &&
                focused_share.visible_seats.front().width <= 1920 &&
                focused_share.visible_seats.front().height <= 1080,
            "main screen share did not use the bounded 1080p tier");

    share_view.view_revision = 2;
    share_view.selected_share.reset();
    share_view.pinned = CameraKey(share_catalog, 0);
    share_policy.UpdateViewport(share_view, At(1ms));
    const auto& pinned_camera = share_policy.Reconcile(At(1ms));
    Require(pinned_camera.visible_seats.front().priority == 500 &&
                pinned_camera.visible_seats.front().quality ==
                    livekit::VideoQualityTier::P720,
            "pinned main camera did not receive top priority with a 720p cap");

    share_policy.Retire();
    Require(!share_policy.accepting() &&
                share_policy.plan().selected_video.empty() &&
                share_policy.plan().selected_audio.empty(),
            "retired policy retained active media demand");
}

} // namespace

int main() {
    TestBoundedGridPaginationAndAudioIndependence();
    TestStablePageAnchorAndSourceOrder();
    TestSharePriorityAndExplicitSelection();
    TestPinPlaceholderAndRestore();
    TestSpeakerHysteresisAndFocusedLayouts();
    TestGridSpeakerDoesNotMovePage();
    TestVisibilityPermissionAndIdempotence();
    TestGenerationReplacementAndQualityCaps();
    std::cout << "video demand policy contract tests passed" << std::endl;
    return 0;
}
