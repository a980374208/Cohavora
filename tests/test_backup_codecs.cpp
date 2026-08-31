#include <iostream>
#include <cassert>
#include <memory>
#include "local_video_track.h"
#include "video_source.h"
#include "participant.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  LiveKit Native C++ SDK Backup Codecs & Multi-Codec Suite" << std::endl;
    std::cout << "========================================================" << std::endl;

    // Test 1: AV1 Primary + Auto VP8 Backup Codec calculation
    {
        std::cout << "  -> Test 1: AV1 Primary + Auto VP8 Backup Codec calculation..." << std::endl;
        livekit::VideoPublishOptions av1_opts;
        av1_opts.video_codec = "av1";
        av1_opts.simulcast = true;
        av1_opts.auto_backup_codec = true;

        auto computed = livekit::LocalVideoTrack::ComputeMultiCodecSimulcastOptions(1280, 720, av1_opts);
        assert(computed.simulcast == true);
        assert(computed.video_codec == "av1");
        assert(computed.backup_codec.has_value());
        assert(computed.backup_codec.value() == "vp8");
        assert(computed.simulcast_codecs.size() == 2);

        // Primary AV1 Spec
        const auto& pri = computed.simulcast_codecs[0];
        assert(pri.codec == "av1");
        assert(pri.layers.size() == 3);
        assert(pri.layers[0].max_bitrate_bps == static_cast<int>(1700000 * 0.70)); // AV1 factor 0.7x

        // Backup VP8 Spec
        const auto& bak = computed.simulcast_codecs[1];
        assert(bak.codec == "vp8");
        assert(bak.layers.size() == 3);
        assert(bak.layers[0].max_bitrate_bps == 1700000); // VP8 factor 1.0x

        std::cout << "     [PASS] AV1 primary (0.7x) + VP8 backup (1.0x) specs correctly computed." << std::endl;
    }

    // Test 2: VP9 Primary + Custom H264 Backup Codec
    {
        std::cout << "  -> Test 2: VP9 Primary + Custom H264 Backup Codec..." << std::endl;
        livekit::VideoPublishOptions vp9_opts;
        vp9_opts.video_codec = "vp9";
        vp9_opts.simulcast = true;
        vp9_opts.backup_codec = "h264";
        vp9_opts.backup_codec_policy = livekit::BackupCodecPolicy::Simulcast;

        auto computed = livekit::LocalVideoTrack::ComputeMultiCodecSimulcastOptions(1280, 720, vp9_opts);
        assert(computed.simulcast_codecs.size() == 2);
        assert(computed.simulcast_codecs[0].codec == "vp9");
        assert(computed.simulcast_codecs[1].codec == "h264");
        assert(computed.backup_codec_policy == livekit::BackupCodecPolicy::Simulcast);

        std::cout << "     [PASS] VP9 + H264 multi-codec simulcast options verified." << std::endl;
    }

    // Test 3: Protobuf AddTrackRequest Multi-Codec & BackupCodecPolicy Serialization
    {
        std::cout << "  -> Test 3: Protobuf AddTrackRequest Multi-Codec & Policy Serialization..." << std::endl;
        auto vsrc = std::make_shared<livekit::VideoSource>(1280, 720);
        
        livekit::VideoPublishOptions opts;
        opts.video_codec = "av1";
        opts.simulcast = true;
        opts.backup_codec = "vp8";
        opts.backup_codec_policy = livekit::BackupCodecPolicy::PreferRegression;

        auto vtrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_av1", vsrc, livekit::TrackSource::Camera, opts);

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
        assert(add_t.name() == "camera_av1");
        assert(add_t.type() == livekit::proto::TrackType::VIDEO);
        assert(add_t.backup_codec_policy() == livekit::proto::BackupCodecPolicy::PREFER_REGRESSION);

        // Verify 2 Simulcast Codecs
        assert(add_t.simulcast_codecs_size() == 2);
        
        // Codec 0: AV1
        const auto& c0 = add_t.simulcast_codecs(0);
        assert(c0.codec() == "av1");
        assert(c0.cid() == add_t.cid());
        assert(c0.layers_size() == 3);

        // Codec 1: VP8 Backup
        const auto& c1 = add_t.simulcast_codecs(1);
        assert(c1.codec() == "vp8");
        assert(c1.cid() == add_t.cid() + "_backup");
        assert(c1.layers_size() == 3);

        std::cout << "     [PASS] AddTrackRequest serialized 2 codecs (AV1 + VP8_backup) with PREFER_REGRESSION policy." << std::endl;
    }

    // Test 4: Simulcast Policy Mapping
    {
        std::cout << "  -> Test 4: Simulcast Policy Mapping..." << std::endl;
        auto vsrc = std::make_shared<livekit::VideoSource>(1280, 720);
        
        livekit::VideoPublishOptions opts;
        opts.video_codec = "vp9";
        opts.simulcast = true;
        opts.backup_codec = "vp8";
        opts.backup_codec_policy = livekit::BackupCodecPolicy::Simulcast;

        auto vtrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_vp9", vsrc, livekit::TrackSource::Camera, opts);

        livekit::proto::SignalRequest sent_req;
        auto local_p = std::make_shared<livekit::LocalParticipant>(
            "PA_LOCAL_2", "test_identity_2",
            [&sent_req](const livekit::proto::SignalRequest& req) {
                sent_req = req;
            }
        );

        local_p->PublishTrack(vtrack);
        assert(sent_req.add_track().backup_codec_policy() == livekit::proto::BackupCodecPolicy::SIMULCAST);

        std::cout << "     [PASS] SIMULCAST policy correctly serialized in AddTrackRequest." << std::endl;
    }

    std::cout << "[SUCCESS] ALL Backup Codecs & Multi-Codec Unit Tests Passed!" << std::endl;
    return 0;
}
