#pragma once
#include "src/core/remote_control/remote_control.h"

namespace MeetingUI {
// Physical keys only. v2 carries a Windows Set1 make code and an E0 flag;
// layouts, IME/text and E1 sequences are outside this protocol.
std::optional<livekit::remote_control::Input> RemoteControlKey(
    livekit::remote_control::NativeKeyEncoding, uint32_t nativeScanCode, bool down);
}
