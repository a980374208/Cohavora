#pragma once

#include <asio.hpp>
#include <exception>
#include <iostream>
#include <functional>
#include <string>
#include "crash_handler.h"

namespace livekit {

// 包装 asio::co_spawn，确保协程内部发生的未捕获异常会被拦截，
// 打印 Critical 日志、Flush log 并通过 CrashHandler/回调触发 Panic 防护，
// 避免直接导致 std::terminate() 崩溃。

template <typename Executor, typename F>
void safe_co_spawn(Executor&& exec, F&& factor_coro, 
                   std::function<void(std::exception_ptr)> on_error = nullptr) {
    asio::co_spawn(
        std::forward<Executor>(exec),
        [factor_coro = std::forward<F>(factor_coro)]() -> asio::awaitable<void> {
            try {
                co_await factor_coro();
            } catch (const std::exception& e) {
                std::string err_msg = std::string("Unhandled coroutine std::exception: ") + e.what();
                std::cerr << "[CRITICAL COROUTINE EXCEPTION]: " << err_msg << std::endl;
                CrashHandler::FlushLogs();
                throw; // 传递给 completion token 统一处理
            } catch (...) {
                std::cerr << "[CRITICAL COROUTINE EXCEPTION]: Unhandled unknown exception in coroutine." << std::endl;
                CrashHandler::FlushLogs();
                throw;
            }
        },
        [on_error = std::move(on_error)](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    std::string msg = std::string("Coroutine crashed: ") + e.what();
                    if (on_error) {
                        on_error(ep);
                    } else {
                        CrashHandler::TriggerPanic(msg, /*raise_sigterm=*/false);
                    }
                } catch (...) {
                    if (on_error) {
                        on_error(ep);
                    } else {
                        CrashHandler::TriggerPanic("Coroutine crashed with unknown exception", /*raise_sigterm=*/false);
                    }
                }
            }
        }
    );
}

} // namespace livekit
