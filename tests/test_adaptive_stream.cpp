#include <iostream>
#include <cassert>
#include "remote_track_publication.h"
#include "adaptive_stream_manager.h"
#include "livekit_rtc.pb.h"

using namespace livekit;

void TestSignalRequestGeneration() {
    std::cout << "[TEST 1] Testing UpdateTrackSettings & UpdateSubscription Protobuf generation..." << std::endl;

    proto::SignalRequest req;
    auto* setting = req.mutable_track_setting();
    setting->add_track_sids("TR_video_test_123");
    setting->set_disabled(false);
    setting->set_quality(proto::VideoQuality::MEDIUM);
    setting->set_width(640);
    setting->set_height(360);

    assert(req.has_track_setting());
    assert(req.track_setting().track_sids(0) == "TR_video_test_123");
    assert(req.track_setting().disabled() == false);
    assert(req.track_setting().quality() == proto::VideoQuality::MEDIUM);
    assert(req.track_setting().width() == 640);
    assert(req.track_setting().height() == 360);

    std::cout << "  [PASS] UpdateTrackSettings Protobuf verified." << std::endl;

    proto::SignalRequest sub_req;
    auto* sub = sub_req.mutable_subscription();
    sub->set_subscribe(true);
    sub->add_track_sids("TR_video_test_123");

    assert(sub_req.has_subscription());
    assert(sub_req.subscription().subscribe() == true);
    assert(sub_req.subscription().track_sids(0) == "TR_video_test_123");

    std::cout << "  [PASS] UpdateSubscription Protobuf verified." << std::endl;
}

void TestRemoteTrackPublicationDimensions() {
    std::cout << "[TEST 2] Testing RemoteTrackPublication VideoQuality Auto-Calculation..." << std::endl;

    auto pub = std::make_shared<RemoteTrackPublication>(
        "TR_video_456",
        "remote_cam",
        proto::TrackType::VIDEO,
        nullptr
    );

    assert(pub->is_subscribed() == true);
    assert(pub->is_enabled() == true);

    // Test 180p -> LOW
    pub->SetVideoDimensions(320, 180);
    assert(pub->current_quality() == proto::VideoQuality::LOW);
    std::cout << "  [PASS] 320x180 mapped to VideoQuality::LOW" << std::endl;

    // Test 480p -> MEDIUM
    pub->SetVideoDimensions(640, 360);
    assert(pub->current_quality() == proto::VideoQuality::MEDIUM);
    std::cout << "  [PASS] 640x360 mapped to VideoQuality::MEDIUM" << std::endl;

    // Test 1080p -> HIGH
    pub->SetVideoDimensions(1920, 1080);
    assert(pub->current_quality() == proto::VideoQuality::HIGH);
    std::cout << "  [PASS] 1920x1080 mapped to VideoQuality::HIGH" << std::endl;
}

void TestAdaptiveStreamManager() {
    std::cout << "[TEST 3] Testing AdaptiveStreamManager Track Registration & Dimension Updates..." << std::endl;

    auto pub = std::make_shared<RemoteTrackPublication>(
        "TR_video_789",
        "remote_screen",
        proto::TrackType::VIDEO,
        nullptr
    );

    AdaptiveStreamManager::Instance().RegisterTrack(pub);
    AdaptiveStreamManager::Instance().UpdateTrackDimensions("TR_video_789", 640, 360);

    assert(pub->current_quality() == proto::VideoQuality::MEDIUM);
    std::cout << "  [PASS] AdaptiveStreamManager successfully updated track dimension & quality." << std::endl;

    AdaptiveStreamManager::Instance().SetTrackVisibility("TR_video_789", false);
    assert(pub->is_enabled() == false);
    std::cout << "  [PASS] AdaptiveStreamManager successfully paused disabled track." << std::endl;

    AdaptiveStreamManager::Instance().Clear();
}

int main() {
    std::cout << "[TEST] Starting Adaptive Stream Unit Tests..." << std::endl;
    TestSignalRequestGeneration();
    TestRemoteTrackPublicationDimensions();
    TestAdaptiveStreamManager();
    std::cout << "[SUCCESS] ALL Adaptive Stream Unit Tests Passed!" << std::endl;
    return 0;
}
