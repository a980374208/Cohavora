#pragma once

#include <asio.hpp>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

namespace livekit {

// The lease owns only the execution context, never Room or a session handler.
// Closing admission and counting already admitted calls share the same lock.
class ExecutorCallbackGate final
    : public std::enable_shared_from_this<ExecutorCallbackGate> {
public:
    class Ticket final {
    public:
        Ticket() = default;
        explicit Ticket(std::shared_ptr<ExecutorCallbackGate> owner)
            : owner_(std::move(owner)) {}
        Ticket(Ticket&&) = default;
        Ticket& operator=(Ticket&&) = delete;
        Ticket(const Ticket&) = delete;
        ~Ticket() { if (owner_) owner_->Release(); }
        explicit operator bool() const noexcept { return bool(owner_); }
    private:
        std::shared_ptr<ExecutorCallbackGate> owner_;
    };

    class Cancellation final {
    public:
        Cancellation(std::shared_ptr<ExecutorCallbackGate> owner, std::uint64_t id)
            : owner_(std::move(owner)), id_(id) {}
        Cancellation(Cancellation&& other) noexcept
            : owner_(std::move(other.owner_)), id_(other.id_) {}
        Cancellation(const Cancellation&) = delete;
        ~Cancellation() { if (owner_) owner_->ForgetCancellation(id_); }
    private:
        std::shared_ptr<ExecutorCallbackGate> owner_;
        std::uint64_t id_;
    };

    ExecutorCallbackGate(asio::any_io_executor executor,
                         std::shared_ptr<void> lifetime = {})
        : lifetime_(std::move(lifetime)), executor_(std::move(executor)) {}

    Ticket Enter() {
        std::lock_guard lock(mutex_);
        if (closed_) return {};
        ++in_flight_;
        return Ticket(shared_from_this());
    }

    template <typename Callback>
    bool Post(Callback&& callback) {
        auto ticket = Enter();
        if (!ticket) return false;
        asio::post(executor_,
            [ticket = std::move(ticket), callback = std::forward<Callback>(callback)]() mutable {
                callback();
            });
        return true;
    }

    // Only registered, single-shot operations use this completion channel.
    // Their local cancellation must complete even after business admission closes.
    template <typename Callback>
    void PostCompletion(Callback&& callback) {
        {
            std::lock_guard lock(mutex_);
            ++in_flight_;
        }
        auto ticket = Ticket(shared_from_this());
        asio::post(executor_,
            [ticket = std::move(ticket), callback = std::forward<Callback>(callback)]() mutable {
                callback();
            });
    }

    std::uint64_t RegisterCancellation(std::function<void()> cancel) {
        std::lock_guard lock(mutex_);
        if (closed_) return 0;
        const auto id = ++next_id_;
        cancellations_.emplace(id, std::move(cancel));
        return id;
    }

    void ForgetCancellation(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        cancellations_.erase(id);
    }

    Cancellation CancelWhenClosed(std::function<void()> cancel) {
        const auto id = RegisterCancellation(cancel);
        if (!id) cancel();
        return Cancellation(shared_from_this(), id);
    }

    void Close() {
        std::map<std::uint64_t, std::function<void()>> cancellations;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            closed_ = true;
            cancellations.swap(cancellations_);
        }
        for (auto& [_, cancel] : cancellations) cancel();
        {
            std::lock_guard lock(mutex_);
            cancellations_finished_ = true;
        }
        wake_.notify_all();
    }

    // Shutdown-worker only: the I/O runner must still be alive. No deadline is
    // interpreted as permission to destroy the context or native resources.
    void WaitForDrain() {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] {
            return closed_ && cancellations_finished_ && in_flight_ == 0;
        });
    }

private:
    void Release() {
        {
            std::lock_guard lock(mutex_);
            --in_flight_;
        }
        wake_.notify_all();
    }

    // Declaration order is deliberate: executor is destroyed before its lease.
    const std::shared_ptr<void> lifetime_;
    asio::any_io_executor executor_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::size_t in_flight_ = 0;
    std::uint64_t next_id_ = 0;
    bool closed_ = false;
    bool cancellations_finished_ = false;
    std::map<std::uint64_t, std::function<void()>> cancellations_;
};

// The gate holds only a weak cancellation closure; a pending native callback
// cannot create gate -> callback -> Room -> gate ownership cycles.
template <typename... Args>
class CancellableExecutorCallback final
    : public std::enable_shared_from_this<CancellableExecutorCallback<Args...>> {
public:
    using Callback = std::function<void(Args...)>;
    static std::shared_ptr<CancellableExecutorCallback> Create(
            std::shared_ptr<ExecutorCallbackGate> gate, Callback callback,
            Args... cancelled) {
        auto result = std::shared_ptr<CancellableExecutorCallback>(
            new CancellableExecutorCallback(std::move(gate), std::move(callback)));
        const std::weak_ptr<CancellableExecutorCallback> weak = result;
        {
            std::lock_guard lock(result->mutex_);
            result->id_ = result->gate_->RegisterCancellation(
                [weak, values = std::make_tuple(cancelled...)]() mutable {
                    if (auto live = weak.lock()) {
                        std::apply([&](auto&&... values) {
                            live->Complete(std::forward<decltype(values)>(values)...);
                        }, std::move(values));
                    }
                });
        }
        if (result->id_ == 0) result->Complete(std::move(cancelled)...);
        return result;
    }

    void Complete(Args... args) {
        std::lock_guard lock(mutex_);
        if (!callback_) return;
        gate_->PostCompletion(
            [callback = std::move(callback_), values = std::make_tuple(std::move(args)...)]() mutable {
                std::apply(callback, std::move(values));
            });
        gate_->ForgetCancellation(id_);
    }

    bool pending() const {
        std::lock_guard lock(mutex_);
        return bool(callback_);
    }

    ~CancellableExecutorCallback() { gate_->ForgetCancellation(id_); }

private:
    CancellableExecutorCallback(std::shared_ptr<ExecutorCallbackGate> gate, Callback callback)
        : gate_(std::move(gate)), callback_(std::move(callback)) {}
    const std::shared_ptr<ExecutorCallbackGate> gate_;
    mutable std::mutex mutex_;
    Callback callback_;
    std::uint64_t id_ = 0;
};

} // namespace livekit
