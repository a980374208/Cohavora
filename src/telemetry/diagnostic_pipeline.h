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
#include <vector>

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

struct TimelineSnapshot {
    std::vector<Event> events;
    // Process-wide truncation, not a claim of per-session loss.
    std::uint64_t omitted = 0;
    std::uint64_t admission_drops = 0;
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
    // In-flight callbacks can outlive Close. Capture owned state or weak owners,
    // never borrowed Pipeline/UI pointers. Clearing prevents future acquisition.
    void SetMirror(Mirror mirror);
    TimelineSnapshot RecentTimeline(std::string_view anonymous_session_id,
                                    std::uint64_t generation = 0) const;
    void SetRetentionEnabled(bool enabled) noexcept;
    // Explicit, time-bounded external benchmark only. Never persisted as a
    // user preference; zero/expiry restores normal production automatically.
    void PauseProductionForBenchmark(std::chrono::milliseconds duration) noexcept;
    bool ProductionPausedForBenchmark() const noexcept;
    std::uint64_t BenchmarkSuppressed() const noexcept;
    void CountSuppressed() noexcept;
    void RetryNow() noexcept;
    Status GetStatus() const noexcept;
    std::string_view run_id() const noexcept;

private:
    friend struct DiagnosticPipelineTestAccess;
    struct WriterContext;
    // Stable until destruction: concurrent API calls never race with a reset.
    // Detached workers retain their own reference after Pipeline destruction.
    const std::shared_ptr<WriterContext> context_;
    std::thread writer_;
    std::mutex close_mutex_;
};

void InstallBusinessPipeline(const std::shared_ptr<DiagnosticPipeline>& pipeline) noexcept;
std::shared_ptr<DiagnosticPipeline> InstalledBusinessPipeline() noexcept;
bool EmitBusinessEvent(Event event) noexcept;
OpaqueId NewCorrelationId() noexcept;
bool DiagnosticWindowActive() noexcept;

} // namespace livekit::diagnostic
