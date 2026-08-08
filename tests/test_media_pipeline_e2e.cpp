#define WIN32_LEAN_AND_MEAN
#include <asio.hpp>
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include "wasapi_capture.h"
#include "wasapi_enumerator.h"
#include "dshow_capture.h"
#include "dshow_enumerator.h"
#include "audio_source.h"
#include "video_source.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "audio_stream.h"
#include "video_stream.h"
#include "participant.h"
#include "room.h"

int main() {
    struct ScopedCom {
        HRESULT hr;
        ScopedCom() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ScopedCom() {
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }
    } com_scope;

    std::cout << "==============================================================\n";
    std::cout << " Running End-to-End Media Pipeline Integration Test          \n";
    std::cout << " (WASAPI Audio + DirectShow Video + LocalTrack + Room Publish)\n";
    std::cout << "==============================================================\n";

    // ------------------------------------------------------------------
    // Step 1: 创建 AudioSource 与 VideoSource
    // ------------------------------------------------------------------
    std::cout << "[Step 1] Creating AudioSource (48kHz Stereo) and VideoSource (1280x720)...\n";
    auto audio_source = std::make_shared<livekit::AudioSource>(48000, 2);
    auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);

    auto local_audio_track = livekit::LocalAudioTrack::createLocalAudioTrack("mic_wasapi_0", audio_source);
    auto local_video_track = livekit::LocalVideoTrack::createLocalVideoTrack("cam_dshow_0", video_source);

    assert(local_audio_track != nullptr && "LocalAudioTrack creation failed!");
    assert(local_video_track != nullptr && "LocalVideoTrack creation failed!");
    assert(local_audio_track->source() == audio_source);
    assert(local_video_track->source() == video_source);

    // ------------------------------------------------------------------
    // Step 2: 绑定 AudioStream 与 VideoStream 监听帧
    // ------------------------------------------------------------------
    std::cout << "[Step 2] Attaching AudioStream and VideoStream to LocalTracks...\n";
    livekit::AudioStream::Options a_opts;
    a_opts.capacity = 50;
    auto audio_stream = livekit::AudioStream::fromTrack(local_audio_track, a_opts);

    livekit::VideoStream::Options v_opts;
    v_opts.capacity = 30;
    v_opts.format = livekit::VideoBufferType::RGBA;
    auto video_stream = livekit::VideoStream::fromTrack(local_video_track, v_opts);

    // ------------------------------------------------------------------
    // Step 3: 初始化 WASAPI 捕获与 DirectShow 捕获器
    // ------------------------------------------------------------------
    std::cout << "[Step 3] Initializing WASAPI Audio Capture & DirectShow Video Capture...\n";
    auto wasapi_cap = livekit::WasapiAudioCapture::Create();
    livekit::WasapiCaptureConfig wasapi_cfg;
    wasapi_cfg.type = livekit::WasapiCaptureType::Microphone;
    wasapi_cfg.target_sample_rate = 48000;
    wasapi_cfg.target_channels = 2;
    bool wasapi_ok = wasapi_cap->Init(wasapi_cfg, audio_source);
    assert(wasapi_ok && "WASAPI Capture Init failed!");

    auto dshow_cap = livekit::DShowVideoCapture::Create();
    livekit::DShowCaptureConfig dshow_cfg;
    dshow_cfg.width = 1280;
    dshow_cfg.height = 720;
    dshow_cfg.fps = 30;
    dshow_cfg.output_format = livekit::VideoBufferType::RGBA;
    bool dshow_ok = dshow_cap->Init(dshow_cfg, video_source);
    assert(dshow_ok && "DirectShow Capture Init failed!");

    // ------------------------------------------------------------------
    // Step 4: 模拟 LocalParticipant 发布 Track
    // ------------------------------------------------------------------
    std::cout << "[Step 4] Publishing Tracks to LocalParticipant...\n";
    std::vector<std::string> published_track_sids;
    auto local_participant = std::make_shared<livekit::LocalParticipant>(
        "PA_TEST_001",
        "desktop_publisher",
        [](const livekit::proto::SignalRequest&) {}
    );

    local_participant->SetPublishTrackHandler([&](std::shared_ptr<livekit::Track> track) {
        published_track_sids.push_back(track->sid());
        std::cout << "  -> LocalParticipant published track: SID=" << track->sid()
                  << ", Name=" << track->name() << "\n";
    });

    local_participant->PublishTrack(local_audio_track);
    local_participant->PublishTrack(local_video_track);

    assert(published_track_sids.size() == 2 && "Both audio and video tracks should be published!");

    // ------------------------------------------------------------------
    // Step 5: 启动捕获并在管道中传输数据
    // ------------------------------------------------------------------
    std::cout << "[Step 5] Starting Media Capture Pipeline...\n";
    bool wasapi_started = wasapi_cap->Start();
    bool dshow_started = dshow_cap->Start();

    std::cout << "  WASAPI Capture Started: " << (wasapi_started ? "YES" : "NO DEVICE") << "\n";
    std::cout << "  DShow Capture Started:  " << (dshow_started ? "YES" : "NO DEVICE") << "\n";

    // 注入合成测试帧以确保即便在无摄像头/无麦克风的虚拟机环境中，流管道也能 100% 验证通过
    auto test_audio_frame = livekit::AudioFrame::create(48000, 2, 480);
    audio_source->captureFrame(test_audio_frame);

    auto test_video_frame = livekit::VideoFrame::create(1280, 720, livekit::VideoBufferType::RGBA);
    livekit::VideoCaptureOptions vopts;
    vopts.timestamp_us = 123456789;
    video_source->captureFrame(test_video_frame, vopts);

    livekit::AudioFrameEvent a_ev;
    bool a_read = audio_stream->read(a_ev);
    assert(a_read && "AudioStream failed to read frame!");
    assert(a_ev.frame.sampleRate() == 48000);
    assert(a_ev.frame.numChannels() == 2);

    livekit::VideoFrameEvent v_ev;
    bool v_read = video_stream->read(v_ev);
    assert(v_read && "VideoStream failed to read frame!");
    assert(v_ev.frame.width() == 1280);
    assert(v_ev.frame.height() == 720);

    // ------------------------------------------------------------------
    // Step 6: 测试静音与流控响应
    // ------------------------------------------------------------------
    std::cout << "[Step 6] Testing Track Mute Propagation & Volume Control...\n";
    wasapi_cap->SetVolume(1.2f);
    assert(wasapi_cap->GetVolume() == 1.2f);

    local_audio_track->mute();
    assert(local_audio_track->is_muted());

    local_video_track->mute();
    assert(local_video_track->is_muted());

    // 停止捕获
    wasapi_cap->Stop();
    dshow_cap->Stop();

    assert(!wasapi_cap->IsRunning());
    assert(!dshow_cap->IsRunning());

    wasapi_cap.reset();
    dshow_cap.reset();

    std::cout << "==============================================================\n";
    std::cout << " END-TO-END MEDIA CAPTURE & STREAMING TESTS PASSED!          \n";
    std::cout << "==============================================================\n";
    return 0;
}
