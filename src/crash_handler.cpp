#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace livekit {

PanicCallback CrashHandler::panic_callback_ = nullptr;
std::mutex CrashHandler::mutex_;
bool CrashHandler::handlers_installed_ = false;

void CrashHandler::InstallSignalHandlers() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handlers_installed_) return;

    std::signal(SIGSEGV, CrashHandler::OnSignalReceived);
    std::signal(SIGABRT, CrashHandler::OnSignalReceived);
    std::signal(SIGFPE,  CrashHandler::OnSignalReceived);
    std::signal(SIGILL,  CrashHandler::OnSignalReceived);
    std::signal(SIGTERM, CrashHandler::OnSignalReceived);

    handlers_installed_ = true;
}

void CrashHandler::SetPanicCallback(PanicCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    panic_callback_ = std::move(callback);
}

void CrashHandler::FlushLogs() {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
}

void CrashHandler::TriggerPanic(const std::string& message, bool raise_sigterm) {
    // 类似于 client-sdk-cpp ffi_client.cpp:L260-L265
    std::cerr << "\n==================================================\n"
              << "[CRITICAL PANIC]: " << message << "\n"
              << "==================================================\n"
              << std::endl;

    FlushLogs();

    PanicCallback cb_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb_copy = panic_callback_;
    }

    if (cb_copy) {
        try {
            cb_copy(message);
        } catch (...) {
            // 防止回调再次崩溃
        }
    }

    if (raise_sigterm) {
        FlushLogs();
        std::raise(SIGTERM);
    }
}

void CrashHandler::OnSignalReceived(int signal) {
    const char* sig_name = "UNKNOWN";
    switch (signal) {
        case SIGSEGV: sig_name = "SIGSEGV (Segmentation Fault)"; break;
        case SIGABRT: sig_name = "SIGABRT (Abort)"; break;
        case SIGFPE:  sig_name = "SIGFPE (Arithmetic Exception)"; break;
        case SIGILL:  sig_name = "SIGILL (Illegal Instruction)"; break;
        case SIGTERM: sig_name = "SIGTERM (Termination Request)"; break;
    }

    std::cerr << "\n==================================================\n"
              << "[FATAL SIGNAL]: Caught OS Signal " << signal << " - " << sig_name << "\n"
              << "Flushing logs before exit...\n"
              << "==================================================\n"
              << std::endl;

    FlushLogs();

    // 重新恢复默认 handler 并 raise 退出
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

} // namespace livekit
