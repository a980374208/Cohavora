#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include <asio.hpp>
#include "room.h"
#include "participant.h"
#include "rpc_types.h"

int main() {
    std::cout << "==================================================\n";
    std::cout << " Running LiveKit RPC System Automated Tests       \n";
    std::cout << "==================================================\n";

    asio::io_context io_ctx;

    auto room = livekit::Room::Create(io_ctx.get_executor());

    // 创建模拟 LocalParticipant
    auto local_p = std::make_shared<livekit::LocalParticipant>(
        "PA_local_999", "user_alice",
        [](const livekit::proto::SignalRequest&) {}
    );

    room->SetLocalParticipantForTesting(local_p);

    // 模拟 DataChannel/Network 将 RPC 数据包自动回环组包路由
    local_p->SetPublishDataHandler([room](const std::vector<uint8_t>& payload, bool reliable, const std::vector<std::string>& dest, const std::string& topic) {
        room->OnIncomingDataPacket(payload, "user_alice", topic);
    });

    local_p->SetSendRpcHandler([room](const livekit::RpcPacket& packet) -> asio::awaitable<std::string> {
        co_return co_await room->SendRpcRequest(packet);
    });

    // 绑定至 room
    // 为测试注入 local_participant_ 句柄
    // 在纯集成模拟逻辑中手动配置
    std::string test_result_1;
    bool test_2_failed_as_expected = false;
    bool test_3_timeout_as_expected = false;

    // 注册 RPC 方法 1: greeting.hello
    local_p->registerRpcMethod("greeting.hello", [](const livekit::RpcInvocationData& inv) -> asio::awaitable<std::string> {
        std::cout << "  -> [RPC Handler] Executing greeting.hello for caller=" << inv.caller_identity
                  << " payload='" << inv.payload << "'" << std::endl;
        co_return "Hello, " + inv.payload + "!";
    });

    // 注册 RPC 方法 2: slow.method (模拟超时)
    local_p->registerRpcMethod("slow.method", [](const livekit::RpcInvocationData& inv) -> asio::awaitable<std::string> {
        std::cout << "  -> [RPC Handler] Executing slow.method..." << std::endl;
        // 挂起超长时间
        asio::steady_timer long_timer(co_await asio::this_coro::executor, std::chrono::seconds(5));
        std::error_code ec;
        co_await long_timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        co_return "Slow Result";
    });

    // 模拟连入状态
    livekit::safe_co_spawn(
        io_ctx.get_executor(),
        [&]() -> asio::awaitable<void> {
            // [Test 1] 跨端/自调 RPC 成功用例
            std::cout << "[Test 1] Testing successful RPC performRpc call..." << std::endl;
            try {
                // 模拟通过房间连通性环境
                // 设置内部指针
                std::string res = co_await local_p->performRpc("user_alice", "greeting.hello", "Alice");
                test_result_1 = res;
                std::cout << "  -> [Test 1 Result] Received RPC response: '" << res << "'" << std::endl;
            } catch (const livekit::RpcError& e) {
                std::cout << "  -> [Test 1 FAIL] Unexpected RpcError: " << e.what() << std::endl;
            }

            // [Test 2] 调用未注册的方法 (UNSUPPORTED_METHOD)
            std::cout << "\n[Test 2] Testing unsupported RPC method call..." << std::endl;
            try {
                co_await local_p->performRpc("user_alice", "non_existent_method", "data");
            } catch (const livekit::RpcError& e) {
                std::cout << "  -> [Test 2 Result] Caught expected RpcError code="
                          << static_cast<int>(e.code()) << " msg='" << e.message() << "'" << std::endl;
                if (e.code() == livekit::RpcErrorCode::UNSUPPORTED_METHOD) {
                    test_2_failed_as_expected = true;
                }
            }

            // [Test 3] RPC 超时用例 (TIMEOUT)
            std::cout << "\n[Test 3] Testing RPC timeout handling..." << std::endl;
            try {
                co_await local_p->performRpc("user_alice", "slow.method", "data", /*timeout_sec=*/0.1);
            } catch (const livekit::RpcError& e) {
                std::cout << "  -> [Test 3 Result] Caught expected RpcError code="
                          << static_cast<int>(e.code()) << " msg='" << e.message() << "'" << std::endl;
                if (e.code() == livekit::RpcErrorCode::TIMEOUT) {
                    test_3_timeout_as_expected = true;
                }
            }

            co_return;
        }
    );

    // 运行 asio 事件循环
    io_ctx.run();

    assert(test_result_1 == "Hello, Alice!" && "Test 1 failed: RPC response did not match expected value!");
    std::cout << "[Test 1 PASSED] Successful RPC invocation verified!\n";

    assert(test_2_failed_as_expected && "Test 2 failed: UNSUPPORTED_METHOD was not returned!");
    std::cout << "[Test 2 PASSED] Unsupported method error handling verified!\n";

    assert(test_3_timeout_as_expected && "Test 3 failed: TIMEOUT error was not triggered!");
    std::cout << "[Test 3 PASSED] RPC timeout handling verified!\n";

    std::cout << "==================================================\n";
    std::cout << " ALL LIVEKIT RPC SYSTEM TESTS PASSED 100%!        \n";
    std::cout << "==================================================\n";

    return 0;
}
