#include <iostream>
#include "tests/support/test_check.h"
#include <string>
#include <asio.hpp>
#include "crash_handler.h"
#include "log_redaction.h"
#include "safe_spawn.h"

int main() {
    std::cout << "=========================================\n";
    std::cout << " Running Panic Guard & Safe Spawn Tests \n";
    std::cout << "=========================================\n";

    int panic_callback_count = 0;
    std::string captured_panic_msg;

    // 1. 测试 CrashHandler panic 回调与 Log Flush
    livekit::CrashHandler::SetPanicCallback([&](const std::string& msg) {
        ++panic_callback_count;
        captured_panic_msg = msg;
    });

    std::cout << "[Test 1] Testing CrashHandler::TriggerPanic..." << std::endl;
    constexpr const char* kPanicSecret = "panic-access_token=synthetic-panic-secret";
    livekit::CrashHandler::TriggerPanic(kPanicSecret, /*raise_sigterm=*/false);

    TEST_CHECK(panic_callback_count == 1);
    TEST_CHECK(captured_panic_msg == livekit::secure_log::OpaqueSummary("panic"));
    TEST_CHECK(captured_panic_msg.find("synthetic-panic-secret") == std::string::npos);
    std::cout << "[Test 1 PASSED] Panic callback successfully intercepted panic message!\n" << std::endl;

    // 2. 测试 safe_co_spawn 拦截协程未捕获异常
    std::cout << "[Test 2] Testing safe_co_spawn coroutine exception interception..." << std::endl;
    asio::io_context io_ctx;

    bool coroutine_error_caught = false;
    livekit::safe_co_spawn(
        io_ctx.get_executor(),
        []() -> asio::awaitable<void> {
            std::cout << "  -> Inside coroutine, preparing to throw runtime_error..." << std::endl;
            throw std::runtime_error("access_token=synthetic-coroutine-secret");
            co_return;
        },
        [&](std::exception_ptr ep) {
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                TEST_CHECK(std::string(e.what()).find("synthetic-coroutine-secret") != std::string::npos);
                coroutine_error_caught = true;
            }
        }
    );

    io_ctx.run();

    TEST_CHECK(coroutine_error_caught && "safe_co_spawn should have intercepted coroutine exception without process crash!");
    std::cout << "[Test 2 PASSED] Coroutine exception intercepted safely without crash!\n" << std::endl;

    // 3. Without an explicit error handler, safe_co_spawn must preserve panic
    // delivery while keeping the exception detail out of stderr/callback data.
    asio::io_context panic_io;
    livekit::safe_co_spawn(
        panic_io.get_executor(),
        []() -> asio::awaitable<void> {
            throw std::runtime_error("password=synthetic-unhandled-secret");
            co_return;
        });
    panic_io.run();
    TEST_CHECK(panic_callback_count == 2);
    TEST_CHECK(captured_panic_msg == livekit::secure_log::OpaqueSummary("panic"));
    TEST_CHECK(captured_panic_msg.find("synthetic-unhandled-secret") == std::string::npos);

    std::cout << "=========================================\n";
    std::cout << " ALL PANIC GUARD TESTS PASSED SUCCESSFULLY! \n";
    std::cout << "=========================================\n";

    return 0;
}
