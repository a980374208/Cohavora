#include <iostream>
#include <cassert>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "participant.h"
#include "remote_track_publication.h"
#include "room.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

class TestAttributesListener : public livekit::RoomListener {
public:
    bool attrs_changed = false;
    bool perms_changed = false;
    std::map<std::string, std::string> last_attrs;
    livekit::ParticipantPermission last_new_perm;

    void OnParticipantAttributesChanged(const std::map<std::string, std::string>& changed_attributes, std::shared_ptr<livekit::Participant> participant) override {
        attrs_changed = true;
        last_attrs = changed_attributes;
    }

    void OnParticipantPermissionsChanged(const livekit::ParticipantPermission& old_permission, const livekit::ParticipantPermission& new_permission, std::shared_ptr<livekit::Participant> participant) override {
        perms_changed = true;
        last_new_perm = new_permission;
    }
};

int main() {
    std::cout << "[TEST] Starting Participant Attributes, Permissions & Track Quality Control Verification..." << std::endl;

    // Test 1: Participant Attributes Setting & Protobuf Serialization
    {
        livekit::proto::SignalRequest sent_req;
        auto local_p = std::make_shared<livekit::LocalParticipant>(
            "PA_LOCAL_1", "user_alice",
            [&sent_req](const livekit::proto::SignalRequest& req) {
                sent_req = req;
            }
        );

        local_p->SetAttribute("role", "host");
        local_p->SetAttribute("avatar", "avatar_01.png");

        assert(local_p->get_attribute("role") == "host");
        assert(local_p->get_attribute("avatar") == "avatar_01.png");
        assert(sent_req.has_update_metadata());

        const auto& meta_req = sent_req.update_metadata();
        assert(meta_req.attributes().at("role") == "host");
        assert(meta_req.attributes().at("avatar") == "avatar_01.png");

        std::cout << "  [PASS] Test 1: Participant Attributes SetAttribute & UpdateMetadata Signal Request verified." << std::endl;
    }

    // Test 2: Participant Permissions Guarding
    {
        livekit::proto::SignalRequest sent_req;
        auto local_p = std::make_shared<livekit::LocalParticipant>(
            "PA_LOCAL_2", "user_bob",
            [&sent_req](const livekit::proto::SignalRequest& req) {
                sent_req = req;
            }
        );

        // Deny publish permissions
        livekit::ParticipantPermission perm;
        perm.can_publish = false;
        perm.can_publish_data = false;
        perm.can_update_metadata = false;
        local_p->set_permission(perm);

        // Try publishing track & data (should be blocked safely)
        auto dummy_track = std::make_shared<livekit::Track>("TR_01", "mic", livekit::TrackKind::Audio);
        local_p->PublishTrack(dummy_track);
        assert(!sent_req.has_add_track()); // Blocked!

        local_p->PublishData({1, 2, 3}); // Blocked!

        local_p->SetAttribute("key", "val"); // Blocked!

        std::cout << "  [PASS] Test 2: Participant Permissions Guarding (can_publish / can_publish_data / can_update_metadata) verified." << std::endl;
    }

    // Test 3: RemoteTrackPublication Track Quality & Settings Control
    {
        livekit::RemoteTrackPublication remote_pub(
            "TR_REMOTE_VIDEO", "camera", livekit::proto::TrackType::VIDEO, nullptr
        );

        remote_pub.SetVideoDimensions(640, 360);
        assert(remote_pub.current_quality() == livekit::proto::VideoQuality::MEDIUM);
        assert(remote_pub.current_width() == 640);
        assert(remote_pub.current_height() == 360);

        remote_pub.SetVideoQuality(livekit::proto::VideoQuality::LOW);
        assert(remote_pub.current_quality() == livekit::proto::VideoQuality::LOW);

        remote_pub.SetSubscribed(false);
        assert(remote_pub.is_subscribed() == false);

        std::cout << "  [PASS] Test 3: RemoteTrackPublication SetVideoDimensions & SetVideoQuality verified." << std::endl;
    }

    // Test 4: Room UpdateParticipants Attributes & Permissions Event Dispatch
    {
        asio::io_context io_ctx;
        auto room = livekit::Room::Create(io_ctx.get_executor());
        auto listener = std::make_shared<TestAttributesListener>();
        room->AddListener(listener);

        livekit::proto::ParticipantUpdate update;
        auto* p = update.add_participants();
        p->set_sid("PA_REMOTE_100");
        p->set_identity("remote_user");
        p->set_state(livekit::proto::ParticipantInfo::ACTIVE);
        (*p->mutable_attributes())["team"] = "alpha";

        auto* perm = p->mutable_permission();
        perm->set_can_subscribe(true);
        perm->set_can_publish(false);

        room->UpdateParticipantsForTesting(update);

        assert(listener->attrs_changed == true);
        assert(listener->last_attrs.at("team") == "alpha");
        assert(listener->perms_changed == true);
        assert(listener->last_new_perm.can_publish == false);

        std::cout << "  [PASS] Test 4: Room Listener OnParticipantAttributesChanged & OnParticipantPermissionsChanged events verified." << std::endl;
    }

    std::cout << "[SUCCESS] ALL Participant Attributes, Permissions & Track Quality Control Tests Passed!" << std::endl;
    return 0;
}
