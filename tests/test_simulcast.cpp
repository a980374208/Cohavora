#include <iostream>
#include "tests/support/test_check.h"
#include <memory>
#include <cmath>
#include <vector>
#include "local_video_track.h"
#include "video_source.h"
#include "participant.h"
#include "livekit_rtc.pb.h"

// Expected outputs from client-sdk-cpp's unmodified Rust calculation bodies.
// Rust HEAD: a0c91f5ae2309f3911c4a5d5843f385e6f20846a
// livekit/src/room/options.rs SHA256:
// ed535f7cc5266319d8a7fdd0b911d4034cb76007eb7f44ed5debc8e813518a41
// Includes strict preset boundaries, portrait/odd dimensions, codec bitrate
// factors, screen-share defaults and explicit single-encoding modes.
namespace {

struct EncodingCase {
    int width, height;
    livekit::TrackSource source;
    const char* codec;
    bool simulcast;
    const char* mode;
    std::vector<livekit::VideoLayerSetting> expected;
};

void CheckLayers(const std::vector<livekit::VideoLayerSetting>& actual,
                 const std::vector<livekit::VideoLayerSetting>& expected) {
    TEST_CHECK(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_CHECK(actual[i].width == expected[i].width);
        TEST_CHECK(actual[i].height == expected[i].height);
        TEST_CHECK(actual[i].rid == expected[i].rid);
        TEST_CHECK(actual[i].max_bitrate_bps == expected[i].max_bitrate_bps);
        TEST_CHECK(actual[i].max_fps == expected[i].max_fps);
        TEST_CHECK(std::abs(actual[i].scale_resolution_down_by - expected[i].scale_resolution_down_by) < 1e-12);
    }
}

void TestRustEncodingPolicy() {
    using Source = livekit::TrackSource;
    const EncodingCase cases[] = {
        {320, 240, Source::Camera, "vp8", true, "",
            {{320, 240, 225000, 20, "q", 1.0}}},
        {479, 359, Source::Camera, "vp8", true, "",
            {{479, 359, 225000, 20, "q", 1.0}}},
        {480, 360, Source::Camera, "vp8", true, "",
            {{480, 360, 300000, 20, "h", 1.0}, {480, 360, 225000, 20, "q", 1.0}}},
        {640, 480, Source::Camera, "vp8", true, "",
            {{640, 480, 450000, 25, "h", 1.0}, {480, 360, 225000, 20, "q", 1.3333333333333333}}},
        {480, 640, Source::Camera, "vp8", true, "",
            {{480, 640, 450000, 25, "h", 1.0}, {360, 480, 225000, 20, "q", 1.3333333333333333}}},
        {800, 600, Source::Camera, "vp8", true, "",
            {{800, 600, 1500000, 30, "h", 1.0}, {480, 360, 225000, 20, "q", 1.6666666666666667}}},
        {960, 720, Source::Camera, "vp8", true, "",
            {{960, 720, 2500000, 30, "f", 1.0}, {480, 360, 225000, 20, "h", 2.0}, {240, 180, 100000, 15, "q", 4.0}}},
        {640, 360, Source::Camera, "vp8", true, "",
            {{640, 360, 800000, 25, "h", 1.0}, {640, 360, 450000, 20, "q", 1.0}}},
        {360, 640, Source::Camera, "vp8", true, "",
            {{360, 640, 800000, 25, "h", 1.0}, {360, 640, 450000, 20, "q", 1.0}}},
        {854, 480, Source::Camera, "vp8", true, "",
            {{854, 480, 800000, 25, "h", 1.0}, {640, 360, 450000, 20, "q", 1.3333333333333333}}},
        {853, 479, Source::Camera, "vp8", true, "",
            {{853, 479, 800000, 25, "h", 1.0}, {641, 360, 450000, 20, "q", 1.3305555555555555}}},
        {1280, 720, Source::Camera, "vp8", true, "",
            {{1280, 720, 3000000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 2.0}, {320, 180, 160000, 15, "q", 4.0}}},
        {1920, 1080, Source::Camera, "vp8", true, "",
            {{1920, 1080, 5000000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 3.0}, {320, 180, 160000, 15, "q", 6.0}}},
        {1080, 1920, Source::Camera, "vp8", true, "",
            {{1080, 1920, 5000000, 30, "f", 1.0}, {360, 640, 450000, 20, "h", 3.0}, {180, 320, 160000, 15, "q", 6.0}}},
        {3840, 2160, Source::Camera, "vp8", true, "",
            {{3840, 2160, 8000000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 6.0}, {320, 180, 160000, 15, "q", 12.0}}},
        {640, 480, Source::Camera, "h264", true, "",
            {{640, 480, 450000, 25, "h", 1.0}, {480, 360, 225000, 20, "q", 1.3333333333333333}}},
        {1280, 720, Source::Camera, "h264", true, "",
            {{1280, 720, 3000000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 2.0}, {320, 180, 160000, 15, "q", 4.0}}},
        {640, 480, Source::Camera, "vp9", true, "",
            {{640, 480, 382500, 25, "h", 1.0}, {480, 360, 225000, 20, "q", 1.3333333333333333}}},
        {1280, 720, Source::Camera, "vp9", true, "",
            {{1280, 720, 2550000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 2.0}, {320, 180, 160000, 15, "q", 4.0}}},
        {640, 480, Source::Camera, "av1", true, "",
            {{640, 480, 315000, 25, "h", 1.0}, {480, 360, 225000, 20, "q", 1.3333333333333333}}},
        {1280, 720, Source::Camera, "av1", true, "",
            {{1280, 720, 2100000, 30, "f", 1.0}, {640, 360, 450000, 20, "h", 2.0}, {320, 180, 160000, 15, "q", 4.0}}},
        {320, 240, Source::ScreenShareVideo, "vp8", true, "",
            {{320, 240, 200000, 3, "q", 1.0}}},
        {640, 480, Source::ScreenShareVideo, "vp8", true, "",
            {{640, 480, 400000, 5, "h", 1.0}, {320, 240, 150000, 3, "q", 2.0}}},
        {1280, 720, Source::ScreenShareVideo, "vp8", true, "",
            {{1280, 720, 1500000, 15, "h", 1.0}, {640, 360, 150000, 3, "q", 2.0}}},
        {1281, 721, Source::ScreenShareVideo, "vp8", true, "",
            {{1281, 721, 1500000, 15, "h", 1.0}, {639, 360, 150000, 3, "q", 2.0027777777777778}}},
        {1920, 1080, Source::ScreenShareVideo, "vp8", true, "",
            {{1920, 1080, 3000000, 30, "h", 1.0}, {960, 540, 150000, 3, "q", 2.0}}},
        {3840, 2160, Source::ScreenShareVideo, "vp8", true, "",
            {{3840, 2160, 3000000, 30, "h", 1.0}, {1920, 1080, 150000, 3, "q", 2.0}}},
        {1920, 1080, Source::ScreenShareVideo, "av1", true, "",
            {{1920, 1080, 2100000, 30, "h", 1.0}, {960, 540, 150000, 3, "q", 2.0}}},
        {640, 480, Source::Camera, "vp8", false, "",
            {{640, 480, 450000, 25, "q", 1.0}}},
        {1280, 720, Source::Camera, "av1", false, "",
            {{1280, 720, 2100000, 30, "q", 1.0}}},
        {1920, 1080, Source::ScreenShareVideo, "vp8", false, "",
            {{1920, 1080, 3000000, 30, "q", 1.0}}},
        {1280, 720, Source::Camera, "av1", true, "L3T3_KEY",
            {{1280, 720, 2100000, 30, "q", 1.0}}},
        {1920, 1080, Source::ScreenShareVideo, "vp9", true, "L1T3",
            {{1920, 1080, 2550000, 30, "q", 1.0}}},
    };
    for (const auto& c : cases) {
        std::cout << "[Rust policy] " << c.width << "x" << c.height << " " << c.codec
                  << " source=" << static_cast<int>(c.source) << " simulcast=" << c.simulcast
                  << " mode=" << c.mode << std::endl;
        livekit::VideoPublishOptions input;
        input.source = c.source;
        input.video_codec = c.codec;
        input.simulcast = c.simulcast;
        input.scalability_mode = c.mode;
        const auto actual = livekit::LocalVideoTrack::ComputeMultiCodecSimulcastOptions(c.width, c.height, input);
        TEST_CHECK(actual.source == c.source);
        TEST_CHECK(actual.simulcast == c.simulcast);
        TEST_CHECK(actual.scalability_mode == c.mode);
        CheckLayers(actual.layers, c.expected);
        TEST_CHECK(!actual.simulcast_codecs.empty());
        TEST_CHECK(actual.simulcast_codecs[0].codec == c.codec);
        TEST_CHECK(actual.simulcast_codecs[0].scalability_mode == c.mode);
        CheckLayers(actual.simulcast_codecs[0].layers, c.expected);
    }

    // The convenience entry point must use the same camera defaults.
    CheckLayers(livekit::LocalVideoTrack::DefaultVp8SimulcastOptions(640, 480).layers,
                {{640, 480, 450000, 25, "h", 1.0}, {480, 360, 225000, 20, "q", 4.0 / 3.0}});
}

} // namespace

int main() {
    TestRustEncodingPolicy();

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

        TEST_CHECK(sent_req.has_add_track());
        const auto& add_t = sent_req.add_track();
        TEST_CHECK(add_t.name() == "camera_track");
        TEST_CHECK(add_t.type() == livekit::proto::TrackType::VIDEO);
        TEST_CHECK(add_t.layers_size() == 3);

        // The wire contract orders layers by ascending spatial index (q,h,f),
        // whereas publish_options stores the highest layer first (f,h,q).
        TEST_CHECK(add_t.layers(2).quality() == livekit::proto::VideoQuality::HIGH);
        TEST_CHECK(add_t.layers(2).width() == 1280);
        TEST_CHECK(add_t.layers(2).height() == 720);
        TEST_CHECK(add_t.layers(2).rid() == "f");
        TEST_CHECK(add_t.layers(2).spatial_layer() == 2);

        TEST_CHECK(add_t.layers(1).quality() == livekit::proto::VideoQuality::MEDIUM);
        TEST_CHECK(add_t.layers(1).width() == 640);
        TEST_CHECK(add_t.layers(1).height() == 360);
        TEST_CHECK(add_t.layers(1).rid() == "h");
        TEST_CHECK(add_t.layers(1).spatial_layer() == 1);

        TEST_CHECK(add_t.layers(0).quality() == livekit::proto::VideoQuality::LOW);
        TEST_CHECK(add_t.layers(0).width() == 320);
        TEST_CHECK(add_t.layers(0).height() == 180);
        TEST_CHECK(add_t.layers(0).rid() == "q");
        TEST_CHECK(add_t.layers(0).spatial_layer() == 0);

        TEST_CHECK(add_t.simulcast_codecs_size() == 1);
        TEST_CHECK(add_t.simulcast_codecs(0).codec() == "vp8");
        TEST_CHECK(add_t.simulcast_codecs(0).layers_size() == 3);

        std::cout << "  [PASS] Test 3: LocalParticipant PublishTrack VideoLayers & SimulcastCodec Protobuf serialization verified." << std::endl;
    }

    // Test 4: 480p dimensions must reach both the primary and backup codec's
    // AddTrackRequest layers, not only the standalone options calculator.
    for (const auto* codec : {"vp8", "av1"}) {
        auto source = std::make_shared<livekit::VideoSource>(640, 480);
        livekit::VideoPublishOptions options;
        options.video_codec = codec;
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
            "camera_480p", source, livekit::TrackSource::Camera, options);
        livekit::proto::SignalRequest sent;
        auto participant = std::make_shared<livekit::LocalParticipant>(
            "PA_480P", "camera_480p",
            [&sent](const livekit::proto::SignalRequest& request) { sent = request; });
        participant->PublishTrack(track);

        TEST_CHECK(sent.has_add_track());
        const auto& add = sent.add_track();
        TEST_CHECK(add.width() == 640);
        TEST_CHECK(add.height() == 480);
        TEST_CHECK(add.layers_size() == 2);
        TEST_CHECK(add.layers(0).rid() == "q");
        TEST_CHECK(add.layers(0).width() == 480);
        TEST_CHECK(add.layers(0).height() == 360);
        TEST_CHECK(add.layers(1).rid() == "h");
        TEST_CHECK(add.layers(1).width() == 640);
        TEST_CHECK(add.layers(1).height() == 480);
        const bool has_backup = options.video_codec == "av1";
        TEST_CHECK(add.layers(0).quality() == livekit::proto::VideoQuality::LOW);
        TEST_CHECK(add.layers(0).spatial_layer() == 0);
        TEST_CHECK(add.layers(0).bitrate() == 225000);
        TEST_CHECK(add.layers(1).quality() == livekit::proto::VideoQuality::MEDIUM);
        TEST_CHECK(add.layers(1).spatial_layer() == 1);
        TEST_CHECK(add.layers(1).bitrate() == (has_backup ? 315000 : 450000));
        TEST_CHECK(add.simulcast_codecs_size() == (has_backup ? 2 : 1));
        TEST_CHECK(add.simulcast_codecs(0).codec() == options.video_codec);
        if (has_backup) TEST_CHECK(add.simulcast_codecs(1).codec() == "vp8");
        for (const auto& spec : add.simulcast_codecs()) {
            TEST_CHECK(spec.layers_size() == 2);
            TEST_CHECK(spec.layers(0).rid() == "q");
            TEST_CHECK(spec.layers(0).width() == 480);
            TEST_CHECK(spec.layers(0).height() == 360);
            TEST_CHECK(spec.layers(0).bitrate() == 225000);
            TEST_CHECK(spec.layers(1).rid() == "h");
            TEST_CHECK(spec.layers(1).width() == 640);
            TEST_CHECK(spec.layers(1).height() == 480);
            TEST_CHECK(spec.layers(1).bitrate() == (spec.codec() == "av1" ? 315000 : 450000));
        }
    }

    std::cout << "[SUCCESS] Rust encoding policy and publication serialization tests passed!" << std::endl;
    return 0;
}
