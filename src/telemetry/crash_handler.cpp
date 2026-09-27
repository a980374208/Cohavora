#include "crash_handler.h"
#include "diagnostic_pipeline.h"
#include "log_redaction.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace livekit {

PanicCallback CrashHandler::panic_callback_ = nullptr;
std::mutex CrashHandler::mutex_;
bool CrashHandler::handlers_installed_ = false;

void CrashHandler::InstallSignalHandlers() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handlers_installed_) return;
    // Fatal process exceptions are owned by CrashEvidenceProvider, installed
    // before QApplication. A room must not replace its abort/SEH handlers.
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
    (void)message;
    const std::string safe_message = secure_log::OpaqueSummary("panic");
    diagnostic::EmitBusinessEvent(
        diagnostic::Event::Issue(diagnostic::IssueCode::PanicTriggered));

    FlushLogs();

    PanicCallback cb_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb_copy = panic_callback_;
    }

    if (cb_copy) {
        try {
            cb_copy(safe_message);
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
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

} // namespace livekit
