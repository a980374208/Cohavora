#pragma once

#include "telemetry_report.h"

namespace livekit::telemetry {

struct TelemetryExportRange {
    std::int64_t first_utc_ms = 0;
    std::int64_t last_utc_ms = 0;
};

TelemetryExportResult WriteTelemetryDiagnosticBundle(
    const std::filesystem::path& report_root,
    const TelemetryReportEntry& report,
    const std::filesystem::path& diagnostic_root,
    const std::filesystem::path& destination_root,
    const std::shared_ptr<std::atomic_bool>& cancelled = {},
    TelemetryExportRange range = {});

} // namespace livekit::telemetry
