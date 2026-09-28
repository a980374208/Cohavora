#pragma once

#include "session_telemetry.h"
#include "stability_ledger.h"
#include "bounded_callback.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace livekit::telemetry {

class TelemetryRunLease;
struct TelemetryCheckpointControl;
struct TelemetryExportRange;

inline constexpr char kTelemetryReportSchema[] = "cohavora-telemetry-report";
inline constexpr std::uint32_t kTelemetryReportSchemaVersion = 1;
inline constexpr std::uint32_t kTelemetryDefinitionVersion = 1;

using MetricValue = std::variant<
    std::monostate, bool, std::int64_t, std::uint64_t, double, std::string>;

struct SafeMetricRow {
    std::string key;
    MetricValue value;
    std::string unit;
    std::string availability;
    std::string reason;
    std::string measurement_point;
};

struct SafeTelemetryRecord {
    std::uint32_t schema_version = kTelemetryReportSchemaVersion;
    std::uint32_t definition_version = kTelemetryDefinitionVersion;
    std::string anonymous_session_id;
    std::int64_t captured_utc_ms = 0;
    std::int64_t source_utc_ms = 0;
    std::uint64_t source_monotonic_us = 0;
    bool complete = false;
    Snapshot snapshot;
    StabilitySummary stability;
};

using SafeTelemetryRecordPtr = std::shared_ptr<const SafeTelemetryRecord>;

struct TelemetryCheckpointRecord {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::int64_t source_utc_ms = 0;
    bool complete = false;
    std::string jsonl;
};

struct TelemetryLossRange {
    std::string anonymous_session_id;
    std::uint64_t dropped_records = 0;
    std::uint64_t first_revision = 0;
    std::uint64_t last_revision = 0;
    std::int64_t first_source_utc_ms = 0;
    std::int64_t last_source_utc_ms = 0;
};

struct TelemetryReportEntry {
    std::string record_id;
    std::int64_t created_utc_ms = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t record_count = 0;
    bool complete = false;
};

struct TelemetryCommitWatermark {
    std::string session_id;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
};

struct TelemetryStoreStatus {
    Availability availability = Availability::WarmingUp;
    std::string reason = "history_store_starting";
    bool history_enabled = true;
    std::uint64_t snapshots_accepted = 0;
    std::uint64_t one_second_buckets_coalesced = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t write_failures = 0;
    std::uint64_t corrupt_reports = 0;
    std::uint64_t unsupported_reports = 0;
    std::uint64_t corrupt_artifacts_pruned = 0;
    std::uint64_t exports_succeeded = 0;
    std::uint64_t exports_cancelled = 0;
    std::uint64_t checkpoint_revision = 0;
    std::uint64_t pending_records_dropped = 0;
    std::uint64_t pending_reports_dropped = 0;
    std::uint64_t loss_ranges_persisted = 0;
    std::uint64_t loss_persist_failures = 0;
    std::uint64_t loss_ranges_omitted = 0;
    std::size_t loss_ranges_pending = 0;
    std::size_t loss_ranges_inflight = 0;
    std::size_t pending_records = 0;
    // Lower-bound confirmations for the most recent 256 sessions, not a disk index.
    std::vector<TelemetryCommitWatermark> confirmed_commits;
    std::size_t pending_reports = 0;
    std::size_t pending_bytes = 0;
    std::vector<std::string> pending_report_ids;
    std::size_t queue_depth = 0;
    std::size_t inflight_jobs = 0;
    std::size_t queue_capacity = 0;
    std::size_t queue_bytes = 0;
    std::size_t queue_byte_capacity = 0;
    std::size_t queue_peak_jobs = 0;
    std::size_t queue_peak_bytes = 0;
    std::uint64_t queue_job_limit_hits = 0;
    std::uint64_t queue_byte_limit_hits = 0;
    std::uint64_t snapshot_max_us = 0;
    std::uint64_t checkpoint_max_us = 0;
    std::uint64_t history_refresh_max_us = 0;
    std::uint64_t history_refresh_last_us = 0;
    std::uint64_t history_refresh_count = 0;
    std::size_t memory_records = 0;
    std::size_t memory_bytes = 0;
    std::uint64_t memory_records_evicted = 0;
    std::vector<TelemetryReportEntry> reports;
};

struct TelemetryExportResult {
    bool success = false;
    bool cancelled = false;
    std::string reason;
    std::filesystem::path report_directory;
};

std::vector<SafeMetricRow> BuildSafeMetricRows(
    const SafeTelemetryRecord& record);
std::string SerializeSafeTelemetryCheckpointRecord(
    const SafeTelemetryRecord& record);

TelemetryExportResult WriteTelemetryReportAtomically(
    const std::vector<SafeTelemetryRecordPtr>& records,
    const std::filesystem::path& destination_root,
    bool managed_history,
    const std::shared_ptr<std::atomic_bool>& cancelled = {});

enum class TelemetryCloseState { Completed, Failed, TimedOut };
struct TelemetryCloseResult {
    TelemetryCloseState state = TelemetryCloseState::Failed;
    // Immutable last observation, not proof that a late commit did not occur.
    std::shared_ptr<const TelemetryStoreStatus> status;
    bool disk_outcome_unknown = false;
};

class TelemetryHistoryStore final {
public:
    // Callbacks must own their captures and must not synchronously Close this store.
    using ExportCallback = BoundedCallback<void(TelemetryExportResult)>;
    using MutationCallback = BoundedCallback<void(bool, std::string)>;

    static constexpr std::size_t kDefaultMemoryBuckets = 300;
    static constexpr std::size_t kDefaultMemoryBytes = 8 * 1024 * 1024;
    // Real catalog/page transitions publish >100 revisions in 100 ms. Keep
    // that burst within the existing retained-memory budget; the byte limit
    // below remains authoritative for large snapshots and callbacks.
    static constexpr std::size_t kDefaultQueueCapacity = 256;
    static constexpr std::size_t kDefaultQueueBytes = 16 * 1024 * 1024;
    static constexpr std::uint64_t kDefaultMaximumBytes = 100ull * 1024ull * 1024ull;
    static constexpr auto kDefaultRetention = std::chrono::hours(24 * 7);

    explicit TelemetryHistoryStore(
        std::filesystem::path root,
        std::size_t memory_buckets = kDefaultMemoryBuckets,
        std::size_t queue_capacity = kDefaultQueueCapacity,
        std::uint64_t maximum_bytes = kDefaultMaximumBytes,
        std::chrono::hours retention = kDefaultRetention,
        std::size_t queue_byte_capacity = kDefaultQueueBytes,
        std::string process_run_id = {},
        std::filesystem::path diagnostic_root = {});
    TelemetryHistoryStore(std::filesystem::path root,
                          std::string process_run_id,
                          std::filesystem::path diagnostic_root = {});
    ~TelemetryHistoryStore();

    // Stop admission and drain within budget; timeout detaches owned state.
    // Call on the managed cleanup worker. Repeated calls return the same result.
    TelemetryCloseResult Close(std::chrono::milliseconds budget = std::chrono::seconds(5));

    TelemetryHistoryStore(const TelemetryHistoryStore&) = delete;
    TelemetryHistoryStore& operator=(const TelemetryHistoryStore&) = delete;

    bool SubmitSnapshot(
        SessionTelemetry::SnapshotPtr snapshot,
        StabilitySummary stability = {},
        std::string anonymous_session_id = {});
    bool ExportCurrent(
        std::filesystem::path destination_root,
        ExportCallback callback,
        std::shared_ptr<std::atomic_bool> cancelled = {});
    bool ExportReport(
        std::string record_id,
        std::filesystem::path destination_root,
        ExportCallback callback,
        std::shared_ptr<std::atomic_bool> cancelled = {},
        std::int64_t first_utc_ms = 0,
        std::int64_t last_utc_ms = 0);
    bool ClearReport(std::string record_id, MutationCallback callback = {});
    bool RetryCheckpoint(std::string record_id, MutationCallback callback = {});
    void SetHistoryEnabled(bool enabled);

    std::shared_ptr<const TelemetryStoreStatus> Status() const;
    std::vector<SafeTelemetryRecordPtr> CurrentRecords() const;

private:
    friend struct TelemetryHistoryStoreTestAccess;
    struct WorkerContext;
    const std::shared_ptr<TelemetryCheckpointControl> control_;
    const std::shared_ptr<WorkerContext> context_;
    std::mutex close_mutex_;
    std::thread worker_;
    bool closed_ = false;
    TelemetryCloseResult close_result_;
};

void InstallTelemetryHistoryStore(std::shared_ptr<TelemetryHistoryStore> store);
std::shared_ptr<TelemetryHistoryStore> InstalledTelemetryHistoryStore();

} // namespace livekit::telemetry
