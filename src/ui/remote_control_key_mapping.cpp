#include "remote_control_key_mapping.h"

namespace MeetingUI {
std::optional<livekit::remote_control::Input> RemoteControlKey(
        livekit::remote_control::NativeKeyEncoding encoding, uint32_t nativeScanCode, bool down) {
    namespace rc = livekit::remote_control;
    rc::Input result;
    result.kind = rc::InputKind::Key;
    result.down = down;
    if (encoding == rc::NativeKeyEncoding::WindowsSet1) {
        // Preserve the existing Qt Windows native scan/E0 interpretation.
        result.code = int(nativeScanCode & 0xff);
        result.extended = (nativeScanCode & 0xff00) != 0;
    } else if (encoding == rc::NativeKeyEncoding::LinuxEvdevOffset8) {
        if (nativeScanCode < 9) return {};
        const auto evdev = nativeScanCode - 8;
        // The original PC keys retain Set1 numbering; later evdev keys do not.
        if ((evdev >= 1 && evdev <= 83) || (evdev >= 86 && evdev <= 88)) {
            result.code = int(evdev);
        } else {
            result.extended = true;
            switch (evdev) {
            case 96: result.code = 0x1c; break; // keypad Enter
            case 97: result.code = 0x1d; break; // right Control
            case 98: result.code = 0x35; break; // keypad divide
            case 99: result.code = 0x37; break; // Print Screen
            case 100: result.code = 0x38; break; // right Alt
            case 102: result.code = 0x47; break; // Home
            case 103: result.code = 0x48; break; // Up
            case 104: result.code = 0x49; break; // Page Up
            case 105: result.code = 0x4b; break; // Left
            case 106: result.code = 0x4d; break; // Right
            case 107: result.code = 0x4f; break; // End
            case 108: result.code = 0x50; break; // Down
            case 109: result.code = 0x51; break; // Page Down
            case 110: result.code = 0x52; break; // Insert
            case 111: result.code = 0x53; break; // Delete
            case 125: result.code = 0x5b; break; // left Meta
            case 126: result.code = 0x5c; break; // right Meta
            case 127: result.code = 0x5d; break; // Menu
            default: return {}; // No truncation, symbolic fallback or E1 emulation.
            }
        }
    } else return {};
    return result.valid() ? std::optional(result) : std::nullopt;
}
}
