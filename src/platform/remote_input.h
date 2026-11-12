#pragma once
#include "src/core/remote_control/remote_control.h"

namespace livekit::remote_control {
// Called on the UI thread with the actual QPA name, never an environment hint.
Capabilities PlatformRemoteControlCapabilities(std::string_view qtPlatformName);
std::shared_ptr<InputBackend> CreatePlatformInputBackend();
}
