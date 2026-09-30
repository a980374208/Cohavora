// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_room_state_events.cpp
// Original SHA256: deb11becd6aee1933d67af34f578c3e01191958e9063b3157aa44bf382bd245b
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: always-active checks (34 original expressions).

#include "tests/support/test_check.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "room.h"
#include "livekit_models.pb.h"
#include "livekit_rtc.pb.h"

namespace {

class StateEventListener final : public livekit::RoomListener {
public:
    int room_metadata_changed = 0;
    int room_updated = 0;
    int connection_quality_changed = 0;
    int stream_state_changed = 0;
    int subscription_permission_changed = 0;
    std::vector<std::string> event_order;

    livekit::RoomInfo latest_room;
    std::string old_metadata;
    std::string new_metadata;
    std::shared_ptr<livekit::Participant> quality_participant;
    livekit::ConnectionQuality latest_quality = livekit::ConnectionQuality::Unknown;
    float latest_score = 0.0f;
    std::shared_ptr<livekit::TrackPublication> stream_publication;
    livekit::TrackPublication::StreamState latest_stream_state =
        livekit::TrackPublication::StreamState::Active;
    livekit::TrackSubscriptionPermission latest_subscription_permission;

    void OnRoomMetadataChanged(const livekit::RoomInfo& room,
                               const std::string& old_value,
                               const std::string& new_value) override {
        ++room_metadata_changed;
        event_order.push_back("metadata");
        latest_room = room;
        old_metadata = old_value;
        new_metadata = new_value;
    }

    void OnRoomUpdated(const livekit::RoomInfo& room) override {
        ++room_updated;
        event_order.push_back("room");
        latest_room = room;
    }

    void OnConnectionQualityChanged(std::shared_ptr<livekit::Participant> participant,
                                    livekit::ConnectionQuality quality,
                                    float score) override {
        ++connection_quality_changed;
        quality_participant = std::move(participant);
        latest_quality = quality;
        latest_score = score;
    }

    void OnTrackStreamStateChanged(
        std::shared_ptr<livekit::Participant>,
        std::shared_ptr<livekit::TrackPublication> publication,
        livekit::TrackPublication::StreamState state) override {
        ++stream_state_changed;
        stream_publication = std::move(publication);
        latest_stream_state = state;
    }

    void OnTrackSubscriptionPermissionChanged(
        const livekit::TrackSubscriptionPermission& permission,
        std::shared_ptr<livekit::Participant>,
        std::shared_ptr<livekit::TrackPublication>) override {
        ++subscription_permission_changed;
        latest_subscription_permission = permission;
    }
};

livekit::proto::SignalResponse RoomUpdateResponse() {
    livekit::proto::SignalResponse response;
    auto* room = response.mutable_room_update()->mutable_room();
    room->set_sid("RM_100");
    room->set_name("state-event-room");
    room->set_metadata(R"({"detail":{"info":{"meetingID":"m-100"}}})");
    room->set_empty_timeout(30);
    room->set_departure_timeout(15);
    room->set_max_participants(50);
    room->set_creation_time_ms(1700000000000LL);
    room->set_num_participants(2);
    room->set_num_publishers(1);
    room->set_active_recording(true);
    return response;
}

} // namespace

int main() {
    asio::io_context io_context;
    auto room = livekit::Room::Create(io_context.get_executor());
    auto listener = std::make_shared<StateEventListener>();
    room->AddListener(listener);

    livekit::proto::ParticipantUpdate participant_update;
    auto* participant = participant_update.add_participants();
    participant->set_sid("PA_REMOTE_100");
    participant->set_identity("remote-user");
    participant->set_state(livekit::proto::ParticipantInfo::ACTIVE);
    auto* track = participant->add_tracks();
    track->set_sid("TR_REMOTE_VIDEO_100");
    track->set_name("camera");
    track->set_type(livekit::proto::TrackType::VIDEO);
    track->set_muted(false);
    room->UpdateParticipantsForTesting(participant_update);

    // Room state is committed before listeners run, and metadata notification
    // precedes the general RoomUpdated notification.
    const auto room_update = RoomUpdateResponse();
    room->HandleSignalMessageForTesting(room_update);
    const auto room_info = room->room_info();
    TEST_CHECK(room_info.sid == "RM_100");
    TEST_CHECK(room_info.name == "state-event-room");
    TEST_CHECK(room_info.num_participants == 2);
    TEST_CHECK(room_info.active_recording);
    TEST_CHECK(listener->room_metadata_changed == 1);
    TEST_CHECK(listener->room_updated == 1);
    TEST_CHECK(listener->old_metadata.empty());
    TEST_CHECK(listener->new_metadata == room_info.metadata);
    TEST_CHECK(listener->event_order.size() == 2);
    TEST_CHECK(listener->event_order[0] == "metadata");
    TEST_CHECK(listener->event_order[1] == "room");

    // Idempotent duplicate protocol events must not produce a second visible
    // state mutation.
    room->HandleSignalMessageForTesting(room_update);
    TEST_CHECK(listener->room_metadata_changed == 1);
    TEST_CHECK(listener->room_updated == 1);

    livekit::proto::SignalResponse quality_response;
    auto* quality = quality_response.mutable_connection_quality()->add_updates();
    quality->set_participant_sid("PA_REMOTE_100");
    quality->set_quality(livekit::proto::ConnectionQuality::GOOD);
    quality->set_score(0.82f);
    room->HandleSignalMessageForTesting(quality_response);

    const auto remote = room->remote_participants().at("PA_REMOTE_100");
    TEST_CHECK(remote->connection_quality() == livekit::ConnectionQuality::Good);
    TEST_CHECK(remote->connection_quality_score() == 0.82f);
    TEST_CHECK(listener->connection_quality_changed == 1);
    TEST_CHECK(listener->quality_participant == remote);
    TEST_CHECK(listener->latest_quality == livekit::ConnectionQuality::Good);
    room->HandleSignalMessageForTesting(quality_response);
    TEST_CHECK(listener->connection_quality_changed == 1);

    livekit::proto::SignalResponse stream_response;
    auto* stream = stream_response.mutable_stream_state_update()->add_stream_states();
    stream->set_participant_sid("PA_REMOTE_100");
    stream->set_track_sid("TR_REMOTE_VIDEO_100");
    stream->set_state(livekit::proto::StreamState::PAUSED);
    room->HandleSignalMessageForTesting(stream_response);

    const auto publication = remote->get_publication("TR_REMOTE_VIDEO_100");
    TEST_CHECK(publication);
    TEST_CHECK(publication->stream_state() == livekit::TrackPublication::StreamState::Paused);
    TEST_CHECK(listener->stream_state_changed == 1);
    TEST_CHECK(listener->stream_publication == publication);
    room->HandleSignalMessageForTesting(stream_response);
    TEST_CHECK(listener->stream_state_changed == 1);

    livekit::proto::SignalResponse permission_response;
    auto* permission = permission_response.mutable_subscription_permission_update();
    permission->set_participant_sid("PA_REMOTE_100");
    permission->set_track_sid("TR_REMOTE_VIDEO_100");
    permission->set_allowed(false);
    room->HandleSignalMessageForTesting(permission_response);

    const auto cached_permission = room->track_subscription_permission(
        "PA_REMOTE_100", "TR_REMOTE_VIDEO_100");
    TEST_CHECK(cached_permission.has_value());
    TEST_CHECK(!cached_permission->allowed);
    TEST_CHECK(!publication->subscription_allowed());
    TEST_CHECK(listener->subscription_permission_changed == 1);
    TEST_CHECK(listener->latest_subscription_permission.participant_sid == "PA_REMOTE_100");
    TEST_CHECK(listener->latest_subscription_permission.track_sid == "TR_REMOTE_VIDEO_100");
    TEST_CHECK(!listener->latest_subscription_permission.allowed);
    room->HandleSignalMessageForTesting(permission_response);
    TEST_CHECK(listener->subscription_permission_changed == 1);

    // Unknown participants and tracks are ignored rather than creating a
    // phantom Participant/TrackPublication.
    livekit::proto::SignalResponse unknown_stream_response;
    auto* unknown_stream = unknown_stream_response.mutable_stream_state_update()->add_stream_states();
    unknown_stream->set_participant_sid("PA_UNKNOWN");
    unknown_stream->set_track_sid("TR_UNKNOWN");
    unknown_stream->set_state(livekit::proto::StreamState::PAUSED);
    room->HandleSignalMessageForTesting(unknown_stream_response);
    TEST_CHECK(listener->stream_state_changed == 1);
    TEST_CHECK(room->remote_participants().size() == 1);

    std::cout << "[SUCCESS] Room server state-event closure tests passed." << std::endl;
    return 0;
}
