#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>

#include "media/audio_apm.h"
#include "rtc/webrtc_manager.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "[TEST FAIL] Assertion failed: " #cond " at " __FILE__ ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (0)

using namespace livekit;

int main() {
    std::cout << "[TEST] Starting Audio Render Reference APM Tests..." << std::endl;

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
