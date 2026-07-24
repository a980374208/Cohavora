#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "audio_frame.h"
#include "video_frame.h"
#include "audio_source.h"
#include "video_source.h"
#include "audio_stream.h"
#include "video_stream.h"
#include "local_audio_track.h"
#include "local_video_track.h"

int main() {
    std::cout << "==================================================\n";
    std::cout << " Running Media Streaming (Source & Stream) Tests  \n";
    std::cout << "==================================================\n";

    // ------------------------------------------------------------------
    // [Test 1] PCM Audio Capture & Stream Pipeline Test
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Testing AudioSource -> LocalAudioTrack -> AudioStream Pipeline...\n";

    auto audio_source = std::make_shared<livekit::AudioSource>(48000, 2);
    auto audio_track = livekit::LocalAudioTrack::createLocalAudioTrack("microphone_test", audio_source);

    livekit::AudioStream::Options audio_opts;
    audio_opts.capacity = 10;
    auto audio_stream = livekit::AudioStream::fromTrack(audio_track, audio_opts);

    // 构造 10ms 的 48kHz 双声道 PCM 采样包 (480 samples/channel * 2 channels = 960 total samples)
    auto frame_in = livekit::AudioFrame::create(48000, 2, 480);
    // 写入模拟 440Hz 采样音频数据
    for (std::size_t i = 0; i < frame_in.data().size(); ++i) {
        frame_in.data()[i] = static_cast<std::int16_t>(i % 3000);
    }

    std::cout << "  -> Pushing AudioFrame into AudioSource: " << frame_in.toString() << std::endl;
    audio_source->captureFrame(frame_in);

    livekit::AudioFrameEvent audio_ev;
    bool audio_read_success = audio_stream->read(audio_ev);
    assert(audio_read_success && "AudioStream failed to read frame!");
    assert(audio_ev.frame.sampleRate() == 48000 && "AudioFrame sample rate mismatch!");
    assert(audio_ev.frame.numChannels() == 2 && "AudioFrame num channels mismatch!");
    assert(audio_ev.frame.samplesPerChannel() == 480 && "AudioFrame samples per channel mismatch!");
    assert(audio_ev.frame.data().size() == 960 && "AudioFrame data size mismatch!");
    assert(audio_ev.frame.data()[10] == static_cast<std::int16_t>(10 % 3000) && "AudioFrame sample content modified!");

    std::cout << "  -> [Test 1 PASSED] Successfully read PCM AudioFrame from AudioStream!\n\n";

    // ------------------------------------------------------------------
    // [Test 2] Video Frame Capture & Stream Pipeline Test
    // ------------------------------------------------------------------
    std::cout << "[Test 2] Testing VideoSource -> LocalVideoTrack -> VideoStream Pipeline...\n";

    auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
    auto video_track = livekit::LocalVideoTrack::createLocalVideoTrack("camera_test", video_source);

    livekit::VideoStream::Options video_opts;
    video_opts.capacity = 5;
    video_opts.format = livekit::VideoBufferType::RGBA;
    auto video_stream = livekit::VideoStream::fromTrack(video_track, video_opts);

    auto vframe_in = livekit::VideoFrame::create(1280, 720, livekit::VideoBufferType::RGBA);
    // 填充假 RGBA 像素数据 (Red plane = 255)
    std::uint8_t* pdata = vframe_in.data();
    pdata[0] = 255; // R
    pdata[1] = 128; // G
    pdata[2] = 64;  // B
    pdata[3] = 255; // A

    livekit::VideoCaptureOptions vopts;
    vopts.timestamp_us = 987654321;
    vopts.rotation = livekit::VideoRotation::VIDEO_ROTATION_90;

    std::cout << "  -> Pushing VideoFrame (1280x720 RGBA) into VideoSource timestamp=" << vopts.timestamp_us << "...\n";
    video_source->captureFrame(vframe_in, vopts);

    livekit::VideoFrameEvent video_ev;
    bool video_read_success = video_stream->read(video_ev);
    assert(video_read_success && "VideoStream failed to read frame!");
    assert(video_ev.frame.width() == 1280 && "VideoFrame width mismatch!");
    assert(video_ev.frame.height() == 720 && "VideoFrame height mismatch!");
    assert(video_ev.rotation == livekit::VideoRotation::VIDEO_ROTATION_90 && "VideoRotation mismatch!");
    assert(video_ev.timestamp_us == 987654321 && "VideoFrame timestamp mismatch!");
    assert(video_ev.frame.data()[0] == 255 && video_ev.frame.data()[1] == 128 && "VideoFrame pixel content mismatch!");

    auto planes = video_ev.frame.planeInfos();
    assert(planes.size() == 1 && "RGBA plane count should be 1!");
    assert(planes[0].stride == 1280 * 4 && "RGBA stride should be width * 4!");

    std::cout << "  -> [Test 2 PASSED] Successfully read VideoFrame from VideoStream!\n\n";

    // ------------------------------------------------------------------
    // [Test 3] Mute Filtering & Stream Isolation Test
    // ------------------------------------------------------------------
    std::cout << "[Test 3] Testing Track Mute Isolation...\n";

    audio_track->mute();
    assert(audio_track->muted() && "AudioTrack should be muted!");

    audio_source->captureFrame(frame_in);

    // 线程异步关闭测试防止阻塞
    std::thread close_thread([audio_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        audio_stream->close();
    });

    livekit::AudioFrameEvent muted_ev;
    bool read_after_mute = audio_stream->read(muted_ev);
    if (close_thread.joinable()) close_thread.join();

    assert(!read_after_mute && "AudioStream should NOT deliver frames while track is muted!");
    std::cout << "  -> [Test 3 PASSED] Muted track correctly isolated frame delivery!\n\n";

    std::cout << "==================================================\n";
    std::cout << " ALL MEDIA STREAMING TESTS PASSED 100%!           \n";
    std::cout << "==================================================\n";

    return 0;
}
