#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <atomic>
#include <asio.hpp>
#include "room.h"
#include "participant.h"
#include "chat_message.h"
#include "rpc_types.h"

// SimpleRpc 事件监听器
class SimpleRpcListener : public livekit::RoomListener {
public:
    void OnConnected() override {
        std::cout << "\n==================================================" << std::endl;
        std::cout << "[SUCCESS] SimpleRpc::OnConnected - Connected to LiveKit Server!" << std::endl;
        std::cout << "==================================================" << std::endl;
    }

    void OnDisconnected(const std::string& reason) override {
        std::cout << "\n[INFO] SimpleRpc::OnDisconnected - Reason: "
                  << (reason.empty() ? "Normal Disconnect" : reason) << std::endl;
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Joined: identity='" << participant->identity()
                      << "', sid=" << participant->sid() << std::endl;
        }
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Left: identity='" << participant->identity()
                      << "', sid=" << participant->sid() << std::endl;
        }
    }
};

void PrintUsage(const char* prog_name) {
    std::cout << "LiveKit C++ SimpleRpc Example App\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " --url <livekit_ws_url> --token <access_token> [options]\n";
    std::cout << "  or set environment variables: LIVEKIT_URL, LIVEKIT_TOKEN\n\n";
    std::cout << "Options:\n";
    std::cout << "  -u, --url <URL>            LiveKit WebSocket URL (e.g. wss://my-project.livekit.cloud)\n";
    std::cout << "  -t, --token <TOKEN>        LiveKit Room Access Token (JWT)\n";
    std::cout << "  -r, --role <ROLE>          Role: 'math-genius' (server/receiver), 'caller' (client/initiator), or 'auto' (default: auto)\n";
    std::cout << "  -d, --dest <IDENTITY>      Target Participant Identity for RPC calls\n";
    std::cout << "  -m, --method <NAME>        RPC Method Name to call (default: 'greeting.hello')\n";
    std::cout << "  -p, --payload <TEXT>       RPC Method Payload (default: 'LiveKit User')\n";
    std::cout << "  --timeout <SECONDS>        RPC Timeout in seconds (default: 15.0)\n";
    std::cout << "  -h, --help                 Show this help menu\n\n";
    std::cout << "Examples:\n";
    std::cout << "  1. Start RPC Provider / Receiver:\n";
    std::cout << "     " << prog_name << " --url wss://... --token <TOKEN> --role math-genius\n\n";
    std::cout << "  2. Start RPC Caller / Initiator:\n";
    std::cout << "     " << prog_name << " --url wss://... --token <TOKEN> --role caller --dest math-genius --method greeting.hello --payload Alice\n";
}

std::string MaskToken(const std::string& token) {
    if (token.length() <= 12) return "***";
    return token.substr(0, 6) + "..." + token.substr(token.length() - 6);
}

void RegisterDefaultRpcMethods(std::shared_ptr<livekit::LocalParticipant> lp) {
    if (!lp) return;

    // 1. greeting.hello
    lp->registerRpcMethod("greeting.hello", [](const livekit::RpcInvocationData& inv) -> asio::awaitable<std::string> {
        std::cout << "\n[RPC RECEIVER] -> Executing 'greeting.hello' requested by caller='"
                  << inv.caller_identity << "', payload='" << inv.payload << "'" << std::endl;
        co_return "Hello, " + (inv.payload.empty() ? "Guest" : inv.payload) + "!";
    });

    // 2. math.square_root
    lp->registerRpcMethod("math.square_root", [](const livekit::RpcInvocationData& inv) -> asio::awaitable<std::string> {
        std::cout << "\n[RPC RECEIVER] -> Executing 'math.square_root' requested by caller='"
                  << inv.caller_identity << "', payload='" << inv.payload << "'" << std::endl;
        try {
            double val = std::stod(inv.payload);
            if (val < 0) {
                throw livekit::RpcError(livekit::RpcErrorCode::APPLICATION_ERROR,
                                       "Cannot calculate square root of negative number: " + inv.payload);
            }
            double res = std::sqrt(val);
            co_return std::to_string(res);
        } catch (const livekit::RpcError&) {
            throw;
        } catch (const std::exception& e) {
            throw livekit::RpcError(livekit::RpcErrorCode::APPLICATION_ERROR,
                                   std::string("Invalid numeric payload: ") + e.what());
        }
    });

    // 3. echo
    lp->registerRpcMethod("echo", [](const livekit::RpcInvocationData& inv) -> asio::awaitable<std::string> {
        std::cout << "\n[RPC RECEIVER] -> Executing 'echo' requested by caller='"
                  << inv.caller_identity << "'" << std::endl;
        co_return inv.payload;
    });

    std::cout << "[RPC Engine] Registered standard RPC handlers: 'greeting.hello', 'math.square_root', 'echo'." << std::endl;
}

int main(int argc, char* argv[]) {
    std::string url;
    std::string token;
    std::string role = "auto";
    std::string dest_identity;
    std::string method_name = "greeting.hello";
    std::string payload_text = "LiveKit C++ User";
    double timeout_sec = 15.0;

    // 1. 从环境变量读取
    const char* env_url = std::getenv("LIVEKIT_URL");
    const char* env_token = std::getenv("LIVEKIT_TOKEN");
    if (env_url) url = env_url;
    if (env_token) token = env_token;

    // 2. 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
            url = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            token = argv[++i];
        } else if ((arg == "-r" || arg == "--role") && i + 1 < argc) {
            role = argv[++i];
        } else if ((arg == "-d" || arg == "--dest") && i + 1 < argc) {
            dest_identity = argv[++i];
        } else if ((arg == "-m" || arg == "--method") && i + 1 < argc) {
            method_name = argv[++i];
        } else if ((arg == "-p" || arg == "--payload") && i + 1 < argc) {
            payload_text = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout_sec = std::stod(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "==================================================\n";
    std::cout << "        LiveKit C++ Client SDK - SimpleRpc        \n";
    std::cout << "==================================================\n";

    if (url.empty() || token.empty()) {
        std::cout << "[NOTICE] Required arguments --url or --token not provided.\n";
        PrintUsage(argv[0]);
        std::cout << "\n[INFO] Dry-run check completed successfully.\n";
        return 0;
    }

    std::cout << "[Config] LiveKit URL   : " << url << std::endl;
    std::cout << "[Config] Access Token  : " << MaskToken(token) << std::endl;
    std::cout << "[Config] Role Mode     : " << role << std::endl;
    if (!dest_identity.empty()) {
        std::cout << "[Config] Destination   : " << dest_identity << std::endl;
        std::cout << "[Config] RPC Method    : " << method_name << std::endl;
        std::cout << "[Config] RPC Payload   : " << payload_text << std::endl;
        std::cout << "[Config] RPC Timeout   : " << timeout_sec << "s" << std::endl;
    }

    asio::io_context io_ctx;

    // 捕获 Ctrl+C 信号量
    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);

    auto room = livekit::Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<SimpleRpcListener>();
    room->AddListener(listener);

    signals.async_wait([room, &io_ctx](const std::error_code& error, int signal_number) {
        if (!error) {
            std::cout << "\n[SIGNAL] Exit signal (" << signal_number << ") received, closing simple_rpc..." << std::endl;
            room->Disconnect();
            io_ctx.stop();
        }
    });

    livekit::SignalOptions opts;
    opts.auto_subscribe = true;
    opts.single_peer_connection = true;
    opts.connect_timeout = std::chrono::seconds(10);

    asio::co_spawn(io_ctx, [room, url, token, opts, role, dest_identity, method_name, payload_text, timeout_sec]() -> asio::awaitable<void> {
        try {
            std::cout << "[Connect] Connecting to LiveKit Room..." << std::endl;
            bool success = co_await room->Connect(url, token, opts);
            if (success) {
                auto lp = room->local_participant();
                std::cout << "[State] Connected! Identity: " << (lp ? lp->identity() : "N/A")
                          << ", SID: " << (lp ? lp->sid() : "N/A") << std::endl;

                // 注册标准 RPC 处理方法
                if (lp) {
                    RegisterDefaultRpcMethods(lp);
                }

                // 若为 caller 模式或指定了 destination 的 auto 模式，发起 RPC 请求
                if ((role == "caller" || (role == "auto" && !dest_identity.empty())) && lp) {
                    std::string target_dest = dest_identity;
                    if (target_dest.empty()) {
                        // 寻找第一个远程 Participant
                        auto remotes = room->remote_participants();
                        if (!remotes.empty()) {
                            target_dest = remotes.begin()->second->identity();
                        } else {
                            target_dest = lp->identity(); // 自环回逻辑测试
                        }
                    }

                    std::cout << "\n[RPC CALL] Initiating performRpc call to destination='" << target_dest
                              << "', method='" << method_name << "', payload='" << payload_text << "'..." << std::endl;

                    auto start_time = std::chrono::high_resolution_clock::now();
                    try {
                        std::string response = co_await lp->performRpc(target_dest, method_name, payload_text, timeout_sec);
                        auto end_time = std::chrono::high_resolution_clock::now();
                        double rtt_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

                        std::cout << "\n==================================================" << std::endl;
                        std::cout << "[RPC SUCCESS] Response received from [" << target_dest << "]:" << std::endl;
                        std::cout << "  Payload : " << response << std::endl;
                        std::cout << "  Latency : " << rtt_ms << " ms (Round-Trip Time)" << std::endl;
                        std::cout << "==================================================" << std::endl;
                    } catch (const livekit::RpcError& e) {
                        std::cerr << "\n[RPC ERROR] Caught RpcError!" << std::endl;
                        std::cerr << "  Code    : " << static_cast<int>(e.code()) << std::endl;
                        std::cerr << "  Message : " << e.message() << std::endl;
                    }
                }

                std::cout << "\n[Running] SimpleRpc process is active. Press Ctrl+C to exit." << std::endl;
            } else {
                std::cerr << "[ERROR] Room::Connect returned false." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] Connection exception: " << e.what() << std::endl;
        }
    }, asio::detached);

    io_ctx.run();

    std::cout << "[SimpleRpc] Disconnected. Exit completed." << std::endl;
    return 0;
}
