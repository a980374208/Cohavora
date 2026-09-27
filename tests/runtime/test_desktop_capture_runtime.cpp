// Opt-in real Windows window capture. Only captures this test's controlled pattern;
// no desktop pixels, images, window lists or source titles are persisted.
#include "desktop_capture.h"
#include "screen_capture_fallback_test.h"
#include "wgc_capability_probe.h"
#include "wgc_window_capture.h"
#include "modules/desktop_capture/win/screen_capturer_win_directx.h"
#include "tests/support/test_check.h"
#include "modules/desktop_capture/win/wgc_capturer_win.h"
#include "modules/desktop_capture/desktop_capture_types.h"
#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#include "modules/desktop_capture/desktop_frame.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace {
struct Resources { SIZE_T private_bytes; DWORD handles, threads; };
Resources SampleMemory(int cycle, const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    TEST_CHECK(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0);
    DWORD handles = 0;
    TEST_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles) != 0);
    DWORD threads = 0;
    std::vector<DWORD> thread_ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    TEST_CHECK(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                ++threads;
                thread_ids.push_back(entry.th32ThreadID);
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(thread_ids.begin(), thread_ids.end());
    std::cout << "[CAPTURE_LIFECYCLE] {\"cycle\":" << cycle
              << ",\"pid\":" << GetCurrentProcessId()
              << ",\"epoch_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()
              << ",\"phase\":\"" << phase << "\""
              << ",\"private_bytes\":" << memory.PrivateUsage
              << ",\"handles\":" << handles
              << ",\"threads\":" << threads
              << ",\"gdi_objects\":" << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
              << ",\"user_objects\":" << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)
              << ",\"thread_ids\":[";
    for (std::size_t i = 0; i < thread_ids.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << thread_ids[i];
    }
    std::cout << "]}" << std::endl;
    return {memory.PrivateUsage, handles, threads};
}

const char* BackendName(std::uint32_t id) {
    using namespace webrtc::DesktopCapturerId;
    if (id == kWgcCapturerWin) return "wgc";
    if (id == kWindowCapturerWinGdi) return "window_gdi";
    if (id == kScreenCapturerWinGdi) return "screen_gdi";
    if (id == kScreenCapturerWinDirectx) return "screen_dxgi";
    return "unknown";
}

const char* PhaseName(livekit::DesktopCaptureProbePhase phase) {
    using Phase = livekit::DesktopCaptureProbePhase;
    switch (phase) {
    case Phase::Created: return "created";
    case Phase::Started: return "started";
    case Phase::BackendFrame: return "backend_frame";
    case Phase::Destroyed: return "destroyed";
    case Phase::Joined: return "joined";
    case Phase::SessionClosed: return "session_closed";
    case Phase::FramePoolClosed: return "frame_pool_closed";
    case Phase::D3dReleased: return "d3d_released";
    case Phase::CloseFailed: return "close_failed";
    }
    return "unknown";
}

std::uint64_t ProcessCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    TEST_CHECK(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0);
    const auto ticks = [](FILETIME value) {
        ULARGE_INTEGER count{};
        count.LowPart = value.dwLowDateTime;
        count.HighPart = value.dwHighDateTime;
        return count.QuadPart;
    };
    return ticks(kernel) + ticks(user);
}

LRESULT CALLBACK PatternWindow(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP) {
        if (wparam) KillTimer(hwnd, 1);
        else SetTimer(hwnd, 1, 500, nullptr);
        return 0;
    }
    if (message == WM_TIMER) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, !GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        InvalidateRect(hwnd, nullptr, false);
        return 0;
    }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const bool inverted = GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0;
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? BLACK_BRUSH : WHITE_BRUSH)));
        rect.right /= 2;
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? WHITE_BRUSH : BLACK_BRUSH)));
        if (message == WM_PAINT) EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
void RunPatternWindow(std::promise<HWND>& ready) {
    WNDCLASSW type{};
    type.lpfnWndProc = PatternWindow;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"LiveKitCaptureRegression";
    RegisterClassW(&type);
    HWND hwnd = CreateWindowExW(0, type.lpszClassName, L"LiveKit capture regression",
        WS_OVERLAPPEDWINDOW, 80, 80, 800, 600, nullptr, nullptr, type.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 500, nullptr);
    UpdateWindow(hwnd);
    ready.set_value(hwnd);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::wstring TargetName(DWORD parent, const wchar_t* suffix) {
    return L"Local\\LiveKitCaptureTarget-" + std::to_wstring(parent) + suffix;
}

int RunPatternTarget(DWORD parent_pid) {
    const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, TargetName(parent_pid, L"-map").c_str());
    const HANDLE ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, TargetName(parent_pid, L"-ready").c_str());
    const HANDLE exit_event = OpenEventW(SYNCHRONIZE, FALSE, TargetName(parent_pid, L"-exit").c_str());
    TEST_CHECK(parent && mapping && ready_event && exit_event);
    auto shared = static_cast<HWND*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(HWND)));
    TEST_CHECK(shared);
    std::promise<HWND> ready;
    auto future = ready.get_future();
    std::thread ui([&] { RunPatternWindow(ready); });
    HWND hwnd = future.get();
    TEST_CHECK(hwnd);
    *shared = hwnd;
    TEST_CHECK(SetEvent(ready_event));
    const HANDLE waits[] = {parent, exit_event};
    // Parent failure cannot leave an orphan target window/process.
    WaitForMultipleObjects(2, waits, FALSE, 300000);
    if (IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ui.join();
    UnmapViewOfFile(shared);
    for (HANDLE handle : {parent, mapping, ready_event, exit_event}) CloseHandle(handle);
    return 0;
}

struct ExternalPatternTarget {
    HANDLE mapping = nullptr, ready = nullptr, exit = nullptr, process = nullptr;
    HWND* shared = nullptr;
    HWND window = nullptr;
    DWORD pid = 0;
    void Start() {
        const DWORD parent = GetCurrentProcessId();
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(HWND), TargetName(parent, L"-map").c_str());
        ready = CreateEventW(nullptr, TRUE, FALSE, TargetName(parent, L"-ready").c_str());
        exit = CreateEventW(nullptr, TRUE, FALSE, TargetName(parent, L"-exit").c_str());
        TEST_CHECK(mapping && ready && exit);
        shared = static_cast<HWND*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HWND)));
        TEST_CHECK(shared);
        wchar_t executable[32768]{};
        TEST_CHECK(GetModuleFileNameW(nullptr, executable, 32768));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --pattern-target " + std::to_wstring(parent);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        TEST_CHECK(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info));
        process = info.hProcess;
        pid = info.dwProcessId;
        CloseHandle(info.hThread);
        const HANDLE waits[] = {ready, process};
        TEST_CHECK(WaitForMultipleObjects(2, waits, FALSE, 10000) == WAIT_OBJECT_0);
        window = *shared;
        DWORD owner = 0;
        TEST_CHECK(GetWindowThreadProcessId(window, &owner) && owner == pid && owner != parent);
        std::cout << "[CAPTURE_TARGET] {\"capture_pid\":" << parent
                  << ",\"target_pid\":" << pid << ",\"external\":true}" << std::endl;
    }
    void CloseWindow() {
        TEST_CHECK(PostMessageW(window, WM_CLOSE, 0, 0));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (IsWindow(window) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        TEST_CHECK(!IsWindow(window));
        TEST_CHECK(WaitForSingleObject(process, 0) == WAIT_TIMEOUT);
    }
    void Exit() {
        TEST_CHECK(SetEvent(exit));
        TEST_CHECK(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
        DWORD code = 1;
        TEST_CHECK(GetExitCodeProcess(process, &code) && code == 0);
    }
    ~ExternalPatternTarget() {
        if (exit) SetEvent(exit);
        if (process) WaitForSingleObject(process, 10000);
        if (shared) UnmapViewOfFile(shared);
        for (HANDLE handle : {mapping, ready, exit, process}) if (handle) CloseHandle(handle);
    }
};

// Isolation uses the same fresh MTA worker and owned capturer as production,
// without changing production feature detection or resource ownership.
int RunWgcIsolation(const std::string& mode) {
    const bool query = mode == "query";
    const bool session = mode == "session";
    TEST_CHECK(query || session || mode == "apartment");
    ExternalPatternTarget target;
    target.Start();
    constexpr int cycles = 24;
    SampleMemory(0, "baseline");
    Resources warm{};
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        std::thread worker([&] {
            const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
            TEST_CHECK(SUCCEEDED(initialized));
            bool supported = false;
            int frames = 0, closed = 0, pools = 0, devices = 0, close_failed = 0;
            bool pattern = false;
            if (query) supported = webrtc::IsWgcSupported(webrtc::CaptureType::kWindow);
            if (session) {
                struct Sink final : webrtc::DesktopCapturer::Callback {
                    int& frames;
                    bool& pattern;
                    Sink(int& count, bool& valid) : frames(count), pattern(valid) {}
                    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                                         std::unique_ptr<webrtc::DesktopFrame> frame) override {
                        TEST_CHECK(result != webrtc::DesktopCapturer::Result::ERROR_PERMANENT);
                        if (!frame) return;
                        TEST_CHECK(frame->capturer_id() == webrtc::DesktopCapturerId::kWgcCapturerWin);
                        ++frames;
                        if (frame->size().width() > 400 && frame->size().height() > 300) {
                            const auto* row = frame->data() + frame->stride() * (frame->size().height() / 2);
                            const int left = row[4 * (frame->size().width() / 4)];
                            const int right = row[4 * (3 * frame->size().width() / 4)];
                            pattern = pattern || (left < 60 && right > 190) || (right < 60 && left > 190);
                        }
                    }
                } sink(frames, pattern);
                auto capturer = livekit::CreateOwnedWgcWindowCapturer([&](auto phase) {
                    using Phase = livekit::DesktopCaptureProbePhase;
                    if (phase == Phase::SessionClosed) ++closed;
                    if (phase == Phase::FramePoolClosed) ++pools;
                    if (phase == Phase::D3dReleased) ++devices;
                    if (phase == Phase::CloseFailed) ++close_failed;
                });
                TEST_CHECK(capturer->SelectSource(reinterpret_cast<intptr_t>(target.window)));
                capturer->Start(&sink);
                capturer->SetMaxFrameRate(15);
                const auto begin = std::chrono::steady_clock::now();
                do {
                    capturer->CaptureFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(66));
                } while (std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
                capturer.reset();
                TEST_CHECK(frames >= 3 && pattern && closed == 1 && pools == 1 && devices == 1 && close_failed == 0);
            }
            RoUninitialize();
            TEST_CHECK(!query || supported);
            std::cout << "[WGC_ISOLATION] {\"mode\":\"" << mode << "\",\"cycle\":" << cycle
                      << ",\"supported\":" << (supported ? "true" : "false")
                      << ",\"frames\":" << frames << ",\"pattern\":" << (pattern ? "true" : "false")
                      << ",\"session_closed\":" << closed << ",\"pool_closed\":" << pools
                      << ",\"d3d_released\":" << devices << "}" << std::endl;
        });
        worker.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto stopped = SampleMemory(cycle, "stopped");
        if (cycle == 1) warm = stopped;
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const auto settled = SampleMemory(cycles, "settled_window_open");
    target.CloseWindow();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    SampleMemory(cycles, "target_window_closed");
    target.Exit();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    SampleMemory(cycles, "target_process_exited");
    const auto bytes = static_cast<int64_t>(settled.private_bytes) - static_cast<int64_t>(warm.private_bytes);
    const auto handles = static_cast<int64_t>(settled.handles) - warm.handles;
    const auto threads = static_cast<int64_t>(settled.threads) - warm.threads;
    const bool pass = bytes <= 8 * 1024 * 1024 && handles <= 32 && threads <= 2;
    std::cout << "[ISOLATION_FUNCTIONAL] PASS " << mode << " 24 cycles" << std::endl;
    std::cout << "[CAPTURE_STABILITY] {\"status\":\"" << (pass ? "PASS" : "FAIL")
              << "\",\"warm_private_delta\":" << bytes << ",\"warm_handle_delta\":" << handles
              << ",\"warm_thread_delta\":" << threads << "}" << std::endl;
    return pass ? 0 : 1;
}

int RunPersistentWgcIsolation() {
    constexpr int cycles = 24;
    ExternalPatternTarget target;
    target.Start();
    std::mutex mutex;
    std::condition_variable wake;
    int requested = 0;
    int completed = 0;
    bool supported = false;
    bool ready = false;
    bool shutdown = false;
    std::thread worker([&] {
        const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
        TEST_CHECK(SUCCEEDED(initialized));
        supported = webrtc::IsWgcSupported(webrtc::CaptureType::kWindow);
        TEST_CHECK(supported);
        {
            std::lock_guard lock(mutex);
            ready = true;
        }
        wake.notify_all();
        for (;;) {
            int cycle = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return requested > completed || shutdown; });
                if (shutdown) break;
                cycle = requested;
            }
            int frames = 0, closed = 0, pools = 0, devices = 0, close_failed = 0;
            bool pattern = false;
            struct Sink final : webrtc::DesktopCapturer::Callback {
                int& frames;
                bool& pattern;
                Sink(int& count, bool& valid) : frames(count), pattern(valid) {}
                void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                                     std::unique_ptr<webrtc::DesktopFrame> frame) override {
                    TEST_CHECK(result != webrtc::DesktopCapturer::Result::ERROR_PERMANENT);
                    if (!frame) return;
                    TEST_CHECK(frame->capturer_id() == webrtc::DesktopCapturerId::kWgcCapturerWin);
                    ++frames;
                    if (frame->size().width() > 400 && frame->size().height() > 300) {
                        const auto* row = frame->data() + frame->stride() * (frame->size().height() / 2);
                        const int left = row[4 * (frame->size().width() / 4)];
                        const int right = row[4 * (3 * frame->size().width() / 4)];
                        pattern = pattern || (left < 60 && right > 190) || (right < 60 && left > 190);
                    }
                }
            } sink(frames, pattern);
            auto capturer = livekit::CreateOwnedWgcWindowCapturer([&](auto phase) {
                using Phase = livekit::DesktopCaptureProbePhase;
                if (phase == Phase::SessionClosed) ++closed;
                if (phase == Phase::FramePoolClosed) ++pools;
                if (phase == Phase::D3dReleased) ++devices;
                if (phase == Phase::CloseFailed) ++close_failed;
            });
            TEST_CHECK(capturer->SelectSource(reinterpret_cast<intptr_t>(target.window)));
            capturer->Start(&sink);
            capturer->SetMaxFrameRate(15);
            const auto begin = std::chrono::steady_clock::now();
            do {
                capturer->CaptureFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(66));
            } while (std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
            capturer.reset();
            TEST_CHECK(frames >= 3 && pattern && closed == 1 && pools == 1 && devices == 1 && close_failed == 0);
            std::cout << "[WGC_PERSISTENT] {\"cycle\":" << cycle
                      << ",\"thread_id\":" << GetCurrentThreadId()
                      << ",\"frames\":" << frames
                      << ",\"session_closed\":" << closed
                      << ",\"pool_closed\":" << pools
                      << ",\"d3d_released\":" << devices << "}" << std::endl;
            {
                std::lock_guard lock(mutex);
                completed = cycle;
            }
            wake.notify_all();
        }
        RoUninitialize();
    });
    {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return ready; });
    }
    std::cout << "[WGC_CAPABILITY] {\"supported\":true,\"queries\":1}" << std::endl;
    SampleMemory(0, "baseline");
    Resources warm{};
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        {
            std::lock_guard lock(mutex);
            requested = cycle;
        }
        wake.notify_all();
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return completed == cycle; });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto stopped = SampleMemory(cycle, "stopped_worker_alive");
        if (cycle == 1) warm = stopped;
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const auto settled = SampleMemory(cycles, "settled_window_open_worker_alive");
    target.CloseWindow();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    SampleMemory(cycles, "target_window_closed_worker_alive");
    target.Exit();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    SampleMemory(cycles, "target_process_exited_worker_alive");
    {
        std::lock_guard lock(mutex);
        shutdown = true;
    }
    wake.notify_all();
    worker.join();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    SampleMemory(cycles, "worker_exited");
    const auto bytes = static_cast<int64_t>(settled.private_bytes) - static_cast<int64_t>(warm.private_bytes);
    const auto handles = static_cast<int64_t>(settled.handles) - warm.handles;
    const auto threads = static_cast<int64_t>(settled.threads) - warm.threads;
    // The diagnostic must reject a repeatable per-cycle increase even below
    // the older 32-handle end-to-end gate.
    const bool pass = bytes <= 8 * 1024 * 1024 && handles <= 8 && threads <= 2;
    std::cout << "[ISOLATION_FUNCTIONAL] PASS persistent 24 cycles" << std::endl;
    std::cout << "[CAPTURE_STABILITY] {\"status\":\"" << (pass ? "PASS" : "FAIL")
              << "\",\"warm_private_delta\":" << bytes << ",\"warm_handle_delta\":" << handles
              << ",\"warm_thread_delta\":" << threads << ",\"handle_limit\":8}" << std::endl;
    return pass ? 0 : 1;
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--wgc-persistent-isolation") return RunPersistentWgcIsolation();
    if (argc == 3 && std::string(argv[1]) == "--wgc-isolation") return RunWgcIsolation(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--pattern-target")
        return RunPatternTarget(static_cast<DWORD>(std::stoul(argv[2])));
    const bool external_probe = argc == 2 && std::string(argv[1]) == "--external-wgc-window-lifecycle-probe";
    if (argc == 2 && std::string(argv[1]) == "--wgc-capability-probe") return ProbeWgcCapability();
    if (argc == 2 && std::string(argv[1]) == "--screen-fallback-unit") {
        TestScreenCaptureFallback();
        std::cout << "SCREEN_FALLBACK_UNIT PASS: 9 cases\n";
        return 0;
    }
    const bool production_default = external_probe || (argc == 2 && std::string(argv[1]) == "--production-default");
    const bool unsupported_probe = argc == 2 && std::string(argv[1]) == "--wgc-unsupported-probe";
    const bool lifecycle_probe = argc == 2 && std::string(argv[1]) == "--lifecycle-probe";
    const bool legacy_wgc = argc == 2 && std::string(argv[1]) == "--legacy-wgc-window-lifecycle-probe";
    const bool wgc_window_probe = external_probe || legacy_wgc || (argc == 2 && std::string(argv[1]) == "--wgc-window-lifecycle-probe");
    const bool window_perf = argc == 2 && std::string(argv[1]) == "--window-perf";
    const bool screen_fallback = argc == 2 && std::string(argv[1]) == "--screen-fallback-probe";
    const bool screen_probe = screen_fallback || (argc == 2 && std::string(argv[1]) == "--screen-wgc-probe");
    if (argc > 1 && !lifecycle_probe && !wgc_window_probe && !window_perf && !screen_probe && !production_default && !unsupported_probe) return 2;
    std::promise<HWND> ready;
    auto window = ready.get_future();
    std::thread ui;
    ExternalPatternTarget target;
    HWND hwnd = nullptr;
    if (external_probe) {
        target.Start();
        hwnd = target.window;
    } else {
        ui = std::thread([&] { RunPatternWindow(ready); });
        hwnd = window.get();
    }
    TEST_CHECK(hwnd != nullptr);
    livekit::DesktopSource source{livekit::DesktopSourceKind::Window,
                                  reinterpret_cast<intptr_t>(hwnd), {}};
    bool screen_wgc_supported = false;
    bool screen_dxgi_supported = false;
    if (screen_probe) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        TEST_CHECK(SUCCEEDED(com));
        screen_wgc_supported = livekit::IsOwnedWgcSupported();
        screen_dxgi_supported = !screen_fallback && webrtc::ScreenCapturerWinDirectx::IsSupported();
        const bool window_wgc_supported = webrtc::IsWgcSupported(webrtc::CaptureType::kWindow);
        CoUninitialize();
        std::cout << "[CAPTURE_PATH] screen_wgc_supported=" << screen_wgc_supported
                  << " screen_dxgi_supported=" << screen_dxgi_supported
                  << " window_wgc_supported=" << window_wgc_supported << std::endl;
        const auto sources = livekit::EnumerateDesktopSources();
        const auto first = std::find_if(sources.begin(), sources.end(), [](const auto& candidate) {
            return candidate.kind == livekit::DesktopSourceKind::Screen;
        });
        TEST_CHECK(first != sources.end());
        source = *first;
        source.title.clear();
    }
    if (wgc_window_probe || production_default) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        TEST_CHECK(SUCCEEDED(com));
        const bool supported = webrtc::IsWgcSupported(webrtc::CaptureType::kWindow);
        CoUninitialize();
        if (!supported) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            if (external_probe) target.Exit();
            else ui.join();
            std::cout << "[RESULT] WINDOW_WGC NOT_RUN unsupported_on_this_desktop" << std::endl;
            return 2;
        }
    }
    if (lifecycle_probe || wgc_window_probe || screen_probe) SampleMemory(0, "baseline");
    int total_frames = 0;
    Resources warm{}, settled{};
    int cycles = (lifecycle_probe || wgc_window_probe) ? 12 : screen_probe ? 3 : 1;
    if (const auto requested = std::getenv("LIVEKIT_TEST_SHARE_CYCLES")) {
        const int value = std::atoi(requested);
        TEST_CHECK(value >= 1 && value <= 30);
        cycles = value;
    }
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        std::mutex mutex;
        std::condition_variable changed;
        int frames = 0;
        bool failed = false, pattern = false;
        std::atomic<std::uint32_t> backend_id{0};
        int session_closed = 0, pool_closed = 0, d3d_released = 0, close_failed = 0;
        livekit::DesktopCaptureProbeOptions probe;
        if (!production_default && !unsupported_probe) probe.allow_wgc_window = wgc_window_probe;
        probe.simulate_wgc_unsupported = unsupported_probe;
        probe.simulate_dxgi_unsupported = screen_fallback;
        probe.use_legacy_wgc_window = legacy_wgc;
        probe.on_event = [cycle, &backend_id, &session_closed, &pool_closed,
                          &d3d_released, &close_failed](livekit::DesktopCaptureProbeEvent event) {
            using Phase = livekit::DesktopCaptureProbePhase;
            if (event.phase == Phase::SessionClosed) ++session_closed;
            if (event.phase == Phase::FramePoolClosed) ++pool_closed;
            if (event.phase == Phase::D3dReleased) ++d3d_released;
            if (event.phase == Phase::CloseFailed) ++close_failed;
            if (event.phase == livekit::DesktopCaptureProbePhase::BackendFrame)
                backend_id.store(event.capturer_id);
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            std::cout << "[CAPTURE_PROBE] {\"cycle\":" << cycle
                      << ",\"epoch_ms\":" << epoch_ms
                      << ",\"phase\":\"" << PhaseName(event.phase)
                      << "\",\"thread_id\":" << event.thread_id
                      << ",\"capturer_id\":" << event.capturer_id
                      << ",\"backend\":\"" << BackendName(event.capturer_id)
                      << "\"}" << std::endl;
        };
        auto capture = livekit::CreateDesktopCaptureForProbe(std::move(probe));
        capture->Start(source,
            [&](const livekit::VideoFrame& frame) {
                std::lock_guard lock(mutex);
                ++frames;
                TEST_CHECK(frame.type() == livekit::VideoBufferType::I420);
                if (!screen_probe && frame.width() > 400 && frame.height() > 300) {
                    const auto left = frame.data()[(frame.height() / 2) * frame.width() + frame.width() / 4];
                    const auto right = frame.data()[(frame.height() / 2) * frame.width() + 3 * frame.width() / 4];
                    pattern = pattern || (left < 60 && right > 190) ||
                        (right < 60 && left > 190);
                }
                if (screen_probe) pattern = frame.width() >= 320 && frame.height() >= 180;
                changed.notify_all();
            }, [&] { std::lock_guard lock(mutex); failed = true; changed.notify_all(); });
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(10), [&] { return failed || (frames >= 3 && pattern); });
        }
        TEST_CHECK(!failed && frames >= 3 && pattern);
        if (wgc_window_probe || production_default) {
            TEST_CHECK(backend_id.load() == webrtc::DesktopCapturerId::kWgcCapturerWin);
        } else if (screen_probe) {
            TEST_CHECK(backend_id.load() == (screen_dxgi_supported
                ? webrtc::DesktopCapturerId::kScreenCapturerWinDirectx : screen_wgc_supported
                ? webrtc::DesktopCapturerId::kWgcCapturerWin
                : webrtc::DesktopCapturerId::kScreenCapturerWinGdi));
        } else {
            TEST_CHECK(backend_id.load() == webrtc::DesktopCapturerId::kWindowCapturerWinGdi);
        }
        if ((wgc_window_probe || production_default) && !legacy_wgc && cycle == 1) {
            SendMessageW(hwnd, WM_APP, 1, 0);
            int before;
            { std::lock_guard lock(mutex); before = frames; }
            std::this_thread::sleep_for(std::chrono::seconds(6));
            { std::lock_guard lock(mutex); TEST_CHECK(!failed && frames > before + 5); }
            SendMessageW(hwnd, WM_APP, 0, 0);
            std::cout << "[CAPTURE_STATIC] PASS static_window_exceeds_watchdog" << std::endl;
        }
        if (window_perf) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int begin_frames;
            { std::lock_guard lock(mutex); begin_frames = frames; }
            const auto cpu_start = ProcessCpuTicks();
            const auto wall_start = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::seconds(30));
            const auto wall_end = std::chrono::steady_clock::now();
            const auto cpu_end = ProcessCpuTicks();
            int end_frames;
            { std::lock_guard lock(mutex); end_frames = frames; }
            const double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
            const double cpu_cores = static_cast<double>(cpu_end - cpu_start) / 1e7 / wall_seconds;
            std::cout << "[CAPTURE_PERF] {\"backend\":\"" << BackendName(backend_id.load())
                      << "\",\"seconds\":"
                      << wall_seconds << ",\"frames\":" << end_frames - begin_frames
                      << ",\"fps\":" << (end_frames - begin_frames) / wall_seconds
                      << ",\"cpu_core_percent\":" << cpu_cores * 100.0
                      << ",\"logical_processors\":" << std::thread::hardware_concurrency()
                      << "}" << std::endl;
        }
        if (lifecycle_probe || wgc_window_probe || screen_probe) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            SampleMemory(cycle, "active");
        }
        capture->Stop();
        const int stopped_frames = frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        TEST_CHECK(frames == stopped_frames);
        capture.reset();
        if (((wgc_window_probe || production_default) && !legacy_wgc) ||
            (screen_fallback && screen_wgc_supported)) {
            TEST_CHECK(session_closed == 1 && pool_closed == 1 && d3d_released == 1 && close_failed == 0);
        }
        total_frames += frames;
        if (lifecycle_probe || wgc_window_probe || screen_probe) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            settled = SampleMemory(cycle, "stopped");
            if (cycle == 1) warm = settled;
        }
    }
    if (external_probe) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        settled = SampleMemory(cycles, "settled_window_open");
        target.CloseWindow();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        SampleMemory(cycles, "target_window_closed");
        target.Exit();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        SampleMemory(cycles, "target_process_exited");
    } else {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ui.join();
    }
    if (lifecycle_probe || wgc_window_probe) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!external_probe) settled = SampleMemory(cycles, "settled");
        const auto bytes = static_cast<int64_t>(settled.private_bytes) - static_cast<int64_t>(warm.private_bytes);
        const auto handles = static_cast<int64_t>(settled.handles) - warm.handles;
        const auto threads = static_cast<int64_t>(settled.threads) - warm.threads;
        const bool pass = bytes <= 8 * 1024 * 1024 && handles <= 32 && threads <= 2;
        std::cout << "[CAPTURE_STABILITY] {\"status\":\"" << (pass ? "PASS" : "FAIL")
                  << "\",\"warm_private_delta\":" << bytes << ",\"warm_handle_delta\":" << handles
                  << ",\"warm_thread_delta\":" << threads << "}" << std::endl;
        if (!pass) return 1;
    }
    std::cout << "DESKTOP_CAPTURE_RUNTIME PASS: "
              << (screen_probe ? "screen I420 delivery" : "owned-window I420 pattern")
              << "; frames=" << total_frames
              << "; no delivery after Stop; worker joined\n";
    if (screen_probe && !screen_wgc_supported) {
        std::cout << "[RESULT] SCREEN_WGC NOT_RUN unsupported_on_this_desktop" << std::endl;
    }
}
