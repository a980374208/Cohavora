#pragma once

#include <string>
#include <functional>
#include <csignal>
#include <iostream>
#include <mutex>

namespace livekit {

using PanicCallback = std::function<void(const std::string& message)>;

class CrashHandler {
public:
    // 安装 OS 级别信号处理程序 (SIGSEGV, SIGABRT, SIGTERM 等)
    static void InstallSignalHandlers();

    // 触发 Panic（类似 client-sdk-cpp 收到 FFI Panic）
    // 强制 Flush 所有日志并发出 SIGTERM / 优雅退出
    static void TriggerPanic(const std::string& message, bool raise_sigterm = true);

    // 设置全局 Panic 发生时的紧急通知回调
    static void SetPanicCallback(PanicCallback callback);

    // 强行 Flush 日志缓冲区
    static void FlushLogs();

private:
    static void OnSignalReceived(int signal);

private:
    static PanicCallback panic_callback_;
    static std::mutex mutex_;
    static bool handlers_installed_;
};

} // namespace livekit
