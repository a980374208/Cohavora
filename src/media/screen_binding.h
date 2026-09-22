#pragma once

#include <cstdint>
#include <string>

namespace livekit {

enum class DesktopSourceKind { Screen, Window };

struct DesktopSource {
    DesktopSourceKind kind = DesktopSourceKind::Screen;
    intptr_t id = 0;
    std::string title;
};

struct ScreenBinding {
    std::string share_session_id;
    std::uint64_t source_epoch = 0;
    intptr_t source_id = 0;
    std::string display_name;
    std::wstring device_key;
    int physical_x = 0;
    int physical_y = 0;
    int physical_width = 0;
    int physical_height = 0;
    int canonical_width = 0;
    int canonical_height = 0;

    bool operator==(const ScreenBinding &other) const = default;
};

} // namespace livekit
