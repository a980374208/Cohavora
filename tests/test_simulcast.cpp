#include <iostream>
#include <cassert>
#include <memory>
#include "local_video_track.h"
#include "video_source.h"
#include "participant.h"
#include "livekit_rtc.pb.h"

int main() {
    std::cout << "[TEST] Starting VP8 Simulcast Unit Tests..." << std::endl;

    // Test 1: Default 720p Simulcast options (3 layers: f, h, q)
    {
        auto opts720p = livekit::LocalVideoTrack::DefaultVp8SimulcastOptions(1280, 720);
        assert(opts720p.simulcast == true);
        assert(opts720p.layers.size() == 3);
        
        assert(opts720p.layers[0].rid == "f");
        assert(opts720p.layers[0].width == 1280);
        assert(opts720p.layers[0].height == 720);
        assert(opts720p.layers[0].scale_resolution_down_by == 1.0);

        assert(opts720p.layers[1].rid == "h");
        assert(opts720p.layers[1].width == 640);
        assert(opts720p.layers[1].height == 360);
        assert(opts720p.layers[1].scale_resolution_down_by == 2.0);

        assert(opts720p.layers[2].rid == "q");
        assert(opts720p.layers[2].width == 320);
        assert(opts720p.layers[2].height == 180);
        assert(opts720p.layers[2].scale_resolution_down_by == 4.0);

        std::cout << "  [PASS] Test 1: 720p Default 3-Layer VP8 Simulcast verified." << std::endl;
    }

    // Test 2: Default 480p Simulcast options (2 layers: f, q)
    {
        auto opts480p = livekit::LocalVideoTrack::DefaultVp8SimulcastOptions(640, 480);
        assert(opts480p.simulcast == true);
        assert(opts480p.layers.size() == 2);

        assert(opts480p.layers[0].rid == "f");
        assert(opts480p.layers[0].width == 640);
        assert(opts480p.layers[0].height == 480);

        assert(opts480p.layers[1].rid == "q");
        assert(opts480p.layers[1].width == 320);
        assert(opts480p.layers[1].height == 240);

        std::cout << "  [PASS] Test 2: 480p Default 2-Layer VP8 Simulcast verified." << std::endl;
    }

    // Test 3: LocalParticipant PublishTrack Protobuf layers serialization
    {
        auto vsrc = std::make_shared<livekit::VideoSource>(1280, 720);
        auto vtrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_track", vsrc);

        livekit::proto::SignalRequest sent_req;
        auto local_p = std::make_shared<livekit::LocalParticipant>(
            "PA_LOCAL_1", "test_identity",
            [&sent_req](const livekit::proto::SignalRequest& req) {
                sent_req = req;
            }
        );

        local_p->PublishTrack(vtrack);

        assert(sent_req.has_add_track());
        const auto& add_t = sent_req.add_track();
        assert(add_t.name() == "camera_track");
        assert(add_t.type() == livekit::proto::TrackType::VIDEO);
        assert(add_t.layers_size() == 3);

        assert(add_t.layers(0).quality() == livekit::proto::VideoQuality::HIGH);
        assert(add_t.layers(0).width() == 1280);
        assert(add_t.layers(0).height() == 720);
        assert(add_t.layers(0).rid() == "f");
        assert(add_t.layers(0).spatial_layer() == 2);

        assert(add_t.layers(1).quality() == livekit::proto::VideoQuality::MEDIUM);
        assert(add_t.layers(1).width() == 640);
        assert(add_t.layers(1).height() == 360);
        assert(add_t.layers(1).rid() == "h");
        assert(add_t.layers(1).spatial_layer() == 1);

        assert(add_t.layers(2).quality() == livekit::proto::VideoQuality::LOW);
        assert(add_t.layers(2).width() == 320);
        assert(add_t.layers(2).height() == 180);
        assert(add_t.layers(2).rid() == "q");
        assert(add_t.layers(2).spatial_layer() == 0);

        assert(add_t.simulcast_codecs_size() == 1);
        assert(add_t.simulcast_codecs(0).codec() == "vp8");
        assert(add_t.simulcast_codecs(0).layers_size() == 3);

        std::cout << "  [PASS] Test 3: LocalParticipant PublishTrack VideoLayers & SimulcastCodec Protobuf serialization verified." << std::endl;
    }

    std::cout << "[SUCCESS] ALL VP8 Simulcast Unit Tests Passed!" << std::endl;
    return 0;
}
