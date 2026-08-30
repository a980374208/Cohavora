#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <string>
#include <asio.hpp>
#include "room.h"
#include "participant.h"
#include "local_video_track.h"
#include "video_source.h"

class TestDataListener : public livekit::RoomListener {
public:
    bool received = false;
    std::vector<uint8_t> received_payload;
    std::string received_topic;

    void OnDataReceived(const std::vector<uint8_t>& payload, std::shared_ptr<livekit::RemoteParticipant> participant, const std::string& topic) override {
        received = true;
        received_payload = payload;
        received_topic = topic;
    }
};

int main() {
    std::cout << "[TEST] Starting Data Packet Chunking & Simulcast Verification Tests..." << std::endl;

    asio::io_context io_ctx;
    auto room = livekit::Room::Create(io_ctx.get_executor());

    auto listener = std::make_shared<TestDataListener>();
    room->AddListener(listener);

    // Test 1: Small user packets wrapped in DataPacket still reach raw data listeners.
    {
        const std::vector<uint8_t> payload{'h', 'e', 'l', 'l', 'o'};
        room->PublishData(payload, /*reliable=*/true, {}, "test.small");

        assert(listener->received);
        assert(listener->received_topic == "test.small");
        assert(listener->received_payload == payload);
        std::cout << "  [PASS] Test 1: Small DataPacket payload dispatch verified successfully!" << std::endl;
    }

    listener->received = false;
    listener->received_payload.clear();
    listener->received_topic.clear();

    // Test 2: Data Packet Chunking (Sending 100 KB payload)
    {
        std::vector<uint8_t> large_payload(100 * 1024);
        for (size_t i = 0; i < large_payload.size(); ++i) {
            large_payload[i] = static_cast<uint8_t>(i % 256);
        }

        std::string topic = "test.large_chunking";
        room->PublishData(large_payload, /*reliable=*/true, {}, topic);

        assert(listener->received == true);
        assert(listener->received_topic == topic);
        assert(listener->received_payload.size() == large_payload.size());
        assert(listener->received_payload == large_payload);

        std::cout << "  [PASS] Test 2: Data Packet Chunking 100KB payload assembly verified successfully!" << std::endl;
    }

    // Test 3: Simulcast Video Track Options & ApplySimulcastParameters
    {
        auto vsrc = std::make_shared<livekit::VideoSource>(1280, 720);
        auto vtrack = livekit::LocalVideoTrack::createLocalVideoTrack("simulcast_cam", vsrc);

        assert(vtrack != nullptr);
        auto opts = vtrack->publish_options();
        assert(opts.simulcast == true);
        assert(opts.layers.size() == 3);

        std::cout << "  [PASS] Test 3: Simulcast Video Track options and layers initialized correctly!" << std::endl;
    }

    std::cout << "[SUCCESS] ALL Data Packet Chunking & Simulcast Verification Tests Passed!" << std::endl;
    return 0;
}
