// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_remote_publication_lifecycle.cpp
// Original SHA256: 9ed41b85bd9fe04dbf47d4d3f7d676d42fe5acf8557343990b8394c9817dd892
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: always-active checks (28 original expressions).

#include "tests/support/test_check.h"
#include <iostream>
#include <memory>
#include <vector>

#include "participant.h"
#include "remote_track_publication.h"
#include "room.h"
#include "livekit_models.pb.h"

int main() {
    using livekit::RemotePublicationControlDispatch;
    using livekit::RemotePublicationControlRequest;

    auto track = std::make_shared<livekit::Track>(
        "TR_REMOTE_VIDEO_200", "camera", livekit::TrackKind::Video);
    std::vector<RemotePublicationControlRequest> requests;
    int detach_count = 0;
    bool detach_notified = false;

    auto publication = std::make_shared<livekit::RemoteTrackPublication>(
        track,
        "TR_REMOTE_VIDEO_200",
        "camera",
        livekit::proto::TrackType::VIDEO,
        42,
        [&requests](livekit::RemoteTrackPublication*,
                    const RemotePublicationControlRequest& request) {
            requests.push_back(request);
            return RemotePublicationControlDispatch::Committed;
        });
    auto participant = std::make_shared<livekit::RemoteParticipant>(
        "PA_REMOTE_200", "remote-user");
    participant->add_publication(publication);

    // Metadata, media state, and the control surface resolve to exactly one
    // object from the participant's canonical publication map.
    TEST_CHECK(participant->get_publication(publication->sid()) == publication);
    TEST_CHECK(participant->get_remote_publication(publication->sid()) == publication);
    TEST_CHECK(participant->get_remote_publication("TR_UNKNOWN") == nullptr);
    TEST_CHECK(publication->is_subscribed());
    TEST_CHECK(publication->is_enabled());

    publication->SetMediaBinding(
        "rtc-video-200",
        [&detach_count, &detach_notified](livekit::RemoteTrackPublication* actual,
                                          bool notify_listener) {
            ++detach_count;
            detach_notified = notify_listener;
            actual->ClearMediaBinding();
        });
    TEST_CHECK(publication->has_media_binding());
    TEST_CHECK(publication->SetSubscribed(false));
    TEST_CHECK(detach_count == 1);
    TEST_CHECK(detach_notified);
    TEST_CHECK(!publication->has_media_binding());
    TEST_CHECK(!publication->is_subscribed());
    TEST_CHECK(requests.back().kind == RemotePublicationControlRequest::Kind::Subscription);
    TEST_CHECK(requests.back().subscribed.has_value() && !*requests.back().subscribed);

    // Retained from the retired AdaptiveStreamManager suite. Exercise the
    // production publication/controller contract, including max-dimension
    // thresholds and portrait orientation, instead of protobuf setters alone.
    using Quality = livekit::proto::VideoQuality;
    struct DimensionCase { uint32_t width; uint32_t height; Quality quality; };
    const DimensionCase dimensions[] = {
        {320, 180, Quality::LOW},
        {640, 360, Quality::MEDIUM},
        {1920, 1080, Quality::HIGH},
        {180, 320, Quality::LOW},
        {360, 640, Quality::MEDIUM},
        {1080, 1920, Quality::HIGH},
        {360, 360, Quality::LOW},
        {361, 180, Quality::MEDIUM},
        {360, 720, Quality::MEDIUM},
        {360, 721, Quality::HIGH},
    };
    for (const auto &value : dimensions) {
        const auto before = requests.size();
        TEST_CHECK(publication->SetVideoDimensions(value.width, value.height));
        TEST_CHECK(requests.size() == before + 1);
        const auto &request = requests.back();
        TEST_CHECK(request.kind == RemotePublicationControlRequest::Kind::Settings);
        TEST_CHECK(request.width == value.width && request.height == value.height);
        TEST_CHECK(request.quality == value.quality);
        TEST_CHECK(publication->current_width() == value.width);
        TEST_CHECK(publication->current_height() == value.height);
        TEST_CHECK(publication->current_quality() == value.quality);
    }
    TEST_CHECK(publication->SetPriority(7));
    TEST_CHECK(publication->priority() == 7);
    TEST_CHECK(publication->SetEnabled(false));
    TEST_CHECK(!publication->is_enabled());

    // A rejected operation must not mutate the actual desired publication
    // state or issue a successful-looking result to a caller.
    auto rejected = std::make_shared<livekit::RemoteTrackPublication>(
        std::make_shared<livekit::Track>("TR_REJECTED", "camera", livekit::TrackKind::Video),
        "TR_REJECTED", "camera", livekit::proto::TrackType::VIDEO, 42,
        [](livekit::RemoteTrackPublication*, const RemotePublicationControlRequest&) {
            return RemotePublicationControlDispatch::Rejected;
        });
    TEST_CHECK(!rejected->SetPriority(9));
    TEST_CHECK(rejected->priority() == 0);

    // Room creates the same derived publication for a real ParticipantUpdate.
    // Without an active SignalClient/session its controller rejects safely,
    // proving a stale or removed map entry cannot mutate state or send signal.
    asio::io_context io_context;
    auto room = livekit::Room::Create(io_context.get_executor());
    livekit::proto::ParticipantUpdate update;
    auto* remote = update.add_participants();
    remote->set_sid("PA_REMOTE_201");
    remote->set_identity("remote-room-user");
    remote->set_state(livekit::proto::ParticipantInfo::ACTIVE);
    auto* remote_track = remote->add_tracks();
    remote_track->set_sid("TR_REMOTE_VIDEO_201");
    remote_track->set_name("camera");
    remote_track->set_type(livekit::proto::TrackType::VIDEO);
    remote_track->set_width(640);
    remote_track->set_height(360);
    remote_track->set_mime_type("video/VP8");
    const auto set_layer = [](livekit::proto::VideoLayer* layer,
                              Quality quality, uint32_t width, uint32_t height,
                              const std::string& rid) {
        layer->set_quality(quality);
        layer->set_width(width);
        layer->set_height(height);
        layer->set_rid(rid);
    };
    set_layer(remote_track->add_layers(), Quality::HIGH, 640, 360, "only");
    room->UpdateParticipantsForTesting(update);

    const auto room_participant = room->remote_participants().at("PA_REMOTE_201");
    const auto canonical = room_participant->get_remote_publication("TR_REMOTE_VIDEO_201");
    TEST_CHECK(canonical);
    TEST_CHECK(room_participant->get_publication(canonical->sid()) == canonical);
    TEST_CHECK(canonical->SnapshotState().source_width == 640);
    TEST_CHECK(canonical->SnapshotState().source_height == 360);
    using Layer = livekit::PublishedVideoLayer;
    using PublishedQuality = livekit::PublishedVideoQuality;
    const std::vector<Layer> legacy_layers = {
        {PublishedQuality::High, 640, 360, "only"}};
    TEST_CHECK(canonical->SnapshotState().published_video_layers == legacy_layers);
    TEST_CHECK(canonical->current_width() == 0 && canonical->current_height() == 0);
    TEST_CHECK(!canonical->SetPriority(3));
    TEST_CHECK(canonical->priority() == 0);

    // Source metadata updates keep the canonical publication and remain
    // independent of its requested subscription dimensions.
    remote_track->set_width(3840);
    remote_track->set_height(2160);
    auto* backup_codec = remote_track->add_codecs();
    backup_codec->set_mime_type("video/h264");
    set_layer(backup_codec->add_layers(), Quality::LOW, 320, 180, "q");
    auto* selected_codec = remote_track->add_codecs();
    selected_codec->set_mime_type("VIDEO/vp8");
    // A highest screen layer can be MEDIUM/h. Preserve those labels even when
    // its pixel dimensions exceed the HIGH layer of the legacy declaration.
    set_layer(selected_codec->add_layers(), Quality::MEDIUM, 2560, 1440, "h");
    set_layer(selected_codec->add_layers(), Quality::LOW, 1280, 720, "q");
    set_layer(selected_codec->add_layers(), Quality::OFF, 3840, 2160, "f");
    set_layer(selected_codec->add_layers(), Quality::HIGH, 0, 2160, "f");
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(room_participant->get_remote_publication("TR_REMOTE_VIDEO_201") == canonical);
    TEST_CHECK(canonical->SnapshotState().source_width == 3840);
    TEST_CHECK(canonical->SnapshotState().source_height == 2160);
    const std::vector<Layer> selected_layers = {
        {PublishedQuality::Low, 1280, 720, "q"},
        {PublishedQuality::Medium, 2560, 1440, "h"}};
    TEST_CHECK(canonical->SnapshotState().published_video_layers == selected_layers);
    TEST_CHECK(canonical->current_width() == 0 && canonical->current_height() == 0);

    // A layer-only update reaches the same canonical object, without altering
    // source or requested dimensions or borrowing any backup-codec candidates.
    selected_codec->mutable_layers(0)->set_width(3840);
    selected_codec->mutable_layers(0)->set_height(2160);
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(room_participant->get_remote_publication("TR_REMOTE_VIDEO_201") == canonical);
    const std::vector<Layer> changed_layers = {
        {PublishedQuality::Low, 1280, 720, "q"},
        {PublishedQuality::Medium, 3840, 2160, "h"}};
    TEST_CHECK(canonical->SnapshotState().published_video_layers == changed_layers);
    TEST_CHECK(canonical->SnapshotState().source_width == 3840);
    TEST_CHECK(canonical->current_width() == 0 && canonical->current_height() == 0);

    // Unmatched or empty codec metadata falls back only to the legacy layers.
    remote_track->set_mime_type("video/av1");
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(canonical->SnapshotState().published_video_layers == legacy_layers);
    remote_track->set_mime_type("video/vp8");
    selected_codec->clear_layers();
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(canonical->SnapshotState().published_video_layers == legacy_layers);
    remote_track->clear_layers();
    remote_track->clear_codecs();
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(canonical->SnapshotState().published_video_layers.empty());
    TEST_CHECK(canonical->SnapshotState().source_width == 3840);
    TEST_CHECK(room_participant->get_remote_publication("TR_REMOTE_VIDEO_201") == canonical);
    remote_track->set_height(0);
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(canonical->SnapshotState().source_width == 0);
    TEST_CHECK(canonical->SnapshotState().source_height == 0);
    remote_track->set_width(1920);
    remote_track->set_height(1080);
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(canonical->SnapshotState().source_width == 1920);
    TEST_CHECK(canonical->SnapshotState().source_height == 1080);

    auto* disconnected = update.mutable_participants(0);
    disconnected->set_state(livekit::proto::ParticipantInfo::DISCONNECTED);
    disconnected->clear_tracks();
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(room->remote_participants().empty());
    TEST_CHECK(!canonical->SetVideoQuality(livekit::proto::VideoQuality::LOW));
    TEST_CHECK(canonical->current_quality() == livekit::proto::VideoQuality::HIGH);

    std::cout << "[SUCCESS] Remote publication lifecycle tests passed." << std::endl;
    return 0;
}
