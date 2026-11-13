#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <utility>
#include <vector>

namespace livekit::detail {

// Own a whole discovery/preflight transaction, including all encoder cleanup.
// Driver calls and frame ownership belong to the SDK's separate backend worker.
// Unfinished jobs retain a joinable pthread in a process-lifetime bounded registry;
// a deadline never detaches a thread or destroys objects still used by that thread.
template <typename Result>
class LinuxEncoderProbeExecutor final {
public:
    enum class Wait { Completed, TimedOut, Unavailable };
    struct Reply {
        Wait wait = Wait::Unavailable;
        std::optional<Result> value;
    };

    static Reply Run(std::function<Result()> owned_work,
                     std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        if (!owned_work || budget <= std::chrono::milliseconds::zero()) return {};
        std::shared_ptr<Job> job;
        try { job = RegistryInstance().Start(std::move(owned_work)); }
        catch (...) { return {}; }
        if (!job) return {};
        for (;;) {
            // pthread_tryjoin_np, rather than a userspace finished flag, proves
            // that thread-local destructors and the pthread itself have exited.
            if (RegistryInstance().TryJoin(job)) {
                // The waiting caller may resume after its deadline. A joined
                // thread alone does not prove retirement met the total budget.
                if (std::chrono::steady_clock::now() >= deadline)
                    return {Wait::TimedOut, std::nullopt};
                return {Wait::Completed, std::move(job->result)};
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return {Wait::TimedOut, std::nullopt};
            std::unique_lock lock(job->mutex);
            if (!job->finished.load(std::memory_order_acquire)) {
                job->changed.wait_until(lock, deadline, [&] {
                    return job->finished.load(std::memory_order_acquire);
                });
            } else {
                job->changed.wait_until(lock, std::min(deadline, now +
                    std::chrono::milliseconds(1)));
            }
        }
    }

private:
    struct Job {
        std::function<Result()> work;
        std::optional<Result> result;
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> finished{false};
        pthread_t thread{};
        bool joined = false; // Registry mutex owns this flag and pthread handle.
    };

    class Registry {
    public:
        std::shared_ptr<Job> Start(std::function<Result()> work) {
            std::lock_guard lock(mutex_);
            Reap();
            // Camera and screen-share may preflight concurrently. Exhaustion
            // before a thread starts is ordinary unavailable, without poisoning.
            if (jobs_.size() >= 2) return {};
            auto job = std::make_shared<Job>();
            job->work = std::move(work);
            jobs_.push_back(job);
            if (pthread_create(&job->thread, nullptr, &RunJob, job.get()) != 0) {
                jobs_.pop_back();
                return {};
            }
            return job;
        }

        bool TryJoin(const std::shared_ptr<Job>& job) {
            std::lock_guard lock(mutex_);
            if (!job->joined && pthread_tryjoin_np(job->thread, nullptr) == 0)
                job->joined = true;
            return job->joined;
        }

    private:
        static void* RunJob(void* argument) noexcept {
            auto& job = *static_cast<Job*>(argument);
            try { job.result = job.work(); } catch (...) { job.result = Result{}; }
            // Drop the environment, encoder group and all captures on this owner
            // before the caller is allowed to publish a completed result.
            job.work = {};
            {
                std::lock_guard lock(job.mutex);
                job.finished.store(true, std::memory_order_release);
            }
            job.changed.notify_all();
            return nullptr;
        }

        void Reap() {
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                auto& job = **it;
                if (!job.joined && pthread_tryjoin_np(job.thread, nullptr) == 0)
                    job.joined = true;
                if (job.joined) it = jobs_.erase(it);
                else ++it;
            }
        }

        std::mutex mutex_;
        std::vector<std::shared_ptr<Job>> jobs_;
    };

    static Registry& RegistryInstance() {
        // Deliberately retain blocked joinable workers until process exit.
        static auto* registry = new Registry;
        return *registry;
    }
};

} // namespace livekit::detail
