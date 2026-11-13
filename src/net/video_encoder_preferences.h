#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace OpenMeeting {

enum class VideoEncoderPreferencePlatform { Windows, Linux };
enum class VideoEncoderPreferenceReason { None, UnsupportedOnPlatform, InvalidValue };
struct VideoEncoderPreferenceSelection {
    std::string value = "auto";
    VideoEncoderPreferenceReason reason = VideoEncoderPreferenceReason::None;
    bool migrate_to_platform = false;
};

inline std::string_view VideoEncoderPreferenceStorageKey(VideoEncoderPreferencePlatform platform) noexcept {
    return platform == VideoEncoderPreferencePlatform::Linux
        ? "media/videoEncoders/linux/preference" : "media/videoEncoders/windows/preference";
}

inline VideoEncoderPreferenceSelection NormalizeVideoEncoderPreference(
        std::string_view value, VideoEncoderPreferencePlatform platform) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    const auto end = value.find_last_not_of(" \t\r\n");
    std::string normalized = begin == std::string_view::npos ? std::string{} : std::string(value.substr(begin, end - begin + 1));
    for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (normalized == "auto" || normalized == "software" || normalized == "nvenc" || normalized == "qsv" ||
        (normalized == "amf" && platform == VideoEncoderPreferencePlatform::Windows) ||
        (normalized == "vaapi" && platform == VideoEncoderPreferencePlatform::Linux)) {
        return {std::move(normalized), VideoEncoderPreferenceReason::None, false};
    }
    return {"auto", normalized == "amf" || normalized == "vaapi"
        ? VideoEncoderPreferenceReason::UnsupportedOnPlatform : VideoEncoderPreferenceReason::InvalidValue, false};
}

inline VideoEncoderPreferenceSelection RestoreVideoEncoderPreference(
        const std::optional<std::string>& platform_value, const std::optional<std::string>& legacy_value,
        VideoEncoderPreferencePlatform platform) {
    if (platform_value) return NormalizeVideoEncoderPreference(*platform_value, platform);
    if (!legacy_value) return {};
    auto result = NormalizeVideoEncoderPreference(*legacy_value, platform);
    // Never migrate a foreign platform's valid preference as a fabricated Auto
    // value, and never rewrite the shared legacy or another platform namespace.
    result.migrate_to_platform = result.reason == VideoEncoderPreferenceReason::None;
    return result;
}

} // namespace OpenMeeting
