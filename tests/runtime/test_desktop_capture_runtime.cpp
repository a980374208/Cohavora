// Opt-in real Windows window capture. Only captures this test's own pattern;
// no desktop pixels, images, window lists or source titles are persisted.
#include "desktop_capture.h"
#include "tests/support/test_check.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
LRESULT CALLBACK PatternWindow(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        rect.right /= 2;
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (message == WM_PAINT) EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int main() {
    std::promise<HWND> ready;
    auto window = ready.get_future();
    std::thread ui([&] {
        WNDCLASSW type{};
        type.lpfnWndProc = PatternWindow;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"LiveKitCaptureRegression";
        RegisterClassW(&type);
        HWND hwnd = CreateWindowExW(0, type.lpszClassName, L"LiveKit capture regression",
            WS_OVERLAPPEDWINDOW, 80, 80, 800, 600, nullptr, nullptr, type.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        ready.set_value(hwnd);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    HWND hwnd = window.get();
    TEST_CHECK(hwnd != nullptr);
    std::mutex mutex;
    std::condition_variable changed;
    int frames = 0;
    bool failed = false, pattern = false;
    auto capture = livekit::CreateDesktopCapture();
    capture->Start({livekit::DesktopSourceKind::Window, reinterpret_cast<intptr_t>(hwnd), {}},
        [&](const livekit::VideoFrame& frame) {
            std::lock_guard lock(mutex);
            ++frames;
            TEST_CHECK(frame.type() == livekit::VideoBufferType::I420);
            if (frame.width() > 400 && frame.height() > 300) {
                const auto left = frame.data()[(frame.height() / 2) * frame.width() + frame.width() / 4];
                const auto right = frame.data()[(frame.height() / 2) * frame.width() + 3 * frame.width() / 4];
                pattern = pattern || (left < 60 && right > 190);
            }
            changed.notify_all();
        }, [&] { std::lock_guard lock(mutex); failed = true; changed.notify_all(); });
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(10), [&] { return failed || (frames >= 3 && pattern); });
    }
    capture->Stop();
    const int stopped_frames = frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    TEST_CHECK(frames == stopped_frames);
    capture.reset();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ui.join();
    TEST_CHECK(!failed && frames >= 3 && pattern);
    std::cout << "DESKTOP_CAPTURE_RUNTIME PASS: owned-window I420 pattern; frames=" << frames
              << "; no delivery after Stop; worker joined\n";
}
