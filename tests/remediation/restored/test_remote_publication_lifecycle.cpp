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

    TEST_CHECK(publication->SetVideoDimensions(640, 360));
    TEST_CHECK(publication->current_width() == 640);
    TEST_CHECK(publication->current_height() == 360);
    TEST_CHECK(publication->current_quality() == livekit::proto::VideoQuality::MEDIUM);
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
    room->UpdateParticipantsForTesting(update);

    const auto room_participant = room->remote_participants().at("PA_REMOTE_201");
    const auto canonical = room_participant->get_remote_publication("TR_REMOTE_VIDEO_201");
    TEST_CHECK(canonical);
    TEST_CHECK(room_participant->get_publication(canonical->sid()) == canonical);
    TEST_CHECK(!canonical->SetPriority(3));
    TEST_CHECK(canonical->priority() == 0);

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
