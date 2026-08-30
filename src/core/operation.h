#pragma once

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace livekit {

enum class OperationKind {
    Connect,
    PublishTrack,
    Negotiate,
    Reconnect,
    Disconnect,
};

enum class OperationErrorCode {
    InvalidState,
    Cancelled,
    SignalConnectFailed,
    JoinTimeout,
    JoinRejected,
    PeerConnectionCreateFailed,
    NegotiationFailed,
    PeerConnectionTimeout,
    PermissionDenied,
    TrackPublishTimeout,
    TrackPublishRejected,
    ReconnectExhausted,
    SessionClosed,
    StateUncertain,
};

class OperationError final : public std::runtime_error {
public:
    OperationError(OperationKind operation,
                   OperationErrorCode code,
                   std::string stage,
                   std::string message,
                   bool retryable = false)
        : std::runtime_error(std::move(message)),
          operation_(operation),
          code_(code),
          stage_(std::move(stage)),
          retryable_(retryable) {}

    OperationKind operation() const noexcept { return operation_; }
    OperationErrorCode code() const noexcept { return code_; }
    const std::string& stage() const noexcept { return stage_; }
    bool retryable() const noexcept { return retryable_; }

private:
    OperationKind operation_;
    OperationErrorCode code_;
    std::string stage_;
    bool retryable_;
};

struct OperationTimeouts {
    std::chrono::milliseconds peer_connection{std::chrono::seconds(10)};
    std::chrono::milliseconds negotiation{std::chrono::seconds(10)};
    std::chrono::milliseconds publish{std::chrono::seconds(10)};
    std::chrono::milliseconds reconnect_attempt{std::chrono::seconds(10)};
    std::chrono::milliseconds reconnect_total{std::chrono::seconds(60)};
    std::chrono::milliseconds disconnect_grace{std::chrono::seconds(2)};
};

struct OperationContext {
    uint64_t id = 0;
    uint64_t session_generation = 0;
    OperationKind kind = OperationKind::Connect;
    std::chrono::steady_clock::time_point deadline{};
    std::atomic<bool> cancelled{false};

    void ThrowIfCancelled(const char* stage) const {
        if (cancelled.load(std::memory_order_acquire)) {
            throw OperationError(kind, OperationErrorCode::Cancelled, stage,
                                 "operation cancelled");
        }
    }
};

template <typename T>
struct AwaitableState {
    explicit AwaitableState(asio::any_io_executor executor)
        : timer(std::make_shared<asio::steady_timer>(executor)) {}

    mutable std::mutex mutex;
    std::shared_ptr<asio::steady_timer> timer;
    bool completed = false;
    std::optional<T> value;
    std::exception_ptr error;
};

template <>
struct AwaitableState<void> {
    explicit AwaitableState(asio::any_io_executor executor)
        : timer(std::make_shared<asio::steady_timer>(executor)) {}

    mutable std::mutex mutex;
    std::shared_ptr<asio::steady_timer> timer;
    bool completed = false;
    std::exception_ptr error;
};

template <typename T>
inline bool CompleteAwaitable(const std::shared_ptr<AwaitableState<T>>& state, T value) {
    if (!state) return false;
    {
        std::lock_guard lock(state->mutex);
        if (state->completed) return false;
        state->completed = true;
        state->value = std::move(value);
    }
    state->timer->cancel();
    return true;
}

inline bool CompleteAwaitable(const std::shared_ptr<AwaitableState<void>>& state) {
    if (!state) return false;
    {
        std::lock_guard lock(state->mutex);
        if (state->completed) return false;
        state->completed = true;
    }
    state->timer->cancel();
    return true;
}

template <typename T>
inline bool FailAwaitable(const std::shared_ptr<AwaitableState<T>>& state,
                          std::exception_ptr error) {
    if (!state) return false;
    {
        std::lock_guard lock(state->mutex);
        if (state->completed) return false;
        state->completed = true;
        state->error = std::move(error);
    }
    state->timer->cancel();
    return true;
}

template <typename T>
asio::awaitable<T> WaitAwaitable(
    const std::shared_ptr<AwaitableState<T>>& state,
    std::chrono::milliseconds timeout,
    OperationKind operation,
    OperationErrorCode timeout_code,
    std::string stage) {
    {
        std::lock_guard lock(state->mutex);
        if (state->completed) {
            if (state->error) std::rethrow_exception(state->error);
            co_return *state->value;
        }
        state->timer->expires_after(timeout);
    }

    std::error_code ec;
    co_await state->timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));

    std::exception_ptr error;
    std::optional<T> value;
    {
        std::lock_guard lock(state->mutex);
        if (!state->completed) {
            state->completed = true;
            state->error = std::make_exception_ptr(OperationError(
                operation, timeout_code, stage, stage + " timed out", true));
        }
        error = state->error;
        value = state->value;
    }
    if (error) std::rethrow_exception(error);
    co_return std::move(*value);
}

template <>
inline asio::awaitable<void> WaitAwaitable<void>(
    const std::shared_ptr<AwaitableState<void>>& state,
    std::chrono::milliseconds timeout,
    OperationKind operation,
    OperationErrorCode timeout_code,
    std::string stage) {
    {
        std::lock_guard lock(state->mutex);
        if (state->completed) {
            if (state->error) std::rethrow_exception(state->error);
            co_return;
        }
        state->timer->expires_after(timeout);
    }

    std::error_code ec;
    co_await state->timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));

    std::exception_ptr error;
    {
        std::lock_guard lock(state->mutex);
        if (!state->completed) {
            state->completed = true;
            state->error = std::make_exception_ptr(OperationError(
                operation, timeout_code, stage, stage + " timed out", true));
        }
        error = state->error;
    }
    if (error) std::rethrow_exception(error);
    co_return;
}

} // namespace livekit
