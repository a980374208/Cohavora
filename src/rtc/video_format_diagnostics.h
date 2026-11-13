#pragma once

#include "api/rtp_parameters.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include <cstdint>
#include <optional>
#include <string_view>

namespace livekit::detail {

// Pure, closed projection. No source parameter string, SDP or identifier is
// retained, including when a peer supplies malformed format parameters.
struct H264FormatProjection {
    bool valid = false;
    bool profile_level_defaulted = false;
    std::uint32_t profile_level_id = 0;
    // 0 unknown; 1 constrained baseline, 2 baseline, 3 main,
    // 4 constrained high, 5 high, 6 predictive high 4:4:4.
    std::uint8_t profile = 0;
    // WebRTC H264Level values: 0 is level 1b, 255 is unknown.
    std::uint8_t level = 255;
    // SDP defaults are zero; malformed/unsupported values are unknown (-1).
    std::int8_t packetization_mode = -1;
    std::int8_t level_asymmetry = -1;
};

inline std::optional<H264FormatProjection> ProjectH264Format(
    std::string_view name, const webrtc::CodecParameterMap& parameters) {
    if (name.size() != 4 || (name[0] != 'H' && name[0] != 'h') || name.substr(1) != "264") {
        return std::nullopt;
    }
    H264FormatProjection result;
    const auto binary_parameter = [&](const char* key) -> std::int8_t {
        const auto found = parameters.find(key);
        if (found == parameters.end() || found->second == "0") return 0;
        return found->second == "1" ? 1 : -1;
    };
    result.packetization_mode = binary_parameter("packetization-mode");
    result.level_asymmetry = binary_parameter("level-asymmetry-allowed");
    result.profile_level_defaulted = parameters.find("profile-level-id") == parameters.end();
    std::uint32_t profile_level_id = 0x42e01f;
    if (!result.profile_level_defaulted) {
        const auto& text = parameters.at("profile-level-id");
        if (text.size() != 6) return result;
        profile_level_id = 0;
        for (const char ch : text) {
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9') digit = unsigned(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = unsigned(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') digit = unsigned(ch - 'A' + 10);
            else return result;
            profile_level_id = (profile_level_id << 4) | digit;
        }
    }
    const auto parsed = webrtc::ParseSdpForH264ProfileLevelId(parameters);
    if (!parsed) return result;
    switch (parsed->profile) {
    case webrtc::H264Profile::kProfileConstrainedBaseline: result.profile = 1; break;
    case webrtc::H264Profile::kProfileBaseline: result.profile = 2; break;
    case webrtc::H264Profile::kProfileMain: result.profile = 3; break;
    case webrtc::H264Profile::kProfileConstrainedHigh: result.profile = 4; break;
    case webrtc::H264Profile::kProfileHigh: result.profile = 5; break;
    case webrtc::H264Profile::kProfilePredictiveHigh444: result.profile = 6; break;
    }
    result.level = static_cast<std::uint8_t>(parsed->level);
    result.profile_level_id = profile_level_id;
    result.valid = result.profile != 0 && result.packetization_mode >= 0 && result.level_asymmetry >= 0;
    return result;
}

} // namespace livekit::detail
