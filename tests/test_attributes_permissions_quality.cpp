#include <iostream>
#include "tests/support/test_check.h"
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

        TEST_CHECK(local_p->get_attribute("role") == "host");
        TEST_CHECK(local_p->get_attribute("avatar") == "avatar_01.png");
        TEST_CHECK(sent_req.has_update_metadata());

        const auto& meta_req = sent_req.update_metadata();
        TEST_CHECK(meta_req.attributes().at("role") == "host");
        TEST_CHECK(meta_req.attributes().at("avatar") == "avatar_01.png");

        std::cout << "  [PASS] Test 1: Participant Attributes SetAttribute & UpdateMetadata Signal Request verified." << std::endl;
    }

    // Test 2: Participant Permissions Guarding
    {
        std::vector<livekit::proto::SignalRequest> requests;
        int data_calls = 0;
        const std::vector<uint8_t> payload{1, 2, 3};
        auto local_p = std::make_shared<livekit::LocalParticipant>(
            "PA_LOCAL_2", "user_bob",
            [&](const livekit::proto::SignalRequest& req) { requests.push_back(req); });
        local_p->SetPublishDataHandler([&](const std::vector<uint8_t>& data,
            bool reliable, const std::vector<std::string>& destinations,
            const std::string& topic) {
            ++data_calls;
            TEST_CHECK(data == payload && reliable);
            TEST_CHECK(destinations.empty() && topic.empty());
        });
        // Seed existing state so denied metadata updates must preserve it.
        local_p->SetAttribute("key", "before");
        requests.clear();
        livekit::ParticipantPermission perm;
        perm.can_publish = false;
        perm.can_publish_data = false;
        perm.can_update_metadata = false;
        local_p->set_permission(perm);
        auto track = std::make_shared<livekit::Track>("TR_01", "mic", livekit::TrackKind::Audio);
        local_p->PublishTrack(track);
        TEST_CHECK(requests.empty());
        TEST_CHECK(local_p->tracks().empty());
        local_p->PublishData(payload);
        TEST_CHECK(data_calls == 0);
        local_p->SetAttribute("key", "after");
        TEST_CHECK(requests.empty());
        TEST_CHECK(local_p->get_attribute("key") == "before");

        // Positive controls prove that each denied path had a working sink.
        perm.can_publish = true;
        perm.can_publish_data = true;
        perm.can_update_metadata = true;
        local_p->set_permission(perm);
        local_p->PublishTrack(track);
        TEST_CHECK(requests.size() == 1 && requests.back().has_add_track());
        TEST_CHECK(local_p->tracks().size() == 1);
        local_p->PublishData(payload);
        TEST_CHECK(data_calls == 1);
        local_p->SetAttribute("key", "after");
        TEST_CHECK(requests.size() == 2 && requests.back().has_update_metadata());
        TEST_CHECK(requests.back().update_metadata().attributes().at("key") == "after");
        TEST_CHECK(local_p->get_attribute("key") == "after");
        std::cout << "  [PASS] Test 2: Denied operations preserve state and emit nothing; allowed controls reach every sink.\n";
    }

    // Test 3: RemoteTrackPublication Track Quality & Settings Control
    {
        livekit::RemoteTrackPublication remote_pub(
            "TR_REMOTE_VIDEO", "camera", livekit::proto::TrackType::VIDEO, nullptr
        );

        remote_pub.SetVideoQuality(livekit::proto::VideoQuality::LOW);
        TEST_CHECK(remote_pub.current_quality() == livekit::proto::VideoQuality::LOW);

        remote_pub.SetSubscribed(false);
        TEST_CHECK(remote_pub.is_subscribed() == false);

        std::cout << "  [PASS] Test 3: RemoteTrackPublication SetVideoQuality & SetSubscribed verified." << std::endl;
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
        auto* perm = p->mutable_permission();
        perm->set_can_subscribe(true);
        perm->set_can_publish(true);

        // Joining installs the initial snapshot. Change callbacks describe a
        // later update to an existing participant, not that initial snapshot.
        room->UpdateParticipantsForTesting(update);
        TEST_CHECK(room->remote_participants().size() == 1);
        TEST_CHECK(!listener->attrs_changed);
        TEST_CHECK(!listener->perms_changed);

        (*p->mutable_attributes())["team"] = "alpha";
        perm->set_can_publish(false);

        room->UpdateParticipantsForTesting(update);

        TEST_CHECK(listener->attrs_changed == true);
        TEST_CHECK(listener->last_attrs.at("team") == "alpha");
        TEST_CHECK(listener->perms_changed == true);
        TEST_CHECK(listener->last_new_perm.can_publish == false);

        std::cout << "  [PASS] Test 4: Room Listener OnParticipantAttributesChanged & OnParticipantPermissionsChanged events verified." << std::endl;
    }

    std::cout << "[SUCCESS] ALL Participant Attributes, Permissions & Track Quality Control Tests Passed!" << std::endl;
    return 0;
}
