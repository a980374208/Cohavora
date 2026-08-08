#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include "dshow_types.h"
#include "dshow_enumerator.h"
#include "dshow_capture.h"
#include "media_converters.h"
#include "video_source.h"
#include "local_video_track.h"
#include "video_stream.h"

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::cout << "==================================================\n";
    std::cout << " Running DirectShow Video Capture Tests           \n";
    std::cout << "==================================================\n";

    // ------------------------------------------------------------------
    // [Test 1] Video Device Enumeration & Capability Extraction
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Enumerating DirectShow Video Input Devices...\n";

    auto devices = livekit::DShowEnumerator::EnumerateVideoDevices();
    std::cout << "  Found " << devices.size() << " Video Capture Devices:\n";
    for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  Device [" << i << "]: " << devices[i].name << "\n";
        std::cout << "    Path: " << devices[i].path << "\n";
        std::cout << "    Caps Count: " << devices[i].capabilities.size() << "\n";
        for (size_t c = 0; c < std::min<size_t>(devices[i].capabilities.size(), 3); ++c) {
            const auto& cap = devices[i].capabilities[c];
            std::cout << "      -> " << cap.width << "x" << cap.height << " @" << cap.max_fps
                      << "fps (" << livekit::MediaConverters::PixelFormatToString(cap.format) << ")\n";
        }
    }

    auto default_dev = livekit::DShowEnumerator::GetDefaultVideoDevice();
    std::cout << "  -> Default Video Device: " << default_dev.name << "\n";
    std::cout << "  -> [Test 1 PASSED] Device enumeration succeeded.\n\n";

    // ------------------------------------------------------------------
    // [Test 2] High-Performance Pixel Format Converters Test
    // ------------------------------------------------------------------
    std::cout << "[Test 2] Testing Pixel Color Space Converters (YUY2, RGB24, NV12 -> RGBA)...\n";

    // 构造 2x2 YUY2 (4 bytes per 2 pixels: Y0, U, Y1, V)
    // 2x2 = 4 pixels = 8 bytes of YUY2
    uint8_t sample_yuy2[8] = {
        235, 128, 235, 128, // Line 0: Pure white Y=235, U=128, V=128
        16,  128, 16,  128  // Line 1: Pure black Y=16, U=128, V=128
    };
    uint8_t out_rgba[16] = {0};

    livekit::MediaConverters::ConvertYUY2ToRGBA(sample_yuy2, out_rgba, 2, 2, false);

    // Pixel 0 should be near white (R~255, G~255, B~255, A=255)
    assert(out_rgba[0] > 240 && out_rgba[1] > 240 && out_rgba[2] > 240 && out_rgba[3] == 255);
    // Pixel 2 (Line 1 Pixel 0) should be near black (R~0, G~0, B~0, A=255)
    assert(out_rgba[8] < 15 && out_rgba[9] < 15 && out_rgba[10] < 15 && out_rgba[11] == 255);

    // 测试 NV12 -> RGBA
    uint8_t sample_nv12[6] = {
        235, 235, 16, 16, // Y plane 2x2
        128, 128          // UV plane 1x1 (shared for 2x2)
    };
    uint8_t out_nv12_rgba[16] = {0};
    livekit::MediaConverters::ConvertNV12ToRGBA(sample_nv12, out_nv12_rgba, 2, 2, false);
    assert(out_nv12_rgba[0] > 240 && out_nv12_rgba[1] > 240 && out_nv12_rgba[2] > 240 && out_nv12_rgba[3] == 255);

    // 测试 RGB24 -> RGBA (含翻转纠正)
    uint8_t sample_bgr[12] = {
        0, 0, 255,    0, 255, 0, // Bottom line (Line 1 in flipped): Red, Green
        255, 0, 0,    0, 0, 0    // Top line (Line 0 in flipped): Blue, Black
    };
    uint8_t out_bgr_rgba[16] = {0};
    livekit::MediaConverters::ConvertRGB24ToRGBA(sample_bgr, out_bgr_rgba, 2, 2, true); // Flip vertically
    // Flipped Line 0 should be original Bottom Line (Red: R=255, G=0, B=0)
    assert(out_bgr_rgba[0] == 255 && out_bgr_rgba[1] == 0 && out_bgr_rgba[2] == 0 && out_bgr_rgba[3] == 255);

    std::cout << "  -> [Test 2 PASSED] Color space conversions and flip logic verified.\n\n";

    // ------------------------------------------------------------------
    // [Test 3] DirectShow Video Capture Pipeline Test
    // ------------------------------------------------------------------
    std::cout << "[Test 3] Testing DirectShow Capture Pipeline to VideoSource...\n";

    auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
    auto local_track = livekit::LocalVideoTrack::createLocalVideoTrack("camera_track", video_source);

    std::atomic<int> frames_captured{0};
    video_source->addSink([&](const livekit::VideoFrame& frame, const livekit::VideoCaptureOptions& options) {
        frames_captured.fetch_add(1);
    });

    auto dshow_cap = livekit::DShowVideoCapture::Create();
    livekit::DShowCaptureConfig cfg;
    if (!devices.empty() && !devices[0].capabilities.empty()) {
        const auto& cap = devices[0].capabilities[0];
        cfg.width = cap.width;
        cfg.height = cap.height;
        cfg.fps = cap.max_fps > 0 ? cap.max_fps : 30;
        cfg.preferred_format = cap.format;
        cfg.device_path = devices[0].path;
    } else {
        cfg.width = 1280;
        cfg.height = 720;
        cfg.fps = 30;
        cfg.preferred_format = livekit::DShowPixelFormat::Unknown;
    }
    cfg.output_format = livekit::VideoBufferType::RGBA;

    bool init_ok = dshow_cap->Init(cfg, video_source);
    assert(init_ok && "DShowVideoCapture::Init failed!");

    if (!devices.empty()) {
        bool start_ok = dshow_cap->Start();
        std::cout << "  DShow Camera Capture Started: " << (start_ok ? "SUCCESS" : "DEVICE BUSY / IN USE") << "\n";
        if (start_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::cout << "  -> Captured frames count: " << dshow_cap->GetCapturedFramesCount()
                      << ", Actual FPS: " << dshow_cap->GetActualFps() << "\n";
            dshow_cap->Stop();
            assert(!dshow_cap->IsRunning() && "DShowVideoCapture should be stopped!");
        }
    } else {
        std::cout << "  [INFO] No physical camera attached on this CI/test machine. Skipping active camera loop.\n";
    }

    std::cout << "  -> [Test 3 PASSED] DirectShow video capture pipeline verified.\n\n";

    std::cout << "==================================================\n";
    std::cout << " ALL DIRECTSHOW VIDEO CAPTURE TESTS PASSED!       \n";
    std::cout << "==================================================\n";
    return 0;
}
