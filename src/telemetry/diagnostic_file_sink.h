#pragma once

#include "diagnostic_event.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace livekit::diagnostic {

struct DiagnosticClearResult {
    bool success = false;
    std::uint64_t removed_segments = 0;
    std::uint64_t active_runs_skipped = 0;
    std::string reason = "clear_failed";
};

class DiagnosticFileSink final {
public:
    static constexpr std::uint64_t kSegmentBytes = 10 * 1024 * 1024;
    static constexpr std::uint64_t kTotalBytes = 100 * 1024 * 1024;
    static DiagnosticClearResult ClearInactiveHistory(
        const std::filesystem::path& root) noexcept;

    DiagnosticFileSink(std::filesystem::path root, std::string run_id);
    ~DiagnosticFileSink();

    DiagnosticFileSink(const DiagnosticFileSink&) = delete;
    DiagnosticFileSink& operator=(const DiagnosticFileSink&) = delete;

    bool Write(const Event& event) noexcept;
    bool Flush() noexcept;
    void Close() noexcept;
    FailureReason failure_reason() const noexcept { return failure_reason_; }
    std::uint64_t last_committed_sequence() const noexcept { return last_committed_sequence_; }
    std::filesystem::path run_directory() const { return run_directory_; }

private:
    bool OpenSegment() noexcept;
    bool CheckQuota(std::uint64_t incoming_bytes) noexcept;
    bool LockQuota() noexcept;
    void UnlockQuota() noexcept;
    std::string Serialize(const Event& event) const;

    std::filesystem::path root_;
    std::filesystem::path run_directory_;
    std::ofstream output_;
    void* run_lease_ = nullptr;
    void* quota_mutex_ = nullptr;
    bool valid_run_id_ = false;
    std::uint32_t segment_index_ = 0;
    std::uint64_t segment_bytes_ = 0;
    std::uint64_t last_written_sequence_ = 0;
    std::uint64_t last_committed_sequence_ = 0;
    FailureReason failure_reason_ = FailureReason::Unknown;
    std::chrono::steady_clock::time_point last_flush_{};
};

} // namespace livekit::diagnostic
