#pragma once
#include "media_diagnostic_config.h"

#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS

// Opt-in diagnostic instrumentation; this is never acceptance evidence.
// Collection is opt-in from the owned runtime fixture. No file/Qt/network work
// occurs in a media callback or session strand.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace livekit::telemetry::callback_diagnostic {

using Clock = std::chrono::steady_clock;
inline std::int64_t Ns(Clock::time_point value = Clock::now()) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count();
}

enum class FramePoint : std::size_t {
    Begin, MetadataBegin, MetadataEnd, CopyBegin, ToI420Begin, ToI420End,
    AllocateBegin, StorageBegin, StorageEnd, AllocateEnd, PlanesBegin,
    YEnd, UEnd, VEnd, CopyEnd, DispatchBegin, DispatchEnd, End, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(FramePoint::Count)>
    kFramePoints = {"begin", "metadata_begin", "metadata_end", "copy_begin",
        "to_i420_begin", "to_i420_end", "allocate_begin", "storage_begin",
        "storage_end", "allocate_end", "planes_begin", "y_end", "u_end", "v_end",
        "copy_end", "dispatch_begin", "dispatch_end", "end"};
enum class SnapshotPoint : std::size_t {
    Begin, UpdatesEnd, BuildBegin, BuildEnd, SharedSnapshotEnd,
    CallbackBegin, CallbackEnd, End, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(SnapshotPoint::Count)>
    kSnapshotPoints = {"begin", "updates_end", "build_begin", "build_end",
        "shared_snapshot_end", "callback_begin", "callback_end", "end"};

// Each sample has its own wall bracket. The original phase timestamp may have
// been captured before diagnostic bookkeeping; never equate the two instants.
struct CycleSample {
    std::int64_t before_ns = 0, after_ns = 0;
    std::uint64_t cycles = 0;
    std::uint32_t tid = 0, error = 0;
    std::uint16_t group_before = 0, group_after = 0;
    std::uint8_t processor_before = 0, processor_after = 0;
    bool attempted = false, succeeded = false;
};
inline CycleSample SampleThreadCycles() noexcept {
    CycleSample value;
    value.before_ns = Ns();
#if defined(_WIN32)
    value.attempted = true;
    value.tid = ::GetCurrentThreadId();
    PROCESSOR_NUMBER before{}, after{};
    ::GetCurrentProcessorNumberEx(&before);
    ULONG64 cycles = 0;
    const BOOL ok = ::QueryThreadCycleTime(::GetCurrentThread(), &cycles);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    ::GetCurrentProcessorNumberEx(&after);
    value.cycles = static_cast<std::uint64_t>(cycles);
    value.error = static_cast<std::uint32_t>(error);
    value.succeeded = ok != FALSE;
    value.group_before = before.Group; value.processor_before = before.Number;
    value.group_after = after.Group; value.processor_after = after.Number;
#else
    value.error = 50; // Unsupported platform, not an observed zero-cycle sample.
#endif
    value.after_ns = Ns();
    return value;
}
inline constexpr std::array<FramePoint, 5> kCyclePoints = {
    FramePoint::Begin, FramePoint::PlanesBegin, FramePoint::YEnd,
    FramePoint::CopyEnd, FramePoint::DispatchEnd};
inline constexpr std::array<const char*, 5> kCyclePointNames = {
    "begin", "planes_begin", "y_end", "copy_end", "dispatch_end"};
struct CyclePair { CycleSample first{}, second{}; };
struct CycleCalibration {
    static constexpr std::size_t kClockPairs = 256, kProbePairs = 256, kBusyPairs = 16;
    std::array<std::int64_t, kClockPairs> clock_pair_ns{};
    std::array<CyclePair, kProbePairs> probe_pairs{};
    std::array<CyclePair, kBusyPairs> busy_pairs{};
    std::uint64_t checksum = 0;
};

// A separate opt-in mode owns one dispatch profile per actual media worker.
// The two inline reads bracket Y-copy work; their own cost remains included.
// No cycle-to-time conversion or pure-memcpy attribution is supported.
struct DispatchSample {
    std::int64_t before_ns = 0, after_ns = 0;
    std::uint64_t context_switch_count = 0, wait_reason_bitmap = 0, cycle_time = 0;
    std::uint32_t error = 0, size = 0, retry_count = 0, reserved = 0, caller_tid = 0;
    std::uint8_t version = 0, hardware_counter_count = 0;
    bool attempted = false;
};
enum class ProfilePhase { Discovering, Arming, Armed, Measured, Closed };
enum class WorkerProfileState { Empty, Registering, Registered, Enabling, Enabled, Warmed, Failed };
struct WorkerProfile {
    std::atomic<WorkerProfileState> state{WorkerProfileState::Empty};
    std::atomic<bool> reading{false};
    std::atomic<std::uint64_t> warmup_ack{0};
    std::atomic<bool> self_cleanup_started{false}, self_cleanup_ack{false};
    std::uint32_t worker_tag = 0, tid = 0;
    std::uint64_t creation_time = 0;
    std::uint64_t callback_visits = 0, enable_callback = 0, warmup_callback = 0;
    std::uint32_t query_before_caller_tid = 0, enable_caller_tid = 0;
    std::uint32_t disable_caller_tid = 0, query_after_caller_tid = 0;
#if defined(_WIN32)
    HANDLE thread = nullptr, profile = nullptr;
#endif
    std::uint32_t registration_error = 0, query_before_error = 0, enable_error = 0;
    std::uint32_t disable_error = 0, query_after_error = 0, close_error = 0;
    bool preexisting = false, enable_succeeded = false, disable_attempted = false;
    bool query_before_attempted = false, enable_attempted = false, query_after_attempted = false;
    bool early_thread_exit = false, cleanup_from_thread_exit = false;
    bool query_after_enabled = false, thread_closed = false;
    DispatchSample warmup_read{}, final_read{};
};
inline DispatchSample ReadDispatchProfile(WorkerProfile& worker) noexcept {
    DispatchSample value;
    value.before_ns = Ns();
#if defined(_WIN32)
    value.attempted = true;
    value.caller_tid = ::GetCurrentThreadId();
    PERFORMANCE_DATA data{};
    data.Size = sizeof(data);
    data.Version = PERFORMANCE_DATA_VERSION;
    value.error = ::ReadThreadProfilingData(worker.profile,
        READ_THREAD_PROFILING_FLAG_DISPATCHING, &data);
    value.size = data.Size; value.version = data.Version;
    value.hardware_counter_count = data.HwCountersCount;
    value.context_switch_count = data.ContextSwitchCount;
    value.wait_reason_bitmap = data.WaitReasonBitMap;
    value.cycle_time = data.CycleTime; value.retry_count = data.RetryCount;
    value.reserved = data.Reserved;
#else
    value.error = 50;
#endif
    value.after_ns = Ns();
    return value;
}
inline bool ValidDispatchRead(const DispatchSample& value) noexcept {
    return value.attempted && !value.error && value.version == 1 &&
        value.hardware_counter_count == 0 && value.reserved == 0;
}
inline CycleCalibration CalibrateThreadCycles() noexcept {
    CycleCalibration value;
    for (auto& duration : value.clock_pair_ns) {
        const auto start = Ns(); duration = Ns() - start;
    }
    for (auto& pair : value.probe_pairs) {
        pair.first = SampleThreadCycles(); pair.second = SampleThreadCycles();
    }
    // Bounded arithmetic validates an executing interval; this is not a CPU
    // frequency calibration, reference workload, or evidence about memcpy.
    std::uint64_t checksum = 1;
    for (auto& pair : value.busy_pairs) {
        pair.first = SampleThreadCycles();
        checksum ^= pair.first.cycles; // Runtime dependency prevents constant evaluation.
        for (std::uint32_t i = 0; i < 4096; ++i) {
            checksum ^= checksum << 13; checksum ^= checksum >> 7;
            checksum ^= checksum << 17;
        }
        pair.second = SampleThreadCycles();
    }
    value.checksum = checksum;
    return value;
}

struct FrameRecord {
    std::array<std::int64_t, static_cast<std::size_t>(FramePoint::Count)> ns{};
    std::uint64_t sequence = 0, room_generation = 0, binding_epoch = 0, frame_token = 0;
    std::uint32_t worker_tag = 0;
    int width = 0, height = 0;
    std::array<CycleSample, kCyclePoints.size()> cycle_samples{};
    bool cycle_enabled = false;
    bool profile_enabled = false, profile_valid = true;
    std::uint32_t profile_worker_index = 0, profile_reads = 0;
    std::array<DispatchSample, 2> profile_samples{};
    WorkerProfile* profile_worker = nullptr; // Window lease owns this storage.
    bool storage_reused = false, completed = false;
};
struct SnapshotRecord {
    std::array<std::int64_t, static_cast<std::size_t>(SnapshotPoint::Count)> ns{};
    std::uint64_t sequence = 0, revision = 0;
    std::uint32_t worker_tag = 0;
    bool publish_path = false, completed = false;
};
template <class Record> struct Slot {
    Record record{};
    std::atomic<bool> ready{false};
};
struct Window {
    static constexpr std::size_t kFrameCapacity = 8192;
    static constexpr std::size_t kSnapshotCapacity = 4096;
    // All storage is allocated and value-initialized before publishing active.
    std::unique_ptr<Slot<FrameRecord>[]> frames =
        std::make_unique<Slot<FrameRecord>[]>(kFrameCapacity);
    std::unique_ptr<Slot<SnapshotRecord>[]> snapshots =
        std::make_unique<Slot<SnapshotRecord>[]>(kSnapshotCapacity);
    std::atomic<std::uint64_t> frame_attempts{0}, snapshot_attempts{0};
    std::atomic<std::uint64_t> writers{0};
    std::atomic<bool> closed{false};
    std::int64_t opened_ns = 0, closed_ns = 0;
    bool cycle_enabled = false; // Set before publishing the preallocated window.
    bool profile_enabled = false; // Mutually exclusive with cycle_enabled.
    static constexpr std::size_t kWorkerCapacity = 32;
    std::array<WorkerProfile, kWorkerCapacity> workers{};
    std::atomic<ProfilePhase> profile_phase{ProfilePhase::Discovering};
    std::atomic<bool> profile_failure{false}, cleanup_started{false}, cleanup_complete{false};
    std::atomic<std::uint64_t> unknown_workers{0}, reentries{0}, worker_overflows{0};
    bool cleanup_valid = true;
    bool profile_gate_owned = false;
    bool Ready() const noexcept { return closed.load() && writers.load() == 0; }
    ~Window();
};

inline std::atomic<std::shared_ptr<Window>> active_window;
inline std::atomic<bool> profile_cleanup_pending{false};
inline bool Stop(const std::shared_ptr<Window>& value) noexcept;
inline std::atomic<std::uint32_t> next_worker_tag{0};
inline std::uint32_t WorkerTag() noexcept {
    thread_local const auto value = next_worker_tag.fetch_add(1) + 1;
    return value;
}
inline WorkerProfile* FindProfileWorker(Window& window, std::uint32_t tag) noexcept {
    for (auto& worker : window.workers) {
        const auto state = worker.state.load(std::memory_order_acquire);
        if (state != WorkerProfileState::Empty && state != WorkerProfileState::Registering &&
                worker.worker_tag == tag) return &worker;
    }
    return nullptr;
}
// Only the enabling worker may Disable. TLS retains the Window independently
// of the recording entry, so later callbacks can drain even after Stop.
struct ThreadProfileOwner {
    std::shared_ptr<Window> window;
    WorkerProfile* worker = nullptr;
    bool Drain(bool exiting = false) noexcept {
        if (!window || !worker) return true;
        if (!exiting && (!window->closed.load() || window->writers.load() != 0)) return false;
        if (exiting && (!window->closed.load() || window->writers.load() != 0)) {
            worker->early_thread_exit = true;
            window->profile_failure.store(true);
            Stop(window);
        }
        bool expected = false;
        if (!worker->reading.compare_exchange_strong(expected, true)) {
            window->profile_failure.store(true);
            return false;
        }
        if (worker->self_cleanup_started.exchange(true)) {
            worker->reading.store(false);
            return worker->self_cleanup_ack.load();
        }
        worker->cleanup_from_thread_exit = exiting;
#if defined(_WIN32)
        const auto caller = ::GetCurrentThreadId();
        if (caller != worker->tid || caller != worker->enable_caller_tid) {
            window->profile_failure.store(true);
            worker->reading.store(false);
            return false; // Never call Disable from a different thread.
        }
        worker->final_read = ReadDispatchProfile(*worker);
        worker->disable_attempted = true;
        worker->disable_caller_tid = caller;
        worker->disable_error = ::DisableThreadProfiling(worker->profile);
        worker->profile = nullptr; // One attempt for this owned Enable.
        BOOLEAN enabled = FALSE;
        worker->query_after_attempted = true;
        worker->query_after_caller_tid = caller;
        worker->query_after_error = ::QueryThreadProfiling(::GetCurrentThread(), &enabled);
        worker->query_after_enabled = enabled != FALSE;
#endif
        worker->reading.store(false, std::memory_order_release);
        worker->self_cleanup_ack.store(true, std::memory_order_release);
        worker = nullptr;
        window.reset();
        return true;
    }
    ~ThreadProfileOwner() { Drain(true); }
};
inline thread_local ThreadProfileOwner thread_profile_owner;
inline thread_local bool profile_callback_active = false;

inline void ProfileWarmup(const std::shared_ptr<Window>& owner) noexcept {
    auto& window = *owner;
    const auto tag = WorkerTag();
    auto* worker = FindProfileWorker(window, tag);
    if (!worker) {
        for (auto& candidate : window.workers) {
            auto expected = WorkerProfileState::Empty;
            if (!candidate.state.compare_exchange_strong(expected, WorkerProfileState::Registering)) continue;
            worker = &candidate;
            worker->worker_tag = tag;
            worker->callback_visits = 1;
#if defined(_WIN32)
            worker->tid = ::GetCurrentThreadId();
            FILETIME birth{}, exit{}, kernel{}, user{};
            if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                    ::GetCurrentProcess(), &worker->thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
                worker->registration_error = ::GetLastError();
            else if (!::GetThreadTimes(worker->thread, &birth, &exit, &kernel, &user))
                worker->registration_error = ::GetLastError();
            worker->creation_time = (static_cast<std::uint64_t>(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
            if (!worker->registration_error) {
                BOOLEAN enabled = FALSE;
                worker->query_before_attempted = true;
                worker->query_before_caller_tid = ::GetCurrentThreadId();
                worker->query_before_error = ::QueryThreadProfiling(::GetCurrentThread(), &enabled);
                worker->preexisting = enabled != FALSE;
                if (!worker->query_before_error && !worker->preexisting && !thread_profile_owner.worker) {
                    worker->enable_attempted = true;
                    worker->enable_caller_tid = ::GetCurrentThreadId();
                    worker->enable_callback = worker->callback_visits;
                    worker->enable_error = ::EnableThreadProfiling(::GetCurrentThread(),
                        THREAD_PROFILING_FLAG_DISPATCH, 0, &worker->profile);
                    worker->enable_succeeded = !worker->enable_error && worker->profile;
                    if (worker->enable_succeeded) {
                        thread_profile_owner.window = owner;
                        thread_profile_owner.worker = worker;
                    }
                }
            }
#else
            worker->registration_error = 50;
#endif
            worker->state.store(worker->enable_succeeded ? WorkerProfileState::Enabled :
                WorkerProfileState::Failed, std::memory_order_release);
            if (!worker->enable_succeeded) window.profile_failure.store(true);
            return;
        }
        window.worker_overflows.fetch_add(1);
        window.profile_failure.store(true);
        return;
    }
    ++worker->callback_visits;
    if (worker->state.load(std::memory_order_acquire) != WorkerProfileState::Enabled) return;
    bool expected = false;
    if (!worker->reading.compare_exchange_strong(expected, true)) {
        window.reentries.fetch_add(1); window.profile_failure.store(true); return;
    }
    worker->warmup_read = ReadDispatchProfile(*worker);
    if (ValidDispatchRead(worker->warmup_read)) {
        worker->warmup_ack.fetch_add(1);
        worker->warmup_callback = worker->callback_visits;
        worker->state.store(WorkerProfileState::Warmed, std::memory_order_release);
    } else {
        worker->state.store(WorkerProfileState::Failed, std::memory_order_release);
        window.profile_failure.store(true);
    }
    worker->reading.store(false, std::memory_order_release);
}
// Control thread only observes registration and arms already self-warmed workers.
inline void PollProfilePreparation(Window& window) noexcept {
    if (!window.profile_enabled || window.closed.load()) return;
    auto phase = window.profile_phase.load();
    if (phase == ProfilePhase::Arming) {
        if (window.writers.load() != 0) return;
        bool all_warmed = true, any = false;
        for (const auto& worker : window.workers) {
            const auto state = worker.state.load(std::memory_order_acquire);
            if (state == WorkerProfileState::Empty) continue;
            any = true;
            if (state != WorkerProfileState::Warmed) all_warmed = false;
        }
        window.profile_phase.store(any && all_warmed && !window.profile_failure.load()
            ? ProfilePhase::Armed : ProfilePhase::Discovering);
        return;
    }
    if (phase != ProfilePhase::Discovering) return;
    bool any = false, all_warmed = true;
    for (const auto& worker : window.workers) {
        const auto state = worker.state.load(std::memory_order_acquire);
        if (state == WorkerProfileState::Empty) continue;
        any = true;
        if (state != WorkerProfileState::Warmed) all_warmed = false;
    }
    if (any && all_warmed && !window.profile_failure.load()) {
        auto expected = ProfilePhase::Discovering;
        window.profile_phase.compare_exchange_strong(expected, ProfilePhase::Arming);
    }
}
inline bool CleanupProfiles(Window& window) noexcept {
    if (!window.profile_enabled) return true;
    if (!window.Ready()) return false;
    for (const auto& worker : window.workers) {
        if (worker.enable_succeeded && !worker.self_cleanup_ack.load(std::memory_order_acquire)) return false;
    }
    if (window.cleanup_started.exchange(true)) return window.cleanup_complete.load();
    for (auto& worker : window.workers) {
#if defined(_WIN32)
        if (worker.enable_succeeded) {
            if (!ValidDispatchRead(worker.final_read) || worker.disable_error ||
                    !worker.disable_attempted || !worker.query_after_attempted ||
                    worker.query_after_error || worker.query_after_enabled ||
                    worker.final_read.caller_tid != worker.tid || worker.disable_caller_tid != worker.tid ||
                    worker.query_after_caller_tid != worker.tid) window.cleanup_valid = false;
        }
        if (worker.thread) {
            worker.thread_closed = ::CloseHandle(worker.thread) != FALSE;
            worker.close_error = worker.thread_closed ? ERROR_SUCCESS : ::GetLastError();
            worker.thread = nullptr;
            if (!worker.thread_closed) window.cleanup_valid = false;
        }
#endif
    }
    window.cleanup_complete.store(true);
    if (window.profile_gate_owned && window.cleanup_valid) profile_cleanup_pending.store(false);
    return true;
}
inline Window::~Window() {
    // TLS owns every live profile until its enabling thread attempts cleanup.
    // This destructor may only close real thread handles; never cross-Disable.
    closed.store(true);
    CleanupProfiles(*this);
}
inline void PrepareProfileWindow(const std::shared_ptr<Window>& value) {
    if (!value || !value->profile_enabled || value->cycle_enabled || value->closed.load())
        throw std::logic_error("callback_profile_fresh_exclusive_window_required");
    bool pending = false;
    if (!profile_cleanup_pending.compare_exchange_strong(pending, true))
        throw std::logic_error("callback_profile_previous_cleanup_pending");
    std::shared_ptr<Window> expected;
    if (!active_window.compare_exchange_strong(expected, value)) {
        profile_cleanup_pending.store(false);
        throw std::logic_error("callback_diagnostic_window_already_active");
    }
    value->profile_gate_owned = true;
}
inline void Begin(const std::shared_ptr<Window>& value) {
    if (!value || value->closed.load() || value->frame_attempts.load() ||
            value->snapshot_attempts.load())
        throw std::logic_error("callback_diagnostic_fresh_preallocated_window_required");
    if (value->profile_enabled) {
        if (active_window.load() != value || value->profile_failure.load())
            throw std::logic_error("callback_profile_worker_preparation_failed");
        auto phase = ProfilePhase::Armed;
        value->opened_ns = Ns();
        if (!value->profile_phase.compare_exchange_strong(phase, ProfilePhase::Measured))
            throw std::logic_error("callback_profile_workers_not_warmed");
        return;
    }
    if (profile_cleanup_pending.load())
        throw std::logic_error("callback_profile_previous_cleanup_pending");
    std::shared_ptr<Window> expected;
    value->opened_ns = Ns();
    if (!active_window.compare_exchange_strong(expected, value))
        throw std::logic_error("callback_diagnostic_window_already_active");
}
inline bool Stop(const std::shared_ptr<Window>& value) noexcept {
    if (!value) return false;
    auto expected = value;
    if (!active_window.compare_exchange_strong(expected, std::shared_ptr<Window>{}))
        return false;
    value->closed_ns = Ns();
    value->profile_phase.store(ProfilePhase::Closed);
    // Sequential consistency is deliberate: a writer which observes !closed
    // has incremented writers before this close and the subsequent Ready load.
    // A late holder of the shared_ptr only changes atomics, never record memory.
    value->closed.store(true);
    return true;
}

class WriterLease {
public:
    explicit WriterLease(bool acquire = true) noexcept { if (acquire) Acquire(); }
    void Acquire() noexcept {
        owner_ = active_window.load();
        if (!owner_) return;
        owner_->writers.fetch_add(1);
        if (owner_->closed.load()) {
            owner_->writers.fetch_sub(1);
            owner_.reset();
        }
    }
    ~WriterLease() { if (owner_) owner_->writers.fetch_sub(1); }
    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
    Window* get() const noexcept { return owner_.get(); }
    const std::shared_ptr<Window>& share() const noexcept { return owner_; }
private:
    std::shared_ptr<Window> owner_;
};

inline thread_local FrameRecord* current_frame = nullptr;
inline void MarkProfile(FrameRecord& frame, FramePoint point) noexcept {
    if (!frame.profile_enabled || (point != FramePoint::PlanesBegin && point != FramePoint::YEnd)) return;
    const std::uint32_t index = point == FramePoint::PlanesBegin ? 0 : 1;
    if (!frame.profile_worker || frame.profile_reads != index) { frame.profile_valid = false; return; }
    auto& sample = frame.profile_samples[index];
    sample = ReadDispatchProfile(*frame.profile_worker);
    ++frame.profile_reads;
    if (!ValidDispatchRead(sample)) frame.profile_valid = false;
}
inline void MarkCycle(FrameRecord& frame, FramePoint point) noexcept {
    if (!frame.cycle_enabled) return;
    for (std::size_t i = 0; i < kCyclePoints.size(); ++i)
        if (point == kCyclePoints[i]) {
            frame.cycle_samples[i] = SampleThreadCycles();
            return;
        }
}
inline void Mark(FramePoint point, Clock::time_point at) noexcept {
    if (current_frame) {
        current_frame->ns[static_cast<std::size_t>(point)] = Ns(at);
        MarkCycle(*current_frame, point);
        MarkProfile(*current_frame, point);
    }
}
inline void Mark(FramePoint point) noexcept {
    if (current_frame) Mark(point, Clock::now());
}
inline FrameRecord* ActiveCopyRecord() noexcept {
    // A synchronous dispatch consumer may make another OwnedI420Frame. Only
    // the primary copy before CopyEnd belongs to this record's copy interval.
    return current_frame &&
        current_frame->ns[static_cast<std::size_t>(FramePoint::CopyBegin)] != 0 &&
        current_frame->ns[static_cast<std::size_t>(FramePoint::CopyEnd)] == 0
        ? current_frame : nullptr;
}
inline void MarkCopy(FramePoint point) noexcept {
    if (auto* frame = ActiveCopyRecord()) {
        frame->ns[static_cast<std::size_t>(point)] = Ns();
        MarkCycle(*frame, point);
        MarkProfile(*frame, point);
    }
}
inline void StorageReused() noexcept {
    if (auto* frame = ActiveCopyRecord()) frame->storage_reused = true;
}
class FrameScope {
public:
    explicit FrameScope(Clock::time_point begin) noexcept : previous_(current_frame) {
        current_frame = nullptr;
        // Pending TLS cleanup is independent of the withdrawn record entry.
        // Do it before acquiring a new recording lease (and never recursively).
        if (!profile_callback_active) thread_profile_owner.Drain();
        lease_.Acquire();
        auto* owner = lease_.get();
        if (!owner) return;
        if (owner->profile_enabled) {
            if (profile_callback_active) {
                owner->reentries.fetch_add(1); owner->profile_failure.store(true); return;
            }
            profile_callback_active = true;
            profile_callback_entered_ = true;
            auto phase = owner->profile_phase.load();
            if (phase == ProfilePhase::Armed && !FindProfileWorker(*owner, WorkerTag())) {
                auto expected = ProfilePhase::Armed;
                owner->profile_phase.compare_exchange_strong(expected, ProfilePhase::Discovering);
                phase = owner->profile_phase.load();
            }
            if (phase != ProfilePhase::Measured) {
                if (phase == ProfilePhase::Discovering) ProfileWarmup(lease_.share());
                return;
            }
        }
        const auto index = owner->frame_attempts.fetch_add(1);
        if (index >= Window::kFrameCapacity) return;
        slot_ = &owner->frames[static_cast<std::size_t>(index)];
        auto& value = slot_->record;
        value.sequence = index + 1;
        value.worker_tag = WorkerTag();
        value.cycle_enabled = owner->cycle_enabled;
        value.profile_enabled = owner->profile_enabled;
        if (value.profile_enabled) {
            auto* worker = FindProfileWorker(*owner, value.worker_tag);
            if (!worker || worker->state.load(std::memory_order_acquire) != WorkerProfileState::Warmed) {
                owner->unknown_workers.fetch_add(1); owner->profile_failure.store(true);
                value.profile_valid = false;
            } else {
                bool expected = false;
                if (!worker->reading.compare_exchange_strong(expected, true)) {
                    owner->reentries.fetch_add(1); owner->profile_failure.store(true);
                    value.profile_valid = false;
                } else {
                    value.profile_worker = worker;
                    value.profile_worker_index = static_cast<std::uint32_t>(worker - owner->workers.data());
                }
            }
        }
        current_frame = &value;
        Mark(FramePoint::Begin, begin);
    }
    ~FrameScope() {
        current_frame = previous_;
        if (slot_) slot_->ready.store(true, std::memory_order_release);
        if (slot_ && slot_->record.profile_worker)
            slot_->record.profile_worker->reading.store(false, std::memory_order_release);
        if (profile_callback_entered_) profile_callback_active = false;
    }
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
    explicit operator bool() const noexcept { return slot_ != nullptr; }
    void Identity(std::uint64_t room, std::uint64_t binding, std::uint64_t frame,
                  int width, int height) noexcept {
        if (!slot_) return;
        auto& value = slot_->record;
        value.room_generation = room; value.binding_epoch = binding;
        value.frame_token = frame; value.width = width; value.height = height;
    }
    void Complete(Clock::time_point end) noexcept {
        if (!slot_) return;
        Mark(FramePoint::End, end);
        slot_->record.completed = true;
    }
private:
    WriterLease lease_{false};
    FrameRecord* previous_ = nullptr;
    Slot<FrameRecord>* slot_ = nullptr;
    bool profile_callback_entered_ = false;
};

class StorageScope {
public:
    StorageScope() noexcept : frame_(ActiveCopyRecord()) {
        if (frame_) frame_->ns[static_cast<std::size_t>(FramePoint::StorageBegin)] = Ns();
    }
    ~StorageScope() {
        if (frame_) frame_->ns[static_cast<std::size_t>(FramePoint::StorageEnd)] = Ns();
    }
private:
    FrameRecord* frame_;
};
class SnapshotScope {
public:
    explicit SnapshotScope(bool publish_path) noexcept {
        auto* owner = lease_.get();
        if (!owner) return;
        if (owner->profile_enabled && owner->profile_phase.load() != ProfilePhase::Measured) return;
        const auto index = owner->snapshot_attempts.fetch_add(1);
        if (index >= Window::kSnapshotCapacity) return;
        slot_ = &owner->snapshots[static_cast<std::size_t>(index)];
        auto& value = slot_->record;
        value.sequence = index + 1; value.worker_tag = WorkerTag();
        value.publish_path = publish_path;
        Point(SnapshotPoint::Begin);
    }
    ~SnapshotScope() {
        if (slot_) slot_->ready.store(true, std::memory_order_release);
    }
    SnapshotScope(const SnapshotScope&) = delete;
    SnapshotScope& operator=(const SnapshotScope&) = delete;
    void Point(SnapshotPoint point) noexcept {
        if (slot_) slot_->record.ns[static_cast<std::size_t>(point)] = Ns();
    }
    void Complete(std::uint64_t revision) noexcept {
        if (!slot_) return;
        slot_->record.revision = revision;
        Point(SnapshotPoint::End);
        slot_->record.completed = true;
    }
private:
    WriterLease lease_;
    Slot<SnapshotRecord>* slot_ = nullptr;
};

} // namespace livekit::telemetry::callback_diagnostic

#else
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
// Default product hooks are stateless: no TLS, atomic shared_ptr, or clock reads.
namespace livekit::telemetry::callback_diagnostic {
using Clock = std::chrono::steady_clock;
enum class FramePoint : std::size_t {
    Begin, MetadataBegin, MetadataEnd, CopyBegin, ToI420Begin, ToI420End,
    AllocateBegin, StorageBegin, StorageEnd, AllocateEnd, PlanesBegin,
    YEnd, UEnd, VEnd, CopyEnd, DispatchBegin, DispatchEnd, End, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(FramePoint::Count)>
    kFramePoints = {"begin", "metadata_begin", "metadata_end", "copy_begin",
        "to_i420_begin", "to_i420_end", "allocate_begin", "storage_begin",
        "storage_end", "allocate_end", "planes_begin", "y_end", "u_end", "v_end",
        "copy_end", "dispatch_begin", "dispatch_end", "end"};
enum class SnapshotPoint : std::size_t {
    Begin, UpdatesEnd, BuildBegin, BuildEnd, SharedSnapshotEnd,
    CallbackBegin, CallbackEnd, End, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(SnapshotPoint::Count)>
    kSnapshotPoints = {"begin", "updates_end", "build_begin", "build_end",
        "shared_snapshot_end", "callback_begin", "callback_end", "end"};

inline constexpr void Mark(FramePoint) noexcept {}
inline constexpr void Mark(FramePoint, Clock::time_point) noexcept {}
inline constexpr void MarkCopy(FramePoint) noexcept {}
inline constexpr void StorageReused() noexcept {}
class FrameScope {
public:
    explicit constexpr FrameScope(Clock::time_point) noexcept {}
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
    explicit constexpr operator bool() const noexcept { return false; }
    constexpr void Identity(std::uint64_t, std::uint64_t, std::uint64_t, int, int) noexcept {}
    constexpr void Complete(Clock::time_point) noexcept {}
};
class StorageScope {};
class SnapshotScope {
public:
    explicit constexpr SnapshotScope(bool) noexcept {}
    SnapshotScope(const SnapshotScope&) = delete;
    SnapshotScope& operator=(const SnapshotScope&) = delete;
    constexpr void Point(SnapshotPoint) noexcept {}
    constexpr void Complete(std::uint64_t) noexcept {}
};
} // namespace livekit::telemetry::callback_diagnostic
#endif
