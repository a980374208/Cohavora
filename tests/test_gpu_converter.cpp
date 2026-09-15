#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>
#include "src/ui/dx11/gpu_video_converter.h"
#include "src/rtc/video_frame.h"

int main(int argc, char* argv[]) {
    std::cout << "[TestGpuConverter] Starting GPU Video Converter Test..." << std::endl;

	// Exercise the exact fallback branch that previously attempted to lock the
	// same std::mutex twice. It must return promptly and remain reusable.
	livekit::dx11::GpuVideoConverter::SetForceInitializationFailureForTesting(true);
    auto& converter = livekit::dx11::GpuVideoConverter::Instance();
	const int probeW = 2;
	const int probeH = 2;
	std::vector<uint8_t> probeData(probeW * probeH * 3 / 2, 128);
	livekit::VideoFrame probeFrame(probeW, probeH, livekit::VideoBufferType::I420, std::move(probeData));
	for (int i = 0; i < 8; ++i) {
		if (converter.Initialize() || !converter.ConvertToQImage(probeFrame).isNull()) {
			std::cerr << "[TestGpuConverter] FAILED: forced initialization failure did not fall back safely" << std::endl;
			return 1;
		}
		converter.Cleanup();
	}
	livekit::dx11::GpuVideoConverter::SetForceInitializationFailureForTesting(false);

    if (!converter.Initialize()) {
        std::cerr << "[TestGpuConverter] Warning: GPU DX11 device initialization failed (no compatible GPU or headless environment)." << std::endl;
		std::cout << "[TestGpuConverter] Forced-failure lifecycle test passed; skipping hardware-only conversion checks." << std::endl;
		return 0;
    }

    // 1. 测试 1920x1080 I420 转码
    const int w1 = 1920;
    const int h1 = 1080;
    std::vector<uint8_t> i420_data(w1 * h1 * 3 / 2, 128); // 纯灰色测试帧
    livekit::VideoFrame frame_i420(w1, h1, livekit::VideoBufferType::I420, i420_data);

    auto t0 = std::chrono::high_resolution_clock::now();
    QImage img1 = converter.ConvertToQImage(frame_i420);
    auto t1 = std::chrono::high_resolution_clock::now();
    double cost_ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (img1.isNull() || img1.width() != w1 || img1.height() != h1) {
        std::cerr << "[TestGpuConverter] FAILED: I420 conversion output invalid!" << std::endl;
        return 1;
    }
    std::cout << "[TestGpuConverter] 1080p I420 -> QImage conversion success! Cost: " << cost_ms1 << " ms" << std::endl;

    // 2. 测试 1280x720 NV12 转码
    const int w2 = 1280;
    const int h2 = 720;
    std::vector<uint8_t> nv12_data(w2 * h2 * 3 / 2, 128);
    livekit::VideoFrame frame_nv12(w2, h2, livekit::VideoBufferType::NV12, nv12_data);

    auto t2 = std::chrono::high_resolution_clock::now();
    QImage img2 = converter.ConvertToQImage(frame_nv12);
    auto t3 = std::chrono::high_resolution_clock::now();
    double cost_ms2 = std::chrono::duration<double, std::milli>(t3 - t2).count();

    if (img2.isNull() || img2.width() != w2 || img2.height() != h2) {
        std::cerr << "[TestGpuConverter] FAILED: NV12 conversion output invalid!" << std::endl;
        return 1;
    }
    std::cout << "[TestGpuConverter] 720p NV12 -> QImage conversion success! Cost: " << cost_ms2 << " ms" << std::endl;

    // 3. 连续转换 30 帧压测 (模拟 30fps 实时会议场景)
    const int num_frames = 30;
    auto t_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_frames; ++i) {
        QImage res = converter.ConvertToQImage(frame_i420);
        if (res.isNull()) {
            std::cerr << "[TestGpuConverter] FAILED during loop iteration " << i << std::endl;
            return 1;
        }
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    double avg_cost = std::chrono::duration<double, std::milli>(t_end - t_start).count() / num_frames;
    std::cout << "[TestGpuConverter] 30-frame 1080p stream simulation completed! Avg GPU conversion time: " << avg_cost << " ms/frame" << std::endl;

    std::cout << "[TestGpuConverter] ALL GPU VIDEO CONVERTER TESTS PASSED!" << std::endl;
    return 0;
}
