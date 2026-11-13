#pragma once

#include <array>
#include <cstdint>

#include "api/video_codecs/h264_profile_level_id.h"

namespace livekit::detail {

enum class H264LevelConstraintStatus {
    Supported,
    InvalidParameters,
    UnsupportedLevel,
    FrameSizeExceeded,
    MacroblockRateExceeded,
    DimensionExceeded,
};

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
    struct Limits {
        webrtc::H264Level level;
        std::uint32_t max_frame_macroblocks;
        std::uint32_t max_macroblocks_per_second;
    };
    static constexpr std::array<Limits, 17> limits{{
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
        if (limit.level != level) continue;
        const auto macroblock_width = (std::uint64_t(width) + 15) / 16;
        const auto macroblock_height = (std::uint64_t(height) + 15) / 16;
        const auto frame_macroblocks = macroblock_width * macroblock_height;
        if (frame_macroblocks > limit.max_frame_macroblocks) {
            return Status::FrameSizeExceeded;
        }
        // Annex A also bounds each coded dimension by sqrt(MaxFS * 8).
        // Compare squares exactly instead of rounding a floating-point sqrt.
        const auto max_dimension_squared = std::uint64_t(limit.max_frame_macroblocks) * 8;
        if (macroblock_width * macroblock_width > max_dimension_squared ||
            macroblock_height * macroblock_height > max_dimension_squared) {
            return Status::DimensionExceeded;
        }
        // Division avoids overflow for untrusted frame rates and dimensions.
        if (frame_macroblocks > limit.max_macroblocks_per_second / framerate_fps) {
            return Status::MacroblockRateExceeded;
        }
        return Status::Supported;
    }
    return Status::UnsupportedLevel;
}

} // namespace livekit::detail
