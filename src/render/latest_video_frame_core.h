#pragma once

#include "render/owned_i420_frame.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace livekit::render {

// Only proven, independently retained inputs may implement this interface.
// Materialize runs outside all core locks, on the caller's conversion worker.
class FrameInput {
public:
    using Ptr = std::shared_ptr<const FrameInput>;
    virtual ~FrameInput() = default;
    virtual OwnedI420Frame::Ptr Materialize() const = 0;
    virtual size_t RetainedBytes() const noexcept = 0;
};

// Deterministic scheduling and ownership core. It starts no threads. All public
// operations are thread safe; a WorkLease has one owner and is not concurrently
// callable. A binding is a native media binding, never a reusable track SID.
class LatestVideoFrameCore final {
    struct State;
    struct Job;
public:
    using Clock = std::chrono::steady_clock;
    struct Config {
        size_t max_bindings = 128;
        size_t max_consumers = 256;
        // Includes pending, the single global in-flight input, and inputs whose
        // final core reference is being released outside the mutex.
        size_t max_retained_inputs = 257;
        size_t max_retained_bytes = 512 * 1024 * 1024;
    };
    struct BindingToken {
        uint64_t id = 0;
        explicit operator bool() const noexcept { return id != 0; }
        bool operator==(const BindingToken&) const = default;
    };
    struct ConsumerToken {
        uint64_t id = 0;
        explicit operator bool() const noexcept { return id != 0; }
        bool operator==(const ConsumerToken&) const = default;
    };
    struct ReadyFrame {
        uint64_t sequence = 0;
        OwnedI420Frame::Ptr frame;
    };
    enum class OfferResult { Accepted, Closed, InvalidBinding, InvalidInput,
                             NoDemand, StaleSequence, CapacityExceeded };
    struct Statistics {
        uint64_t pipeline_id = 0;
        uint64_t offers_accepted = 0;
        uint64_t pending_replaced = 0;
        uint64_t input_capacity_rejected = 0;
        // FrameInput::Materialize wall time, including null/throwing calls.
        // Excludes skipped jobs, queueing, publication and native release.
        uint64_t worker_calls = 0;
        uint64_t worker_failures = 0;
        uint64_t worker_wall_ns = 0;
        // Inclusive bounds 1us * 2^i; last bucket is unbounded overflow.
        std::array<uint64_t, 25> worker_wall_histogram{};
        size_t bindings = 0;
        size_t consumers = 0;
        size_t retained_inputs = 0;
        size_t retained_bytes = 0;
        size_t peak_retained_inputs = 0;
        size_t peak_retained_bytes = 0;
        size_t pending_inputs = 0;
        size_t in_flight_inputs = 0;
        size_t requested_consumers = 0;
        size_t ready_consumers = 0;
        uint64_t materializations_started = 0;
        uint64_t materializations_completed = 0;
        uint64_t materializations_failed = 0;
        uint64_t jobs_cancelled = 0;
        uint64_t capacity_rejections = 0;
        bool closing = false;
    };

    class WorkLease {
    public:
        WorkLease();
        ~WorkLease();
        WorkLease(WorkLease&&) noexcept;
        WorkLease& operator=(WorkLease&&) noexcept;
        WorkLease(const WorkLease&) = delete;
        WorkLease& operator=(const WorkLease&) = delete;
        explicit operator bool() const noexcept;
        // At most one call reaches FrameInput. Exceptions become a null result.
        // The native owner cannot be obtained from this lease.
        OwnedI420Frame::Ptr Materialize() noexcept;
        // Consumes the lease; a null result cancels delivery. Returns whether a
        // valid binding accepted the result. UI delivery remains pull driven.
        bool Complete(OwnedI420Frame::Ptr frame, Clock::time_point now);
        void Cancel() noexcept;
        uint64_t sequence() const noexcept;
    private:
        friend class LatestVideoFrameCore;
        explicit WorkLease(std::unique_ptr<Job> job);
        std::unique_ptr<Job> job_;
    };

    LatestVideoFrameCore();
    explicit LatestVideoFrameCore(Config config);
    ~LatestVideoFrameCore();
    LatestVideoFrameCore(const LatestVideoFrameCore&) = delete;
    LatestVideoFrameCore& operator=(const LatestVideoFrameCore&) = delete;

    std::optional<BindingToken> AddBinding();
    bool RemoveBinding(BindingToken binding);
    // New consumers start with one request. Zero fps means no rate ceiling,
    // not every-frame delivery: this API explicitly has latest-frame semantics.
    std::optional<ConsumerToken> AddConsumer(
        BindingToken binding, uint32_t max_fps, Clock::time_point now);
    bool RemoveConsumer(ConsumerToken consumer);
    // UI-safe invalidation. The scheduler retires native owners in TryStart;
    // a closing worker retires them in BeginClose. No native release here.
    bool RetireConsumer(ConsumerToken consumer);
    bool UpdateConsumerFps(ConsumerToken consumer, uint32_t max_fps);
    OfferResult Offer(BindingToken binding, const FrameInput::Ptr& input,
                      uint64_t sequence);
    bool Request(ConsumerToken consumer, Clock::time_point now);
    bool CancelRequest(ConsumerToken consumer);
    bool Refresh(ConsumerToken consumer, Clock::time_point now);
    std::optional<ReadyFrame> TakeReadyAndRequest(
        ConsumerToken consumer, Clock::time_point now);

    // Also publishes a shared cached frame to eligible requests. No request,
    // or a request without a new input/cache, produces no work or timer.
    WorkLease TryStart(Clock::time_point now);
    std::optional<Clock::time_point> NextWakeup(Clock::time_point now) const;
    void SetWakeCallback(std::function<void()> callback);
    Statistics statistics() const;

    // Fast admission barrier: no input/frame owners are released on this caller.
    // The conversion worker must subsequently call BeginClose to retire them.
    void CloseAdmission();
    // Closing rejects all new work. Releasing pending/cache/ready owners happens
    // outside the core mutex. Drain additionally waits for work leases and all
    // counted input retirements. Call BeginClose before Drain. Already returned
    // OwnedI420Frames are independent and do not delay native-owner drain.
    void BeginClose();
    void Drain();
    bool DrainFor(std::chrono::milliseconds timeout);

private:
    std::shared_ptr<State> state_;
};

} // namespace livekit::render
