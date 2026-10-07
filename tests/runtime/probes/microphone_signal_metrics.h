#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace microphone_witness {

// Diagnostic input conditions, independent of the product's 200 ms PCM gate.
inline constexpr double kToneHz = 997.0;
inline constexpr double kMinimumRms = 0.005;
inline constexpr double kMinimumToneFraction = 0.30;
inline constexpr double kMaximumPeak = 0.95;

struct SignalBlock {
    std::uint64_t frames = 0;
    double sum = 0, squares = 0, peak = 0, cosine = 0, sine = 0;
    double phaseCosine = 1, phaseSine = 0;
    double stepCosine, stepSine;

    explicit SignalBlock(unsigned rate)
        : stepCosine(std::cos(2 * std::acos(-1.0) * kToneHz / rate)),
          stepSine(std::sin(2 * std::acos(-1.0) * kToneHz / rate)) {}

    void add(double value) {
        ++frames;
        sum += value;
        squares += value * value;
        peak = (std::max)(peak, std::abs(value));
        cosine += value * phaseCosine;
        sine += value * phaseSine;
        const auto nextCosine = phaseCosine * stepCosine - phaseSine * stepSine;
        phaseSine = phaseSine * stepCosine + phaseCosine * stepSine;
        phaseCosine = nextCosine;
    }

    double rms() const {
        if (!frames) return 0;
        return std::sqrt((std::max)(0.0, squares / frames - (sum / frames) * (sum / frames)));
    }

    double toneFraction() const {
        const auto energy = rms() * rms();
        if (!frames || energy <= 0) return 0;
        return (std::min)(1.0, 2 * (cosine * cosine + sine * sine) / (frames * frames) / energy);
    }

    bool nonSilent() const { return rms() >= kMinimumRms && peak < kMaximumPeak; }
    bool matchesTone() const { return nonSilent() && toneFraction() >= kMinimumToneFraction; }
};

} // namespace microphone_witness
