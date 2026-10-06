#include <algorithm>
#include "tests/support/test_check.h"
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

#include "audio_playout_transport.h"
#include "audio_playout_warmup.h"
#include "media/audio_apm.h"
#include "webrtc_manager.h"
#include "api/environment/environment_factory.h"
#include "modules/audio_device/audio_device_buffer.h"

namespace {

constexpr uint32_t kNativeSampleRate = 48000;
constexpr size_t kNativeFramesPerBuffer = kNativeSampleRate / 100;
constexpr int16_t kUnusedSample = 12345;

class FakePlayoutTransport final : public webrtc::AudioTransport {
public:
    int16_t signal = 10000;
    std::optional<size_t> sample_count;
    int32_t result = 0;
    size_t last_bytes_per_frame = 0;
    size_t last_channels = 0;
    size_t last_frames = 0;
    uint32_t last_sample_rate = 0;

    int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t,
                                   uint32_t, uint32_t, int32_t, uint32_t,
                                   bool, uint32_t& new_mic_level) override {
        new_mic_level = 0;
        return 0;
    }

    int32_t NeedMorePlayData(size_t frames, size_t bytes_per_frame,
                            size_t channels, uint32_t sample_rate,
                            void* audio_samples, size_t& samples_out,
                            int64_t* elapsed_time_ms,
                            int64_t* ntp_time_ms) override {
        last_bytes_per_frame = bytes_per_frame;
        last_channels = channels;
        last_frames = frames;
        last_sample_rate = sample_rate;
        const size_t capacity = frames * channels;
        samples_out = sample_count.value_or(capacity);
        TEST_CHECK(samples_out <= capacity);
        auto* pcm = static_cast<int16_t*>(audio_samples);
        std::fill(pcm, pcm + capacity, kUnusedSample);
        std::fill(pcm, pcm + samples_out, signal);
        if (elapsed_time_ms) *elapsed_time_ms = 123;
        if (ntp_time_ms) *ntp_time_ms = 456;
        return result;
    }

    void PullRenderData(int, int, size_t, size_t, void*, int64_t*,
                        int64_t*) override {}
};

// The actual linked WebRTC buffer supplies the callback ABI. No audio device,
// PeerConnection factory or WebRTCManager initialization is needed.
class NativePlayoutFixture {
public:
    explicit NativePlayoutFixture(size_t channels)
        : wrapper(&inner), buffer(webrtc::CreateEnvironment()), channels_(channels) {
        TEST_CHECK(buffer.SetPlayoutSampleRate(kNativeSampleRate) == 0);
        TEST_CHECK(buffer.SetPlayoutChannels(channels) == 0);
        TEST_CHECK(buffer.RegisterAudioCallback(&wrapper) == 0);
    }

    std::vector<int16_t> Pull(size_t expected_frames = kNativeFramesPerBuffer) {
        TEST_CHECK(buffer.RequestPlayoutData(kNativeFramesPerBuffer) == expected_frames);
        std::vector<int16_t> guarded(kNativeFramesPerBuffer * channels_ + 2, kUnusedSample);
        TEST_CHECK(buffer.GetPlayoutData(guarded.data() + 1) == kNativeFramesPerBuffer);
        TEST_CHECK(guarded.front() == kUnusedSample);
        TEST_CHECK(guarded.back() == kUnusedSample);
        TEST_CHECK(inner.last_bytes_per_frame == channels_ * sizeof(int16_t));
        TEST_CHECK(inner.last_channels == channels_);
        TEST_CHECK(inner.last_frames == kNativeFramesPerBuffer);
        TEST_CHECK(inner.last_sample_rate == kNativeSampleRate);
        return {guarded.begin() + 1, guarded.end() - 1};
    }

    FakePlayoutTransport inner;
    livekit::detail::PlayoutAudioTransportWrapper wrapper;
    webrtc::AudioDeviceBuffer buffer;

private:
    size_t channels_;
};

void TestNativeAbiAndFade(size_t channels) {
    NativePlayoutFixture fixture(channels);
    fixture.inner.signal = 0;
    for (int i = 0; i < 12; ++i) {
        const auto silence = fixture.Pull();
        TEST_CHECK(std::all_of(silence.begin(), silence.end(), [](int16_t v) { return v == 0; }));
    }
    fixture.inner.signal = 10000;
    const auto first_signal = fixture.Pull();
    TEST_CHECK(first_signal.front() == 0);
    TEST_CHECK(first_signal.back() > 0 && first_signal.back() < 2000);
    if (channels == 2) {
        for (size_t frame = 0; frame < kNativeFramesPerBuffer; ++frame) {
            TEST_CHECK(first_signal[2 * frame] == first_signal[2 * frame + 1]);
        }
    }
    for (int i = 1; i < 10; ++i) fixture.Pull();
    fixture.inner.signal = -7777;
    const auto active = fixture.Pull();
    TEST_CHECK(std::all_of(active.begin(), active.end(), [](int16_t v) { return v == -7777; }));
    fixture.wrapper.ResetWarmup();
    TEST_CHECK(fixture.Pull().front() == 0);
}

void TestNativePartialSamples(size_t channels, size_t valid_samples) {
    NativePlayoutFixture fixture(channels);
    fixture.inner.sample_count = valid_samples;
    const size_t valid_frames = valid_samples / channels;
    const auto partial = fixture.Pull(valid_frames);
    TEST_CHECK(partial.front() == 0);
    TEST_CHECK(partial[valid_samples - 1] > 0 && partial[valid_samples - 1] < 2000);
    TEST_CHECK(std::all_of(partial.begin() + valid_samples, partial.end(),
                           [](int16_t v) { return v == kUnusedSample; }));
    fixture.inner.sample_count.reset();
    const auto next = fixture.Pull();
    // Only frames actually returned by the mixer advance the 100 ms fade.
    TEST_CHECK(next.front() == static_cast<int16_t>(10000 * valid_frames / 4800));
}

void TestEmptyAndFailedCallbacksDoNotConsumeFade() {
    NativePlayoutFixture fixture(2);
    fixture.inner.sample_count = 0;
    fixture.Pull(0);
    fixture.inner.sample_count.reset();
    fixture.inner.result = -1;
    fixture.Pull();
    fixture.inner.result = 0;
    TEST_CHECK(fixture.Pull().front() == 0);
}

void TestNativeRenderReference(size_t channels) {
    auto& manager = livekit::WebRTCManager::Instance();
    livekit::ApmConfig config;
    config.enable_aec = true;
    auto apm = livekit::AudioApmProcessor::Create(config);
    manager.SetApmProcessor(apm);
    NativePlayoutFixture fixture(channels);
    fixture.Pull();
    TEST_CHECK(apm->GetRenderFramesProcessed() == 1);
    fixture.Pull();
    TEST_CHECK(apm->GetRenderFramesProcessed() == 2);

    apm = livekit::AudioApmProcessor::Create(config);
    manager.SetApmProcessor(apm);
    fixture.inner.sample_count = 240;
    const size_t callbacks_per_10ms = (kNativeFramesPerBuffer * channels) / 240;
    for (size_t i = 1; i <= callbacks_per_10ms; ++i) {
        fixture.Pull(240 / channels);
        TEST_CHECK(apm->GetRenderFramesProcessed() == i / callbacks_per_10ms);
    }
    if (channels == 2) {
        apm = livekit::AudioApmProcessor::Create(config);
        manager.SetApmProcessor(apm);
        fixture.inner.sample_count = 720;
        fixture.Pull(360);
        TEST_CHECK(apm->GetRenderFramesProcessed() == 0);
        fixture.inner.sample_count = 240;
        fixture.Pull(120);
        TEST_CHECK(apm->GetRenderFramesProcessed() == 1);
    }
}

void TestNativeTransport() {
    auto& manager = livekit::WebRTCManager::Instance();
    const auto previous_apm = manager.apm_processor();
    manager.SetApmProcessor(nullptr);
    TestNativeAbiAndFade(1);
    TestNativeAbiAndFade(2);
    TestNativePartialSamples(1, 240);
    TestNativePartialSamples(2, 240); // Total samples are less than requested frames.
    TestNativePartialSamples(2, 720); // Total samples are greater than requested frames.
    TestEmptyAndFailedCallbacksDoNotConsumeFade();
    TestNativeRenderReference(1);
    TestNativeRenderReference(2);
    manager.SetApmProcessor(previous_apm);
}

} // namespace

int main() {
    constexpr uint32_t kSampleRate = 48000;
    constexpr size_t kChannels = 2;
    constexpr size_t kFramesPerBuffer = kSampleRate / 100;
    constexpr size_t kSamplesPerBuffer = kFramesPerBuffer * kChannels;

    livekit::AudioPlayoutWarmup warmup;

    // Long periods of mixer silence must not consume the audible fade window.
    for (int i = 0; i < 100; ++i) {
        std::vector<int16_t> silence(kSamplesPerBuffer, 0);
        warmup.Process(silence.data(), silence.size(), kChannels, kSampleRate);
        TEST_CHECK(std::all_of(silence.begin(), silence.end(), [](int16_t v) { return v == 0; }));
    }

    // Guard samples prove that stereo processing stays within the advertised
    // interleaved sample count.
    constexpr int16_t kGuard = 12345;
    std::vector<int16_t> guarded(kSamplesPerBuffer + 2, 10000);
    guarded.front() = kGuard;
    guarded.back() = kGuard;
    warmup.Process(guarded.data() + 1, kSamplesPerBuffer, kChannels, kSampleRate);
    TEST_CHECK(guarded.front() == kGuard);
    TEST_CHECK(guarded.back() == kGuard);
    TEST_CHECK(guarded[1] == 0);
    TEST_CHECK(guarded[kSamplesPerBuffer] > 0);
    TEST_CHECK(guarded[kSamplesPerBuffer] < 2000);

    // Complete the remaining 90 ms. The next buffer must pass through exactly.
    for (int i = 1; i < 10; ++i) {
        std::vector<int16_t> signal(kSamplesPerBuffer, 10000);
        warmup.Process(signal.data(), signal.size(), kChannels, kSampleRate);
    }
    std::vector<int16_t> active(kSamplesPerBuffer, -7777);
    warmup.Process(active.data(), active.size(), kChannels, kSampleRate);
    TEST_CHECK(std::all_of(active.begin(), active.end(), [](int16_t v) { return v == -7777; }));

    // Device/session reset must re-arm the first-audible-frame gate.
    warmup.Reset();
    std::vector<int16_t> reset_signal(kFramesPerBuffer, 9000);
    warmup.Process(reset_signal.data(), reset_signal.size(), 1, kSampleRate);
    TEST_CHECK(reset_signal.front() == 0);
    TEST_CHECK(reset_signal.back() > 0);
    TEST_CHECK(reset_signal.back() < 2000);

    // Sub-threshold comfort noise is suppressed and does not start the fade.
    warmup.Reset();
    std::vector<int16_t> comfort_noise(kSamplesPerBuffer, 16);
    warmup.Process(comfort_noise.data(), comfort_noise.size(), kChannels, kSampleRate);
    TEST_CHECK(std::all_of(comfort_noise.begin(), comfort_noise.end(), [](int16_t v) { return v == 0; }));
    std::vector<int16_t> first_signal(kSamplesPerBuffer, 5000);
    warmup.Process(first_signal.data(), first_signal.size(), kChannels, kSampleRate);
    TEST_CHECK(first_signal.front() == 0);

    TestNativeTransport();

    std::cout << "Audio playout warmup tests PASSED!\n";
    return 0;
}
