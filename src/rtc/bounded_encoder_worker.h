#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace livekit::detail {

// Tasks must own their captures. In particular, a timed-out task must not retain
// caller stack references or invoke a caller-owned callback. Return owned output
// instead; the caller publishes it only after observing kCompleted.
//
// A timeout seals this worker permanently. The outstanding task, its captures,
// Close, and backend destruction stay on the worker in that order, even when a
// vendor call never returns. No vendor work or capture destruction holds mutex.
template<class Backend, class Result>
class BoundedEncoderWorker final {
    static_assert(std::is_copy_constructible_v<Result>,
                  "Result must be copyable so repeated Close can retain its result");

public:
    using Task = std::function<Result(Backend&)>;

    struct Reply {
        enum class Wait { kCompleted, kTimedOut, kStopped, kException };
        Wait wait = Wait::kStopped;
        std::optional<Result> value;
        std::string error;
    };
    using Wait = typename Reply::Wait;

    BoundedEncoderWorker(std::unique_ptr<Backend> backend, Task close_task)
        : state_(std::make_shared<State>(std::move(backend), std::move(close_task))) {
        if (!state_->backend || !state_->close_task) {
            throw std::invalid_argument("encoder worker requires a backend and Close task");
        }
        // If thread creation fails, the unopened backend may be destroyed here.
        // After launch, only Run accesses or destroys it.
        worker_ = std::thread([state = state_] { Run(std::move(state)); });
    }

    BoundedEncoderWorker(const BoundedEncoderWorker&) = delete;
    BoundedEncoderWorker& operator=(const BoundedEncoderWorker&) = delete;
    BoundedEncoderWorker(BoundedEncoderWorker&&) = delete;
    BoundedEncoderWorker& operator=(BoundedEncoderWorker&&) = delete;

    ~BoundedEncoderWorker() {
        bool finished;
        {
            std::lock_guard lock(state_->mutex);
            state_->sealed = true;
            finished = state_->finished;
        }
        state_->changed.notify_all();
        if (worker_.joinable()) {
            if (finished && worker_.get_id() != std::this_thread::get_id()) {
                worker_.join();
            } else {
                // The thread's strong State reference keeps the backend and
                // active task/frame alive until their vendor call returns.
                worker_.detach();
            }
        }
    }

    Reply Invoke(Task task, std::chrono::milliseconds timeout) {
        auto completion = std::make_shared<Completion>();
        auto job = std::make_unique<Job>(Job{std::move(task), completion});
        std::unique_lock lock(state_->mutex);
        if (state_->sealed) return {Wait::kStopped, {}, "encoder worker is stopped"};
        if (state_->busy) return {Wait::kStopped, {}, "encoder worker is busy"};
        state_->busy = true;
        state_->pending = std::move(job);
        state_->changed.notify_all();
        if (!state_->changed.wait_for(lock, timeout, [&] { return completion->ready; })) {
            state_->sealed = true;
            lock.unlock();
            state_->changed.notify_all();
            return {Wait::kTimedOut, {}, "encoder worker operation timed out"};
        }
        // Result moves, and even destruction of a late result, may own vendor
        // resources. Never perform them while holding State's metadata lock.
        lock.unlock();
        return std::move(*completion->reply);
    }

    Reply Close(std::chrono::milliseconds timeout) {
        std::unique_lock lock(state_->mutex);
        state_->sealed = true;
        state_->changed.notify_all();
        if (!state_->changed.wait_for(lock, timeout, [&] { return state_->finished; })) {
            return {Wait::kTimedOut, {}, "encoder worker Close timed out"};
        }
        lock.unlock();
        // finished is published only after Close and backend destruction. The
        // immutable reply can now be copied without holding the metadata lock.
        return *state_->close_reply;
    }

private:
    struct Completion {
        bool ready = false; // Protected by State::mutex.
        std::optional<Reply> reply; // Worker writes before publishing ready.
    };
    struct Job {
        Task task;
        std::shared_ptr<Completion> completion;
    };
    struct State {
        State(std::unique_ptr<Backend> owned_backend, Task close)
            : backend(std::move(owned_backend)), close_task(std::move(close)) {}
        std::mutex mutex;
        std::condition_variable changed;
        bool busy = false;
        bool sealed = false;
        bool finished = false;
        std::unique_ptr<Job> pending;
        std::unique_ptr<Backend> backend;
        Task close_task;
        std::optional<Reply> close_reply;
    };

    static Reply CurrentException() noexcept {
        Reply reply;
        reply.wait = Wait::kException;
        try {
            throw;
        } catch (const std::exception& error) {
            try { reply.error = error.what(); } catch (...) {}
        } catch (...) {
            try { reply.error = "unknown encoder worker exception"; } catch (...) {}
        }
        return reply;
    }

    static Reply Call(Task& task, Backend& backend) {
        try {
            return {Wait::kCompleted, task(backend), {}};
        } catch (...) {
            return CurrentException();
        }
    }

    static void Run(std::shared_ptr<State> state) noexcept {
        std::unique_ptr<Job> job;
        std::shared_ptr<Completion> active;
        try {
            for (;;) {
                {
                    std::unique_lock lock(state->mutex);
                    state->changed.wait(lock, [&] { return state->sealed || state->pending; });
                    if (!state->pending) break;
                    job = std::move(state->pending);
                    active = job->completion;
                }
                active->reply.emplace(Call(job->task, *state->backend));
                const bool failed = active->reply->wait == Wait::kException;
                // Captures can own frames whose destruction calls a driver.
                // Retire them before publication, and outside the State lock.
                job.reset();
                {
                    std::lock_guard lock(state->mutex);
                    active->ready = true;
                    state->busy = false;
                    if (failed) state->sealed = true;
                }
                active.reset();
                state->changed.notify_all();
            }
        } catch (...) {
            // Contain unexpected worker exceptions as well as Task exceptions.
            // Swapping pointer ownership under the lock never destroys captures.
            const auto failure = CurrentException();
            std::unique_ptr<Job> pending;
            {
                std::lock_guard lock(state->mutex);
                state->sealed = true;
                pending = std::move(state->pending);
            }
            if (active) active->reply.emplace(failure);
            auto pending_completion = pending ? pending->completion : nullptr;
            if (pending_completion) pending_completion->reply.emplace(failure);
            job.reset();
            pending.reset();
            {
                std::lock_guard lock(state->mutex);
                state->busy = false;
                if (active) active->ready = true;
                if (pending_completion) pending_completion->ready = true;
            }
            active.reset();
            state->changed.notify_all();
        }

        // Also destroy the Close task's captures on this thread, before the
        // backend. No caller is allowed to touch either object after launch.
        Task close = std::move(state->close_task);
        try {
            state->close_reply.emplace(Call(close, *state->backend));
        } catch (...) {
            state->close_reply.emplace(CurrentException());
        }
        close = {};
        state->backend.reset();
        {
            std::lock_guard lock(state->mutex);
            state->finished = true;
        }
        state->changed.notify_all();
    }

    std::shared_ptr<State> state_;
    std::thread worker_;
};

} // namespace livekit::detail
