// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_audio_render_reference_apm.cpp
// Original SHA256: 46067f2975d3785b65f9dc2d62766c7075040082afa5e9dd52182efd3d0fba20
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: provenance comments only; original active checks retained.
// Later regression: device-independent default/explicit playout selection.

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <condition_variable>
#include <mutex>

#include "media/audio_apm.h"
#include "rtc/webrtc_manager.h"
#include "rtc/audio_playout_device_selection.h"
#include "media/wasapi_capture.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "[TEST FAIL] Assertion failed: " #cond " at " __FILE__ ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (0)

using namespace livekit;

namespace {

void TestCaptureStartupFailureContract() {
    auto capture = WasapiAudioCapture::Create();
    auto source = std::make_shared<AudioSource>(48000, 2);
    WasapiCaptureConfig config;
    // Explicitly invalid endpoint: this regression never opens a real microphone.
    config.device_id = "cohavora-invalid-endpoint-startup-regression";
    config.auto_reconnect = false;
    TEST_ASSERT(capture->Init(config, source));

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    unsigned callback_count = 0;
    capture->SetCaptureStateCallback([&](bool running) {
        TEST_ASSERT(!running);
        TEST_ASSERT(!capture->IsRunning());
        TEST_ASSERT(capture->GetConfig().device_id == config.device_id);
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++callback_count;
        }
        callback_condition.notify_all();
    });

    TEST_ASSERT(!capture->Start());
    TEST_ASSERT(!capture->IsRunning());
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        TEST_ASSERT(callback_count == 1);
    }
    // Failed startup keeps its watcher. Retrying Start must not replace a
    // joinable thread, or report success simply because that thread exists.
    TEST_ASSERT(!capture->Start());
    TEST_ASSERT(!capture->Init(config, source));
    TEST_ASSERT(capture->SwitchDevice(config.device_id));
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        TEST_ASSERT(callback_condition.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return callback_count == 2; }));
    }
    capture->SetCaptureStateCallback({});
    capture->Stop();
    TEST_ASSERT(!capture->IsRunning());
    TEST_ASSERT(capture->Init(config, source));
    TEST_ASSERT(!capture->Start());
    capture->Stop();
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        TEST_ASSERT(callback_count == 2);
    }
    std::cout << "[TEST] WASAPI startup failure, callback and watcher lifecycle passed.\n";
}

struct FakePlayoutDevice {
    std::vector<std::string> ids = {"endpoint-b", "endpoint-a"};
    std::vector<std::string> calls;
    bool playing = true;
    bool initialized = true;
    int stop_result = 0;
    int select_result = 0;
    int init_result = 0;
    int selected_index = -1;
    std::optional<webrtc::AudioDeviceModule::WindowsDeviceType> selected_role;

    bool Playing() const { return playing; }
    bool PlayoutIsInitialized() const { return initialized; }
    int16_t PlayoutDevices() { return static_cast<int16_t>(ids.size()); }
    int32_t PlayoutDeviceName(uint16_t index, char*, char* guid) {
        std::memcpy(guid, ids[index].c_str(), ids[index].size() + 1);
        return 0;
    }
    int32_t StopPlayout() {
        calls.push_back("stop");
        if (stop_result == 0) {
            playing = false;
            initialized = false;
        }
        return stop_result;
    }
    int32_t SetPlayoutDevice(uint16_t index) {
        calls.push_back("select-index");
        if (select_result == 0) selected_index = index;
        return select_result;
    }
    int32_t SetPlayoutDevice(webrtc::AudioDeviceModule::WindowsDeviceType role) {
        calls.push_back("select-role");
        if (select_result == 0) selected_role = role;
        return select_result;
    }
    int32_t InitSpeaker() {
        calls.push_back("speaker");
        return 0;
    }
    int32_t InitPlayout() {
        calls.push_back("init");
        initialized = init_result == 0;
        return init_result;
    }
    int32_t StartPlayout() {
        calls.push_back("start");
        playing = true;
        return 0;
    }
};

void TestPlayoutSelection() {
    {
        FakePlayoutDevice device;
        int resets = 0;
        TEST_ASSERT(detail::SelectPlayoutDeviceById(device, "", [&] { ++resets; }));
        TEST_ASSERT(device.selected_role == webrtc::AudioDeviceModule::kDefaultDevice);
        TEST_ASSERT(device.selected_index == -1);
        TEST_ASSERT(device.playing && device.initialized && resets == 1);
        TEST_ASSERT((device.calls == std::vector<std::string>{"stop", "select-role", "speaker", "init", "start"}));
    }
    // The UI order is unrelated to the ADM order. Resolve the stable endpoint
    // identity on each switch, including after the native list is reordered.
    for (const auto& ids : std::vector<std::vector<std::string>>{
            {"endpoint-b", "endpoint-a"}, {"endpoint-a", "endpoint-b"}}) {
        FakePlayoutDevice device;
        device.ids = ids;
        TEST_ASSERT(detail::SelectPlayoutDeviceById(device, "ENDPOINT-A", [] {}));
        TEST_ASSERT(device.ids[device.selected_index] == "endpoint-a");
        TEST_ASSERT(!device.selected_role.has_value());
        TEST_ASSERT(device.playing);
    }
    {
        FakePlayoutDevice device;
        int resets = 0;
        TEST_ASSERT(!detail::SelectPlayoutDeviceById(device, "removed-endpoint", [&] { ++resets; }));
        TEST_ASSERT(device.calls.empty());
        TEST_ASSERT(device.playing && device.initialized && resets == 0);
    }
    {
        FakePlayoutDevice device;
        device.playing = false;
        TEST_ASSERT(detail::SelectPlayoutDeviceById(device, "", [] {}));
        TEST_ASSERT(!device.playing && device.initialized);
        TEST_ASSERT((device.calls == std::vector<std::string>{"stop", "select-role", "speaker", "init"}));
    }
    {
        FakePlayoutDevice device;
        device.playing = false;
        device.initialized = false;
        TEST_ASSERT(detail::SelectPlayoutDeviceById(device, "", [] {}));
        TEST_ASSERT(!device.playing && !device.initialized);
        TEST_ASSERT((device.calls == std::vector<std::string>{"select-role"}));
    }
    {
        FakePlayoutDevice device;
        device.stop_result = -1;
        int resets = 0;
        TEST_ASSERT(!detail::SelectPlayoutDeviceById(device, "endpoint-a", [&] { ++resets; }));
        TEST_ASSERT((device.calls == std::vector<std::string>{"stop"}));
        TEST_ASSERT(device.playing && device.initialized && resets == 0);
    }
    {
        FakePlayoutDevice device;
        device.select_result = -1;
        int resets = 0;
        TEST_ASSERT(!detail::SelectPlayoutDeviceById(device, "endpoint-a", [&] { ++resets; }));
        TEST_ASSERT(device.selected_index == -1);
        TEST_ASSERT(device.playing && device.initialized && resets == 0);
        TEST_ASSERT((device.calls == std::vector<std::string>{"stop", "select-index", "speaker", "init", "start"}));
    }
    {
        FakePlayoutDevice device;
        device.init_result = -1;
        TEST_ASSERT(!detail::SelectPlayoutDeviceById(device, "", [] {}));
        TEST_ASSERT(!device.playing);
        TEST_ASSERT((device.calls == std::vector<std::string>{"stop", "select-role", "speaker", "init"}));
    }
    std::cout << "[TEST] Playout default, endpoint identity and switch lifecycle passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "[TEST] Starting Audio Render Reference APM Tests..." << std::endl;
    TestCaptureStartupFailureContract();
    TestPlayoutSelection();

    // -------------------------------------------------------------
    // Test 1: Normal 10ms Playout Frame Ingestion & Diagnostic State
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 1: Ingest 10ms Playout Frames..." << std::endl;
        ApmConfig config;
        config.enable_aec = true;
        auto apm = AudioApmProcessor::Create(config);

        TEST_ASSERT(!apm->HasActiveRenderReference());
        TEST_ASSERT(apm->GetRenderFramesProcessed() == 0);

        // 48kHz 双声道 10ms = 480 frames = 960 samples
        const int rate = 48000;
        const int channels = 2;
        const int frames_per_10ms = rate / 100;
        std::vector<int16_t> pcm_10ms(frames_per_10ms * channels, 1000);

        AudioFrame render_frame(pcm_10ms, rate, channels, frames_per_10ms);

        // 连续喂入 5 帧
        for (int i = 0; i < 5; ++i) {
            apm->ProcessRenderFrame(render_frame);
        }

        TEST_ASSERT(apm->GetRenderFramesProcessed() == 5);
        TEST_ASSERT(apm->HasActiveRenderReference(1000));
        std::cout << "[TEST] Case 1 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 2: Inactive Render Reference Diagnostic (No False Positive)
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 2: Render Reference Inactive Diagnostic..." << std::endl;
        ApmConfig config;
        config.enable_aec = true;
        auto apm = AudioApmProcessor::Create(config);

        const int rate = 48000;
        const int channels = 2;
        const int frames_per_10ms = rate / 100;
        std::vector<int16_t> pcm_10ms(frames_per_10ms * channels, 500);
        AudioFrame render_frame(pcm_10ms, rate, channels, frames_per_10ms);

        apm->ProcessRenderFrame(render_frame);
        TEST_ASSERT(apm->HasActiveRenderReference(1000));

        // 等待 80ms，使用 50ms 窗口查询，应诊断为非活跃状态
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        TEST_ASSERT(!apm->HasActiveRenderReference(50));

        std::cout << "[TEST] Case 2 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 3: APM Reset Lifecycle on Device Switch / Session Clear
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 3: APM Reset Lifecycle..." << std::endl;
        ApmConfig config;
        config.enable_aec = true;
        auto apm = AudioApmProcessor::Create(config);

        const int rate = 48000;
        const int channels = 2;
        const int frames_per_10ms = rate / 100;
        std::vector<int16_t> pcm_10ms(frames_per_10ms * channels, 200);
        AudioFrame render_frame(pcm_10ms, rate, channels, frames_per_10ms);

        apm->ProcessRenderFrame(render_frame);
        TEST_ASSERT(apm->HasActiveRenderReference(1000));

        // 设备切换或断开时执行 Reset
        apm->Reset();
        // Reset 之后参考流应立即被视为未激活
        TEST_ASSERT(!apm->HasActiveRenderReference(1000));

        // 重新输入后恢复处理能力
        apm->ProcessRenderFrame(render_frame);
        TEST_ASSERT(apm->HasActiveRenderReference(1000));

        std::cout << "[TEST] Case 3 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 4: Framing Robustness (Arbitrary Chunking via FIFO)
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 4: Non-standard Buffer Framing..." << std::endl;
        ApmConfig config;
        config.enable_aec = true;
        auto apm = AudioApmProcessor::Create(config);

        const int rate = 48000;
        const int channels = 2;
        const int frames_per_10ms = rate / 100; // 480

        // 一次送入 25ms 数据 (1200 frames)
        const int frames_25ms = frames_per_10ms * 2 + (frames_per_10ms / 2);
        std::vector<int16_t> pcm_25ms(frames_25ms * channels, 120);
        AudioFrame chunk_25ms(pcm_25ms, rate, channels, frames_25ms);

        apm->ProcessRenderFrame(chunk_25ms);
        // 25ms 应该切出整整 2 个 10ms 帧，剩余 5ms 暂存 FIFO
        TEST_ASSERT(apm->GetRenderFramesProcessed() == 2);

        // 再次送入 15ms 数据 (720 frames)
        const int frames_15ms = frames_per_10ms + (frames_per_10ms / 2);
        std::vector<int16_t> pcm_15ms(frames_15ms * channels, 120);
        AudioFrame chunk_15ms(pcm_15ms, rate, channels, frames_15ms);

        apm->ProcessRenderFrame(chunk_15ms);
        // 5ms (旧) + 15ms (新) = 20ms = 刚好切出另外 2 个 10ms 帧，累计 4 帧！
        TEST_ASSERT(apm->GetRenderFramesProcessed() == 4);

        std::cout << "[TEST] Case 4 Passed." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 5: Concurrent Capture & Playout High-Stress Safety
    // -------------------------------------------------------------
    {
        std::cout << "[TEST] Case 5: Concurrent Capture & Render Audio Pipeline..." << std::endl;
        ApmConfig config;
        config.enable_aec = true;
        config.enable_ans = true;
        config.enable_agc = true;
        auto apm = AudioApmProcessor::Create(config);

        const int rate = 48000;
        const int channels = 2;
        const int frames_10ms = 480;

        const int target_iterations = 100;
        std::atomic<int> capture_count{0};
        std::atomic<int> render_count{0};

        // 采集线程模拟 (并发 100 次处理)
        std::thread capture_thread([&]() {
            std::vector<int16_t> pcm(frames_10ms * channels, 100);
            for (int i = 0; i < target_iterations; ++i) {
                AudioFrame cap_frame(pcm, rate, channels, frames_10ms);
                apm->ProcessCaptureFrame(cap_frame);
                capture_count.fetch_add(1);
                std::this_thread::yield();
            }
        });

        // 播放线程模拟 (并发 100 次处理)
        std::thread playout_thread([&]() {
            std::vector<int16_t> pcm(frames_10ms * channels, 200);
            for (int i = 0; i < target_iterations; ++i) {
                AudioFrame ren_frame(pcm, rate, channels, frames_10ms);
                apm->ProcessRenderFrame(ren_frame);
                render_count.fetch_add(1);
                std::this_thread::yield();
            }
        });

        // 主线程并发触发 Reset
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        apm->Reset();

        capture_thread.join();
        playout_thread.join();

        TEST_ASSERT(capture_count.load() == target_iterations);
        TEST_ASSERT(render_count.load() == target_iterations);

        std::cout << "[TEST] Case 5 Passed (Capture frames: " << capture_count.load()
                  << ", Render frames: " << render_count.load() << ")." << std::endl;
    }

    std::cout << "[TEST] All Audio Render Reference APM Tests Passed!" << std::endl;
    return 0;
}
