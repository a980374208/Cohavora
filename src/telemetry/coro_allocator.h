#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <coroutine>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

// 前置声明项目中的类
namespace livekit {
class WebSocketClient;
class SignalClient;
class SignalStream;
}

namespace livekit {

class CoroArena {
public:
    static void* Allocate(std::size_t size) {
        return ::operator new(size);
    }

    static void Deallocate(void* ptr, std::size_t size) {
        ::operator delete(ptr);
    }
};

// 辅助 Promise 类通用版本
template <typename T, typename Executor>
struct CustomPromise : public asio::detail::awaitable_frame<T, Executor> {
    using asio::detail::awaitable_frame<T, Executor>::awaitable_frame;
    using asio::detail::awaitable_frame<T, Executor>::await_transform;

    void* operator new(std::size_t size) {
        return livekit::CoroArena::Allocate(size);
    }

    void operator delete(void* ptr, std::size_t size) {
        livekit::CoroArena::Deallocate(ptr, size);
    }

    // 嵌套 awaiter 规避 Local Class 不能包含模板的限制
    template <typename U>
    struct awaiter {
        asio::awaitable<U, Executor> a_;

        bool await_ready() const noexcept {
            return a_.await_ready();
        }

        template <typename PromiseType>
        void await_suspend(std::coroutine_handle<PromiseType> h) {
            auto target_h = std::coroutine_handle<asio::detail::awaitable_frame<T, Executor>>::from_address(h.address());
            a_.await_suspend(target_h);
        }

        U await_resume() {
            return a_.await_resume();
        }
    };

    template <typename U>
    auto await_transform(asio::awaitable<U, Executor> a) {
        auto&& raw_awaitable = this->asio::detail::awaitable_frame_base<Executor>::await_transform(std::move(a));
        return awaiter<U>{std::move(raw_awaitable)};
    }
};

// 针对 void 特化版本的 CustomPromise
template <typename Executor>
struct CustomPromise<void, Executor> : public asio::detail::awaitable_frame<void, Executor> {
    using asio::detail::awaitable_frame<void, Executor>::awaitable_frame;
    using asio::detail::awaitable_frame<void, Executor>::await_transform;

    void* operator new(std::size_t size) {
        return livekit::CoroArena::Allocate(size);
    }

    void operator delete(void* ptr, std::size_t size) {
        livekit::CoroArena::Deallocate(ptr, size);
    }

    template <typename U>
    struct awaiter {
        asio::awaitable<U, Executor> a_;

        bool await_ready() const noexcept {
            return a_.await_ready();
        }

        template <typename PromiseType>
        void await_suspend(std::coroutine_handle<PromiseType> h) {
            auto target_h = std::coroutine_handle<asio::detail::awaitable_frame<void, Executor>>::from_address(h.address());
            a_.await_suspend(target_h);
        }

        U await_resume() {
            return a_.await_resume();
        }
    };

    template <typename U>
    auto await_transform(asio::awaitable<U, Executor> a) {
        auto&& raw_awaitable = this->asio::detail::awaitable_frame_base<Executor>::await_transform(std::move(a));
        return awaiter<U>{std::move(raw_awaitable)};
    }
};

} // namespace livekit

// 特化 std::coroutine_traits。通过更具象的首参数，避开与 Asio 自身万能特化冲突的重定义问题。
namespace std {

// 1. 针对 WebSocketClient 成员函数的特化
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, livekit::WebSocketClient&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, const livekit::WebSocketClient&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};

// 2. 针对 SignalClient 成员函数的特化
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, livekit::SignalClient&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, const livekit::SignalClient&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};

// 3. 针对 SignalStream 成员函数的特化
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, livekit::SignalStream&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};

// 4. 针对静态方法/独立函数中以 url(string) 开始的协程进行特化 (如 Connect)
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, const std::string&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, std::string, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};

// 5. 针对静态方法中以 ssl::context& 开始的协程进行特化 (如 SignalStream::Connect)
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, asio::ssl::context&, Args...> {
    using promise_type = livekit::CustomPromise<T, Executor>;
};

} // namespace std
