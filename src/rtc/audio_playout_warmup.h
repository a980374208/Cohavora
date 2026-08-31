#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace livekit {

// Real-time-safe gate for the first audible playout samples. Silence does not
// consume the fade window, so a playout stream can be opened before a remote
// track without exposing the first decoder/device discontinuity at full scale.
class AudioPlayoutWarmup {
public:
    static constexpr uint32_t kFadeDurationMs = 100;
    static constexpr int32_t kSignalThreshold = 32;

    void Reset() noexcept {
        reset_generation_.fetch_add(1, std::memory_order_release);
    }

    void Process(int16_t* samples,
                 size_t sample_count,
                 size_t channels,
                 uint32_t sample_rate) noexcept {
        if (!samples || sample_count == 0 || channels == 0 || sample_rate == 0) {
            return;
        }

        const uint32_t generation = reset_generation_.load(std::memory_order_acquire);
        if (generation != observed_generation_) {
            observed_generation_ = generation;
            state_ = State::WaitingForSignal;
            faded_frames_ = 0;
        }

        if (state_ == State::Active) {
            return;
        }

        const size_t frame_count = sample_count / channels;
        const size_t process_sample_count = frame_count * channels;
        if (frame_count == 0) {
            return;
        }

        if (state_ == State::WaitingForSignal) {
            int32_t peak = 0;
            for (size_t i = 0; i < process_sample_count; ++i) {
                peak = std::max(peak, std::abs(static_cast<int32_t>(samples[i])));
            }
            if (peak <= kSignalThreshold) {
                std::fill(samples, samples + process_sample_count, int16_t{0});
                return;
            }
            state_ = State::Fading;
        }

        const uint64_t fade_frames = std::max<uint64_t>(
            1, static_cast<uint64_t>(sample_rate) * kFadeDurationMs / 1000);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            const uint64_t numerator = std::min(faded_frames_, fade_frames);
            for (size_t channel = 0; channel < channels; ++channel) {
                const size_t index = frame * channels + channel;
                const int64_t scaled = static_cast<int64_t>(samples[index]) *
                                       static_cast<int64_t>(numerator) /
                                       static_cast<int64_t>(fade_frames);
                samples[index] = static_cast<int16_t>(scaled);
            }
            if (faded_frames_ < fade_frames) {
                ++faded_frames_;
            }
        }

        if (faded_frames_ >= fade_frames) {
            state_ = State::Active;
        }
    }

private:
    enum class State {
        WaitingForSignal,
        Fading,
        Active,
    };

    std::atomic<uint32_t> reset_generation_{1};
    uint32_t observed_generation_ = 0;
    State state_ = State::WaitingForSignal;
    uint64_t faded_frames_ = 0;
};

} // namespace livekit
