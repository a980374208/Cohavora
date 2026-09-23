#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace livekit::telemetry {

struct ConfirmedCrashEvidence {
    std::string process_run_id;
    std::string source;
};

struct StabilityRecoveryEvidence {
    bool provider_configured = false;
    std::vector<ConfirmedCrashEvidence> confirmed_crashes;
};

enum class StabilitySessionTerminal {
    Completed,
    DegradedCompleted,
    AdmissionFailure,
    Timeout,
    Cancelled,
    Stopped,
};

struct StabilitySummary {
    std::string ledger_availability = "UNKNOWN";
    std::string ledger_reason = "ledger_not_initialized";
    std::string confirmed_crash_availability = "UNSUPPORTED";
    std::string confirmed_crash_reason = "crash_evidence_provider_not_configured";
    std::string unknown_termination_availability = "WARMING_UP";
    std::string unknown_termination_reason = "no_terminal_process_runs";
    std::uint64_t process_runs_started = 0;
    std::uint64_t process_runs_terminal = 0;
    std::uint64_t clean_process_exits = 0;
    std::uint64_t unknown_process_terminations = 0;
    std::uint64_t confirmed_process_crashes = 0;
    std::uint64_t crash_evidence_covered_runs = 0;
    std::uint64_t sessions_started = 0;
    std::uint64_t sessions_terminal = 0;
    std::uint64_t unknown_session_terminations = 0;
    std::uint64_t corrupt_inputs = 0;
    std::uint64_t write_failures = 0;
    std::uint64_t duplicate_terminals = 0;
    double unknown_process_termination_ratio = -1.0;
    double confirmed_process_crash_ratio = -1.0;
};

// A local, privacy-bounded journal. It deliberately does not infer a crash
// from a missing terminal record; only externally supplied evidence may turn
// such a run into CONFIRMED_CRASH.
class StabilityLedger final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t kMaxRecords = 256;
    static constexpr std::size_t kMaxSerializedBytes = 256 * 1024;

    explicit StabilityLedger(
        std::filesystem::path path,
        std::size_t max_records = kMaxRecords,
        std::size_t max_serialized_bytes = kMaxSerializedBytes);
    ~StabilityLedger();

    bool BeginProcessRun(const StabilityRecoveryEvidence& evidence = {});
    bool FinishProcessRunClean();
    std::string BeginSession();
    bool FinishSession(
        const std::string& session_id,
        StabilitySessionTerminal terminal);

    std::string process_run_id() const;
    StabilitySummary Summary() const;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    struct Impl;

    bool LoadAndRecoverLocked(const StabilityRecoveryEvidence& evidence);
    bool PersistLocked();
    void RebuildSummaryLocked();
    static std::string NewOpaqueId();

    const std::filesystem::path path_;
    const std::size_t max_records_;
    const std::size_t max_serialized_bytes_;
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
    mutable std::shared_ptr<const StabilitySummary> summary_cache_;
};

class ScopedProcessRun final {
public:
    explicit ScopedProcessRun(
        std::shared_ptr<StabilityLedger> ledger,
        StabilityRecoveryEvidence evidence = {});
    ~ScopedProcessRun();

    ScopedProcessRun(const ScopedProcessRun&) = delete;
    ScopedProcessRun& operator=(const ScopedProcessRun&) = delete;

    bool started() const noexcept { return started_; }

private:
    std::shared_ptr<StabilityLedger> ledger_;
    bool started_ = false;
};

void InstallStabilityLedger(std::shared_ptr<StabilityLedger> ledger);
std::shared_ptr<StabilityLedger> InstalledStabilityLedger();

} // namespace livekit::telemetry
