#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include "api/video_codecs/h264_profile_level_id.h"
#include "media/screen_share_quality.h"

namespace livekit::detail {

enum class H264LevelConstraintStatus {
    Supported,
    InvalidParameters,
    UnsupportedLevel,
    FrameSizeExceeded,
    MacroblockRateExceeded,
    DimensionExceeded,
};

struct H264LevelLimits {
    webrtc::H264Level level;
    std::uint32_t max_frame_macroblocks;
    std::uint32_t max_macroblocks_per_second;
};

inline const H264LevelLimits* FindH264LevelLimits(webrtc::H264Level level) noexcept {
    static constexpr std::array<H264LevelLimits, 17> limits{{
        {webrtc::H264Level::kLevel1_b, 99, 1485},
        {webrtc::H264Level::kLevel1, 99, 1485},
        {webrtc::H264Level::kLevel1_1, 396, 3000},
        {webrtc::H264Level::kLevel1_2, 396, 6000},
        {webrtc::H264Level::kLevel1_3, 396, 11880},
        {webrtc::H264Level::kLevel2, 396, 11880},
        {webrtc::H264Level::kLevel2_1, 792, 19800},
        {webrtc::H264Level::kLevel2_2, 1620, 20250},
        {webrtc::H264Level::kLevel3, 1620, 40500},
        {webrtc::H264Level::kLevel3_1, 3600, 108000},
        {webrtc::H264Level::kLevel3_2, 5120, 216000},
        {webrtc::H264Level::kLevel4, 8192, 245760},
        {webrtc::H264Level::kLevel4_1, 8192, 245760},
        {webrtc::H264Level::kLevel4_2, 8704, 522240},
        {webrtc::H264Level::kLevel5, 22080, 589824},
        {webrtc::H264Level::kLevel5_1, 36864, 983040},
        {webrtc::H264Level::kLevel5_2, 36864, 2073600},
    }};
    for (const auto& limit : limits) {
        if (limit.level == level) return &limit;
    }
    return nullptr;
}

// Progressive-frame geometry limits from ITU-T H.264 Table A-1. This checks
// the negotiated level itself. It does not implement or advertise receiver
// extensions such as max-fs/max-fr/max-mbps, and must not silently use those
// extensions to relax an AMF session that can only express ProfileLevel.
inline H264LevelConstraintStatus CheckH264LevelConstraints(
        webrtc::H264Level level, int width, int height,
        std::uint32_t framerate_fps) noexcept {
    using Status = H264LevelConstraintStatus;
    if (width <= 0 || height <= 0 || framerate_fps == 0) {
        return Status::InvalidParameters;
    }
    const auto* limit = FindH264LevelLimits(level);
    if (!limit) {
        return Status::UnsupportedLevel;
    }
    const auto macroblock_width = (std::uint64_t(width) + 15) / 16;
    const auto macroblock_height = (std::uint64_t(height) + 15) / 16;
    const auto frame_macroblocks = macroblock_width * macroblock_height;
    if (frame_macroblocks > limit->max_frame_macroblocks) {
        return Status::FrameSizeExceeded;
    }
    // Annex A also bounds each coded dimension by sqrt(MaxFS * 8).
    // Compare squares exactly instead of rounding a floating-point sqrt.
    const auto max_dimension_squared = std::uint64_t(limit->max_frame_macroblocks) * 8;
    if (macroblock_width * macroblock_width > max_dimension_squared ||
        macroblock_height * macroblock_height > max_dimension_squared) {
        return Status::DimensionExceeded;
    }
    // Division avoids overflow for untrusted frame rates and dimensions.
    if (frame_macroblocks > limit->max_macroblocks_per_second / framerate_fps) {
        return Status::MacroblockRateExceeded;
    }
    return Status::Supported;
}

// Scales dimensions proportionally to the largest even pair accepted by `level`
// at `framerate_fps`, returning the input untouched when already supported.
inline std::optional<std::pair<int, int>> ConstrainH264Dimensions(
        webrtc::H264Level level, int width, int height,
        std::uint32_t framerate_fps) noexcept {
    if (width <= 0 || height <= 0 || framerate_fps == 0 || !FindH264LevelLimits(level)) {
        return std::nullopt;
    }
    if (CheckH264LevelConstraints(level, width, height, framerate_fps) ==
        H264LevelConstraintStatus::Supported) {
        return std::pair{width, height};
    }
    const bool landscape = width >= height;
    const int major = landscape ? width : height;
    const int minor = landscape ? height : width;
    if (major < 2 || minor < 2) {
        return std::nullopt;
    }
    int low = 1;
    int high = major / 2;
    int best_major = 0;
    int best_minor = 0;
    while (low <= high) {
        const int mid = low + (high - low) / 2;
        const int cand_major = mid * 2;
        const int cand_minor = static_cast<int>(
            (std::int64_t(minor) * cand_major) / major) & ~1;
        if (cand_minor < 2) {
            low = mid + 1;
            continue;
        }
        const int cand_w = landscape ? cand_major : cand_minor;
        const int cand_h = landscape ? cand_minor : cand_major;
        if (CheckH264LevelConstraints(level, cand_w, cand_h, framerate_fps) ==
            H264LevelConstraintStatus::Supported) {
            best_major = cand_major;
            best_minor = cand_minor;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    if (best_major < 2 || best_minor < 2) {
        return std::nullopt;
    }
    return landscape ? std::pair{best_major, best_minor}
                     : std::pair{best_minor, best_major};
}

inline std::optional<ScreenShareFrameProfile> ConstrainH264ScreenShareProfile(
        const ScreenShareFrameProfile& profile, webrtc::H264Level level) noexcept {
    if (!profile.quality.valid() || profile.width < 2 || profile.height < 2) {
        return std::nullopt;
    }
    const auto fps = static_cast<std::uint32_t>(profile.quality.fps);
    if (CheckH264LevelConstraints(level, profile.width, profile.height, fps) ==
        H264LevelConstraintStatus::Supported) {
        return profile;
    }
    const auto constrained_dims =
        ConstrainH264Dimensions(level, profile.width, profile.height, fps);
    if (!constrained_dims) {
        return std::nullopt;
    }
    ScreenShareFrameProfile constrained = profile;
    constrained.width = constrained_dims->first;
    constrained.height = constrained_dims->second;
    const int long_side = std::max(constrained.width, constrained.height);
    const int short_side = std::min(constrained.width, constrained.height);
    if (long_side <= 1280 && short_side <= 720) {
        constrained.quality.resolution = ScreenShareResolution::P720;
    } else if (long_side <= 1920 && short_side <= 1080) {
        constrained.quality.resolution = ScreenShareResolution::P1080;
    } else if (long_side <= 2560 && short_side <= 1440 &&
               (profile.quality.resolution == ScreenShareResolution::Native ||
                profile.quality.resolution == ScreenShareResolution::P2160)) {
        constrained.quality.resolution = ScreenShareResolution::P1440;
    }
    return constrained;
}

} // namespace livekit::detail
