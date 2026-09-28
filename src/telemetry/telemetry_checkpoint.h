#pragma once

#include "telemetry_report.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace livekit::telemetry {

// The atomic append temporarily retains both old and new manifests. Callers
// reserving global history space must include this bound before pruning.
inline constexpr std::size_t kTelemetryCheckpointMaximumManifestBytes = 1024 * 1024;

struct TelemetryCheckpointStatus {
    bool valid = false;
    bool session_complete = false;
    std::string reason;
    std::string anonymous_session_id;
    std::string process_run_id;
    std::uint64_t session_generation = 0;
    std::uint64_t last_committed_revision = 0;
    std::uint64_t segment_count = 0;
    std::uint64_t record_count = 0;
    std::uint64_t size_bytes = 0;
    std::int64_t created_utc_ms = 0;
    std::uint64_t pruned_records = 0;
    std::uint64_t pruned_segments = 0;
    std::int64_t pruned_through_utc_ms = 0;
    std::uint64_t missing_revisions = 0;
    std::uint64_t missing_ranges_omitted = 0;
    std::int64_t first_retained_source_utc_ms = 0;
    std::int64_t last_retained_source_utc_ms = 0;
    std::uint64_t orphan_files = 0;
    std::uint64_t orphan_bytes = 0;
};

struct TelemetryCheckpointResult {
    bool success = false;
    std::string reason;
    std::filesystem::path report_directory;
    TelemetryCheckpointStatus status;
};

class TelemetryCheckpointReadLease final {
public:
    explicit TelemetryCheckpointReadLease(
        const std::filesystem::path& report_directory);
    ~TelemetryCheckpointReadLease();
    TelemetryCheckpointReadLease(const TelemetryCheckpointReadLease&) = delete;
    TelemetryCheckpointReadLease& operator=(
        const TelemetryCheckpointReadLease&) = delete;
    bool acquired() const noexcept;

private:
    void* handle_ = nullptr;
    bool acquired_ = false;
};

class TelemetryRunLease final {
public:
    TelemetryRunLease(const std::filesystem::path& root,
                      std::string_view process_run_id);
    ~TelemetryRunLease();
    TelemetryRunLease(const TelemetryRunLease&) = delete;
    TelemetryRunLease& operator=(const TelemetryRunLease&) = delete;
    bool acquired() const noexcept;

private:
    std::filesystem::path path_;
    void* handle_ = nullptr;
    bool acquired_ = false;
};

bool IsTelemetryRunActive(const std::filesystem::path& root,
                          std::string_view process_run_id);

TelemetryCheckpointRecord MakeTelemetryCheckpointRecord(
    const SafeTelemetryRecord& record);

TelemetryCheckpointStatus InspectTelemetryCheckpoint(
    const std::filesystem::path& report_directory);

TelemetryCheckpointResult AppendTelemetryCheckpoint(
    const std::filesystem::path& root,
    std::string_view anonymous_session_id,
    const std::vector<TelemetryCheckpointRecord>& records,
    std::uint64_t maximum_report_bytes = 64ull * 1024ull * 1024ull,
    std::string_view process_run_id = {},
    std::uint64_t maximum_history_bytes = 100ull * 1024ull * 1024ull);
TelemetryCheckpointResult AppendTelemetryCheckpoint(
    const std::filesystem::path& root,
    const std::vector<SafeTelemetryRecordPtr>& records,
    std::uint64_t maximum_report_bytes = 64ull * 1024ull * 1024ull);
bool RemoveTelemetryCheckpoint(const std::filesystem::path& report_directory);
bool PruneTelemetryCheckpointOrphans(
    const std::filesystem::path& report_directory);
std::uint64_t TelemetryCheckpointArtifactBytes(
    const std::filesystem::path& report_directory);
bool DiscardCorruptTelemetryCheckpointArtifacts(
    const std::filesystem::path& report_directory);
std::optional<std::uint64_t> TelemetryHistoryOwnedBytes(
    const std::filesystem::path& root);
bool PersistTelemetryLossRanges(
    const std::filesystem::path& root,
    const std::vector<TelemetryLossRange>& losses,
    std::uint64_t maximum_history_bytes = 100ull * 1024ull * 1024ull);
bool ClearTelemetryLossSummary(const std::filesystem::path& root);

} // namespace livekit::telemetry
