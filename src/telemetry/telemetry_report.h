#pragma once

#include "session_telemetry.h"
#include "stability_ledger.h"

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
    bool complete = false;
    Snapshot snapshot;
    StabilitySummary stability;
};

using SafeTelemetryRecordPtr = std::shared_ptr<const SafeTelemetryRecord>;

struct TelemetryReportEntry {
    std::string record_id;
    std::int64_t created_utc_ms = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t record_count = 0;
    bool complete = false;
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
    std::uint64_t exports_succeeded = 0;
    std::uint64_t exports_cancelled = 0;
    std::size_t queue_depth = 0;
    std::size_t queue_capacity = 0;
    std::size_t memory_records = 0;
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

TelemetryExportResult WriteTelemetryReportAtomically(
    const std::vector<SafeTelemetryRecordPtr>& records,
    const std::filesystem::path& destination_root,
    bool managed_history,
    const std::shared_ptr<std::atomic_bool>& cancelled = {});

class TelemetryHistoryStore final {
public:
    using ExportCallback = std::function<void(TelemetryExportResult)>;
    using MutationCallback = std::function<void(bool, std::string)>;

    static constexpr std::size_t kDefaultMemoryBuckets = 300;
    static constexpr std::size_t kDefaultQueueCapacity = 64;
    static constexpr std::uint64_t kDefaultMaximumBytes = 100ull * 1024ull * 1024ull;
    static constexpr auto kDefaultRetention = std::chrono::hours(24 * 7);

    explicit TelemetryHistoryStore(
        std::filesystem::path root,
        std::size_t memory_buckets = kDefaultMemoryBuckets,
        std::size_t queue_capacity = kDefaultQueueCapacity,
        std::uint64_t maximum_bytes = kDefaultMaximumBytes,
        std::chrono::hours retention = kDefaultRetention);
    ~TelemetryHistoryStore();

    // Stop admission, persist accepted records, and join. Call on the managed
    // cleanup worker before application exit; retained readers remain valid.
    void Close();

    TelemetryHistoryStore(const TelemetryHistoryStore&) = delete;
    TelemetryHistoryStore& operator=(const TelemetryHistoryStore&) = delete;

    bool SubmitSnapshot(
        SessionTelemetry::SnapshotPtr snapshot,
        StabilitySummary stability = {});
    bool ExportCurrent(
        std::filesystem::path destination_root,
        ExportCallback callback,
        std::shared_ptr<std::atomic_bool> cancelled = {});
    bool ClearReport(std::string record_id, MutationCallback callback = {});
    void SetHistoryEnabled(bool enabled);

    std::shared_ptr<const TelemetryStoreStatus> Status() const;
    std::vector<SafeTelemetryRecordPtr> CurrentRecords() const;

private:
    enum class JobKind { Snapshot, Export, ClearReport, SetHistoryEnabled };
    struct Job {
        JobKind kind = JobKind::Snapshot;
        SessionTelemetry::SnapshotPtr snapshot;
        StabilitySummary stability;
        std::filesystem::path destination;
        std::string record_id;
        ExportCallback export_callback;
        MutationCallback mutation_callback;
        std::shared_ptr<std::atomic_bool> cancelled;
        bool enabled = true;
    };

    void Run();
    void HandleSnapshot(Job job);
    void HandleExport(Job job);
    void HandleClear(Job job);
    void RefreshAndPruneReports();
    void PublishStatusLocked();
    bool Enqueue(Job job, bool terminal_priority);
    static std::string NewOpaqueId();

    const std::filesystem::path root_;
    const std::size_t memory_buckets_;
    const std::size_t queue_capacity_;
    const std::uint64_t maximum_bytes_;
    const std::chrono::hours retention_;

    std::mutex close_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::deque<SafeTelemetryRecordPtr> current_records_;
    std::string current_session_id_;
    std::uint64_t current_generation_ = 0;
    bool current_persisted_ = false;
    bool stopping_ = false;
    TelemetryStoreStatus status_;
    mutable std::shared_ptr<const TelemetryStoreStatus> status_cache_;
    std::thread worker_;
};

void InstallTelemetryHistoryStore(std::shared_ptr<TelemetryHistoryStore> store);
std::shared_ptr<TelemetryHistoryStore> InstalledTelemetryHistoryStore();

} // namespace livekit::telemetry
