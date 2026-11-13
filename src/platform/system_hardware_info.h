#pragma once

namespace livekit::platform {

// Called once at startup on a worker, after the diagnostic pipeline is ready.
// OS inventory only; GPU presence does not prove encoder/decoder availability.
void LogSystemHardwareInfo() noexcept;

} // namespace livekit::platform
