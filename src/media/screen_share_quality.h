#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace livekit {
enum class ScreenShareResolution { Auto, P720, P1080, P1440, Native };

struct ScreenShareQuality {
    ScreenShareResolution resolution = ScreenShareResolution::Auto;
    int fps = 20;
    bool operator==(const ScreenShareQuality&) const = default;
    bool valid() const {
        return resolution >= ScreenShareResolution::Auto && resolution <= ScreenShareResolution::Native &&
            (fps == 15 || fps == 20 || fps == 30);
    }
};

struct ScreenShareFrameProfile {
    ScreenShareQuality quality;
    int source_width = 0, source_height = 0;
    int width = 0, height = 0;
    uint64_t revision = 0;
    bool operator==(const ScreenShareFrameProfile&) const = default;
};

inline std::optional<ScreenShareFrameProfile> ResolveScreenShareProfile(
        int width, int height, ScreenShareQuality quality, uint64_t revision = 0) {
    if (!quality.valid() || width < 2 || height < 2 || width > 16384 || height > 16384) return {};
    int bound_width = 2560, bound_height = 1440;
    switch (quality.resolution) {
    case ScreenShareResolution::P720: bound_width = 1280; bound_height = 720; break;
    case ScreenShareResolution::P1080: bound_width = 1920; bound_height = 1080; break;
    case ScreenShareResolution::Native: bound_width = 3840; bound_height = 2160; break;
    default: break;
    }
    if (height > width) std::swap(bound_width, bound_height);
    if (quality.resolution == ScreenShareResolution::Native &&
        (width > bound_width || height > bound_height)) return {};
    int out_width = width, out_height = height;
    if (width > bound_width || height > bound_height) {
        if (int64_t(bound_width) * height <= int64_t(bound_height) * width) {
            out_width = bound_width;
            out_height = int(int64_t(height) * bound_width / width);
        } else {
            out_height = bound_height;
            out_width = int(int64_t(width) * bound_height / height);
        }
    }
    out_width &= ~1; out_height &= ~1;
    if (out_width < 2 || out_height < 2) return {};
    return ScreenShareFrameProfile{quality, width, height, out_width, out_height, revision};
}

// Absolute capture deadlines exclude capture/conversion time from the period.
// Missed slots are dropped; no burst is scheduled to catch up with old slots.
class ScreenCaptureDeadline {
public:
    using Clock = std::chrono::steady_clock;
    void Reset(Clock::time_point now, int fps) {
        next_ = now;
        period_ = std::chrono::nanoseconds(1000000000 / fps);
    }
    Clock::time_point Advance(Clock::time_point now) {
        next_ += period_;
        if (next_ < now) next_ = now + period_;
        return next_;
    }
private:
    Clock::time_point next_{};
    std::chrono::nanoseconds period_{50000000};
};
} // namespace livekit
