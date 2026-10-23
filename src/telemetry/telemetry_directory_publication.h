#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

namespace livekit::telemetry {

// Local I/O diagnostics only. No paths, OS messages or user data enter status.
struct TelemetryDirectoryPublication {
    bool success = false;
    bool cancelled = false;
    unsigned attempts = 0;
    int first_error = 0;
    int last_error = 0;
};

TelemetryDirectoryPublication PublishTelemetryDirectory(
    const std::filesystem::path& temporary,
    const std::filesystem::path& final,
    const std::shared_ptr<std::atomic_bool>& cancelled = {});

} // namespace livekit::telemetry
