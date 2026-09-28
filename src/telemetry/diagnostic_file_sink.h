#pragma once

#include "diagnostic_event.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <span>

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

    // Windows v2 quota identity is directory FileId + volume, scoped to the
    // Windows session. All concurrent writers must use this protocol; legacy
    // path-hash writers must be stopped before upgrading a shared root.
    DiagnosticFileSink(std::filesystem::path root, std::string run_id,
                       const std::atomic<bool>* abandoned = nullptr);
    ~DiagnosticFileSink();

    DiagnosticFileSink(const DiagnosticFileSink&) = delete;
    DiagnosticFileSink& operator=(const DiagnosticFileSink&) = delete;

    bool Write(const Event& event) noexcept;
    // Returns the flushed prefix. The 100MiB quota is a soft cleanup target:
    // check under the cross-process lock before the first write after each
    // segment open. Same-segment appends may exceed it until the next rotation;
    // neither excess usage nor expired history has a wall-clock cleanup bound.
    std::size_t WriteBatch(std::span<const Event> events) noexcept;
    bool Flush() noexcept;
    void Close() noexcept;
    FailureReason failure_reason() const noexcept { return failure_reason_; }
    std::uint64_t last_committed_sequence() const noexcept { return last_committed_sequence_; }
    std::filesystem::path run_directory() const { return run_directory_; }

private:
    friend struct DiagnosticFileSinkTestAccess;
    bool Stopped() const noexcept;
    bool IsOpen() const noexcept;
    void CloseSegment() noexcept;
#if defined(_WIN32)
    static bool NativeSize(void* file, std::uint64_t& bytes) noexcept;
    static bool NativeWrite(void* file, const char* data, std::uint32_t size,
                            std::uint32_t& written) noexcept;
    void* file_ = nullptr;
    // Private I/O seam; only friend tests replace these operations.
    decltype(&NativeSize) read_size_ = &NativeSize;
    decltype(&NativeWrite) write_ = &NativeWrite;
#else
    std::ofstream output_;
#endif
    const std::atomic<bool>* abandoned_ = nullptr; // Outlives this worker-owned sink.
    bool OpenSegment() noexcept;
    bool CheckQuota(std::uint64_t incoming_bytes) noexcept;
    bool BindQuotaIdentity() noexcept;
    bool LockQuota() noexcept;
    void UnlockQuota() noexcept;
    std::string Serialize(const Event& event) const;

    std::filesystem::path root_;
    std::filesystem::path run_directory_;
    void* run_lease_ = nullptr;
    void* quota_mutex_ = nullptr;
    void* root_identity_ = nullptr;
    bool quota_checked_segment_ = false;
    bool valid_run_id_ = false;
    std::uint32_t segment_index_ = 0;
    std::uint64_t segment_bytes_ = 0;
    std::uint64_t last_written_sequence_ = 0;
    std::uint64_t last_committed_sequence_ = 0;
    FailureReason failure_reason_ = FailureReason::Unknown;
};

} // namespace livekit::diagnostic
