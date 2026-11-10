#include "latest_video_frame_core.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace livekit::render {
namespace {

uint64_t AllocateIdentity() noexcept {
    // Process-wide identities make tokens from another core invalid without
    // relying on the address (which can be reused) of either core instance.
    static std::atomic<uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0; // Saturate permanently; never recycle an old token after wrap.
}

LatestVideoFrameCore::Clock::duration Period(uint32_t fps) {
    if (!fps) return LatestVideoFrameCore::Clock::duration::zero();
    return std::chrono::duration_cast<LatestVideoFrameCore::Clock::duration>(
        std::chrono::nanoseconds((1000000000LL + fps - 1) / fps));
}

} // namespace

struct LatestVideoFrameCore::State : std::enable_shared_from_this<State> {
    enum class Phase { Idle, Requested, InFlight, Ready };
    struct InputHolder {
        FrameInput::Ptr input;
        std::weak_ptr<State> state;
        size_t bytes;

        InputHolder(const FrameInput::Ptr& value, std::weak_ptr<State> owner, size_t size)
            : input(value), state(std::move(owner)), bytes(size) {}
        ~InputHolder() {
            // Destructors may block or reenter the core. Capacity is returned
            // only after its actual input reference has been released.
            input.reset();
            if (auto owner = state.lock()) {
                {
                    std::lock_guard lock(owner->mutex);
                    --owner->stats.retained_inputs;
                    owner->stats.retained_bytes -= bytes;
                }
                owner->drained.notify_all();
                owner->Wake();
            }
        }
    };
    struct Consumer {
        bool retired = false;
        uint64_t binding = 0;
        uint64_t epoch = 1;
        uint32_t fps = 0;
        Phase phase = Phase::Requested;
        uint64_t last_taken_sequence = 0;
        Clock::time_point next_due{};
        std::optional<Clock::time_point> last_published;
        ReadyFrame ready;
    };
    struct Binding {
        std::set<uint64_t> consumers;
        uint64_t latest_sequence = 0;
        std::shared_ptr<InputHolder> pending;
        uint64_t pending_sequence = 0;
        ReadyFrame cache;
        std::optional<Clock::time_point> last_started;
    };

    explicit State(Config value) : config(value) { stats.pipeline_id = AllocateIdentity(); }
    void Wake() noexcept {
        std::function<void()> callback;
        try {
            {
                std::lock_guard lock(mutex);
                callback = wake;
            }
            if (callback) callback();
        } catch (...) {
            // A wake hook is notification only, never ownership or completion.
        }
    }
    uint32_t AggregateFps(const Binding& binding) const {
        uint32_t fps = 0;
        for (const auto id : binding.consumers) {
            const auto& consumer = consumers.at(id);
            if (!consumer.fps) return 0;
            fps = std::max(fps, consumer.fps);
        }
        return fps;
    }
    Clock::time_point MaterializeDue(const Binding& binding, Clock::time_point now) const {
        const auto fps = AggregateFps(binding);
        return binding.last_started && fps
            ? std::max(now, *binding.last_started + Period(fps)) : now;
    }
    bool HasEligibleRequest(const Binding& binding, uint64_t sequence,
                            Clock::time_point now) const {
        for (const auto id : binding.consumers) {
            const auto& consumer = consumers.at(id);
            if (consumer.phase == Phase::Requested &&
                consumer.last_taken_sequence < sequence && consumer.next_due <= now) return true;
        }
        return false;
    }
    void Publish(Consumer& consumer, const ReadyFrame& frame, Clock::time_point now) {
        consumer.ready = frame; // Requested/InFlight never contains an owned frame.
        consumer.phase = Phase::Ready;
        consumer.last_published = now;
        consumer.next_due = now + Period(consumer.fps);
    }
    bool Finish(Job& job, OwnedI420Frame::Ptr frame, Clock::time_point now, bool cancelled);

    const Config config;
    mutable std::mutex mutex;
    std::condition_variable drained;
    std::map<uint64_t, Binding> bindings;
    std::map<uint64_t, Consumer> consumers;
    uint64_t active_job = 0;
    uint64_t last_scheduled_binding = 0;
    bool retirement_started = false;
    bool retirement_finished = false;
    Statistics stats;
    std::function<void()> wake;
};

struct LatestVideoFrameCore::Job {
    struct Recipient { uint64_t consumer; uint64_t epoch; };
    std::shared_ptr<State> state;
    std::shared_ptr<State::InputHolder> input;
    uint64_t id = 0;
    uint64_t binding = 0;
    uint64_t sequence = 0;
    std::vector<Recipient> recipients;
    bool materialize_called = false;
    bool materialize_skipped = false;
};

bool LatestVideoFrameCore::State::Finish(
        Job& job, OwnedI420Frame::Ptr frame, Clock::time_point now, bool cancelled) {
    auto retired_input = std::move(job.input);
    OwnedI420Frame::Ptr retired_cache;
    bool accepted = false;
    {
        std::lock_guard lock(mutex);
        const auto binding = bindings.find(job.binding);
        const bool valid = !stats.closing && binding != bindings.end() &&
            !binding->second.consumers.empty();
        if (valid && frame && !cancelled) {
            retired_cache = std::move(binding->second.cache.frame);
            binding->second.cache = {job.sequence, std::move(frame)};
            accepted = true;
            ++stats.materializations_completed;
        } else if (cancelled) {
            ++stats.jobs_cancelled;
        } else if (!frame) {
            ++stats.materializations_failed;
        }
        for (const auto& recipient : job.recipients) {
            const auto found = consumers.find(recipient.consumer);
            if (found == consumers.end()) continue;
            auto& consumer = found->second;
            if (consumer.epoch != recipient.epoch || consumer.phase != Phase::InFlight) continue;
            if (accepted) Publish(consumer, binding->second.cache, now);
            else consumer.phase = Phase::Requested;
        }
        // A refreshed request is a different job. If the old reservation was
        // invalidated before conversion began, preserve the latest input for
        // that new request instead of losing a static screen's final update.
        if (valid && job.materialize_skipped && !binding->second.pending &&
            job.sequence == binding->second.latest_sequence) {
            const bool requested = std::any_of(
                binding->second.consumers.begin(), binding->second.consumers.end(), [&](uint64_t id) {
                    const auto& consumer = consumers.at(id);
                    return consumer.phase == Phase::Requested && consumer.last_taken_sequence < job.sequence;
                });
            if (requested) {
                binding->second.pending = std::move(retired_input);
                binding->second.pending_sequence = job.sequence;
            }
        }
    }
    retired_cache.reset();
    frame.reset();
    retired_input.reset();
    {
        std::lock_guard lock(mutex);
        // Keep the single global work slot occupied through native retirement.
        if (active_job == job.id) {
            active_job = 0;
            stats.in_flight_inputs = 0;
        }
    }
    drained.notify_all();
    Wake();
    return accepted;
}

LatestVideoFrameCore::WorkLease::WorkLease() = default;
LatestVideoFrameCore::WorkLease::WorkLease(std::unique_ptr<Job> job)
    : job_(std::move(job)) {}
LatestVideoFrameCore::WorkLease::~WorkLease() { Cancel(); }
LatestVideoFrameCore::WorkLease::WorkLease(WorkLease&& other) noexcept
    : job_(std::move(other.job_)) {}
LatestVideoFrameCore::WorkLease& LatestVideoFrameCore::WorkLease::operator=(WorkLease&& other) noexcept {
    if (this != &other) {
        Cancel();
        job_ = std::move(other.job_);
    }
    return *this;
}
LatestVideoFrameCore::WorkLease::operator bool() const noexcept { return bool(job_); }
OwnedI420Frame::Ptr LatestVideoFrameCore::WorkLease::Materialize() noexcept {
    if (!job_ || job_->materialize_called) return {};
    job_->materialize_called = true;
    try {
        {
            std::lock_guard lock(job_->state->mutex);
            const auto& state = *job_->state;
            const bool valid_binding = !state.stats.closing && state.bindings.contains(job_->binding);
            const bool valid_request = valid_binding && std::any_of(
                job_->recipients.begin(), job_->recipients.end(), [&](const auto& recipient) {
                    const auto consumer = state.consumers.find(recipient.consumer);
                    return consumer != state.consumers.end() &&
                        consumer->second.epoch == recipient.epoch &&
                        consumer->second.phase == State::Phase::InFlight;
                });
            if (!valid_request) {
                job_->materialize_skipped = true;
                return {};
            }
        }
        const auto begin = Clock::now();
        OwnedI420Frame::Ptr frame;
        try { frame = job_->input->input->Materialize(); } catch (...) {}
        const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - begin).count());
        {
            std::lock_guard lock(job_->state->mutex);
            auto& stats = job_->state->stats;
            ++stats.worker_calls;
            if (!frame) ++stats.worker_failures;
            stats.worker_wall_ns += elapsed;
            size_t bucket = 0;
            while (bucket + 1 < stats.worker_wall_histogram.size() &&
                   elapsed > (uint64_t{1000} << bucket)) ++bucket;
            ++stats.worker_wall_histogram[bucket];
        }
        return frame;
    } catch (...) {
        return {};
    }
}
bool LatestVideoFrameCore::WorkLease::Complete(OwnedI420Frame::Ptr frame, Clock::time_point now) {
    auto job = std::move(job_);
    return job && job->state->Finish(*job, std::move(frame), now, false);
}
void LatestVideoFrameCore::WorkLease::Cancel() noexcept {
    auto job = std::move(job_);
    if (job) job->state->Finish(*job, {}, Clock::time_point{}, true);
}
uint64_t LatestVideoFrameCore::WorkLease::sequence() const noexcept {
    return job_ ? job_->sequence : 0;
}

LatestVideoFrameCore::LatestVideoFrameCore() : LatestVideoFrameCore(Config{}) {}
LatestVideoFrameCore::LatestVideoFrameCore(Config config) : state_(std::make_shared<State>(config)) {}
LatestVideoFrameCore::~LatestVideoFrameCore() { BeginClose(); }

std::optional<LatestVideoFrameCore::BindingToken> LatestVideoFrameCore::AddBinding() {
    std::optional<BindingToken> result;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stats.closing) return {};
        if (state_->bindings.size() >= state_->config.max_bindings) {
            ++state_->stats.capacity_rejections;
            return {};
        }
        const auto id = AllocateIdentity();
        if (!id) { state_->stats.closing = true; return {}; }
        state_->bindings.emplace(id, State::Binding{});
        result = BindingToken{id};
    }
    state_->Wake();
    return result;
}

bool LatestVideoFrameCore::RemoveBinding(BindingToken token) {
    std::optional<State::Binding> retired;
    std::map<uint64_t, State::Consumer> retired_consumers;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->bindings.find(token.id);
        if (found == state_->bindings.end()) return false;
        retired.emplace(std::move(found->second));
        state_->bindings.erase(found);
        for (const auto id : retired->consumers) {
            retired_consumers.insert(state_->consumers.extract(id));
        }
    }
    retired_consumers.clear();
    retired.reset();
    state_->Wake();
    return true;
}

std::optional<LatestVideoFrameCore::ConsumerToken> LatestVideoFrameCore::AddConsumer(
        BindingToken binding, uint32_t max_fps, Clock::time_point now) {
    std::optional<ConsumerToken> result;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->bindings.find(binding.id);
        if (state_->stats.closing || found == state_->bindings.end()) return {};
        if (state_->consumers.size() >= state_->config.max_consumers) {
            ++state_->stats.capacity_rejections;
            return {};
        }
        const auto id = AllocateIdentity();
        if (!id) { state_->stats.closing = true; return {}; }
        State::Consumer consumer;
        consumer.binding = binding.id;
        consumer.fps = max_fps;
        consumer.next_due = now;
        found->second.consumers.insert(id);
        try {
            state_->consumers.emplace(id, std::move(consumer));
        } catch (...) {
            found->second.consumers.erase(id);
            throw;
        }
        result = ConsumerToken{id};
    }
    state_->Wake();
    return result;
}

bool LatestVideoFrameCore::RemoveConsumer(ConsumerToken token) {
    std::optional<State::Consumer> retired;
    std::shared_ptr<State::InputHolder> retired_input;
    OwnedI420Frame::Ptr retired_cache;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (found == state_->consumers.end()) return false;
        auto& binding = state_->bindings.at(found->second.binding);
        binding.consumers.erase(token.id);
        retired.emplace(std::move(found->second));
        state_->consumers.erase(found);
        if (binding.consumers.empty()) {
            retired_input = std::move(binding.pending);
            retired_cache = std::move(binding.cache.frame);
            binding.cache.sequence = 0;
        }
    }
    retired.reset();
    retired_cache.reset();
    retired_input.reset();
    state_->Wake();
    return true;
}

bool LatestVideoFrameCore::UpdateConsumerFps(ConsumerToken token, uint32_t max_fps) {
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (state_->stats.closing || found == state_->consumers.end() || found->second.retired) return false;
        auto& consumer = found->second;
        if (consumer.fps == max_fps) return true;
        consumer.fps = max_fps;
        ++consumer.epoch;
        if (consumer.phase == State::Phase::InFlight) consumer.phase = State::Phase::Requested;
        consumer.next_due = consumer.last_published
            ? *consumer.last_published + Period(max_fps) : Clock::time_point{};
    }
    state_->Wake();
    return true;
}

bool LatestVideoFrameCore::RetireConsumer(ConsumerToken token) {
    OwnedI420Frame::Ptr owned;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (found == state_->consumers.end()) return false;
        auto& consumer = found->second;
        consumer.retired = true;
        ++consumer.epoch;
        consumer.phase = State::Phase::Idle;
        owned = std::move(consumer.ready.frame);
    }
    owned.reset();
    state_->Wake();
    return true;
}

LatestVideoFrameCore::OfferResult LatestVideoFrameCore::Offer(
        BindingToken token, const FrameInput::Ptr& input, uint64_t sequence) {
    if (!input || !sequence) return OfferResult::InvalidInput;
    const auto bytes = input->RetainedBytes();
    if (!bytes) return OfferResult::InvalidInput;
    std::shared_ptr<State::InputHolder> retired;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stats.closing) return OfferResult::Closed;
        const auto found = state_->bindings.find(token.id);
        if (found == state_->bindings.end()) return OfferResult::InvalidBinding;
        auto& binding = found->second;
        if (binding.consumers.empty()) return OfferResult::NoDemand;
        if (sequence <= binding.latest_sequence) return OfferResult::StaleSequence;
        if (state_->stats.retained_inputs >= state_->config.max_retained_inputs ||
            bytes > state_->config.max_retained_bytes ||
            state_->stats.retained_bytes > state_->config.max_retained_bytes - bytes) {
            ++state_->stats.capacity_rejections;
            ++state_->stats.input_capacity_rejected;
            return OfferResult::CapacityExceeded;
        }
        auto retained = std::make_shared<State::InputHolder>(input, state_, bytes);
        ++state_->stats.retained_inputs;
        state_->stats.retained_bytes += bytes;
        state_->stats.peak_retained_inputs = std::max(
            state_->stats.peak_retained_inputs, state_->stats.retained_inputs);
        state_->stats.peak_retained_bytes = std::max(
            state_->stats.peak_retained_bytes, state_->stats.retained_bytes);
        ++state_->stats.offers_accepted;
        if (binding.pending) ++state_->stats.pending_replaced;
        retired = std::exchange(binding.pending, std::move(retained));
        binding.pending_sequence = sequence;
        binding.latest_sequence = sequence;
    }
    retired.reset();
    state_->Wake();
    return OfferResult::Accepted;
}

bool LatestVideoFrameCore::Request(ConsumerToken token, Clock::time_point now) {
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (state_->stats.closing || found == state_->consumers.end() || found->second.retired) return false;
        auto& consumer = found->second;
        if (consumer.phase == State::Phase::Idle) {
            ++consumer.epoch;
            consumer.phase = State::Phase::Requested;
            consumer.next_due = std::max(consumer.next_due, now);
        }
    }
    state_->Wake();
    return true;
}

bool LatestVideoFrameCore::CancelRequest(ConsumerToken token) {
    OwnedI420Frame::Ptr retired;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (state_->stats.closing || found == state_->consumers.end()) return false;
        auto& consumer = found->second;
        ++consumer.epoch;
        consumer.phase = State::Phase::Idle;
        retired = std::move(consumer.ready.frame);
        consumer.ready.sequence = 0;
    }
    retired.reset();
    state_->Wake();
    return true;
}

bool LatestVideoFrameCore::Refresh(ConsumerToken token, Clock::time_point now) {
    OwnedI420Frame::Ptr retired;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (state_->stats.closing || found == state_->consumers.end() || found->second.retired) return false;
        auto& consumer = found->second;
        ++consumer.epoch;
        consumer.phase = State::Phase::Requested;
        consumer.next_due = now;
        consumer.last_taken_sequence = 0; // Explicit refresh may replay the static latest cache.
        retired = std::move(consumer.ready.frame);
        consumer.ready.sequence = 0;
    }
    retired.reset();
    state_->Wake();
    return true;
}

std::optional<LatestVideoFrameCore::ReadyFrame> LatestVideoFrameCore::TakeReadyAndRequest(
        ConsumerToken token, Clock::time_point now) {
    std::optional<ReadyFrame> result;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->consumers.find(token.id);
        if (state_->stats.closing || found == state_->consumers.end()) return {};
        auto& consumer = found->second;
        if (consumer.phase != State::Phase::Ready) return {};
        result = std::move(consumer.ready);
        consumer.ready = {};
        consumer.last_taken_sequence = result->sequence;
        ++consumer.epoch;
        consumer.phase = State::Phase::Requested;
        consumer.next_due = std::max(consumer.next_due, now);
    }
    state_->Wake();
    return result;
}

LatestVideoFrameCore::WorkLease LatestVideoFrameCore::TryStart(Clock::time_point now) {
    std::vector<ConsumerToken> retired;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& [id, consumer] : state_->consumers)
            if (consumer.retired) retired.push_back({id});
    }
    for (const auto token : retired) RemoveConsumer(token);
    std::unique_ptr<Job> job;
    bool cached_delivery = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stats.closing) return {};
        // A new consumer or a slower consumer can receive the final frame from
        // this cache without converting the native buffer a second time.
        for (auto& [id, binding] : state_->bindings) {
            if (!binding.cache.frame || binding.cache.sequence != binding.latest_sequence) continue;
            for (const auto consumer_id : binding.consumers) {
                auto& consumer = state_->consumers.at(consumer_id);
                if (consumer.phase == State::Phase::Requested && consumer.next_due <= now &&
                    consumer.last_taken_sequence < binding.cache.sequence) {
                    state_->Publish(consumer, binding.cache, now);
                    cached_delivery = true;
                }
            }
        }
        if (!state_->active_job && !state_->bindings.empty()) {
            auto selected = state_->bindings.end();
            const auto first = state_->bindings.upper_bound(state_->last_scheduled_binding);
            const auto eligible = [&](const auto& value) {
                const auto& binding = value.second;
                return binding.pending && state_->MaterializeDue(binding, now) <= now &&
                    state_->HasEligibleRequest(binding, binding.pending_sequence, now);
            };
            for (auto it = first; it != state_->bindings.end(); ++it) {
                if (eligible(*it)) { selected = it; break; }
            }
            if (selected == state_->bindings.end()) {
                for (auto it = state_->bindings.begin(); it != first; ++it) {
                    if (eligible(*it)) { selected = it; break; }
                }
            }
            if (selected != state_->bindings.end()) {
                auto& binding = selected->second;
                const auto job_id = AllocateIdentity();
                if (!job_id) { state_->stats.closing = true; return {}; }
                job = std::make_unique<Job>();
                job->state = state_;
                job->id = job_id;
                job->binding = selected->first;
                job->sequence = binding.pending_sequence;
                job->recipients.reserve(binding.consumers.size());
                for (const auto id : binding.consumers) {
                    auto& consumer = state_->consumers.at(id);
                    if (consumer.phase == State::Phase::Requested && consumer.next_due <= now &&
                        consumer.last_taken_sequence < job->sequence) {
                        job->recipients.push_back({id, consumer.epoch});
                        consumer.phase = State::Phase::InFlight;
                    }
                }
                job->input = std::move(binding.pending);
                binding.last_started = now;
                state_->active_job = job->id;
                state_->last_scheduled_binding = job->binding;
                state_->stats.in_flight_inputs = 1;
                ++state_->stats.materializations_started;
            }
        }
    }
    if (cached_delivery) state_->Wake();
    return WorkLease(std::move(job));
}

std::optional<LatestVideoFrameCore::Clock::time_point> LatestVideoFrameCore::NextWakeup(
        Clock::time_point now) const {
    std::optional<Clock::time_point> next;
    std::lock_guard lock(state_->mutex);
    if (state_->stats.closing) return {};
    for (const auto& [id, binding] : state_->bindings) {
        const bool cached = binding.cache.frame && binding.cache.sequence == binding.latest_sequence;
        const bool pending = binding.pending && !state_->active_job;
        if (!cached && !pending) continue;
        const auto sequence = cached ? binding.cache.sequence : binding.pending_sequence;
        for (const auto consumer_id : binding.consumers) {
            const auto& consumer = state_->consumers.at(consumer_id);
            if (consumer.phase != State::Phase::Requested || consumer.last_taken_sequence >= sequence) continue;
            auto due = std::max(now, consumer.next_due);
            if (!cached) due = std::max(due, state_->MaterializeDue(binding, now));
            if (!next || due < *next) next = due;
        }
    }
    return next;
}

void LatestVideoFrameCore::SetWakeCallback(std::function<void()> callback) {
    std::function<void()> retired;
    {
        std::lock_guard lock(state_->mutex);
        retired = std::exchange(state_->wake, std::move(callback));
    }
    retired = {};
    state_->Wake();
}

LatestVideoFrameCore::Statistics LatestVideoFrameCore::statistics() const {
    std::lock_guard lock(state_->mutex);
    auto result = state_->stats;
    result.bindings = state_->bindings.size();
    result.consumers = state_->consumers.size();
    for (const auto& [id, binding] : state_->bindings) if (binding.pending) ++result.pending_inputs;
    for (const auto& [id, consumer] : state_->consumers) {
        if (consumer.phase == State::Phase::Requested) ++result.requested_consumers;
        if (consumer.phase == State::Phase::Ready) ++result.ready_consumers;
    }
    return result;
}

void LatestVideoFrameCore::CloseAdmission() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stats.closing = true;
    }
    state_->Wake();
}

void LatestVideoFrameCore::BeginClose() {
    std::map<uint64_t, State::Binding> retired_bindings;
    std::map<uint64_t, State::Consumer> retired_consumers;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->retirement_started) return;
        state_->stats.closing = true;
        state_->retirement_started = true;
        retired_bindings.swap(state_->bindings);
        retired_consumers.swap(state_->consumers);
    }
    retired_consumers.clear();
    retired_bindings.clear();
    {
        std::lock_guard lock(state_->mutex);
        state_->retirement_finished = true;
    }
    state_->drained.notify_all();
    state_->Wake();
}

void LatestVideoFrameCore::Drain() {
    std::unique_lock lock(state_->mutex);
    state_->drained.wait(lock, [&] {
        return state_->retirement_finished && !state_->stats.retained_inputs && !state_->active_job;
    });
}

bool LatestVideoFrameCore::DrainFor(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    return state_->drained.wait_for(lock, timeout, [&] {
        return state_->retirement_finished && !state_->stats.retained_inputs && !state_->active_job;
    });
}

} // namespace livekit::render
