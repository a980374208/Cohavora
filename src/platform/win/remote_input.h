#pragma once
#include "src/core/remote_control/remote_control.h"

namespace livekit::remote_control {
// The returned owner has its own message pump, physical-input hooks and lease
// watchdog. Destruction never requires Qt callbacks or the Session Strand.
std::shared_ptr<InputBackend> CreateWindowsInputBackend();
}
