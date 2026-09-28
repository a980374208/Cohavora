#pragma once

#include "diagnostic_event.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace livekit::diagnostic {

struct Status final {
    std::uint64_t accepted = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped_ordinary = 0;
    std::uint64_t dropped_critical = 0;
    std::uint64_t suppressed = 0;
    std::uint64_t sink_failures = 0;
    std::uint64_t queue_high_water = 0;
    std::uint64_t last_committed_sequence = 0;
    std::size_t pending = 0;
    bool sink_available = false;
    bool accepting = true;
    bool retention_enabled = true;
};

class DiagnosticPipeline final {
public:
    using Mirror = std::function<void(const Event&)>;

    DiagnosticPipeline();
    ~DiagnosticPipeline();
    DiagnosticPipeline(const DiagnosticPipeline&) = delete;
    DiagnosticPipeline& operator=(const DiagnosticPipeline&) = delete;

    bool TryEmit(Event event) noexcept;
    void OpenDiagnosticWindow(std::chrono::milliseconds duration) noexcept;
    bool DiagnosticWindowActive() noexcept;
    std::chrono::milliseconds DiagnosticWindowRemaining() const noexcept;
    bool StartWriter(std::filesystem::path root);
    DrainResult Close(ShutdownReason reason = ShutdownReason::UserExit) noexcept;
    void SetMirror(Mirror mirror);
    void SetRetentionEnabled(bool enabled) noexcept;
    // Explicit, time-bounded external benchmark only. Never persisted as a
    // user preference; zero/expiry restores normal production automatically.
    void PauseProductionForBenchmark(std::chrono::milliseconds duration) noexcept;
    bool ProductionPausedForBenchmark() const noexcept;
    std::uint64_t BenchmarkSuppressed() const noexcept;
    void CountSuppressed() noexcept;
    void RetryNow() noexcept;
    Status GetStatus() const noexcept;
    std::string_view run_id() const noexcept { return {run_id_.data(), 32}; }

private:
    struct PendingEvent final {
        Event event;
        bool persist = true;
    };
    void Run(std::filesystem::path root) noexcept;
    bool Pop(PendingEvent& event) noexcept;
    Event Stamp(Event event) noexcept;

    static constexpr std::size_t kOrdinaryCapacity = kOrdinaryEvents;
    static constexpr std::size_t kCriticalCapacity = kCriticalEvents;
    std::unique_ptr<PendingEvent[]> ordinary_;
    std::unique_ptr<PendingEvent[]> critical_;
    std::size_t ordinary_head_ = 0;
    std::size_t ordinary_tail_ = 0;
    std::size_t ordinary_count_ = 0;
    std::size_t critical_head_ = 0;
    std::size_t critical_tail_ = 0;
    std::size_t critical_count_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable writer_done_;
    std::thread writer_;
    std::mutex close_mutex_;
    Mirror mirror_;
    Status status_;
    bool stopping_ = false;
    bool started_ = false;
    bool writer_finished_ = false;
    bool retry_requested_ = false;
    std::atomic<bool> abort_{false};
    bool retention_enabled_ = true;
    DrainResult drain_result_ = DrainResult::Unknown;
    std::array<char, 33> run_id_{};
    std::chrono::steady_clock::time_point started_at_;
    std::atomic<std::uint64_t> next_sequence_{0};
    std::atomic<std::int64_t> diagnostic_deadline_ticks_{0};
    std::atomic<std::uint64_t> diagnostic_window_bytes_{0};
    std::atomic<std::int64_t> benchmark_pause_until_{0};
    std::atomic<std::uint64_t> benchmark_suppressed_{0};
};

void InstallBusinessPipeline(const std::shared_ptr<DiagnosticPipeline>& pipeline) noexcept;
std::shared_ptr<DiagnosticPipeline> InstalledBusinessPipeline() noexcept;
bool EmitBusinessEvent(Event event) noexcept;
OpaqueId NewCorrelationId() noexcept;
bool DiagnosticWindowActive() noexcept;

} // namespace livekit::diagnostic
