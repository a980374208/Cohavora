#include "src/core/video_demand_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
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
            Require(seat.quality == (page_size == 4
                        ? livekit::VideoQualityTier::P720
                        : livekit::VideoQualityTier::P360),
                    "grid quality did not follow its physical tile size");
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

void TestGridCapacityIncludesLocalSeats() {
    constexpr uint64_t session = 501;
    for (const uint32_t page_size : {4u, 9u, 16u}) {
        for (const bool local_participant : {false, true}) {
            for (const bool local_share : {false, true}) {
                const auto local_count = static_cast<uint32_t>(local_participant) +
                    static_cast<uint32_t>(local_share);
                const int capacity = static_cast<int>(page_size);
                for (const int remote_count : {0, 1, capacity - 2, capacity - 1,
                                               capacity, capacity + 1,
                                               2 * capacity - 2, 2 * capacity + 1}) {
                    Policy policy(session);
                    const auto catalog = Catalog(session, 201, remote_count, remote_count);
                    policy.UpdateCatalog(catalog);
                    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid,
                                         0, page_size);
                    view.local_participant_present = local_participant;
                    view.local_screen_share_present = local_share;
                    policy.UpdateViewport(view, At(0ms));
                    const auto total = static_cast<uint32_t>(remote_count) + local_count;
                    const auto expected_pages = std::max<uint32_t>(
                        1, (total + page_size - 1) / page_size);
                    Require(policy.Reconcile(At(0ms)).page_count == expected_pages,
                            "grid page count omitted local participant/share seats");
                    std::vector<livekit::TrackKey> visited;
                    for (uint32_t page = 0; page < expected_pages; ++page) {
                        view.page = page;
                        view.view_revision = page + 2;
                        policy.UpdateViewport(view, At(1ms));
                        const auto& plan = policy.Reconcile(At(1ms));
                        Require(plan.page == page && plan.page_size == page_size &&
                                    plan.page_count == expected_pages,
                                "grid changed page or capacity during full traversal");
                        Require(plan.show_local_participant == (page == 0 && local_participant) &&
                                    plan.show_local_screen_share == (page == 0 && local_share),
                                "local seats were missing or repeated on later grid pages");
                        const auto local_visible = static_cast<uint32_t>(plan.show_local_participant) +
                            static_cast<uint32_t>(plan.show_local_screen_share);
                        const auto expected_visible = std::min(page_size,
                            total - std::min(total, page * page_size));
                        Require(plan.visible_seats.size() + local_visible == expected_visible &&
                                    plan.selected_video.size() == plan.visible_seats.size(),
                                "grid exceeded total capacity or selected invisible remote tracks");
                        Require(plan.selected_audio.size() == static_cast<std::size_t>(remote_count),
                                "local seats or video pages changed remote audio demand");
                        for (const auto& seat : plan.visible_seats) visited.push_back(seat.key);
                    }
                    Require(visited.size() == catalog.participants.size(),
                            "full grid traversal omitted or repeated a remote seat");
                    for (std::size_t index = 0; index < visited.size(); ++index) {
                        Require(visited[index] == CameraKey(catalog, index),
                                "local seat reservation changed remote order or page continuity");
                    }
                }
            }
        }
    }
}

void TestLocalGridShareTransitionsAndClamping() {
    constexpr uint64_t session = 502;
    Policy policy(session);
    const auto catalog = Catalog(session, 202, 7);
    policy.UpdateCatalog(catalog);
    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 4);
    view.local_participant_present = true;
    policy.UpdateViewport(view, At(0ms));
    const auto before_share = policy.Reconcile(At(0ms));
    Require(before_share.visible_seats.size() == 3 && before_share.page_count == 2,
            "local participant did not reserve the first grid seat");
    view.local_screen_share_present = true;
    Require(!policy.UpdateViewport(view, At(1ms)),
            "conflicting local-share flags with the same revision were accepted");
    ++view.view_revision;
    Require(policy.UpdateViewport(view, At(1ms)),
            "local-share-only viewport update was treated as a duplicate");
    const auto sharing = policy.Reconcile(At(1ms));
    Require(sharing.policy_revision > before_share.policy_revision &&
                sharing.show_local_screen_share && sharing.visible_seats.size() == 2 &&
                sharing.page_count == 3,
            "starting a local share did not reserve a seat or update page count");

    view.page = 1;
    ++view.view_revision;
    policy.UpdateViewport(view, At(2ms));
    const auto second = policy.Reconcile(At(2ms));
    Require(second.visible_seats.size() == 4 &&
                second.visible_seats.front().key == CameraKey(catalog, 2) &&
                !second.show_local_participant && !second.show_local_screen_share,
            "later page repeated local seats or skipped displaced remote seats");
    view.page_anchor = second.visible_seats.front().key;
    view.local_screen_share_present = false;
    ++view.view_revision;
    policy.UpdateViewport(view, At(3ms));
    const auto stopped = policy.Reconcile(At(3ms));
    Require(stopped.visible_seats.front().key == CameraKey(catalog, 3) &&
                stopped.page_count == 2,
            "stopping a share reused its stale remote page anchor");

    view.page_anchor = stopped.visible_seats.front().key;
    view.local_screen_share_present = true;
    ++view.view_revision;
    policy.UpdateViewport(view, At(4ms));
    Require(policy.Reconcile(At(4ms)).visible_seats.front().key == CameraKey(catalog, 2),
            "starting a share on a later page retained the old page offset");
    view.page_anchor.reset();
    view.page = 2;
    ++view.view_revision;
    policy.UpdateViewport(view, At(5ms));
    Require(policy.Reconcile(At(5ms)).visible_seats.front().key == CameraKey(catalog, 6),
            "last shared-grid page omitted its displaced remote seat");
    view.local_screen_share_present = false;
    ++view.view_revision;
    policy.UpdateViewport(view, At(6ms));
    const auto clamped = policy.Reconcile(At(6ms));
    Require(clamped.page == 1 && clamped.page_count == 2 &&
                clamped.visible_seats.size() == 4 &&
                clamped.visible_seats.front().key == CameraKey(catalog, 3),
            "share removal did not clamp a disappeared last page correctly");
    view.page = 0;
    ++view.view_revision;
    policy.UpdateViewport(view, At(7ms));
    const auto first = policy.Reconcile(At(7ms));
    Require(first.show_local_participant && !first.show_local_screen_share &&
                first.visible_seats.size() == 3 &&
                first.visible_seats.front().key == CameraKey(catalog, 0),
            "returning to page one after sharing retained its reduced capacity");
}

void TestLocalGridAnchorsAndVisibility() {
    constexpr uint64_t session = 503;
    Policy policy(session);
    auto catalog = Catalog(session, 203, 10, 10);
    policy.UpdateCatalog(catalog);
    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 4);
    view.local_participant_present = true;
    view.local_screen_share_present = true;
    view.page_anchor = CameraKey(catalog, 5);
    policy.UpdateViewport(view, At(0ms));
    Require(policy.Reconcile(At(0ms)).visible_seats.front().key == CameraKey(catalog, 0),
            "first local grid page accepted an anchor that skipped remote seats");
    catalog.participants.erase(catalog.participants.begin());
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    Require(policy.Reconcile(At(1ms)).visible_seats.front().key == CameraKey(catalog, 0),
            "departure moved the first local grid page past the remaining first seat");
    view.page_anchor.reset();
    view.page = 1;
    ++view.view_revision;
    policy.UpdateViewport(view, At(2ms));
    const auto second_key = policy.Reconcile(At(2ms)).visible_seats.front().key;
    catalog.participants.erase(catalog.participants.begin());
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    Require(policy.Reconcile(At(3ms)).visible_seats.front().key == second_key,
            "later local-grid page lost its stable anchor after an earlier departure");

    view.page = 0;
    view.window_visible = false;
    ++view.view_revision;
    policy.UpdateViewport(view, At(4ms));
    const auto hidden = policy.Reconcile(At(4ms));
    Require(!hidden.show_local_participant && !hidden.show_local_screen_share &&
                hidden.visible_seats.empty() && hidden.selected_video.empty() &&
                hidden.selected_audio.size() == 8,
            "hidden grid retained local/remote seats or removed audio demand");
    view.window_visible = true;
    view.stage_content = livekit::StageContent::Whiteboard;
    ++view.view_revision;
    policy.UpdateViewport(view, At(5ms));
    const auto whiteboard = policy.Reconcile(At(5ms));
    Require(!whiteboard.show_local_participant && !whiteboard.show_local_screen_share &&
                whiteboard.visible_seats.empty() && whiteboard.selected_video.empty() &&
                whiteboard.selected_audio.size() == 8,
            "whiteboard grid retained local/remote seats or removed audio demand");

    Policy no_catalog(session + 1);
    auto local_only = Viewport(session + 1, 1, livekit::VideoLayoutMode::Grid, 0, 16);
    local_only.local_participant_present = true;
    local_only.local_screen_share_present = true;
    no_catalog.UpdateViewport(local_only, At(0ms));
    const auto visible_local = no_catalog.Reconcile(At(0ms));
    Require(visible_local.show_local_participant && visible_local.show_local_screen_share &&
                visible_local.visible_seats.empty() && visible_local.selected_video.empty(),
            "local-only layout depended on a remote catalog");
    local_only.window_visible = false;
    ++local_only.view_revision;
    no_catalog.UpdateViewport(local_only, At(1ms));
    const auto hidden_local = no_catalog.Reconcile(At(1ms));
    Require(hidden_local.policy_revision > visible_local.policy_revision &&
                !hidden_local.show_local_participant && !hidden_local.show_local_screen_share,
            "local-only visibility flags did not update the accepted policy revision");
}

void TestLocalSeatsInNonPagedLayouts() {
    constexpr uint64_t session = 504;
    for (const auto mode : {livekit::VideoLayoutMode::Auto,
                           livekit::VideoLayoutMode::Speaker,
                           livekit::VideoLayoutMode::PictureInPicture}) {
        Policy policy(session);
        policy.UpdateCatalog(Catalog(session, 204, 2));
        auto view = Viewport(session, 1, mode, 999, 4);
        view.local_participant_present = true;
        view.local_screen_share_present = true;
        policy.UpdateViewport(view, At(0ms));
        const auto& plan = policy.Reconcile(At(0ms));
        Require(plan.show_local_participant && plan.show_local_screen_share &&
                    plan.visible_seats.size() == 2 && plan.selected_video.size() == 2,
                "non-paged Auto, Speaker or PiP layout changed local/remote visibility");
    }
}

void TestParticipantPlaceholderSeats() {
    constexpr uint64_t session = 301;
    Policy policy(session);
    auto catalog = Catalog(session, 91, 0, 18);
    catalog.participants.front().publications.clear(); // Neither camera nor microphone.
    policy.UpdateCatalog(catalog);
    auto view = Viewport(session, 1, livekit::VideoLayoutMode::Grid, 0, 16);
    policy.UpdateViewport(view, At(0ms));
    const auto initial = policy.Reconcile(At(0ms));
    Require(initial.visible_seats.size() == 16 && initial.page_count == 2 &&
                initial.selected_video.empty() && initial.selected_audio.size() == 17,
            "participants without video lost their bounded display seats or requested video");
    for (const auto& seat : initial.visible_seats) {
        Require(seat.IsParticipantPlaceholder() && seat.key.publication_incarnation == 0 &&
                    seat.key.participant.native_room_generation == 91 &&
                    seat.width == 0 && seat.height == 0 &&
                    seat.quality == livekit::VideoQualityTier::None,
                "participant placeholder invented a publication or media demand");
    }
    view.view_revision = 2;
    view.page = 1;
    policy.UpdateViewport(view, At(1ms));
    Require(policy.Reconcile(At(1ms)).visible_seats.size() == 2,
            "participants without video were omitted from later pages");

    view.view_revision = 3;
    view.page = 0;
    view.pinned = initial.visible_seats.front().key;
    policy.UpdateViewport(view, At(2ms));
    const auto pinned = policy.Reconcile(At(2ms));
    Require(pinned.focused == view.pinned && pinned.visible_seats.size() == 5 &&
                pinned.visible_seats.front().IsParticipantPlaceholder() &&
                pinned.selected_video.empty(),
            "pinning a participant without video requested a nonexistent publication");

    view.view_revision = 4;
    view.pinned.reset();
    policy.UpdateViewport(view, At(3ms));
    auto& participant = catalog.participants.front();
    participant.publications.push_back(Publication(
        participant.key, 500, "CAM_STARTED", livekit::TrackKind::Video,
        livekit::TrackSource::Camera));
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto started = policy.Reconcile(At(3ms));
    Require(started.visible_seats.size() == 16 && started.selected_video.size() == 1 &&
                !started.visible_seats.front().IsParticipantPlaceholder() &&
                started.visible_seats.front().key == participant.publications.front().key,
            "camera publication did not replace its participant placeholder");

    participant.publications.clear();
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto stopped = policy.Reconcile(At(4ms));
    Require(stopped.visible_seats.size() == 16 && stopped.selected_video.empty() &&
                stopped.visible_seats.front().key == initial.visible_seats.front().key,
            "camera unpublish removed the participant display seat");

    const auto departed = participant.key;
    catalog.participants.erase(catalog.participants.begin());
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto remaining = policy.Reconcile(At(5ms));
    Require(std::none_of(remaining.visible_seats.begin(), remaining.visible_seats.end(),
                [&](const auto& seat) { return seat.key.participant == departed; }),
            "departed participant left a display placeholder");
    view.view_revision = 5;
    view.window_visible = false;
    policy.UpdateViewport(view, At(6ms));
    Require(policy.Reconcile(At(6ms)).visible_seats.empty(),
            "hidden window retained participant display seats");

    Policy focused_policy(session + 1);
    auto focused_catalog = Catalog(session + 1, 92, 0, 2);
    auto& speaker = focused_catalog.participants.back();
    speaker.publications.push_back(Publication(
        speaker.key, 501, "CAM_SPEAKER", livekit::TrackKind::Video,
        livekit::TrackSource::Camera));
    focused_policy.UpdateCatalog(focused_catalog);
    focused_policy.UpdateViewport(
        Viewport(session + 1, 1, livekit::VideoLayoutMode::Speaker), At(0ms));
    Require(focused_policy.Reconcile(At(0ms)).visible_seats.front().IsParticipantPlaceholder(),
            "initial focused participant without video lost its display seat");
    focused_policy.UpdateSpeakers({Speaker(focused_catalog, 1)}, At(1ms));
    focused_policy.Reconcile(At(1001ms));
    Require(focused_policy.NextReconcileAt(At(1001ms)) == At(3000ms),
            "placeholder focus blocked the timer for a new active speaker");
    Require(focused_policy.Reconcile(At(3000ms)).focused == speaker.publications.back().key,
            "active speaker failed to replace the placeholder focus");
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

void TestSubscriptionErrorProjectionKeepsDemandStable() {
    constexpr uint64_t session = 302;
    Policy policy(session);
    auto catalog = Catalog(session, 93, 1);
    policy.UpdateCatalog(catalog);
    policy.UpdateViewport(Viewport(session, 1, livekit::VideoLayoutMode::Grid), At(0ms));
    const auto initial = policy.Reconcile(At(0ms));
    Require(initial.visible_seats.size() == 1 && initial.selected_video.size() == 1 &&
                initial.visible_seats.front().subscription_error ==
                    livekit::TrackPublication::SubscriptionError::None,
            "healthy publication started with a subscription failure");

    auto& publication = catalog.participants.front().publications.front();
    publication.subscription_error =
        livekit::TrackPublication::SubscriptionError::CodecUnsupported;
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto failed = policy.Reconcile(At(1ms));
    Require(failed.policy_revision > initial.policy_revision &&
                failed.visible_seats.front().subscription_error ==
                    livekit::TrackPublication::SubscriptionError::CodecUnsupported &&
                failed.selected_video == initial.selected_video,
            "subscription failure was hidden or changed demand into a resubscribe loop");
    Require(policy.Reconcile(At(2ms)).policy_revision == failed.policy_revision,
            "unchanged subscription failure repeatedly advanced the policy");

    publication.subscription_error = livekit::TrackPublication::SubscriptionError::None;
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto recovered = policy.Reconcile(At(3ms));
    Require(recovered.policy_revision > failed.policy_revision &&
                recovered.visible_seats.front().subscription_error ==
                    livekit::TrackPublication::SubscriptionError::None &&
                recovered.selected_video == initial.selected_video,
            "subscription recovery did not clear the displayed error with stable demand");
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
    share.source_width = 3840;
    share.source_height = 2160;
    share_catalog.participants[0].publications[0].source_width = 3840;
    share_catalog.participants[0].publications[0].source_height = 2160;
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
    share_view.stage_rect = {0, 0, 3840, 2160};
    share_policy.UpdateViewport(share_view, At(1ms));
    const auto& large_share = share_policy.Reconcile(At(1ms));
    Require(large_share.visible_seats.front().width > 1920 && large_share.visible_seats.front().height > 1080,
            "large main screen share remains capped at 1080p");
    Require(large_share.visible_seats.front().width <= 3840 && large_share.visible_seats.front().height <= 2160,
            "main screen share exceeds 4K budget");
    share_view.view_revision = 3;
    share_view.selected_share.reset();
    share_view.pinned = CameraKey(share_catalog, 0);
    share_policy.UpdateViewport(share_view, At(1ms));
    const auto& pinned_camera = share_policy.Reconcile(At(1ms));
    Require(pinned_camera.visible_seats.front().priority == 500 &&
                pinned_camera.visible_seats.front().quality ==
                    livekit::VideoQualityTier::P2160,
            "pinned main camera did not share the 4K main-screen policy");

    share_policy.Retire();
    Require(!share_policy.accepting() &&
                share_policy.plan().selected_video.empty() &&
                share_policy.plan().selected_audio.empty(),
            "retired policy retained active media demand");
}

void TestPixelSizedGridDemand() {
    using Tier = livekit::VideoQualityTier;
    struct Case {
        uint32_t source_width, source_height;
        int width, height;
        double dpr;
        uint32_t page_size, expected_width, expected_height;
        Tier quality;
    };
    const Case cases[] = {
        {3840, 2160, 1280, 720, 1.0, 16, 320, 180, Tier::P180},
        {3840, 2160, 2560, 1440, 1.0, 16, 640, 360, Tier::P360},
        {3840, 2160, 3840, 2160, 1.0, 16, 960, 540, Tier::P720},
        {3840, 2160, 7680, 4320, 1.0, 16, 1920, 1080, Tier::P1080},
        {3840, 2160, 15360, 8640, 1.0, 16, 2560, 1440, Tier::P1440},
        {3840, 2160, 1280, 720, 2.0, 16, 640, 360, Tier::P360},
        {640, 360, 3840, 2160, 1.0, 4, 640, 360, Tier::P360},
        {2160, 3840, 1440, 2560, 1.0, 4, 720, 1280, Tier::P720},
        {1920, 1080, 800, 800, 1.0, 4, 400, 225, Tier::P360},
        {0, 0, 3840, 2160, 1.0, 4, 1920, 1080, Tier::P1080},
        {3840, 0, 1280, 720, 1.0, 16, 320, 180, Tier::P180},
    };
    for (const auto& test : cases) {
        Policy policy(401);
        auto catalog = Catalog(401, 101, 1);
        auto& publication = catalog.participants[0].publications[0];
        publication.source_width = test.source_width;
        publication.source_height = test.source_height;
        policy.UpdateCatalog(catalog);
        auto view = Viewport(401, 1, livekit::VideoLayoutMode::Grid, 0,
                             test.page_size);
        view.stage_rect = {0, 0, test.width, test.height};
        view.device_pixel_ratio = test.dpr;
        policy.UpdateViewport(view, At(0ms));
        const auto& plan = policy.Reconcile(At(0ms));
        const auto& seat = plan.visible_seats.front();
        Require(plan.selected_video.size() == 1 &&
                    seat.width == test.expected_width &&
                    seat.height == test.expected_height &&
                    seat.quality == test.quality,
                "grid ignored tile pixels, DPR, source aspect or source maximum");
    }
}

void TestUnifiedMainSourceBounds() {
    using Tier = livekit::VideoQualityTier;
    struct Case {
        uint32_t source_width, source_height;
        int width, height;
        uint32_t expected_width, expected_height;
        Tier quality;
    };
    const Case cases[] = {
        {3840, 2160, 320, 180, 320, 180, Tier::P180},
        {3840, 2160, 1920, 1080, 1920, 1080, Tier::P1080},
        {3840, 2160, 2560, 1440, 2560, 1440, Tier::P1440},
        {7680, 4320, 7680, 4320, 3840, 2160, Tier::P2160},
        {1280, 720, 3840, 2160, 1280, 720, Tier::P720},
        {1920, 1080, 3840, 2160, 1920, 1080, Tier::P1080},
        {640, 360, 320, 180, 320, 180, Tier::P180},
        {960, 540, 3840, 2160, 960, 540, Tier::P720},
        {1920, 1080, 800, 800, 800, 450, Tier::P720},
        {1920, 1440, 320, 180, 240, 180, Tier::P180},
        {2160, 3840, 180, 320, 180, 320, Tier::P180},
        {4320, 7680, 4320, 7680, 2160, 3840, Tier::P2160},
        {0, 0, 320, 180, 320, 180, Tier::P180},
        {0, 2160, 3840, 2160, 3840, 2160, Tier::P2160},
        {3840, 2160, 0, 180, 0, 0, Tier::None},
        {3840, 2160, 320, 0, 0, 0, Tier::None},
    };
    for (const auto source : {livekit::TrackSource::Camera,
                              livekit::TrackSource::ScreenShareVideo}) {
        for (const auto& test : cases) {
            Policy policy(402);
            auto catalog = Catalog(402, 102, 1);
            auto& publication = catalog.participants[0].publications[0];
            publication.source = source;
            publication.source_width = test.source_width;
            publication.source_height = test.source_height;
            policy.UpdateCatalog(catalog);
            auto view = Viewport(402, 1, livekit::VideoLayoutMode::Speaker);
            view.pinned = publication.key;
            view.stage_rect = {0, 0, test.width, test.height};
            policy.UpdateViewport(view, At(0ms));
            const auto& plan = policy.Reconcile(At(0ms));
            if (test.expected_width == 0) {
                Require(plan.visible_seats.empty() && plan.selected_video.empty() &&
                            plan.reason == livekit::VideoDemandReason::Hidden,
                        "zero-sized main viewport invented a seat or subscription");
                continue;
            }
            Require(plan.visible_seats.size() == 1,
                    "valid main viewport did not create its display seat");
            const auto& seat = plan.visible_seats.front();
            Require(seat.role == livekit::VideoSeatRole::Main &&
                        seat.width == test.expected_width &&
                        seat.height == test.expected_height &&
                        seat.quality == test.quality,
                    "camera/share main viewport fit, source fallback or 4K cap differed");
            Require(plan.selected_video.size() == (test.expected_width ? 1u : 0u),
                    "zero-sized main viewport invented a video subscription");
        }
    }
}

void TestSourceRefreshAndPinToGridDemand() {
    Policy policy(403);
    auto catalog = Catalog(403, 103, 1);
    auto& publication = catalog.participants[0].publications[0];
    publication.source_width = 1280;
    publication.source_height = 720;
    policy.UpdateCatalog(catalog);
    auto view = Viewport(403, 1, livekit::VideoLayoutMode::Grid, 0, 16);
    view.stage_rect = {0, 0, 3840, 2160};
    view.pinned = publication.key;
    policy.UpdateViewport(view, At(0ms));
    const auto first = policy.Reconcile(At(0ms));
    Require(first.visible_seats.front().width == 1280 &&
                first.visible_seats.front().height == 720,
            "pin requested more than the advertised source");
    publication.source_width = 3840;
    publication.source_height = 2160;
    ++catalog.catalog_revision;
    Require(policy.UpdateCatalog(catalog), "source-only refresh was rejected");
    const auto refreshed = policy.Reconcile(At(1ms));
    Require(refreshed.policy_revision > first.policy_revision &&
                refreshed.visible_seats.front().width == 3840 &&
                refreshed.visible_seats.front().height == 2160,
            "source-only refresh did not update main demand without a resize");
    auto stale = catalog;
    --stale.catalog_revision;
    stale.participants[0].publications[0].source_width = 640;
    Require(!policy.UpdateCatalog(stale) &&
                policy.Reconcile(At(2ms)).policy_revision == refreshed.policy_revision,
            "stale source metadata replaced current main demand");
    view.pinned.reset();
    view.view_revision = 2;
    view.stage_rect = {0, 0, 1280, 720};
    policy.UpdateViewport(view, At(3ms));
    const auto grid = policy.Reconcile(At(3ms));
    Require(grid.visible_seats.front().width == 320 &&
                grid.visible_seats.front().height == 180 &&
                grid.visible_seats.front().quality == livekit::VideoQualityTier::P180,
            "returning from pin to a small grid retained high-layer demand");
    view.window_visible = false;
    ++view.view_revision;
    policy.UpdateViewport(view, At(4ms));
    Require(policy.Reconcile(At(4ms)).selected_video.empty(),
            "hidden high-resolution source retained video demand");
}

void TestEmptyStageAndDprBounds() {
    for (const auto mode : {livekit::VideoLayoutMode::Speaker,
                            livekit::VideoLayoutMode::PictureInPicture}) {
        for (const auto zero_width : {false, true}) {
            Policy policy(404);
            auto catalog = Catalog(404, 104, 2);
            policy.UpdateCatalog(catalog);
            auto view = Viewport(404, 1, mode);
            view.stage_rect = {0, 0, zero_width ? 0 : 320, zero_width ? 180 : 0};
            policy.UpdateViewport(view, At(0ms));
            const auto& plan = policy.Reconcile(At(0ms));
            Require(plan.visible_seats.empty() && plan.selected_video.empty() &&
                        plan.reason == livekit::VideoDemandReason::Hidden,
                    "empty stage retained a main, sidebar or PiP subscription");
        }
    }
    for (const auto dpr : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::max()}) {
        Policy policy(405);
        auto catalog = Catalog(405, 105, 1);
        catalog.participants[0].publications[0].source_width = 3840;
        catalog.participants[0].publications[0].source_height = 2160;
        policy.UpdateCatalog(catalog);
        auto view = Viewport(405, 1, livekit::VideoLayoutMode::Grid, 0, 16);
        view.stage_rect = {0, 0, 1280, 720};
        view.device_pixel_ratio = dpr;
        policy.UpdateViewport(view, At(0ms));
        const auto& seat = policy.Reconcile(At(0ms)).visible_seats.front();
        const bool saturated = dpr == std::numeric_limits<double>::max();
        Require(seat.width == (saturated ? 2560u : 320u) &&
                    seat.height == (saturated ? 1440u : 180u),
                "invalid or huge DPR escaped fallback, saturation or grid cap");
    }
}

livekit::VideoDemandPlan LayerPlan(
    livekit::VideoLayoutMode mode, livekit::TrackSource source,
    uint32_t source_width, uint32_t source_height,
    int pixel_width, int pixel_height,
    std::vector<livekit::PublishedVideoLayer> layers) {
    Policy policy(406);
    auto catalog = Catalog(406, 106, 1);
    auto& publication = catalog.participants[0].publications[0];
    publication.source = source;
    publication.source_width = source_width;
    publication.source_height = source_height;
    publication.published_video_layers = std::move(layers);
    policy.UpdateCatalog(catalog);
    auto view = Viewport(406, 1, mode, 0, 4);
    const bool main = mode == livekit::VideoLayoutMode::Speaker;
    if (main) view.pinned = publication.key;
    view.stage_rect = {0, 0, main ? pixel_width : 2 * pixel_width,
                                main ? pixel_height : 2 * pixel_height};
    policy.UpdateViewport(view, At(0ms));
    return policy.Reconcile(At(0ms));
}

void TestPublishedLayerFloorCeilingAndSingleLayer() {
    using Quality = livekit::PublishedVideoQuality;
    using Mode = livekit::VideoLayoutMode;
    struct Case {
        Mode mode;
        uint32_t source_width, source_height;
        int width, height;
        std::vector<livekit::PublishedVideoLayer> layers;
        uint32_t expected_width, expected_height;
        Quality expected_quality;
    };
    const std::vector<livekit::PublishedVideoLayer> camera{
        {Quality::Low, 320, 180, "q"}, {Quality::Medium, 640, 360, "h"},
        {Quality::High, 2560, 1440, "f"}};
    const std::vector<livekit::PublishedVideoLayer> uncommon{
        {Quality::Low, 960, 540, "low"}, {Quality::Medium, 1920, 1080, "mid"},
        {Quality::High, 2560, 1440, "top"}};
    const std::vector<livekit::PublishedVideoLayer> large{
        {Quality::Low, 1280, 720, "low"}, {Quality::Medium, 2560, 1440, "mid"},
        {Quality::High, 3840, 2160, "top"}};
    const Case cases[] = {
        {Mode::Grid, 2560, 1440, 875, 492, camera, 640, 360, Quality::Medium},
        {Mode::Speaker, 2560, 1440, 875, 492, camera, 2560, 1440, Quality::High},
        {Mode::Grid, 2560, 1440, 640, 360, camera, 640, 360, Quality::Medium},
        {Mode::Speaker, 2560, 1440, 640, 360, camera, 640, 360, Quality::Medium},
        {Mode::Grid, 2560, 1440, 235, 132, camera, 320, 180, Quality::Low},
        {Mode::Speaker, 2560, 1440, 235, 132, camera, 320, 180, Quality::Low},
        {Mode::Grid, 2560, 1440, 2560, 1440, camera, 2560, 1440, Quality::High},
        {Mode::Speaker, 2560, 1440, 7680, 4320, camera, 2560, 1440, Quality::High},
        {Mode::Grid, 2560, 1440, 1400, 788, uncommon, 960, 540, Quality::Low},
        {Mode::Speaker, 2560, 1440, 1400, 788, uncommon, 1920, 1080, Quality::Medium},
        {Mode::Grid, 3840, 2160, 3840, 2160, large, 2560, 1440, Quality::Medium},
        {Mode::Speaker, 3840, 2160, 3000, 1688, large, 3840, 2160, Quality::High},
        {Mode::Grid, 3840, 2160, 235, 132,
            {{Quality::High, 3840, 2160, "single"}}, 3840, 2160, Quality::High},
        {Mode::Speaker, 7680, 4320, 235, 132,
            {{Quality::High, 7680, 4320, "single"}}, 7680, 4320, Quality::High},
        // A screen-share highest layer can be h/Medium, independent of pixels.
        {Mode::Speaker, 2560, 1440, 1920, 1080,
            {{Quality::Low, 1280, 720, "q"}, {Quality::Medium, 2560, 1440, "h"}},
            2560, 1440, Quality::Medium},
        {Mode::Grid, 1440, 2560, 492, 875,
            {{Quality::Low, 180, 320, "q"}, {Quality::Medium, 360, 640, "h"},
             {Quality::High, 1440, 2560, "f"}}, 360, 640, Quality::Medium},
        {Mode::Speaker, 1440, 2560, 492, 875,
            {{Quality::Low, 180, 320, "q"}, {Quality::Medium, 360, 640, "h"},
             {Quality::High, 1440, 2560, "f"}}, 1440, 2560, Quality::High},
        {Mode::Grid, 0, 0, 875, 492, camera, 640, 360, Quality::Medium},
    };
    for (const auto source : {livekit::TrackSource::Camera,
                              livekit::TrackSource::ScreenShareVideo}) {
        for (const auto& test : cases) {
            const auto plan = LayerPlan(test.mode, source, test.source_width,
                test.source_height, test.width, test.height, test.layers);
            const auto& seat = plan.visible_seats.front();
            Require(plan.selected_video.size() == 1 &&
                        seat.subscription_width == test.expected_width &&
                        seat.subscription_height == test.expected_height &&
                        seat.selected_layer_quality == test.expected_quality,
                    "published layer floor/ceiling, cap, quality or single-layer exception failed");
        }
    }
    const auto small_main = LayerPlan(Mode::Speaker, livekit::TrackSource::Camera,
        2560, 1440, 235, 132, camera);
    Require(small_main.visible_seats.front().width == 235 &&
                small_main.visible_seats.front().height == 132 &&
                small_main.visible_seats.front().quality == livekit::VideoQualityTier::P180,
            "small main retained a 720p minimum instead of current viewport demand");
    const auto ordinary = LayerPlan(Mode::Grid, livekit::TrackSource::Camera,
        2560, 1440, 875, 492, camera);
    Require(ordinary.visible_seats.front().width == 875 &&
                ordinary.visible_seats.front().height == 492,
            "layer selection overwrote the real tile pixel demand");
}

void TestLayerCapFailureAndUnknownMetadata() {
    using Quality = livekit::PublishedVideoQuality;
    using Mode = livekit::VideoLayoutMode;
    for (const auto mode : {Mode::Grid, Mode::Speaker}) {
        const uint32_t width = mode == Mode::Grid ? 3840 : 7680;
        const uint32_t height = mode == Mode::Grid ? 2160 : 4320;
        const auto plan = LayerPlan(mode, livekit::TrackSource::Camera,
            2 * width, 2 * height, 875, 492,
            {{Quality::Low, width, height, "low"},
             {Quality::High, 2 * width, 2 * height, "top"}});
        Require(plan.visible_seats.size() == 1 && plan.selected_video.empty() &&
                    plan.visible_seats.front().width == 875 &&
                    plan.visible_seats.front().subscription_width == 0 &&
                    plan.visible_seats.front().subscription_height == 0 &&
                    !plan.visible_seats.front().selected_layer_quality &&
                    plan.visible_seats.front().reason ==
                        livekit::VideoDemandReason::NoCompatibleLayer,
                "multiple layers above the role cap were silently subscribed or removed from layout");
    }
    const auto source_only = LayerPlan(Mode::Grid, livekit::TrackSource::Camera,
        3840, 2160, 235, 132, {});
    Require(source_only.selected_video.size() == 1 &&
                source_only.visible_seats.front().subscription_width == 235 &&
                source_only.visible_seats.front().subscription_height == 132 &&
                !source_only.visible_seats.front().selected_layer_quality,
            "original source dimensions alone falsely enabled the single-layer exception");
    const auto unknown = LayerPlan(Mode::Grid, livekit::TrackSource::Camera,
        0, 0, 875, 492, {});
    Require(unknown.selected_video.size() == 1 &&
                unknown.visible_seats.front().subscription_width == 875 &&
                unknown.visible_seats.front().subscription_height == 492 &&
                !unknown.visible_seats.front().selected_layer_quality,
            "unknown source/layers fabricated an advertised quality or lost pixel fallback");
}

void TestLayerOnlyRefreshAndSmallSeatRoles() {
    using Quality = livekit::PublishedVideoQuality;
    Policy policy(407);
    auto catalog = Catalog(407, 107, 1);
    auto& publication = catalog.participants[0].publications[0];
    publication.source_width = 2560;
    publication.source_height = 1440;
    publication.published_video_layers = {
        {Quality::Low, 320, 180, "q"}, {Quality::Medium, 640, 360, "h"},
        {Quality::High, 2560, 1440, "f"}};
    policy.UpdateCatalog(catalog);
    auto view = Viewport(407, 1, livekit::VideoLayoutMode::Grid, 0, 4);
    view.stage_rect = {0, 0, 1750, 984};
    policy.UpdateViewport(view, At(0ms));
    const auto initial = policy.Reconcile(At(0ms));
    Require(initial.visible_seats.front().subscription_width == 640,
            "initial floor layer was wrong");
    publication.published_video_layers[1].width = 960;
    publication.published_video_layers[1].height = 540;
    ++catalog.catalog_revision;
    Require(policy.UpdateCatalog(catalog), "layer-only catalog refresh was rejected");
    const auto refreshed = policy.Reconcile(At(1ms));
    Require(refreshed.policy_revision > initial.policy_revision &&
                refreshed.visible_seats.front().width == 875 &&
                refreshed.visible_seats.front().subscription_width == 320,
            "layer-only refresh failed to change selection without a resize");
    publication.published_video_layers[0].quality = Quality::Medium;
    ++catalog.catalog_revision;
    policy.UpdateCatalog(catalog);
    const auto quality_refreshed = policy.Reconcile(At(2ms));
    Require(quality_refreshed.policy_revision > refreshed.policy_revision &&
                quality_refreshed.visible_seats.front().subscription_width == 320 &&
                quality_refreshed.visible_seats.front().selected_layer_quality == Quality::Medium,
            "an advertised quality-only refresh was lost at unchanged dimensions");

    for (const auto mode : {livekit::VideoLayoutMode::Speaker,
                            livekit::VideoLayoutMode::PictureInPicture}) {
        Policy other(408);
        auto other_catalog = Catalog(408, 108, 2);
        for (auto& participant : other_catalog.participants) {
            auto& source = participant.publications.front();
            source.source_width = 3840;
            source.source_height = 2160;
            source.published_video_layers = {
                {Quality::Low, 320, 180, "q"}, {Quality::Medium, 1920, 1080, "h"},
                {Quality::High, 3840, 2160, "f"}};
        }
        other.UpdateCatalog(other_catalog);
        auto other_view = Viewport(408, 1, mode);
        other_view.stage_rect = {0, 0, 7680, 4320};
        other.UpdateViewport(other_view, At(0ms));
        const auto& plan = other.Reconcile(At(0ms));
        const auto small = std::find_if(plan.visible_seats.begin(), plan.visible_seats.end(),
            [](const auto& seat) { return seat.role != livekit::VideoSeatRole::Main; });
        Require(small != plan.visible_seats.end() && small->width > 640 &&
                    small->subscription_width == 1920 &&
                    small->subscription_height == 1080,
                "sidebar/PiP retained a fixed 360p limit or selected above the 2K cap");
    }
}

} // namespace

int main() {
    TestBoundedGridPaginationAndAudioIndependence();
    TestGridCapacityIncludesLocalSeats();
    TestLocalGridShareTransitionsAndClamping();
    TestLocalGridAnchorsAndVisibility();
    TestLocalSeatsInNonPagedLayouts();
    TestParticipantPlaceholderSeats();
    TestStablePageAnchorAndSourceOrder();
    TestSharePriorityAndExplicitSelection();
    TestPinPlaceholderAndRestore();
    TestSpeakerHysteresisAndFocusedLayouts();
    TestGridSpeakerDoesNotMovePage();
    TestVisibilityPermissionAndIdempotence();
    TestSubscriptionErrorProjectionKeepsDemandStable();
    TestGenerationReplacementAndQualityCaps();
    TestPixelSizedGridDemand();
    TestUnifiedMainSourceBounds();
    TestSourceRefreshAndPinToGridDemand();
    TestEmptyStageAndDprBounds();
    TestPublishedLayerFloorCeilingAndSingleLayer();
    TestLayerCapFailureAndUnknownMetadata();
    TestLayerOnlyRefreshAndSmallSeatRoles();
    std::cout << "video demand policy contract tests passed" << std::endl;
    return 0;
}
