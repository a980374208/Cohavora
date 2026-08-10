#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include "audio_apm.h"
#include "audio_frame.h"

int main() {
    std::cout << "[TEST] Starting WebRTC APM 3A (AEC / ANS / AGC / HPF) Processor Tests..." << std::endl;

    // Test 1: Initialize AudioApmProcessor with custom 3A configuration
    livekit::ApmConfig config;
    config.enable_aec = true;
    config.enable_ans = true;
    config.enable_agc = true;
    config.enable_hpf = true;
    config.ns_level = livekit::NoiseSuppressionLevel::VeryHigh;
    config.agc_mode = livekit::GainControlMode::AdaptiveDigital;

    auto apm = livekit::AudioApmProcessor::Create(config);
    assert(apm != nullptr);
    std::cout << "  [PASS] Test 1: AudioApmProcessor created and configured with 3A algorithms." << std::endl;

    // Test 2: Process 10ms Capture Audio Frame with Noise & Low Gain
    {
        int sample_rate = 48000;
        int num_channels = 1;
        int samples_per_channel = 480; // 10ms at 48kHz

        // Generate 10ms test frame with 50Hz hum (HPF target) + background noise (ANS target) + low amplitude (AGC target)
        std::vector<int16_t> raw_pcm(samples_per_channel);
        for (int i = 0; i < samples_per_channel; ++i) {
            double t = static_cast<double>(i) / sample_rate;
            double hum_50hz = 1000.0 * std::sin(2.0 * 3.1415926 * 50.0 * t); // 50Hz hum
            double noise = 500.0 * ((rand() % 100) / 100.0 - 0.5);          // Noise
            double voice = 2000.0 * std::sin(2.0 * 3.1415926 * 1000.0 * t); // 1kHz tone
            raw_pcm[i] = static_cast<int16_t>(hum_50hz + noise + voice);
        }

        livekit::AudioFrame raw_frame(raw_pcm, sample_rate, num_channels, samples_per_channel);
        livekit::AudioFrame clean_frame = apm->ProcessCaptureFrame(raw_frame);

        assert(clean_frame.data().size() == raw_frame.data().size());
        assert(clean_frame.sampleRate() == sample_rate);
        assert(clean_frame.numChannels() == num_channels);

        std::cout << "  [PASS] Test 2: 10ms 48kHz audio capture frame processed via APM 3A successfully!" << std::endl;
    }

    // Test 3: Process Render Frame (Speaker Audio Reference for AEC)
    {
        int sample_rate = 48000;
        int num_channels = 1;
        int samples_per_channel = 480;

        std::vector<int16_t> render_pcm(samples_per_channel, 3000);
        livekit::AudioFrame render_frame(render_pcm, sample_rate, num_channels, samples_per_channel);

        apm->ProcessRenderFrame(render_frame);
        std::cout << "  [PASS] Test 3: ProcessRenderFrame fed speaker reference to AEC canceller successfully." << std::endl;
    }

    std::cout << "[SUCCESS] ALL WebRTC APM 3A Processor Tests Passed!" << std::endl;
    return 0;
}
