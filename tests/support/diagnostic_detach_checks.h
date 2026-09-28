#pragma once
#include "src/telemetry/diagnostic_pipeline.h"
#include "tests/support/test_check.h"
#include <windows.h>
#include <winioctl.h>
#include <fstream>

namespace livekit::diagnostic {
struct DiagnosticPipelineTestAccess {
    static std::weak_ptr<void> Context(DiagnosticPipeline& p) { return p.context_; }
    static HANDLE Thread(DiagnosticPipeline& p) {
        HANDLE copy = nullptr;
        TEST_CHECK(DuplicateHandle(GetCurrentProcess(), p.writer_.native_handle(),
            GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0));
        return copy;
    }
};
}
namespace diagnostic_detach_checks {
using namespace livekit::diagnostic;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
inline void Closed(DiagnosticPipeline& p) {
    const auto start = Clock::now();
    TEST_CHECK(p.Close() == DrainResult::TimedOut);
    const auto elapsed = Clock::now() - start;
    TEST_CHECK(elapsed >= 4900ms && elapsed < 7s);
    TEST_CHECK(!p.GetStatus().accepting && !p.GetStatus().sink_available);
    const auto again = Clock::now();
    TEST_CHECK(p.Close() == DrainResult::TimedOut);
    TEST_CHECK(!p.TryEmit(Event::Received(ChatKind::Text, 1)));
    TEST_CHECK(!p.StartWriter("unused"));
    TEST_CHECK(Clock::now() - again < 1s);
    std::cout << "detach_close_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}
inline void Joined(HANDLE thread, std::weak_ptr<void> context) {
    TEST_CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    TEST_CHECK(context.expired());
}
inline void BlockedMirror(const std::filesystem::path& root) {
    struct State { std::mutex mutex; std::condition_variable cv; bool entered=false, release=false; std::atomic<unsigned> calls{0}; };
    auto state = std::make_shared<State>();
    auto p = std::make_unique<DiagnosticPipeline>();
    auto context = DiagnosticPipelineTestAccess::Context(*p);
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> callback_lifetime = lifetime;
    p->SetMirror([state, lifetime](const Event&) {
        ++state->calls;
        std::unique_lock lock(state->mutex);
        state->entered=true; state->cv.notify_all();
        state->cv.wait(lock, [&] { return state->release; });
    });
    lifetime.reset();
    TEST_CHECK(p->TryEmit(Event::Received(ChatKind::Text, 1)));
    TEST_CHECK(p->TryEmit(Event::Received(ChatKind::Text, 2)));
    TEST_CHECK(p->StartWriter(root));
    auto thread = DiagnosticPipelineTestAccess::Thread(*p);
    { std::unique_lock lock(state->mutex); TEST_CHECK(state->cv.wait_for(lock, 2s, [&] { return state->entered; })); }
    Closed(*p);
    p->SetMirror({});
    const auto start = Clock::now(); p.reset(); TEST_CHECK(Clock::now()-start < 1s);
    TEST_CHECK(!context.expired());
    TEST_CHECK(!callback_lifetime.expired());
    { std::lock_guard lock(state->mutex); state->release=true; } state->cv.notify_all();
    Joined(thread, context);
    TEST_CHECK(callback_lifetime.expired());
    TEST_CHECK(state->calls == 1); // Copied batch must not dispatch late callbacks.
    TEST_CHECK(!std::filesystem::exists(root)); // No terminal or deferred batch write.
}
inline void BlockedFileOpen(const std::filesystem::path& root) {
    auto p = std::make_unique<DiagnosticPipeline>();
    auto context = DiagnosticPipelineTestAccess::Context(*p);
    auto run = root / ("run-" + std::string(p->run_id()));
    std::filesystem::create_directories(run);
    auto path = run / "segment-000000.jsonl";
    { std::ofstream out(path); }
    HANDLE holder = CreateFileW(path.c_str(), GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    TEST_CHECK(holder != INVALID_HANDLE_VALUE);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TEST_CHECK(event != nullptr);
    OVERLAPPED ov{}; ov.hEvent=event; DWORD bytes=0;
    TEST_CHECK(!DeviceIoControl(holder, FSCTL_REQUEST_OPLOCK_LEVEL_1,
        nullptr, 0, nullptr, 0, &bytes, &ov) && GetLastError()==ERROR_IO_PENDING);
    TEST_CHECK(p->TryEmit(Event::Received(ChatKind::Text, 1)));
    TEST_CHECK(p->StartWriter(root));
    HANDLE thread=DiagnosticPipelineTestAccess::Thread(*p);
    TEST_CHECK(WaitForSingleObject(event, 3000)==WAIT_OBJECT_0);
    TEST_CHECK(WaitForSingleObject(thread, 0)==WAIT_TIMEOUT);
    Closed(*p);
    TEST_CHECK(WaitForSingleObject(thread, 0)==WAIT_TIMEOUT); // Still in real file I/O.
    const auto start=Clock::now(); p.reset(); TEST_CHECK(Clock::now()-start < 1s);
    TEST_CHECK(!context.expired());
    CloseHandle(holder); CloseHandle(event); // Release AFTER destroying Pipeline.
    Joined(thread, context);
    TEST_CHECK(std::filesystem::file_size(path) == 0);
    std::ifstream input(path); std::string line;
    while(std::getline(input,line)) TEST_CHECK(line.find("process.terminal")==std::string::npos);
    input.close(); // Do not let the assertion reader itself deny deletion.
    const auto cleared = DiagnosticFileSink::ClearInactiveHistory(root);
    TEST_CHECK(cleared.success && cleared.active_runs_skipped == 0); // Lease released.
}
}
