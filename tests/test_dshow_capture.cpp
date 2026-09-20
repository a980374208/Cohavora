#include <iostream>
#include "tests/support/test_check.h"
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
#include <dvdmedia.h>
#include <condition_variable>
#include <fstream>
#include <mutex>

namespace livekit {
class DShowCaptureTestAccess {
public:
    static void configure(DShowVideoCapture& capture, IAMStreamConfig* stream) {
        capture.ConfigureCaptureFormat(stream);
    }
    static bool connected(DShowVideoCapture& capture, const AM_MEDIA_TYPE& type) {
        return capture.ApplyConnectedFormat(type);
    }
};
}

namespace {
class FormatDevice final : public IAMStreamConfig {
public:
    FormatDevice() {
        native.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        native.bmiHeader.biWidth = 1320;
        native.bmiHeader.biHeight = 960;
        native.bmiHeader.biPlanes = 1;
        native.bmiHeader.biBitCount = 12;
        native.bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
        native.bmiHeader.biSizeImage = 1320 * 960 * 3 / 2;
        native.AvgTimePerFrame = 166666;
        native.rcSource = native.rcTarget = RECT{0, 0, 1320, 960};
        alternate.bmiHeader = native.bmiHeader;
        alternate.bmiHeader.biWidth = 640;
        alternate.bmiHeader.biHeight = 480;
        alternate.bmiHeader.biSizeImage = 640 * 480 * 3 / 2;
        alternate.AvgTimePerFrame = 666666;
        alternate.dwPictAspectRatioX = 4;
        alternate.dwPictAspectRatioY = 3;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* count, int* size) override {
        *count = 2; *size = sizeof(VIDEO_STREAM_CONFIG_CAPS); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE** out, BYTE* buffer) override {
        TEST_CHECK(index == 0 || index == 1);
        auto* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        *type = {};
        type->majortype = MEDIATYPE_Video;
        type->subtype = livekit::MediaConverters::PixelFormatToSubtype(livekit::DShowPixelFormat::NV12);
        type->formattype = index == 0 ? FORMAT_VideoInfo : FORMAT_VideoInfo2;
        type->cbFormat = index == 0 ? sizeof(native) : sizeof(alternate);
        type->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type->cbFormat));
        std::memcpy(type->pbFormat, index == 0 ? static_cast<void*>(&native) : &alternate, type->cbFormat);
        type->lSampleSize = index == 0 ? native.bmiHeader.biSizeImage : alternate.bmiHeader.biSizeImage;
        auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(buffer);
        *caps = {};
        caps->MinFrameInterval = index == 0 ? 166666 : 666666;
        caps->MaxFrameInterval = 1000000;
        *out = type;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE* type) override {
        ++attempts;
        TEST_CHECK(type->subtype == livekit::MediaConverters::PixelFormatToSubtype(livekit::DShowPixelFormat::NV12));
        // Every field except the supported frame interval must remain the
        // complete advertised mode, including rectangles and image byte size.
        if (type->formattype == FORMAT_VideoInfo) {
            auto actual = *reinterpret_cast<VIDEOINFOHEADER*>(type->pbFormat);
            TEST_CHECK(actual.AvgTimePerFrame == 333333);
            actual.AvgTimePerFrame = native.AvgTimePerFrame;
            TEST_CHECK(std::memcmp(&actual, &native, sizeof(native)) == 0);
            if (rejectNative) return VFW_E_INVALIDMEDIATYPE;
            selectedWidth = 1320;
        } else {
            auto actual = *reinterpret_cast<VIDEOINFOHEADER2*>(type->pbFormat);
            TEST_CHECK(actual.AvgTimePerFrame == 666666); // clamped to device range
            TEST_CHECK(std::memcmp(&actual, &alternate, sizeof(alternate)) == 0);
            selectedWidth = 640;
        }
        return S_OK;
    }
    VIDEOINFOHEADER native{};
    VIDEOINFOHEADER2 alternate{};
    bool rejectNative = false;
    int attempts = 0, selectedWidth = 0;
};

void CheckCaptureFormatContract() {
    auto capture = livekit::DShowVideoCapture::Create();
    auto source = std::make_shared<livekit::VideoSource>(1280, 720);
    livekit::DShowCaptureConfig config;
    config.output_format = livekit::VideoBufferType::NV12;
    TEST_CHECK(capture->Init(config, source));
    FormatDevice device;
    livekit::DShowCaptureTestAccess::configure(*capture, &device);
    TEST_CHECK(device.selectedWidth == 1320 && device.attempts == 1);
    device.rejectNative = true;
    livekit::DShowCaptureTestAccess::configure(*capture, &device);
    TEST_CHECK(device.selectedWidth == 640 && device.attempts == 3);

    int received = 0;
    source->addSink([&](const livekit::VideoFrame& frame, const livekit::VideoCaptureOptions&) {
        ++received;
        TEST_CHECK(frame.width() == 1320 && frame.height() == 960);
        TEST_CHECK(frame.dataSize() == 1320 * 960 * 3 / 2);
        TEST_CHECK(frame.data()[0] == 235);
    });
    AM_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video;
    type.subtype = livekit::MediaConverters::PixelFormatToSubtype(livekit::DShowPixelFormat::NV12);
    // The requested dimensions must never be used in place of either header.
    VIDEOINFOHEADER2 info2{};
    info2.bmiHeader = device.native.bmiHeader;
    info2.dwPictAspectRatioX = 11; info2.dwPictAspectRatioY = 8;
    for (const bool secondHeader : {false, true}) {
        type.formattype = secondHeader ? FORMAT_VideoInfo2 : FORMAT_VideoInfo;
        type.cbFormat = secondHeader ? sizeof(info2) : sizeof(device.native);
        type.pbFormat = reinterpret_cast<BYTE*>(secondHeader ? static_cast<void*>(&info2) : &device.native);
        TEST_CHECK(livekit::DShowCaptureTestAccess::connected(*capture, type));
        TEST_CHECK(capture->GetNegotiatedWidth() == 1320 && capture->GetNegotiatedHeight() == 960);
        std::vector<uint8_t> pixels(1320 * 960 * 3 / 2, 128);
        pixels[0] = 235;
        const int before = received;
        capture->OnRawFrameReceived(1.0, pixels.data(), long(pixels.size() - 1));
        TEST_CHECK(received == before); // truncated samples never reach VideoSource
        capture->OnRawFrameReceived(1.0, pixels.data(), long(pixels.size()));
        TEST_CHECK(received == before + 1);
        --type.cbFormat;
        TEST_CHECK(!livekit::DShowCaptureTestAccess::connected(*capture, type));
    }
    std::cout << "DSHOW_FORMAT_CONTRACT PASS: supported mode, rejection fallback, both headers, short buffer\n";
}
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool runtime = argc > 1 && std::string(argv[1]) == "--camera-runtime";
    if (!runtime) CheckCaptureFormatContract();
    std::cout << "==================================================\n";
    std::cout << " Running DirectShow Video Capture Tests           \n";
    std::cout << "==================================================\n";

    // ------------------------------------------------------------------
    // [Test 1] Video Device Enumeration & Capability Extraction
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Enumerating DirectShow Video Input Devices...\n";

    auto devices = runtime ? livekit::DShowEnumerator::EnumerateVideoDevices() : std::vector<livekit::DShowDeviceInfo>{};
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

    std::cout << (runtime ? "  Device enumeration completed.\n" : "  Hardware enumeration deferred (use --camera-runtime).\n");

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
    TEST_CHECK(out_rgba[0] > 240 && out_rgba[1] > 240 && out_rgba[2] > 240 && out_rgba[3] == 255);
    // Pixel 2 (Line 1 Pixel 0) should be near black (R~0, G~0, B~0, A=255)
    TEST_CHECK(out_rgba[8] < 15 && out_rgba[9] < 15 && out_rgba[10] < 15 && out_rgba[11] == 255);

    // 测试 NV12 -> RGBA
    uint8_t sample_nv12[6] = {
        235, 235, 16, 16, // Y plane 2x2
        128, 128          // UV plane 1x1 (shared for 2x2)
    };
    uint8_t out_nv12_rgba[16] = {0};
    livekit::MediaConverters::ConvertNV12ToRGBA(sample_nv12, out_nv12_rgba, 2, 2, false);
    TEST_CHECK(out_nv12_rgba[0] > 240 && out_nv12_rgba[1] > 240 && out_nv12_rgba[2] > 240 && out_nv12_rgba[3] == 255);

    // 测试 RGB24 -> RGBA (含翻转纠正)
    uint8_t sample_bgr[12] = {
        0, 0, 255,    0, 255, 0, // Bottom line (Line 1 in flipped): Red, Green
        255, 0, 0,    0, 0, 0    // Top line (Line 0 in flipped): Blue, Black
    };
    uint8_t out_bgr_rgba[16] = {0};
    livekit::MediaConverters::ConvertRGB24ToRGBA(sample_bgr, out_bgr_rgba, 2, 2, true); // Flip vertically
    // Flipped Line 0 reads the last stored row: Blue, Black.
    TEST_CHECK(out_bgr_rgba[0] == 0 && out_bgr_rgba[1] == 0 && out_bgr_rgba[2] == 255 && out_bgr_rgba[3] == 255);

    std::cout << "  -> [Test 2 PASSED] Color space conversions and flip logic verified.\n\n";
    if (!runtime) { CoUninitialize(); return 0; }

    // ------------------------------------------------------------------
    // [Test 3] DirectShow Video Capture Pipeline Test
    // ------------------------------------------------------------------
    std::cout << "[Test 3] Testing DirectShow Capture Pipeline to VideoSource...\n";

    auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
    auto local_track = livekit::LocalVideoTrack::createLocalVideoTrack("camera_track", video_source);

    std::atomic<int> frames_captured{0};
    std::mutex frame_mutex;
    std::condition_variable frames_ready;
    livekit::VideoFrame captured_frame;
    video_source->addSink([&](const livekit::VideoFrame& frame, const livekit::VideoCaptureOptions& options) {
        std::lock_guard lock(frame_mutex);
        captured_frame = frame;
        frames_captured.fetch_add(1);
        frames_ready.notify_one();
    });

    auto dshow_cap = livekit::DShowVideoCapture::Create();
    livekit::DShowCaptureConfig cfg;
    // Match the meeting client's request, not the device's first capability.
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fps = 30;
    cfg.output_format = livekit::VideoBufferType::NV12;
    if (!devices.empty()) cfg.device_path = devices[0].path;

    bool init_ok = dshow_cap->Init(cfg, video_source);
    TEST_CHECK(init_ok && "DShowVideoCapture::Init failed!");

    if (!devices.empty()) {
        bool start_ok = dshow_cap->Start();
        std::cout << "  DShow Camera Capture Started: " << (start_ok ? "SUCCESS" : "DEVICE BUSY / IN USE") << "\n";
        TEST_CHECK(start_ok);
        if (start_ok) {
            std::unique_lock lock(frame_mutex);
            TEST_CHECK(frames_ready.wait_for(lock, std::chrono::seconds(5), [&] { return frames_captured >= 15; }));
            lock.unlock();
            std::cout << "  -> Captured frames count: " << dshow_cap->GetCapturedFramesCount()
                      << ", Actual FPS: " << dshow_cap->GetActualFps() << "\n";
            dshow_cap->Stop();
            TEST_CHECK(!dshow_cap->IsRunning() && "DShowVideoCapture should be stopped!");
            TEST_CHECK(captured_frame.width() == dshow_cap->GetNegotiatedWidth());
            TEST_CHECK(captured_frame.height() == dshow_cap->GetNegotiatedHeight());
            TEST_CHECK(video_source->width() == captured_frame.width() && video_source->height() == captured_frame.height());
            std::cout << "CAMERA_RUNTIME PASS requested=1280x720 delivered=" << captured_frame.width() << 'x'
                << captured_frame.height() << " frames=" << frames_captured << '\n';
            if (argc > 2) {
                std::vector<uint8_t> rgba(size_t(captured_frame.width()) * captured_frame.height() * 4);
                livekit::MediaConverters::ConvertNV12ToRGBA(captured_frame.data(), rgba.data(),
                    captured_frame.width(), captured_frame.height(), false);
                std::ofstream out(argv[2], std::ios::binary);
                out << "P6\n" << captured_frame.width() << ' ' << captured_frame.height() << "\n255\n";
                for (size_t i = 0; i < rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
                TEST_CHECK(out.good());
            }
        }
    } else {
        std::cout << "CAMERA_RUNTIME NOT_RUN: no camera\n";
        return 2;
    }

    std::cout << "  -> [Test 3 PASSED] DirectShow video capture pipeline verified.\n\n";

    std::cout << "==================================================\n";
    std::cout << " ALL DIRECTSHOW VIDEO CAPTURE TESTS PASSED!       \n";
    std::cout << "==================================================\n";
    return 0;
}
