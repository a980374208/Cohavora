#include "diagnostic_pipeline.h"

#include "diagnostic_file_sink.h"

#include <algorithm>
#include <random>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace livekit::diagnostic {
namespace {

std::mutex& BusinessPipelineMutex() noexcept {
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::weak_ptr<DiagnosticPipeline>& BusinessPipeline() noexcept {
    static auto* pipeline = new std::weak_ptr<DiagnosticPipeline>;
    return *pipeline;
}

std::array<char, 33> NewRunId() {
    std::array<char, 33> id{};
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 16; ++i) {
        const auto byte = static_cast<unsigned int>(random()) & 0xffU;
        id[i * 2] = digits[byte >> 4];
        id[i * 2 + 1] = digits[byte & 15];
    }
    return id;
}

std::chrono::seconds RetryDelay(std::size_t failures) noexcept {
    if (failures == 0) return std::chrono::seconds(1);
    if (failures == 1) return std::chrono::seconds(5);
    if (failures == 2) return std::chrono::seconds(30);
    return std::chrono::seconds(60);
}

} // namespace

void InstallBusinessPipeline(
    const std::shared_ptr<DiagnosticPipeline>& pipeline) noexcept {
    std::lock_guard lock(BusinessPipelineMutex());
    BusinessPipeline() = pipeline;
}

std::shared_ptr<DiagnosticPipeline> InstalledBusinessPipeline() noexcept {
    std::lock_guard lock(BusinessPipelineMutex());
    return BusinessPipeline().lock();
}

bool EmitBusinessEvent(Event event) noexcept {
    std::shared_ptr<DiagnosticPipeline> pipeline;
    {
        std::lock_guard lock(BusinessPipelineMutex());
        pipeline = BusinessPipeline().lock();
    }
    return pipeline && pipeline->TryEmit(event);
}

OpaqueId NewCorrelationId() noexcept {
    OpaqueId id;
    try {
        const auto bytes = NewRunId();
        id.Assign(std::string_view(bytes.data(), 32));
    } catch (...) {}
    return id;
}

bool DiagnosticWindowActive() noexcept {
    std::shared_ptr<DiagnosticPipeline> pipeline;
    {
        std::lock_guard lock(BusinessPipelineMutex());
        pipeline = BusinessPipeline().lock();
    }
    return pipeline && pipeline->DiagnosticWindowActive();
}

DiagnosticPipeline::DiagnosticPipeline()
    : ordinary_(std::make_unique<PendingEvent[]>(kOrdinaryCapacity)),
      critical_(std::make_unique<PendingEvent[]>(kCriticalCapacity)),
      run_id_(NewRunId()),
      started_at_(std::chrono::steady_clock::now()) {}

DiagnosticPipeline::~DiagnosticPipeline() { Close(); }

Event DiagnosticPipeline::Stamp(Event event) noexcept {
    const auto wall = std::chrono::system_clock::now().time_since_epoch();
    event.occurred_at_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall).count();
    event.monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started_at_).count();
    event.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    event.process_id = GetCurrentProcessId();
#endif
    event.process_run_id = run_id_;
    return event;
}

bool DiagnosticPipeline::TryEmit(Event event) noexcept {
    try {
        if (event.kind != EventKind::RetentionChanged && ProductionPausedForBenchmark()) {
            benchmark_suppressed_.fetch_add(1, std::memory_order_relaxed);
            return false; // deliberate benchmark suppression, before sequence allocation
        }
        if (!IsValidEvent(event)) {
            CountSuppressed();
            return false;
        }
        if (event.window_sample && !DiagnosticWindowActive()) {
            CountSuppressed();
            return false;
        }
        event = Stamp(event);
        std::lock_guard lock(mutex_);
        if (!status_.accepting) return false;
        if (event.window_sample) {
            constexpr std::uint64_t kWindowBudget = 20 * 1024 * 1024;
            const auto used = diagnostic_window_bytes_.load(std::memory_order_relaxed);
            if (used + sizeof(Event) > kWindowBudget) {
                ++status_.dropped_ordinary;
                return false;
            }
            diagnostic_window_bytes_.store(used + sizeof(Event),
                                           std::memory_order_relaxed);
        }
        const bool critical = IsCritical(event.kind);
        const auto ordinary_charge = (ordinary_count_ + 1) * sizeof(PendingEvent);
        const auto critical_charge = (critical_count_ + 1) * sizeof(PendingEvent);
        PendingEvent pending{event, retention_enabled_ ||
            event.kind == EventKind::RetentionChanged};
        if (critical && critical_count_ < kCriticalCapacity &&
            critical_charge <= kCriticalBytes) {
            critical_[critical_tail_] = pending;
            critical_tail_ = (critical_tail_ + 1) % kCriticalCapacity;
            ++critical_count_;
        } else if (ordinary_count_ < kOrdinaryCapacity &&
                   ordinary_charge <= kOrdinaryBytes &&
                   (ordinary_count_ + critical_count_ + 1) * sizeof(PendingEvent) <= kQueueBytes) {
            ordinary_[ordinary_tail_] = pending;
            ordinary_tail_ = (ordinary_tail_ + 1) % kOrdinaryCapacity;
            ++ordinary_count_;
        } else {
            if (critical) ++status_.dropped_critical;
            else ++status_.dropped_ordinary;
            return false;
        }
        ++status_.accepted;
        status_.pending = ordinary_count_ + critical_count_;
        status_.queue_high_water = (std::max)(status_.queue_high_water,
                                             static_cast<std::uint64_t>(status_.pending));
        wake_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

void DiagnosticPipeline::PauseProductionForBenchmark(std::chrono::milliseconds duration) noexcept {
    duration = (std::clamp)(duration, std::chrono::milliseconds::zero(),
                           std::chrono::milliseconds(60000));
    benchmark_pause_until_.store(duration.count() ?
        (std::chrono::steady_clock::now() + duration).time_since_epoch().count() : 0,
        std::memory_order_release);
}

bool DiagnosticPipeline::ProductionPausedForBenchmark() const noexcept {
    const auto until = benchmark_pause_until_.load(std::memory_order_acquire);
    return until && std::chrono::steady_clock::now().time_since_epoch().count() < until;
}

std::uint64_t DiagnosticPipeline::BenchmarkSuppressed() const noexcept {
    return benchmark_suppressed_.load(std::memory_order_relaxed);
}

void DiagnosticPipeline::OpenDiagnosticWindow(
    std::chrono::milliseconds duration) noexcept {
    const auto maximum = std::chrono::minutes(10);
    if (duration > maximum) duration = maximum;
    if (duration < std::chrono::milliseconds::zero())
        duration = std::chrono::milliseconds::zero();
    const auto deadline = duration == std::chrono::milliseconds::zero()
        ? std::int64_t{0}
        : (std::chrono::steady_clock::now() + duration)
              .time_since_epoch().count();
    diagnostic_deadline_ticks_.store(deadline, std::memory_order_release);
    diagnostic_window_bytes_.store(0, std::memory_order_release);
    Event event;
    event.kind = EventKind::DiagnosticsModeChanged;
    event.diagnostic_window_enabled = deadline != 0;
    if (deadline != 0) {
        const auto expires = std::chrono::system_clock::now() + duration;
        event.diagnostic_expires_at_utc_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                expires.time_since_epoch()).count();
    }
    TryEmit(event);
}

bool DiagnosticPipeline::DiagnosticWindowActive() noexcept {
    auto deadline = diagnostic_deadline_ticks_.load(std::memory_order_acquire);
    if (deadline == 0) return false;
    if (std::chrono::steady_clock::now().time_since_epoch().count() < deadline)
        return true;
    if (diagnostic_deadline_ticks_.compare_exchange_strong(deadline, 0,
            std::memory_order_acq_rel)) {
        Event event;
        event.kind = EventKind::DiagnosticsModeChanged;
        event.diagnostic_window_enabled = false;
        TryEmit(event);
    }
    return false;
}

std::chrono::milliseconds DiagnosticPipeline::DiagnosticWindowRemaining() const noexcept {
    const auto deadline = diagnostic_deadline_ticks_.load(
        std::memory_order_acquire);
    if (deadline == 0) return std::chrono::milliseconds::zero();
    const auto remaining = std::chrono::steady_clock::duration(deadline) -
        std::chrono::steady_clock::now().time_since_epoch();
    return (std::max)(std::chrono::milliseconds::zero(),
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
}

bool DiagnosticPipeline::StartWriter(std::filesystem::path root) {
    std::lock_guard lock(mutex_);
    if (started_ || stopping_ || root.empty()) return false;
    writer_ = std::thread([this, root = std::move(root)]() mutable {
        Run(std::move(root));
    });
    started_ = true;
    wake_.notify_one();
    return true;
}

bool DiagnosticPipeline::Pop(PendingEvent& event) noexcept {
    if (ordinary_count_ == 0 && critical_count_ == 0) return false;
    const bool from_critical = critical_count_ != 0 &&
        (ordinary_count_ == 0 ||
         critical_[critical_head_].event.event_sequence < ordinary_[ordinary_head_].event.event_sequence);
    if (from_critical) {
        event = critical_[critical_head_];
        critical_head_ = (critical_head_ + 1) % kCriticalCapacity;
        --critical_count_;
    } else {
        event = ordinary_[ordinary_head_];
        ordinary_head_ = (ordinary_head_ + 1) % kOrdinaryCapacity;
        --ordinary_count_;
    }
    status_.pending = ordinary_count_ + critical_count_;
    return true;
}

void DiagnosticPipeline::Run(std::filesystem::path root) noexcept {
    try {
    DiagnosticFileSink sink(std::move(root), std::string(run_id()));
    PendingEvent pending;
    bool has_pending = false;
    bool pending_critical = false;
    bool pending_mirrored = false;
    bool was_failed = false;
    FailureReason last_failure = FailureReason::Unknown;
    std::size_t failures = 0;
    std::chrono::steady_clock::time_point failure_since{};
    std::uint64_t reported_dropped = 0;
    while (true) {
        if (abort_.load(std::memory_order_relaxed)) break;
        Mirror mirror;
        {
            std::unique_lock lock(mutex_);
            if (!has_pending && !Pop(pending)) {
                if (stopping_) break;
                wake_.wait_for(lock, std::chrono::seconds(1), [this] {
                    return stopping_ || ordinary_count_ != 0 || critical_count_ != 0;
                });
                if (ordinary_count_ == 0 && critical_count_ == 0) {
                    const bool retention = retention_enabled_;
                    lock.unlock();
                    if (retention && !sink.Flush()) {
                        std::lock_guard status_lock(mutex_);
                        ++status_.sink_failures;
                        status_.sink_available = false;
                    }
                    continue;
                }
                Pop(pending);
            }
            if (!has_pending) {
                pending_mirrored = false;
                pending_critical = IsCritical(pending.event.kind);
            }
            has_pending = true;
            if (!pending_mirrored) mirror = mirror_;
        }
        if (!pending_mirrored) {
            try { if (mirror) mirror(pending.event); } catch (...) {}
            pending_mirrored = true;
        }
        if (!pending.persist) {
            has_pending = false;
            continue;
        }
        if (sink.Write(pending.event)) {
            std::uint64_t accepted = 0;
            std::uint64_t dropped = 0;
            std::uint64_t high_water = 0;
            {
                std::lock_guard lock(mutex_);
                ++status_.written;
                status_.last_committed_sequence = sink.last_committed_sequence();
                status_.sink_available = true;
                accepted = status_.accepted;
                dropped = status_.dropped_ordinary + status_.dropped_critical;
                high_water = status_.queue_high_water;
            }
            if (was_failed &&
                sink.Write(Stamp(Event::FileFailed(
                    last_failure, sink.last_committed_sequence()))) &&
                sink.Write(Stamp(Event::FileRecovered(
                    sink.last_committed_sequence())))) {
                was_failed = false;
                failures = 0;
                failure_since = {};
            }
            if (dropped != reported_dropped) {
                if (sink.Write(Stamp(Event::QueueHealth(accepted, dropped, high_water)))) {
                    reported_dropped = dropped;
                }
            }
            has_pending = false;
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            ++status_.sink_failures;
            status_.sink_available = false;
        }
        was_failed = true;
        last_failure = sink.failure_reason();
        if (failures == 0) failure_since = std::chrono::steady_clock::now();
        bool stopping;
        {
            std::lock_guard lock(mutex_);
            stopping = stopping_;
        }
        std::unique_lock lock(mutex_);
        if (stopping || abort_.load(std::memory_order_relaxed)) break;
        if (std::chrono::steady_clock::now() - failure_since >=
            std::chrono::minutes(5)) {
            wake_.wait(lock, [this] { return stopping_ || retry_requested_; });
            if (stopping_) break;
            retry_requested_ = false;
            failures = 0;
            failure_since = {};
        } else {
            wake_.wait_for(lock, RetryDelay(failures++),
                           [this] { return stopping_ || retry_requested_; });
            if (stopping_) break;
            if (retry_requested_) {
                retry_requested_ = false;
                failures = 0;
                failure_since = {};
            }
        }
    }
    if (has_pending || abort_.load(std::memory_order_relaxed)) {
        std::lock_guard lock(mutex_);
        if (has_pending) {
            if (pending_critical) ++status_.dropped_critical;
            else ++status_.dropped_ordinary;
        }
        status_.dropped_critical += critical_count_;
        status_.dropped_ordinary += ordinary_count_;
        critical_count_ = ordinary_count_ = 0;
        status_.pending = 0;
    }
    auto terminal = Stamp(Event::Terminal(
        (has_pending || abort_.load(std::memory_order_relaxed))
            ? Outcome::Failure : Outcome::Success,
        abort_.load(std::memory_order_relaxed) ? DrainResult::TimedOut :
        has_pending ? DrainResult::Failed : DrainResult::Completed));
    bool retention;
    {
        std::lock_guard lock(mutex_);
        retention = retention_enabled_;
    }
    if (retention && !sink.Write(terminal)) {
        std::lock_guard lock(mutex_);
        ++status_.sink_failures;
        status_.sink_available = false;
        drain_result_ = DrainResult::Failed;
    } else {
        std::lock_guard lock(mutex_);
        drain_result_ = abort_.load(std::memory_order_relaxed)
            ? DrainResult::TimedOut :
            has_pending ? DrainResult::Failed : DrainResult::Completed;
    }
    sink.Close();
    } catch (...) {
        std::lock_guard lock(mutex_);
        ++status_.sink_failures;
        status_.sink_available = false;
        status_.dropped_critical += critical_count_;
        status_.dropped_ordinary += ordinary_count_;
        critical_count_ = ordinary_count_ = 0;
        status_.pending = 0;
        drain_result_ = DrainResult::Failed;
    }
    {
        std::lock_guard lock(mutex_);
        writer_finished_ = true;
    }
    writer_done_.notify_all();
}

DrainResult DiagnosticPipeline::Close(ShutdownReason reason) noexcept {
    try {
        std::lock_guard close_lock(close_mutex_);
        bool timed_out = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return drain_result_;
        }
        TryEmit(Event::Stopping(reason));
        {
            std::lock_guard lock(mutex_);
            status_.accepting = false;
            stopping_ = true;
            if (!started_) {
                status_.dropped_critical += critical_count_;
                status_.dropped_ordinary += ordinary_count_;
                status_.pending = 0;
                drain_result_ = DrainResult::Failed;
            }
        }
        wake_.notify_all();
        if (writer_.joinable()) {
            std::unique_lock lock(mutex_);
            if (!writer_done_.wait_for(lock, std::chrono::seconds(5),
                                       [this] { return writer_finished_; })) {
                timed_out = true;
                abort_.store(true, std::memory_order_relaxed);
                lock.unlock();
#if defined(_WIN32)
                CancelSynchronousIo(static_cast<HANDLE>(writer_.native_handle()));
#endif
                lock.lock();
                drain_result_ = DrainResult::TimedOut;
                lock.unlock();
                wake_.notify_all();
            }
        }
        if (writer_.joinable()) writer_.join();
        if (timed_out) {
            std::lock_guard lock(mutex_);
            drain_result_ = DrainResult::TimedOut;
        }
        return drain_result_;
    } catch (...) {
        return DrainResult::Failed;
    }
}

void DiagnosticPipeline::SetMirror(Mirror mirror) {
    std::lock_guard lock(mutex_);
    mirror_ = std::move(mirror);
}

void DiagnosticPipeline::SetRetentionEnabled(bool enabled) noexcept {
    bool changed = false;
    bool started = false;
    {
        std::lock_guard lock(mutex_);
        changed = retention_enabled_ != enabled;
        retention_enabled_ = enabled;
        status_.retention_enabled = enabled;
        started = started_;
    }
    if (changed && started) {
        Event event;
        event.kind = EventKind::RetentionChanged;
        event.thread_role = ThreadRole::Ui;
        event.retention_enabled = enabled;
        TryEmit(event);
    }
}

void DiagnosticPipeline::CountSuppressed() noexcept {
    std::lock_guard lock(mutex_);
    ++status_.suppressed;
}

void DiagnosticPipeline::RetryNow() noexcept {
    std::lock_guard lock(mutex_);
    retry_requested_ = true;
    wake_.notify_one();
}

Status DiagnosticPipeline::GetStatus() const noexcept {
    std::lock_guard lock(mutex_);
    return status_;
}

} // namespace livekit::diagnostic
