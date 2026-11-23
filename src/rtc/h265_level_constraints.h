#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "api/rtp_parameters.h"
#include "api/video_codecs/h265_profile_tier_level.h"
#include "modules/video_coding/codecs/external/amf_hevc_level_limits.h"
#include "media/screen_share_quality.h"

namespace livekit::detail {

// A closed projection: never retain peer-supplied format strings in diagnostics.
struct H265FormatProjection {
    bool valid = false;
    bool profile_level_defaulted = false;
    std::uint8_t profile = 0;
    std::uint8_t tier = 255;
    std::uint8_t level = 255;
};

inline std::optional<H265FormatProjection> ProjectH265Format(
        std::string_view name, const webrtc::CodecParameterMap& parameters) {
    if (name.size() != 4 || (name[0] != 'H' && name[0] != 'h') ||
        name.substr(1) != "265") return std::nullopt;
    H265FormatProjection result;
    result.profile_level_defaulted = parameters.find("profile-id") == parameters.end() ||
        parameters.find("tier-flag") == parameters.end() ||
        parameters.find("level-id") == parameters.end();
    const auto parsed = webrtc::ParseSdpForH265ProfileTierLevel(parameters);
    if (!parsed || parsed->profile != webrtc::H265Profile::kProfileMain ||
        parsed->tier != webrtc::H265Tier::kTier0) return result;
    const auto level = static_cast<std::uint8_t>(parsed->level);
    if (!webrtc::external_encoder::FindAmfHevcLevelLimits(level)) return result;
    const auto mode = parameters.find("tx-mode");
    if (mode != parameters.end()) {
        constexpr std::string_view supported_mode = "SRST";
        if (mode->second.size() != supported_mode.size()) return result;
        for (std::size_t i = 0; i < supported_mode.size(); ++i) {
            if (mode->second[i] != supported_mode[i] &&
                mode->second[i] != supported_mode[i] + ('a' - 'A')) return result;
        }
    }
    for (const char* key : {"sprop-max-don-diff", "sprop-depack-buf-nalus",
                            "sprop-depack-buf-bytes"}) {
        const auto value = parameters.find(key);
        if (value != parameters.end() && value->second != "0") return result;
    }
    result.profile = 1;
    result.tier = 0;
    result.level = level;
    result.valid = true;
    return result;
}

enum class H265LevelConstraintStatus {
    Supported,
    InvalidParameters,
    UnsupportedLevel,
    FrameSizeExceeded,
    SampleRateExceeded,
    DimensionExceeded,
    BitrateExceeded,
};

// Reserve complete 64-pixel coding-tree blocks. AMF still independently
// validates the actual SPS coded geometry; visible dimensions cannot relax it.
// A bitrate of 1 can be used for geometry planning before computing the budget.
inline H265LevelConstraintStatus CheckH265LevelConstraints(
        std::uint8_t level, int width, int height, std::uint32_t framerate_fps,
        std::uint32_t bitrate_bps = 1) noexcept {
    using Status = H265LevelConstraintStatus;
    if (width < 2 || height < 2 || width % 2 || height % 2 ||
        framerate_fps == 0 || bitrate_bps == 0) return Status::InvalidParameters;
    const auto* limits = webrtc::external_encoder::FindAmfHevcLevelLimits(level);
    if (!limits) return Status::UnsupportedLevel;
    const auto coded_width = (std::uint64_t(width) + 63) / 64 * 64;
    const auto coded_height = (std::uint64_t(height) + 63) / 64 * 64;
    const auto pixels = coded_width * coded_height;
    if (pixels > limits->max_luma_picture_size) return Status::FrameSizeExceeded;
    if (coded_width * coded_width > 8 * limits->max_luma_picture_size ||
        coded_height * coded_height > 8 * limits->max_luma_picture_size)
        return Status::DimensionExceeded;
    if (framerate_fps > limits->max_luma_sample_rate / pixels)
        return Status::SampleRateExceeded;
    if (bitrate_bps > limits->max_main_tier_bitrate_bps)
        return Status::BitrateExceeded;
    return Status::Supported;
}

// Preserve an already legal profile, otherwise choose a lower existing quality
// mode. Its resolution intent must match its dimensions because capture replays
// ResolveScreenShareProfile when SetQuality is applied or the source changes.
inline std::optional<ScreenShareFrameProfile> ConstrainH265ScreenShareProfile(
        const ScreenShareFrameProfile& requested, std::uint8_t level,
        std::uint32_t bitrate_bps = 1) {
    if (!requested.quality.valid() || requested.width < 2 || requested.height < 2)
        return std::nullopt;
    const auto resolved = ResolveScreenShareProfile(requested.source_width,
        requested.source_height, requested.quality, requested.revision);
    if (!resolved) return std::nullopt;
    const auto legal = [&](const ScreenShareFrameProfile& profile) {
        return profile.width <= requested.width && profile.height <= requested.height &&
            CheckH265LevelConstraints(level, profile.width, profile.height,
                profile.quality.fps, bitrate_bps) == H265LevelConstraintStatus::Supported;
    };
    if (resolved->width == requested.width && resolved->height == requested.height &&
        legal(requested)) return requested;
    static constexpr std::array modes{ScreenShareResolution::P2160,
        ScreenShareResolution::P1440, ScreenShareResolution::P1080,
        ScreenShareResolution::P720};
    for (const auto mode : modes) {
        auto quality = requested.quality;
        quality.resolution = mode;
        const auto candidate = ResolveScreenShareProfile(requested.source_width,
            requested.source_height, quality, requested.revision);
        if (candidate && legal(*candidate)) return candidate;
    }
    return std::nullopt;
}

} // namespace livekit::detail
