#include <iostream>
#include "tests/support/test_check.h"
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
        livekit::VideoPublishOptions requested;
        requested.video_codec = "auto";
        requested.auto_backup_codec = false;
        const auto plan = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, requested, {"AV1", "VP9", "VP8"}, {"VP9", "VP8"});
        TEST_CHECK(plan.ok());
        TEST_CHECK(plan.requested_codec == "auto");
        TEST_CHECK(plan.effective_codec == "vp8");
        TEST_CHECK(!plan.used_fallback());
        TEST_CHECK(plan.fallback_reason.empty());
    }
    {
        std::cout << "  -> Test 1: AV1 Primary + Auto VP8 Backup Codec calculation..." << std::endl;
        livekit::VideoPublishOptions av1_opts;
        av1_opts.video_codec = "av1";
        av1_opts.simulcast = true;
        av1_opts.auto_backup_codec = true;

        auto computed = livekit::LocalVideoTrack::ComputeMultiCodecSimulcastOptions(1280, 720, av1_opts);
        TEST_CHECK(computed.simulcast == true);
        TEST_CHECK(computed.video_codec == "av1");
        TEST_CHECK(computed.backup_codec.has_value());
        TEST_CHECK(computed.backup_codec.value() == "vp8");
        TEST_CHECK(computed.simulcast_codecs.size() == 2);

        // Primary AV1 Spec
        const auto& pri = computed.simulcast_codecs[0];
        TEST_CHECK(pri.codec == "av1");
        TEST_CHECK(pri.layers.size() == 3);
        // The pinned Rust policy selects the first preset wider than 1280:
        // 3 Mbps, with the codec factor applied only to the source layer.
        TEST_CHECK(pri.layers[0].max_bitrate_bps == 2100000);
        TEST_CHECK(pri.layers[1].max_bitrate_bps == 450000);
        TEST_CHECK(pri.layers[2].max_bitrate_bps == 160000);

        // Backup VP8 Spec
        const auto& bak = computed.simulcast_codecs[1];
        TEST_CHECK(bak.codec == "vp8");
        TEST_CHECK(bak.layers.size() == 3);
        TEST_CHECK(bak.layers[0].max_bitrate_bps == 3000000);
        TEST_CHECK(bak.layers[1].max_bitrate_bps == 450000);
        TEST_CHECK(bak.layers[2].max_bitrate_bps == 160000);

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
        TEST_CHECK(computed.simulcast_codecs.size() == 2);
        TEST_CHECK(computed.simulcast_codecs[0].codec == "vp9");
        TEST_CHECK(computed.simulcast_codecs[1].codec == "h264");
        TEST_CHECK(computed.backup_codec_policy == livekit::BackupCodecPolicy::Simulcast);

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

        TEST_CHECK(sent_req.has_add_track());
        const auto& add_t = sent_req.add_track();
        TEST_CHECK(add_t.name() == "camera_av1");
        TEST_CHECK(add_t.type() == livekit::proto::TrackType::VIDEO);
        TEST_CHECK(add_t.backup_codec_policy() == livekit::proto::BackupCodecPolicy::PREFER_REGRESSION);

        // Verify 2 Simulcast Codecs
        TEST_CHECK(add_t.simulcast_codecs_size() == 2);
        
        // Codec 0: AV1
        const auto& c0 = add_t.simulcast_codecs(0);
        TEST_CHECK(c0.codec() == "av1");
        TEST_CHECK(c0.cid() == add_t.cid());
        TEST_CHECK(c0.layers_size() == 3);

        // Codec 1: VP8 Backup
        const auto& c1 = add_t.simulcast_codecs(1);
        TEST_CHECK(c1.codec() == "vp8");
        TEST_CHECK(c1.cid() == add_t.cid() + "_backup");
        TEST_CHECK(c1.layers_size() == 3);

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
        TEST_CHECK(sent_req.add_track().backup_codec_policy() == livekit::proto::BackupCodecPolicy::SIMULCAST);

        std::cout << "     [PASS] SIMULCAST policy correctly serialized in AddTrackRequest." << std::endl;
    }

    // Test 5: Resolve against the local/server intersection and recompute the
    // complete effective plan without overwriting the requested intent.
    {
        livekit::VideoPublishOptions requested;
        requested.video_codec = "av1";
        requested.scalability_mode = "L1T1";
        requested.simulcast = true;

        const auto plan = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, requested,
            {"vp8", "h264"},
            {"video/av1", "video/h264"});
        TEST_CHECK(plan.ok());
        TEST_CHECK(plan.used_fallback());
        TEST_CHECK(plan.requested_codec == "av1");
        TEST_CHECK(plan.effective_codec == "h264");
        TEST_CHECK(plan.requested.scalability_mode == "L1T1");
        TEST_CHECK(plan.effective.scalability_mode.empty());
        TEST_CHECK(plan.effective.layers.size() == 3);
        TEST_CHECK(plan.effective.layers.front().max_bitrate_bps == 3000000);
        TEST_CHECK(plan.effective.simulcast_codecs.size() == 1);
        TEST_CHECK(plan.effective.simulcast_codecs.front().codec == "h264");

        auto source = std::make_shared<livekit::VideoSource>(1280, 720);
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
            "requested_av1", source, livekit::TrackSource::Camera, requested);
        TEST_CHECK(track->requested_publish_options().video_codec == "av1");
        TEST_CHECK(track->publish_options().video_codec == "av1");
    }

    // Test 6: Resolution fails before sender creation when the intersection is
    // empty or an explicitly requested mode/backup cannot be honored.
    {
        livekit::VideoPublishOptions requested;
        requested.video_codec = "vp8";
        auto no_intersection = livekit::LocalVideoTrack::ResolvePublishPlan(
            640, 480, requested,
            {"vp8", "h264"},
            {"video/av1"});
        TEST_CHECK(!no_intersection.ok());

        requested.video_codec = "av1";
        requested.scalability_mode = "L2T1";
        auto unsupported_mode = livekit::LocalVideoTrack::ResolvePublishPlan(
            640, 480, requested,
            {"av1", "vp8"},
            {"video/av1", "video/vp8"});
        TEST_CHECK(!unsupported_mode.ok());

        requested.scalability_mode = "L1T1";
        requested.backup_codec = "av1";
        auto duplicate_backup = livekit::LocalVideoTrack::ResolvePublishPlan(
            640, 480, requested,
            {"av1", "vp8"},
            {"video/av1", "video/vp8"});
        TEST_CHECK(!duplicate_backup.ok());
    }

    std::cout << "[SUCCESS] ALL Backup Codecs & Multi-Codec Unit Tests Passed!" << std::endl;
    return 0;
}
