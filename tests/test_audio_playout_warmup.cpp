#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "audio_playout_warmup.h"

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
        assert(std::all_of(silence.begin(), silence.end(), [](int16_t v) { return v == 0; }));
    }

    // Guard samples prove that stereo processing stays within the advertised
    // interleaved sample count.
    constexpr int16_t kGuard = 12345;
    std::vector<int16_t> guarded(kSamplesPerBuffer + 2, 10000);
    guarded.front() = kGuard;
    guarded.back() = kGuard;
    warmup.Process(guarded.data() + 1, kSamplesPerBuffer, kChannels, kSampleRate);
    assert(guarded.front() == kGuard);
    assert(guarded.back() == kGuard);
    assert(guarded[1] == 0);
    assert(guarded[kSamplesPerBuffer] > 0);
    assert(guarded[kSamplesPerBuffer] < 2000);

    // Complete the remaining 90 ms. The next buffer must pass through exactly.
    for (int i = 1; i < 10; ++i) {
        std::vector<int16_t> signal(kSamplesPerBuffer, 10000);
        warmup.Process(signal.data(), signal.size(), kChannels, kSampleRate);
    }
    std::vector<int16_t> active(kSamplesPerBuffer, -7777);
    warmup.Process(active.data(), active.size(), kChannels, kSampleRate);
    assert(std::all_of(active.begin(), active.end(), [](int16_t v) { return v == -7777; }));

    // Device/session reset must re-arm the first-audible-frame gate.
    warmup.Reset();
    std::vector<int16_t> reset_signal(kFramesPerBuffer, 9000);
    warmup.Process(reset_signal.data(), reset_signal.size(), 1, kSampleRate);
    assert(reset_signal.front() == 0);
    assert(reset_signal.back() > 0);
    assert(reset_signal.back() < 2000);

    // Sub-threshold comfort noise is suppressed and does not start the fade.
    warmup.Reset();
    std::vector<int16_t> comfort_noise(kSamplesPerBuffer, 16);
    warmup.Process(comfort_noise.data(), comfort_noise.size(), kChannels, kSampleRate);
    assert(std::all_of(comfort_noise.begin(), comfort_noise.end(), [](int16_t v) { return v == 0; }));
    std::vector<int16_t> first_signal(kSamplesPerBuffer, 5000);
    warmup.Process(first_signal.data(), first_signal.size(), kChannels, kSampleRate);
    assert(first_signal.front() == 0);

    std::cout << "Audio playout warmup tests PASSED!\n";
    return 0;
}
