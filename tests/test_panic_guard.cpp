#include <iostream>
#include <cassert>
#include <string>
#include <asio.hpp>
#include "crash_handler.h"
#include "safe_spawn.h"

int main() {
    std::cout << "=========================================\n";
    std::cout << " Running Panic Guard & Safe Spawn Tests \n";
    std::cout << "=========================================\n";

    bool panic_callback_triggered = false;
    std::string captured_panic_msg;

    // 1. 测试 CrashHandler panic 回调与 Log Flush
    livekit::CrashHandler::SetPanicCallback([&](const std::string& msg) {
        panic_callback_triggered = true;
        captured_panic_msg = msg;
    });

    std::cout << "[Test 1] Testing CrashHandler::TriggerPanic..." << std::endl;
    livekit::CrashHandler::TriggerPanic("Test Simulating FFI Panic Exception", /*raise_sigterm=*/false);

    assert(panic_callback_triggered && "Panic callback should have been triggered!");
    assert(captured_panic_msg.find("Test Simulating FFI Panic Exception") != std::string::npos);
    std::cout << "[Test 1 PASSED] Panic callback successfully intercepted panic message!\n" << std::endl;

    // 2. 测试 safe_co_spawn 拦截协程未捕获异常
    std::cout << "[Test 2] Testing safe_co_spawn coroutine exception interception..." << std::endl;
    asio::io_context io_ctx;

    bool coroutine_error_caught = false;
    livekit::safe_co_spawn(
        io_ctx.get_executor(),
        []() -> asio::awaitable<void> {
            std::cout << "  -> Inside coroutine, preparing to throw runtime_error..." << std::endl;
            throw std::runtime_error("Simulated unhandled exception inside C++20 coroutine!");
            co_return;
        },
        [&](std::exception_ptr ep) {
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                std::cout << "  -> Successfully intercepted exception in safe_co_spawn handler: " << e.what() << std::endl;
                coroutine_error_caught = true;
            }
        }
    );

    io_ctx.run();

    assert(coroutine_error_caught && "safe_co_spawn should have intercepted coroutine exception without process crash!");
    std::cout << "[Test 2 PASSED] Coroutine exception intercepted safely without crash!\n" << std::endl;

    std::cout << "=========================================\n";
    std::cout << " ALL PANIC GUARD TESTS PASSED SUCCESSFULLY! \n";
    std::cout << "=========================================\n";

    return 0;
}
