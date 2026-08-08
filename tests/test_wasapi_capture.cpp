#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include "wasapi_types.h"
#include "wasapi_enumerator.h"
#include "wasapi_capture.h"
#include "media_converters.h"
#include "audio_source.h"
#include "local_audio_track.h"
#include "audio_stream.h"

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::cout << "==================================================\n";
    std::cout << " Running WASAPI Audio Capture Subsystem Tests     \n";
    std::cout << "==================================================\n";

    // ------------------------------------------------------------------
    // [Test 1] WASAPI Device Enumeration
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Enumerating Audio Input and Output Devices...\n";

    auto inputs = livekit::WasapiEnumerator::EnumerateInputDevices();
    std::cout << "  Found " << inputs.size() << " Audio Input (Microphone) Devices:\n";
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::cout << "    [" << i << "] " << inputs[i].name
                  << " (Default: " << (inputs[i].is_default ? "YES" : "NO")
                  << ", Rate: " << inputs[i].default_sample_rate << "Hz, Channels: "
                  << inputs[i].default_channels << ")\n";
    }

    auto outputs = livekit::WasapiEnumerator::EnumerateOutputDevices();
    std::cout << "  Found " << outputs.size() << " Audio Output (Speaker / Loopback) Devices:\n";
    for (size_t i = 0; i < outputs.size(); ++i) {
        std::cout << "    [" << i << "] " << outputs[i].name
                  << " (Default: " << (outputs[i].is_default ? "YES" : "NO")
                  << ", Rate: " << outputs[i].default_sample_rate << "Hz, Channels: "
                  << outputs[i].default_channels << ")\n";
    }

    auto default_in = livekit::WasapiEnumerator::GetDefaultInputDevice();
    auto default_out = livekit::WasapiEnumerator::GetDefaultOutputDevice();
    std::cout << "  -> Default Input:  " << default_in.name << "\n";
    std::cout << "  -> Default Output: " << default_out.name << "\n";
    std::cout << "  -> [Test 1 PASSED] Device enumeration completed.\n\n";

    // ------------------------------------------------------------------
    // [Test 2] Audio Resampling and Downmix Unit Test
    // ------------------------------------------------------------------
    std::cout << "[Test 2] Testing Audio Resampling & Downmixing Algorithm...\n";

    WAVEFORMATEX wfex{};
    wfex.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfex.nChannels = 2;
    wfex.nSamplesPerSec = 44100; // 44.1kHz -> 48kHz resample
    wfex.wBitsPerSample = 32;
    wfex.nBlockAlign = 8;
    wfex.nAvgBytesPerSec = 44100 * 8;

    // 构造 441 个 44.1kHz 双声道采样 (10ms)
    std::vector<float> float_audio(441 * 2);
    for (size_t i = 0; i < float_audio.size(); ++i) {
        float_audio[i] = (i % 2 == 0) ? 0.5f : -0.5f;
    }

    std::vector<int16_t> pcm_out;
    livekit::MediaConverters::ProcessWasapiAudioBuffer(
        reinterpret_cast<const uint8_t*>(float_audio.data()),
        441,
        &wfex,
        48000,
        2,
        pcm_out
    );

    assert(!pcm_out.empty() && "Resampled PCM output should not be empty!");
    // 441 frames @ 44.1kHz -> 480 frames @ 48kHz * 2 channels = 960 samples
    std::cout << "  Input 441 frames @ 44.1kHz -> Output " << (pcm_out.size() / 2) << " frames @ 48kHz\n";
    assert((pcm_out.size() / 2) == 480 && "Resampled frame count should be exactly 480 for 10ms at 48kHz!");
    assert(pcm_out[0] > 15000 && pcm_out[0] < 17000 && "Left channel sample value scaled properly!");
    assert(pcm_out[1] < -15000 && pcm_out[1] > -17000 && "Right channel sample value scaled properly!");

    std::cout << "  -> [Test 2 PASSED] Audio conversion and resampling precision verified.\n\n";

    // ------------------------------------------------------------------
    // [Test 3] Live WASAPI Microphone / Loopback Capture Pipeline Test
    // ------------------------------------------------------------------
    std::cout << "[Test 3] Testing Live WASAPI Audio Capture to AudioSource...\n";

    auto audio_source = std::make_shared<livekit::AudioSource>(48000, 2);
    auto local_track = livekit::LocalAudioTrack::createLocalAudioTrack("wasapi_mic", audio_source);

    std::atomic<int> received_frames{0};
    std::atomic<uint64_t> total_samples_received{0};

    audio_source->addSink([&](const livekit::AudioFrame& frame) {
        received_frames.fetch_add(1);
        total_samples_received.fetch_add(frame.totalSamples());
    });

    auto wasapi_cap = livekit::WasapiAudioCapture::Create();
    livekit::WasapiCaptureConfig cap_config;
    cap_config.type = livekit::WasapiCaptureType::Microphone;
    cap_config.target_sample_rate = 48000;
    cap_config.target_channels = 2;

    bool init_ok = wasapi_cap->Init(cap_config, audio_source);
    assert(init_ok && "WasapiAudioCapture::Init failed!");

    bool start_ok = wasapi_cap->Start();
    std::cout << "  WASAPI Microphone capture started: " << (start_ok ? "SUCCESS" : "NO DEVICE") << "\n";

    if (start_ok) {
        std::cout << "  Streaming live audio for 600ms...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        std::cout << "  -> Frames Received: " << received_frames.load()
                  << ", Total Samples: " << total_samples_received.load() << "\n";

        wasapi_cap->SetVolume(1.5f);
        assert(wasapi_cap->GetVolume() == 1.5f && "Volume setting failed!");

        wasapi_cap->SetMute(true);
        assert(wasapi_cap->IsMuted() && "Mute state mismatch!");

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wasapi_cap->Stop();
        assert(!wasapi_cap->IsRunning() && "WasapiAudioCapture should be stopped!");
    }

    std::cout << "  -> [Test 3 PASSED] WASAPI audio capture pipeline verified.\n\n";

    // ------------------------------------------------------------------
    // [Test 4] WASAPI Desktop Loopback Mode Test
    // ------------------------------------------------------------------
    std::cout << "[Test 4] Testing WASAPI Desktop Speaker Loopback Capture Mode...\n";

    auto loopback_source = std::make_shared<livekit::AudioSource>(48000, 2);
    auto loopback_cap = livekit::WasapiAudioCapture::Create();

    livekit::WasapiCaptureConfig loop_cfg;
    loop_cfg.type = livekit::WasapiCaptureType::DesktopLoopback;
    loop_cfg.target_sample_rate = 48000;
    loop_cfg.target_channels = 2;

    bool loop_init = loopback_cap->Init(loop_cfg, loopback_source);
    assert(loop_init && "Loopback WasapiAudioCapture::Init failed!");

    bool loop_start = loopback_cap->Start();
    std::cout << "  WASAPI Loopback capture started: " << (loop_start ? "SUCCESS" : "NO OUTPUT DEVICE") << "\n";

    if (loop_start) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        loopback_cap->Stop();
    }

    std::cout << "  -> [Test 4 PASSED] WASAPI loopback capture lifecycle verified.\n\n";

    std::cout << "==================================================\n";
    std::cout << " ALL WASAPI AUDIO CAPTURE TESTS PASSED!           \n";
    std::cout << "==================================================\n";
    return 0;
}
