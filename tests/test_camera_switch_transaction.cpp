#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <cstdlib>

#include "media/camera_source_manager.h"
#include "media/dshow_types.h"
#include "rtc/video_source.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "[TEST FAIL] Assertion failed: " #cond " at " __FILE__ ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (0)

using namespace livekit;

// Mock Capturer for deterministic unit tests
class MockCameraCapturer : public ICameraCapturer {
public:
    MockCameraCapturer(bool fail_to_start = false)
        : fail_to_start_(fail_to_start) {}

    ~MockCameraCapturer() override {
        Stop();
    }

    bool Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source) override {
        config_ = config;
        video_source_ = video_source;
        return true;
    }

    bool Start() override {
        if (fail_to_start_) {
            return false;
        }
        is_running_ = true;
        return true;
    }

    void Stop() override {
        is_running_ = false;
        stop_count_++;
    }

    bool IsRunning() const noexcept override {
        return is_running_;
    }

    DShowCaptureConfig GetConfig() const override {
        return config_;
    }

    std::string GetDevicePath() const override {
        return config_.device_path;
    }

    void ProduceFrame(int w = 640, int h = 480) {
        if (!is_running_ || !video_source_) return;
        VideoFrame frame = VideoFrame::create(w, h, VideoBufferType::RGBA);
        VideoCaptureOptions opts;
        opts.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        video_source_->captureFrame(frame, opts);
    }

    int GetStopCount() const { return stop_count_.load(); }

private:
    bool fail_to_start_{false};
    std::atomic<bool> is_running_{false};
    std::atomic<int> stop_count_{0};
    DShowCaptureConfig config_;
    std::shared_ptr<VideoSource> video_source_;
};

int main() {
    std::cout << "[TEST] Starting Camera Switch Transaction Tests..." << std::endl;

    auto output_source = std::make_shared<VideoSource>(1280, 720);
    std::atomic<int> frames_received{0};
    output_source->addSink([&frames_received](const VideoFrame&, const VideoCaptureOptions&) {
        frames_received.fetch_add(1);
    });

    // -------------------------------------------------------------
    // Test 1: Successful Seamless Hot-Switch with First Frame Check
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 1: Normal Seamless Hot-Switch..." << std::endl;
        std::shared_ptr<MockCameraCapturer> cap_a;
        std::shared_ptr<MockCameraCapturer> cap_b;

        auto factory = [&](void) -> std::shared_ptr<ICameraCapturer> {
            if (!cap_a) {
                cap_a = std::make_shared<MockCameraCapturer>();
                return cap_a;
            }
            cap_b = std::make_shared<MockCameraCapturer>();
            return cap_b;
        };

        auto manager = CameraSourceManager::Create(output_source, factory);

        DShowCaptureConfig cfg_a;
        cfg_a.device_path = "dev://camera_a";
        cfg_a.width = 1280;
        cfg_a.height = 720;
        TEST_ASSERT(manager->Start(cfg_a));
        TEST_ASSERT(manager->IsRunning());
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_a");

        // Cap A delivers 2 frames
        cap_a->ProduceFrame();
        cap_a->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 2);

        // Initiate hot switch to Camera B
        auto switch_done = std::make_shared<std::atomic<bool>>(false);
        auto switch_success = std::make_shared<std::atomic<bool>>(false);
        manager->SwitchDeviceAsync("dev://camera_b", 2000, [switch_done, switch_success](bool ok, const std::string&) {
            switch_success->store(ok);
            switch_done->store(true);
        });

        TEST_ASSERT(manager->GetSwitchState() == CameraSwitchState::Probing);
        // Active device must STILL be Camera A before first usable frame from B!
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_a");

        // Camera A can still deliver frames during probing
        cap_a->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 3);
        TEST_ASSERT(!switch_done->load());

        // Now Camera B delivers its first usable frame
        TEST_ASSERT(cap_b != nullptr);
        cap_b->ProduceFrame();

        // Wait a small moment for switch commit and callback
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        TEST_ASSERT(switch_done->load());
        TEST_ASSERT(switch_success->load());
        TEST_ASSERT(manager->GetSwitchState() == CameraSwitchState::Committed);
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_b");
        // Output source should have received 3 (from A) + 1 (from B first frame) = 4
        TEST_ASSERT(frames_received.load() == 4);

        // Cap B subsequent frames flow to output_source
        cap_b->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 5);

        manager->Stop();
        std::cout << "[TEST] Case 1 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 2: Target Device Start Failure -> Abort and Retain Active
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 2: Replacement Device Start Failure Rollback..." << std::endl;
        frames_received.store(0);
        std::shared_ptr<MockCameraCapturer> cap_a;
        std::shared_ptr<MockCameraCapturer> cap_fail;

        auto factory = [&](void) -> std::shared_ptr<ICameraCapturer> {
            if (!cap_a) {
                cap_a = std::make_shared<MockCameraCapturer>(false);
                return cap_a;
            }
            cap_fail = std::make_shared<MockCameraCapturer>(true); // fails to start
            return cap_fail;
        };

        auto manager = CameraSourceManager::Create(output_source, factory);
        DShowCaptureConfig cfg_a;
        cfg_a.device_path = "dev://camera_a";
        TEST_ASSERT(manager->Start(cfg_a));

        cap_a->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 1);

        auto switch_done = std::make_shared<std::atomic<bool>>(false);
        auto switch_success = std::make_shared<std::atomic<bool>>(true);
        manager->SwitchDeviceAsync("dev://camera_broken", 1000, [switch_done, switch_success](bool ok, const std::string&) {
            switch_success->store(ok);
            switch_done->store(true);
        });

        TEST_ASSERT(switch_done->load());
        TEST_ASSERT(!switch_success->load());
        TEST_ASSERT(manager->GetSwitchState() == CameraSwitchState::Aborted);
        // Active must remain Camera A
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_a");

        // Camera A continues to produce frames unaffected
        cap_a->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 2);

        manager->Stop();
        std::cout << "[TEST] Case 2 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 3: Replacement First Frame Timeout -> Abort and Retain
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 3: Replacement Timeout Rollback..." << std::endl;
        frames_received.store(0);
        std::shared_ptr<MockCameraCapturer> cap_a;
        std::shared_ptr<MockCameraCapturer> cap_silent;

        auto factory = [&](void) -> std::shared_ptr<ICameraCapturer> {
            if (!cap_a) {
                cap_a = std::make_shared<MockCameraCapturer>(false);
                return cap_a;
            }
            cap_silent = std::make_shared<MockCameraCapturer>(false);
            return cap_silent;
        };

        auto manager = CameraSourceManager::Create(output_source, factory);
        DShowCaptureConfig cfg_a;
        cfg_a.device_path = "dev://camera_a";
        TEST_ASSERT(manager->Start(cfg_a));

        auto switch_done = std::make_shared<std::atomic<bool>>(false);
        auto switch_success = std::make_shared<std::atomic<bool>>(true);
        // Short timeout 60ms
        manager->SwitchDeviceAsync("dev://camera_silent", 60, [switch_done, switch_success](bool ok, const std::string&) {
            switch_success->store(ok);
            switch_done->store(true);
        });

        TEST_ASSERT(manager->GetSwitchState() == CameraSwitchState::Probing);
        // Do NOT produce frames on cap_silent
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        TEST_ASSERT(switch_done->load());
        TEST_ASSERT(!switch_success->load());
        TEST_ASSERT(manager->GetSwitchState() == CameraSwitchState::Aborted);
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_a");

        // Old camera is unaffected
        cap_a->ProduceFrame();
        TEST_ASSERT(frames_received.load() == 1);

        manager->Stop();
        std::cout << "[TEST] Case 3 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 4: Concurrency & Generation Invalidation
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 4: Concurrent Rapid Switches..." << std::endl;
        std::shared_ptr<MockCameraCapturer> cap_a;
        std::shared_ptr<MockCameraCapturer> cap_b;
        std::shared_ptr<MockCameraCapturer> cap_c;

        auto factory = [&](void) -> std::shared_ptr<ICameraCapturer> {
            if (!cap_a) { cap_a = std::make_shared<MockCameraCapturer>(); return cap_a; }
            if (!cap_b) { cap_b = std::make_shared<MockCameraCapturer>(); return cap_b; }
            cap_c = std::make_shared<MockCameraCapturer>();
            return cap_c;
        };

        auto manager = CameraSourceManager::Create(output_source, factory);
        DShowCaptureConfig cfg_a;
        cfg_a.device_path = "dev://camera_a";
        TEST_ASSERT(manager->Start(cfg_a));

        auto b_callback_fired = std::make_shared<std::atomic<bool>>(false);
        auto c_callback_fired = std::make_shared<std::atomic<bool>>(false);

        // Switch to B
        manager->SwitchDeviceAsync("dev://camera_b", 2000, [b_callback_fired](bool, const std::string&) {
            b_callback_fired->store(true);
        });

        // Immediately switch to C before B delivers frame
        manager->SwitchDeviceAsync("dev://camera_c", 2000, [c_callback_fired](bool ok, const std::string&) {
            c_callback_fired->store(ok);
        });

        // Belated frame from B should be ignored
        cap_b->ProduceFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_a"); // Still A, because B is superseded

        // Frame from C arrives
        cap_c->ProduceFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        TEST_ASSERT(c_callback_fired->load());
        TEST_ASSERT(manager->GetActiveDevicePath() == "dev://camera_c");

        manager->Stop();
        std::cout << "[TEST] Case 4 Passed." << std::endl;
    }

    std::cout << "[TEST] All Camera Switch Transaction Tests Passed!" << std::endl;
    return 0;
}
