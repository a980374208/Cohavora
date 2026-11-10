#pragma once

#include <string_view>

namespace livekit {

enum class VideoResolutionMatch { Auto, Up, Down, Nearest };

constexpr const char* VideoResolutionMatchName(VideoResolutionMatch mode) {
    switch (mode) {
    case VideoResolutionMatch::Up: return "up";
    case VideoResolutionMatch::Down: return "down";
    case VideoResolutionMatch::Nearest: return "nearest";
    default: return "auto";
    }
}

constexpr VideoResolutionMatch ParseVideoResolutionMatch(std::string_view name) {
    if (name == "up") return VideoResolutionMatch::Up;
    if (name == "down") return VideoResolutionMatch::Down;
    if (name == "nearest") return VideoResolutionMatch::Nearest;
    return VideoResolutionMatch::Auto;
}

} // namespace livekit
