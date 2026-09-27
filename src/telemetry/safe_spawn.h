#pragma once

#include <asio.hpp>
#include <exception>
#include <functional>
#include <string>
#include "crash_handler.h"
#include "log_redaction.h"

namespace livekit {

// 包装 asio::co_spawn，确保协程内部发生的未捕获异常会被拦截，
// 通过 CrashHandler/回调触发 Panic 防护，
// 避免直接导致 std::terminate() 崩溃。

template <typename Executor, typename F>
void safe_co_spawn(Executor&& exec, F&& factor_coro, 
                   std::function<void(std::exception_ptr)> on_error = nullptr) {
    asio::co_spawn(
        std::forward<Executor>(exec),
        [factor_coro = std::forward<F>(factor_coro)]() -> asio::awaitable<void> {
            co_await factor_coro();
        },
        [on_error = std::move(on_error)](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception&) {
                    if (on_error) {
                        on_error(ep);
                    } else {
                        CrashHandler::TriggerPanic(
                            secure_log::ExceptionSummary("coroutine_completion"),
                            /*raise_sigterm=*/false);
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
