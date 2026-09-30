#include "diagnostic_pipeline.h"

#include "diagnostic_file_sink.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include <optional>

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

struct DiagnosticPipeline::WriterContext final {
    WriterContext();
    bool TryEmit(Event event) noexcept;
    void OpenDiagnosticWindow(std::chrono::milliseconds duration) noexcept;
    bool DiagnosticWindowActive() noexcept;
    std::chrono::milliseconds DiagnosticWindowRemaining() const noexcept;
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
    std::shared_ptr<const Mirror> mirror_;
    template<std::size_t Bytes> struct TimelineRing {
        static constexpr auto capacity = (std::min)(std::size_t{2048}, Bytes / sizeof(Event));
        std::unique_ptr<Event[]> events = std::make_unique<Event[]>(capacity);
        std::size_t head = 0, count = 0;
        std::uint64_t omitted = 0;
        void Push(const Event& event) noexcept {
            if (count == capacity) { head = (head + 1) % capacity; --count; ++omitted; }
            events[(head + count++) % capacity] = event;
        }
        void Copy(TimelineSnapshot& result, std::string_view session, std::uint64_t generation) const {
            result.omitted += omitted;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& event = events[(head + i) % capacity];
                if (event.context.anonymous_session_id.View() == session &&
                    (!generation || !event.context.has_session_generation ||
                     event.context.session_generation == generation)) result.events.push_back(event);
            }
        }
    };
    TimelineRing<1024 * 1024> lifecycle_;
    // Reserve space for still-open intervals, so a busy closed-event tail
    // cannot silently erase an ongoing stall. Total storage stays below 512 KiB.
    static constexpr std::size_t kOpenStallSlots = 128;
    std::array<std::optional<Event>, kOpenStallSlots> open_stalls_;
    std::uint64_t omitted_open_stalls_ = 0;
    TimelineRing<512 * 1024 - sizeof(open_stalls_)> stalls_;
    void StoreStall(const Event& event) noexcept {
        const auto same = [&](const auto& slot) {
            return slot && slot->context.anonymous_session_id.View() == event.context.anonymous_session_id.View() &&
                slot->media_endpoint_id.View() == event.media_endpoint_id.View() &&
                slot->binding_epoch == event.binding_epoch && slot->interval_begin_us == event.interval_begin_us;
        };
        auto match = std::find_if(open_stalls_.begin(), open_stalls_.end(), same);
        if (event.stall_boundary != StallBoundary::Open) {
            if (match != open_stalls_.end()) match->reset();
            stalls_.Push(event);
            return;
        }
        if (match == open_stalls_.end())
            match = std::find_if(open_stalls_.begin(), open_stalls_.end(), [](const auto& e) { return !e; });
        if (match == open_stalls_.end()) {
            match = std::min_element(open_stalls_.begin(), open_stalls_.end(), [](const auto& a, const auto& b) {
                return a->event_sequence < b->event_sequence;
            });
            ++omitted_open_stalls_;
        }
        *match = event;
    }
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
    bool frozen_ = false;
    Status closed_status_;
};

DiagnosticPipeline::WriterContext::WriterContext()
    : ordinary_(std::make_unique<PendingEvent[]>(kOrdinaryCapacity)),
      critical_(std::make_unique<PendingEvent[]>(kCriticalCapacity)),
      run_id_(NewRunId()),
      started_at_(std::chrono::steady_clock::now()) {}



Event DiagnosticPipeline::WriterContext::Stamp(Event event) noexcept {
    const auto wall = std::chrono::system_clock::now().time_since_epoch();
    event.occurred_at_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall).count();
    event.monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started_at_).count();
    if (!event.source_monotonic_us)
        event.source_monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    event.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    event.process_id = GetCurrentProcessId();
#endif
    event.process_run_id = run_id_;
    return event;
}

bool DiagnosticPipeline::WriterContext::TryEmit(Event event) noexcept {
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
        if (IsTimelineEvent(event.kind)) {
            if (event.kind == EventKind::RenderStallInterval) StoreStall(event);
            else lifecycle_.Push(event);
        }
        status_.pending = ordinary_count_ + critical_count_;
        status_.queue_high_water = (std::max)(status_.queue_high_water,
                                             static_cast<std::uint64_t>(status_.pending));
        wake_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

void DiagnosticPipeline::WriterContext::PauseProductionForBenchmark(std::chrono::milliseconds duration) noexcept {
    duration = (std::clamp)(duration, std::chrono::milliseconds::zero(),
                           std::chrono::milliseconds(60000));
    benchmark_pause_until_.store(duration.count() ?
        (std::chrono::steady_clock::now() + duration).time_since_epoch().count() : 0,
        std::memory_order_release);
}

bool DiagnosticPipeline::WriterContext::ProductionPausedForBenchmark() const noexcept {
    const auto until = benchmark_pause_until_.load(std::memory_order_acquire);
    return until && std::chrono::steady_clock::now().time_since_epoch().count() < until;
}

std::uint64_t DiagnosticPipeline::WriterContext::BenchmarkSuppressed() const noexcept {
    return benchmark_suppressed_.load(std::memory_order_relaxed);
}

void DiagnosticPipeline::WriterContext::OpenDiagnosticWindow(
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

bool DiagnosticPipeline::WriterContext::DiagnosticWindowActive() noexcept {
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

std::chrono::milliseconds DiagnosticPipeline::WriterContext::DiagnosticWindowRemaining() const noexcept {
    const auto deadline = diagnostic_deadline_ticks_.load(
        std::memory_order_acquire);
    if (deadline == 0) return std::chrono::milliseconds::zero();
    const auto remaining = std::chrono::steady_clock::duration(deadline) -
        std::chrono::steady_clock::now().time_since_epoch();
    return (std::max)(std::chrono::milliseconds::zero(),
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
}

bool DiagnosticPipeline::WriterContext::Pop(PendingEvent& event) noexcept {
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

void DiagnosticPipeline::WriterContext::Run(std::filesystem::path root) noexcept {
    try {
    DiagnosticFileSink sink(std::move(root), std::string(run_id()), &abort_);
    PendingEvent pending;
    std::vector<PendingEvent> batch;
    std::vector<Event> persisted;
    bool has_pending = false;
    bool pending_mirrored = false;
    bool was_failed = false;
    FailureReason last_failure = FailureReason::Unknown;
    std::size_t failures = 0;
    std::chrono::steady_clock::time_point failure_since{};
    std::uint64_t reported_dropped = 0;
    while (true) {
        if (abort_.load(std::memory_order_relaxed)) break;
        std::shared_ptr<const Mirror> mirror;
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
                batch.clear();
                persisted.clear();
                batch.push_back(pending);
                // Drain only events already queued: sparse traffic must not
                // wait to fill a batch. Backlog still amortizes quota/flush
                // work across at most 64 events without delaying new writes.
                PendingEvent next;
                while (batch.size() < 64 && Pop(next)) batch.push_back(next);
            }
            has_pending = true;
            if (!pending_mirrored) mirror = mirror_;
        }
        if (!pending_mirrored) {
            for (const auto& item : batch) {
                if (abort_.load(std::memory_order_acquire)) break;
                try { if (mirror) (*mirror)(item.event); } catch (...) {}
                if (item.persist) persisted.push_back(item.event);
            }
            pending_mirrored = true;
        }
        if (abort_.load(std::memory_order_acquire)) break;
        if (persisted.empty()) {
            has_pending = false;
            continue;
        }
        const auto committed = sink.WriteBatch(persisted);
        if (abort_.load(std::memory_order_acquire)) break;
        {
            std::lock_guard lock(mutex_);
            status_.written += committed;
            status_.last_committed_sequence = sink.last_committed_sequence();
        }
        persisted.erase(persisted.begin(), persisted.begin() + committed);
        if (persisted.empty()) {
            std::uint64_t accepted = 0;
            std::uint64_t dropped = 0;
            std::uint64_t high_water = 0;
            {
                std::lock_guard lock(mutex_);
                status_.last_committed_sequence = sink.last_committed_sequence();
                status_.sink_available = true;
                accepted = status_.accepted;
                dropped = status_.dropped_ordinary + status_.dropped_critical;
                high_water = status_.queue_high_water;
            }
            if (!abort_.load(std::memory_order_acquire) && was_failed &&
                sink.Write(Stamp(Event::FileFailed(
                    last_failure, sink.last_committed_sequence()))) &&
                !abort_.load(std::memory_order_acquire) &&
                sink.Write(Stamp(Event::FileRecovered(
                    sink.last_committed_sequence())))) {
                was_failed = false;
                failures = 0;
                failure_since = {};
            }
            if (!abort_.load(std::memory_order_acquire) && dropped != reported_dropped) {
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
            for (const auto& event : persisted) {
                if (IsCritical(event.kind)) ++status_.dropped_critical;
                else ++status_.dropped_ordinary;
            }
        }
        status_.dropped_critical += critical_count_;
        status_.dropped_ordinary += ordinary_count_;
        critical_count_ = ordinary_count_ = 0;
        status_.pending = 0;
    }
    if (!abort_.load(std::memory_order_acquire)) {
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
        if (abort_.load(std::memory_order_acquire)) drain_result_ = DrainResult::TimedOut;
        writer_finished_ = true;
    }
    writer_done_.notify_all();
}

void DiagnosticPipeline::WriterContext::SetMirror(Mirror mirror) {
    auto replacement = mirror ? std::make_shared<const Mirror>(std::move(mirror)) : nullptr;
    std::lock_guard lock(mutex_);
    if (!stopping_) mirror_.swap(replacement);
}

void DiagnosticPipeline::WriterContext::SetRetentionEnabled(bool enabled) noexcept {
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

void DiagnosticPipeline::WriterContext::CountSuppressed() noexcept {
    std::lock_guard lock(mutex_);
    ++status_.suppressed;
}

void DiagnosticPipeline::WriterContext::RetryNow() noexcept {
    std::lock_guard lock(mutex_);
    retry_requested_ = true;
    wake_.notify_one();
}

Status DiagnosticPipeline::WriterContext::GetStatus() const noexcept {
    std::lock_guard lock(mutex_);
    return frozen_ ? closed_status_ : status_;
}

DiagnosticPipeline::DiagnosticPipeline() : context_(std::make_shared<WriterContext>()) {}
DiagnosticPipeline::~DiagnosticPipeline() { Close(); }
std::string_view DiagnosticPipeline::run_id() const noexcept { return context_->run_id(); }
bool DiagnosticPipeline::TryEmit(Event event) noexcept { return context_->TryEmit(event); }
void DiagnosticPipeline::OpenDiagnosticWindow(std::chrono::milliseconds d) noexcept { context_->OpenDiagnosticWindow(d); }
bool DiagnosticPipeline::DiagnosticWindowActive() noexcept { return context_->DiagnosticWindowActive(); }
std::chrono::milliseconds DiagnosticPipeline::DiagnosticWindowRemaining() const noexcept { return context_->DiagnosticWindowRemaining(); }
void DiagnosticPipeline::SetMirror(Mirror mirror) { context_->SetMirror(std::move(mirror)); }

TimelineSnapshot DiagnosticPipeline::RecentTimeline(std::string_view session, std::uint64_t generation) const {
    TimelineSnapshot result;
    if (session.size() != 32) return result;
    const auto& c = *context_;
    std::lock_guard lock(c.mutex_);
    c.lifecycle_.Copy(result, session, generation);
    c.stalls_.Copy(result, session, generation);
    result.omitted += c.omitted_open_stalls_;
    for (const auto& event : c.open_stalls_)
        if (event && event->context.anonymous_session_id.View() == session &&
            (!generation || event->context.session_generation == generation)) result.events.push_back(*event);
    result.admission_drops = c.status_.dropped_ordinary + c.status_.dropped_critical;
    std::sort(result.events.begin(), result.events.end(), [](const auto& a, const auto& b) {
        return a.source_monotonic_us != b.source_monotonic_us
            ? a.source_monotonic_us < b.source_monotonic_us : a.event_sequence < b.event_sequence;
    });
    return result;
}
void DiagnosticPipeline::SetRetentionEnabled(bool e) noexcept { context_->SetRetentionEnabled(e); }
void DiagnosticPipeline::PauseProductionForBenchmark(std::chrono::milliseconds d) noexcept { context_->PauseProductionForBenchmark(d); }
bool DiagnosticPipeline::ProductionPausedForBenchmark() const noexcept { return context_->ProductionPausedForBenchmark(); }
std::uint64_t DiagnosticPipeline::BenchmarkSuppressed() const noexcept { return context_->BenchmarkSuppressed(); }
void DiagnosticPipeline::CountSuppressed() noexcept { context_->CountSuppressed(); }
void DiagnosticPipeline::RetryNow() noexcept { context_->RetryNow(); }
Status DiagnosticPipeline::GetStatus() const noexcept { return context_->GetStatus(); }

bool DiagnosticPipeline::StartWriter(std::filesystem::path root) {
    std::lock_guard close_lock(close_mutex_);
    const auto& c = context_;
    std::lock_guard lock(c->mutex_);
    if (c->started_ || c->stopping_ || root.empty()) return false;
    writer_ = std::thread([context = c, root = std::move(root)]() mutable {
        context->Run(std::move(root));
    });
    c->started_ = true;
    c->wake_.notify_one();
    return true;
}

DrainResult DiagnosticPipeline::Close(ShutdownReason reason) noexcept {
    std::lock_guard close_lock(close_mutex_);
    const auto& c = context_;
    std::shared_ptr<const Mirror> retired_mirror;
    {
        std::lock_guard lock(c->mutex_);
        if (c->stopping_) return c->frozen_ ? DrainResult::TimedOut : c->drain_result_;
    }
    c->TryEmit(Event::Stopping(reason));
    std::unique_lock lock(c->mutex_);
    c->status_.accepting = false;
    c->stopping_ = true;
    c->wake_.notify_all();
    if (!c->started_) {
        c->status_.dropped_critical += c->critical_count_;
        c->status_.dropped_ordinary += c->ordinary_count_;
        c->critical_count_ = c->ordinary_count_ = 0;
        c->status_.pending = 0;
        c->drain_result_ = DrainResult::Failed;
    } else if (!c->writer_done_.wait_for(lock, std::chrono::seconds(5),
                                        [&] { return c->writer_finished_; })) {
        // Keep callback captures in the context: their destructors also belong
        // to the late worker, not to this bounded return path.
        c->abort_.store(true, std::memory_order_release);
        c->drain_result_ = DrainResult::TimedOut;
        c->status_.sink_available = false;
        c->closed_status_ = c->status_;
        c->frozen_ = true;
        lock.unlock();
        c->wake_.notify_all();
        // No cancellation/cleanup call here may make the caller wait for I/O.
        // The worker alone owns its sink and stack buffers until it unwinds.
        writer_.detach();
        return DrainResult::TimedOut;
    }
    retired_mirror.swap(c->mirror_);
    const auto result = c->drain_result_;
    lock.unlock();
    if (writer_.joinable()) writer_.join();
    return result;
}

} // namespace livekit::diagnostic
