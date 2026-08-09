#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace livekit {

class AudioVad {
public:
    explicit AudioVad(float threshold_db = -40.0f)
        : threshold_db_(threshold_db) {}

    // Calculate RMS value from PCM 16-bit audio samples
    static float CalculateRmsPcm16(const int16_t* samples, size_t count) {
        if (!samples || count == 0) return 0.0f;
        double sum_sq = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double normalized = samples[i] / 32768.0;
            sum_sq += normalized * normalized;
        }
        return static_cast<float>(std::sqrt(sum_sq / count));
    }

    // Convert RMS to normalized audio level [0.0, 1.0]
    static float RmsToAudioLevel(float rms) {
        if (rms <= 0.00001f) return 0.0f;
        float db = 20.0f * std::log10(rms);
        // Map dB [-60dB, 0dB] to [0.0, 1.0]
        float level = (db + 60.0f) / 60.0f;
        return std::clamp(level, 0.0f, 1.0f);
    }

    // Check if voice activity detected
    bool IsSpeaking(float rms) const {
        if (rms <= 0.00001f) return false;
        float db = 20.0f * std::log10(rms);
        return db >= threshold_db_;
    }

    float threshold_db() const { return threshold_db_; }
    void set_threshold_db(float db) { threshold_db_ = db; }

private:
    float threshold_db_{-40.0f};
};

} // namespace livekit
