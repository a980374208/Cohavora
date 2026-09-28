#pragma once

#include "src/telemetry/diagnostic_file_sink.h"
#include "tests/support/test_check.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <thread>

namespace livekit::diagnostic {
// Native I/O injection is private to friend tests; no environment fault switches.
struct DiagnosticFileSinkTestAccess {
    static inline thread_local unsigned hits = 0;
    static inline thread_local bool partial = true;
    static bool FailedSize(void*, std::uint64_t&) noexcept { ++hits; return false; }
    static bool FailedWrite(void* h, const char* p, std::uint32_t n, std::uint32_t& written) noexcept {
        ++hits;
        DiagnosticFileSink::NativeWrite(h, p, partial ? (std::min)(n, 17U) : n, written);
        return false;
    }
    static bool ShortWrite(void* h, const char* p, std::uint32_t n, std::uint32_t& written) noexcept {
        ++hits;
        return DiagnosticFileSink::NativeWrite(h, p, (std::min)(n, 17U), written);
    }
    static void SizeFailure(DiagnosticFileSink& s, bool enable) {
        s.read_size_ = enable ? &FailedSize : &DiagnosticFileSink::NativeSize;
    }
    static void WriteFailure(DiagnosticFileSink& s, bool enable, bool short_write) {
        partial = short_write;
        s.write_ = enable ? &FailedWrite : &DiagnosticFileSink::NativeWrite;
    }
    static bool Lock(DiagnosticFileSink& s) { return s.LockQuota(); }
    static void Unlock(DiagnosticFileSink& s) { s.UnlockQuota(); }
    static void ShortWrites(DiagnosticFileSink& s) { s.write_ = &ShortWrite; }
};
}

namespace diagnostic_sink_checks {
using namespace livekit::diagnostic;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr auto MiB = std::uint64_t{1024 * 1024};

inline std::uint64_t Size(const std::filesystem::path& path) {
    const auto h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    const auto ok = GetFileSizeEx(h, &size);
    CloseHandle(h);
    TEST_CHECK(ok && size.QuadPart >= 0);
    return static_cast<std::uint64_t>(size.QuadPart);
}

inline std::uint64_t Total(const std::filesystem::path& root) {
    std::uint64_t total = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        if (e.path().extension() == ".jsonl") total += Size(e.path());
    return total;
}

inline void Segment(const std::filesystem::path& path, std::uint64_t bytes) {
    std::filesystem::create_directories(path.parent_path());
    { std::ofstream out(path, std::ios::binary); TEST_CHECK(out.good()); }
    std::filesystem::resize_file(path, bytes);
}

inline void History(const std::filesystem::path& root, std::uint64_t bytes) {
    const auto run = root / ("run-" + std::string(32, 'a'));
    for (int i = 0; bytes; ++i) {
        char name[32]{}; std::snprintf(name, sizeof(name), "segment-%06d.jsonl", i);
        const auto n = (std::min)(bytes, 10 * MiB);
        Segment(run / name, n);
        bytes -= n;
    }
}

inline void SizeFailureClosesAndRetries(const std::filesystem::path& root) {
    const auto segment = root / ("run-" + std::string(32, 'b')) / "segment-000000.jsonl";
    Segment(segment, 1024);
    DiagnosticFileSink sink(root, std::string(32, 'b'));
    auto event = Event::Received(ChatKind::Text, 1); event.event_sequence = 1;
    // A real Windows sharing violation must leave the sink retryable.
    const auto blocker = CreateFileW(segment.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(blocker != INVALID_HANDLE_VALUE);
    TEST_CHECK(!sink.Write(event));
    TEST_CHECK(sink.failure_reason() == FailureReason::OpenFailed);
    CloseHandle(blocker);
    DiagnosticFileSinkTestAccess::hits = 0;
    DiagnosticFileSinkTestAccess::SizeFailure(sink, true);
    for (int retry = 0; retry < 2; ++retry) {
        TEST_CHECK(!sink.Write(event));
        TEST_CHECK(sink.failure_reason() == FailureReason::DirectoryUnavailable);
        TEST_CHECK(sink.last_committed_sequence() == 0);
    }
    TEST_CHECK(DiagnosticFileSinkTestAccess::hits >= 2);
    DiagnosticFileSinkTestAccess::SizeFailure(sink, false);
    TEST_CHECK(Size(segment) == 1024); // Neither failed attempt appended anything.
    TEST_CHECK(sink.Write(event));
    TEST_CHECK(sink.last_committed_sequence() == 1);
    TEST_CHECK(Size(segment) > 1024);
}

inline void SlowReclaimSeesLiveGrowth(const std::filesystem::path& root) {
    const auto active = root / ("run-" + std::string(32, 'c'));
    std::filesystem::create_directories(active);
    const auto lease = CreateFileW((active / "active.lock").c_str(), GENERIC_READ,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(lease != INVALID_HANDLE_VALUE);
    const auto growing = active / "segment-000000.jsonl";
    Segment(growing, 16);
    const auto file = CreateFileW(growing.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    // Retain the writer handle while changing length: directory enumeration
    // metadata may remain stale, but both scan paths must observe the handle size.
    LARGE_INTEGER size{}; size.QuadPart = 100 * MiB;
    TEST_CHECK(SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file));
    const auto expired = root / ("run-" + std::string(32, 'a')) / "segment-000000.jsonl";
    Segment(expired, 32);
    std::filesystem::last_write_time(expired,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    DiagnosticFileSink sink(root, std::string(32, 'd'));
    auto event = Event::Received(ChatKind::Text, 1);
    TEST_CHECK(!sink.Write(event));
    TEST_CHECK(sink.failure_reason() == FailureReason::QuotaExceeded);
    TEST_CHECK(!std::filesystem::exists(expired)); // Full reclaim path executed.
    TEST_CHECK(Size(growing) == 100 * MiB); // Active run was not reclaimed.
    size.QuadPart = 99 * MiB;
    TEST_CHECK(SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file));
    TEST_CHECK(sink.Write(event));
    CloseHandle(file); CloseHandle(lease);
}

inline void WriteFaultPreservesResidue(const std::filesystem::path& root, bool partial) {
    DiagnosticFileSink sink(root, std::string(32, 'e'));
    auto event = Event::Received(ChatKind::Text, 1); event.event_sequence = 1;
    TEST_CHECK(sink.Write(event));
    const auto first = sink.run_directory() / "segment-000000.jsonl";
    const auto before = Size(first);
    DiagnosticFileSinkTestAccess::hits = 0;
    DiagnosticFileSinkTestAccess::WriteFailure(sink, true, partial);
    event.event_sequence = 2;
    TEST_CHECK(!sink.Write(event));
    TEST_CHECK(DiagnosticFileSinkTestAccess::hits > 0);
    TEST_CHECK(sink.failure_reason() == FailureReason::WriteFailed);
    TEST_CHECK(sink.last_committed_sequence() == 1);
    DiagnosticFileSinkTestAccess::WriteFailure(sink, false, partial);
    const auto residue = Size(first);
    TEST_CHECK(residue > before);
    if (partial) TEST_CHECK(residue == before + 17);
    // Force recovery to inspect retained bytes; if it reused the old admission,
    // this deliberately expired external history would incorrectly survive.
    const auto expired = root / ("run-" + std::string(32, 'a')) / "segment-000000.jsonl";
    Segment(expired, 32);
    std::filesystem::last_write_time(expired,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    event.event_sequence = 3;
    TEST_CHECK(sink.Write(event));
    TEST_CHECK(sink.last_committed_sequence() == 3);
    TEST_CHECK(!std::filesystem::exists(expired));
    TEST_CHECK(Size(first) == residue);
    TEST_CHECK(Size(sink.run_directory() / "segment-000001.jsonl") > 0);
}

// A real child process owns each writer/run lease. JSON commands permit the
// parent to pause writers at exact boundaries and inspect actual disk lengths.
inline int ChildMain(const std::filesystem::path& root, const std::string& run) {
    DiagnosticFileSink sink(root, run);
    std::uint64_t sequence = 0;
    std::cout << "ready\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") return 0;
        const auto request = Json::parse(line);
        const int count = request.at("count");
        int written = 0;
        for (int offset = 0; offset < count;) {
            std::vector<Event> batch;
            const auto n = (std::min)(64, count - offset);
            for (int i = 0; i < n; ++i) {
                auto event = Event::Received(ChatKind::Text, 1);
                event.event_sequence = ++sequence; batch.push_back(event);
            }
            const auto committed = sink.WriteBatch(batch); written += static_cast<int>(committed);
            if (committed != batch.size()) break;
            offset += n;
        }
        int segments = 0, last_segment = -1; std::uint64_t bytes = 0;
        if (std::filesystem::exists(sink.run_directory()))
            for (const auto& e : std::filesystem::directory_iterator(sink.run_directory()))
                if (e.path().extension() == ".jsonl") {
                    ++segments; bytes += Size(e.path());
                    last_segment = (std::max)(last_segment,
                        std::stoi(e.path().stem().string().substr(8)));
                }
        std::cout << Json{{"written", written}, {"segments", segments}, {"bytes", bytes},
            {"last_segment", last_segment}, {"committed", sink.last_committed_sequence()}}.dump() << '\n' << std::flush;
    }
    return 0;
}

class Child final {
public:
    Child(const std::filesystem::path& root, char run) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE input = nullptr, output = nullptr;
        TEST_CHECK(CreatePipe(&input, &write_, &sa, 0));
        TEST_CHECK(CreatePipe(&read_, &output, &sa, 0));
        TEST_CHECK(SetHandleInformation(write_, HANDLE_FLAG_INHERIT, 0));
        TEST_CHECK(SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0));
        wchar_t exe[32768]{}; TEST_CHECK(GetModuleFileNameW(nullptr, exe, 32768));
        auto command = L"\"" + std::wstring(exe) + L"\" --quota-child \"" + root.wstring() + L"\" " + std::wstring(32, run);
        STARTUPINFOW start{}; start.cb = sizeof(start); start.dwFlags = STARTF_USESTDHANDLES;
        start.hStdInput = input; start.hStdOutput = output; start.hStdError = output;
        PROCESS_INFORMATION process{};
        TEST_CHECK(CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &start, &process));
        process_ = process.hProcess; CloseHandle(process.hThread); CloseHandle(input); CloseHandle(output);
        TEST_CHECK(Read() == "ready");
    }
    ~Child() {
        if (process_) {
            Send("quit");
            TEST_CHECK(WaitForSingleObject(process_, 5000) == WAIT_OBJECT_0);
            DWORD code = 0; TEST_CHECK(GetExitCodeProcess(process_, &code) && code == 0);
            CloseHandle(process_);
        }
        CloseHandle(read_); CloseHandle(write_);
    }
    void SendCount(int count) { Send(Json{{"count", count}}.dump()); }
    Json Reply() { return Json::parse(Read()); }
    Json Write(int count) { SendCount(count); auto r = Reply(); TEST_CHECK(r["written"] == count); return r; }
    void Kill() {
        TEST_CHECK(TerminateProcess(process_, 73));
        TEST_CHECK(WaitForSingleObject(process_, 5000) == WAIT_OBJECT_0);
        CloseHandle(process_); process_ = nullptr;
    }
private:
    void Send(std::string text) {
        text += '\n'; DWORD written = 0;
        TEST_CHECK(WriteFile(write_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
        TEST_CHECK(written == text.size());
    }
    std::string Read() {
        std::string result; const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < deadline) {
            DWORD available = 0; TEST_CHECK(PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr));
            if (available) {
                char ch; DWORD got = 0; TEST_CHECK(ReadFile(read_, &ch, 1, &got, nullptr) && got == 1);
                if (ch == '\n') return result;
                if (ch != '\r') result += ch;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_CHECK(false); return {};
    }
    HANDLE process_ = nullptr, read_ = nullptr, write_ = nullptr;
};

inline void MultipleWritersAndCrash(const std::filesystem::path& root) {
    History(root, 99 * MiB);
    Child a(root, 'b'), b(root, 'c');
    TEST_CHECK(a.Write(1)["segments"] == 1);
    TEST_CHECK(b.Write(1)["segments"] == 1);
    // Both admitted under 100MiB, then append within their existing segments.
    a.SendCount(24000); b.SendCount(24000);
    auto ar = a.Reply(), br = b.Reply();
    TEST_CHECK(ar["written"] == 24000 && br["written"] == 24000);
    TEST_CHECK(ar["segments"] == 1 && br["segments"] == 1);
    const auto peak = Total(root); TEST_CHECK(peak > 110 * MiB && peak < 120 * MiB);
    a.SendCount(16000); b.SendCount(16000);
    ar = a.Reply(); br = b.Reply();
    TEST_CHECK(ar["written"] == 16000 && br["written"] == 16000);
    TEST_CHECK(ar["last_segment"] == 1 && br["last_segment"] == 1);
    const auto after_concurrent_rotation = Total(root);
    TEST_CHECK(after_concurrent_rotation < peak); // Closed history was reclaimed.
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        if (e.path().extension() == ".jsonl") TEST_CHECK(Size(e.path()) <= 10 * MiB);
    const auto clear = DiagnosticFileSink::ClearInactiveHistory(root);
    TEST_CHECK(clear.success && clear.active_runs_skipped == 2);
    TEST_CHECK(Total(root) == ar["bytes"].get<std::uint64_t>() + br["bytes"].get<std::uint64_t>());
    a.Kill(); // Kernel releases its lease; the other process remains alive.
    const auto after_crash = DiagnosticFileSink::ClearInactiveHistory(root);
    TEST_CHECK(after_crash.success && after_crash.active_runs_skipped == 1);
    TEST_CHECK(!std::filesystem::exists(root / ("run-" + std::string(32, 'b')) / "segment-000000.jsonl"));
    br = b.Write(1); TEST_CHECK(br["committed"] == 40002);
    // Restore closed history so the surviving writer must reclaim at rotation.
    History(root, 90 * MiB);
    const auto previous_segment = br["last_segment"].get<int>();
    while (br["last_segment"] == previous_segment) br = b.Write(64);
    TEST_CHECK(br["last_segment"] == previous_segment + 1 && Total(root) <= 100 * MiB);
    std::cout << "quota_multiprocess_peak_bytes=" << peak
              << " concurrent_rotation_bytes=" << after_concurrent_rotation
              << " after_rotation_bytes=" << Total(root) << '\n';
}

inline void TerminationDuringAppend(const std::filesystem::path& root) {
    History(root, 99 * MiB);
    Child writer(root, 'b');
    writer.SendCount(2000000); // Deliberately leave a large request in flight.
    const auto first = root / ("run-" + std::string(32, 'b')) / "segment-000000.jsonl";
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool started = false;
    while (Clock::now() < deadline) {
        if (std::filesystem::exists(first) && Size(first) > 0) { started = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(started);
    writer.Kill();
    const auto after_kill = Total(root);
    Child recovered(root, 'c');
    TEST_CHECK(recovered.Write(1)["committed"] == 1);
    TEST_CHECK(Total(root) <= 100 * MiB);
    std::cout << "quota_killed_writer_bytes=" << after_kill
              << " recovery_bytes=" << Total(root) << '\n';
}

inline void AbandonBeforeQuota(const std::filesystem::path& root) {
    const auto expired = root / ("run-" + std::string(32, 'a')) / "segment-000000.jsonl";
    Segment(expired, 32);
    std::filesystem::last_write_time(expired,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    DiagnosticFileSink gate(root, std::string(32, 'c'));
    TEST_CHECK(DiagnosticFileSinkTestAccess::Lock(gate));
    std::atomic<bool> abandoned{false};
    DiagnosticFileSink sink(root, std::string(32, 'b'), &abandoned);
    bool written = true;
    auto event = Event::Received(ChatKind::Text, 1); event.event_sequence = 1;
    std::thread worker([&] { written = sink.Write(event); });
    const auto path = sink.run_directory() / "segment-000000.jsonl";
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(path) && Clock::now() < deadline) std::this_thread::yield();
    const bool opened = std::filesystem::exists(path);
    abandoned.store(true, std::memory_order_release);
    DiagnosticFileSinkTestAccess::Unlock(gate);
    worker.join();
    TEST_CHECK(opened && !written && sink.last_committed_sequence() == 0);
    TEST_CHECK(Size(path) == 0 && std::filesystem::exists(expired));
}

inline void SameSegmentDoesNotWaitForRoot(const std::filesystem::path& root) {
    DiagnosticFileSink sink(root, std::string(32, 'b'));
    auto event = Event::Received(ChatKind::Text, 1); event.event_sequence = 1;
    TEST_CHECK(sink.Write(event));
    DiagnosticFileSink gate(root, std::string(32, 'c'));
    TEST_CHECK(DiagnosticFileSinkTestAccess::Lock(gate));
    std::atomic<bool> done{false}; bool written = false;
    std::thread worker([&] { event.event_sequence = 2; written = sink.Write(event); done = true; });
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (!done && Clock::now() < deadline) std::this_thread::yield();
    const bool completed_while_locked = done;
    DiagnosticFileSinkTestAccess::Unlock(gate);
    worker.join();
    TEST_CHECK(completed_while_locked && written && sink.last_committed_sequence() == 2);
}

inline void Run(const std::filesystem::path& root) {
    AbandonBeforeQuota(root / "abandon-quota");
    SameSegmentDoesNotWaitForRoot(root / "no-root-wait");
    SizeFailureClosesAndRetries(root / "size-denied");
    SlowReclaimSeesLiveGrowth(root / "live-growth");
    WriteFaultPreservesResidue(root / "partial-write", true);
    WriteFaultPreservesResidue(root / "write-error-after-residue", false);
    {
        DiagnosticFileSink sink(root / "short-writes", std::string(32, 'b'));
        DiagnosticFileSinkTestAccess::hits = 0;
        DiagnosticFileSinkTestAccess::ShortWrites(sink);
        auto event = Event::Received(ChatKind::Text, 1); event.event_sequence = 1;
        TEST_CHECK(sink.Write(event));
        TEST_CHECK(DiagnosticFileSinkTestAccess::hits > 1 && sink.last_committed_sequence() == 1);
    }
    MultipleWritersAndCrash(root / "multiprocess");
    TerminationDuringAppend(root / "kill-during-write");
}
}
