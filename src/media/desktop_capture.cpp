#include "desktop_capture.h"

#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include "libyuv/convert.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <thread>

namespace livekit {
namespace {
using namespace std::chrono_literals;

webrtc::DesktopCaptureOptions Options() {
    auto options = webrtc::DesktopCaptureOptions::CreateDefault();
    // A session shutdown may wait for the worker; never enumerate or send
    // synchronous window messages back into this process's waiting Qt thread.
    options.set_enumerate_current_process_windows(false);
    options.set_allow_cropping_window_capturer(false);
    options.set_allow_directx_capturer(true);
    options.set_allow_wgc_screen_capturer(true);
    options.set_allow_wgc_window_capturer(true);
    options.set_wgc_require_border(true);
    options.set_disable_effects(false);
    options.set_prefer_cursor_embedded(true);
    return options;
}

auto MakeCapturer(DesktopSourceKind kind) {
    return kind == DesktopSourceKind::Screen
        ? webrtc::DesktopCapturer::CreateScreenCapturer(Options())
        : webrtc::DesktopCapturer::CreateWindowCapturer(Options());
}

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_;
};

class DesktopCapture final : public IDesktopCapture, private webrtc::DesktopCapturer::Callback {
public:
    ~DesktopCapture() override { Stop(); }
    void Start(DesktopSource source, FrameCallback frame, EndCallback ended) override {
        Stop();
        frame_ = std::move(frame);
        ended_ = std::move(ended);
        stopped_ = false;
        worker_ = std::thread([this, source = std::move(source)] {
            ComScope com;
            try {
                auto capturer = MakeCapturer(source.kind);
                if (!capturer || !capturer->SelectSource(source.id)) {
                    Fail();
                    return;
                }
                capturer->Start(this);
                capturer->SetMaxFrameRate(15);
                last_frame_ = std::chrono::steady_clock::now();
                while (!stopped_.load(std::memory_order_acquire)) {
                    // WGC may keep returning its cached last frame after the
                    // target closes; successful CaptureFrame alone is not an
                    // ended signal. Observe the selected window's lifetime.
                    if (source.kind == DesktopSourceKind::Window &&
                        !IsWindow(reinterpret_cast<HWND>(source.id))) {
                        Fail();
                        break;
                    }
                    capturer->CaptureFrame();
                    if (std::chrono::steady_clock::now() - last_frame_ > 5s) Fail();
                    std::unique_lock lock(wait_mutex_);
                    wake_.wait_for(lock, 66ms, [this] { return stopped_.load(); });
                }
            } catch (...) {
                Fail();
            }
        });
    }
    void Stop() override {
        stopped_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        frame_ = {};
        ended_ = {};
    }
private:
    void Fail() {
        if (!stopped_.exchange(true) && ended_) ended_();
    }
    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override {
        if (stopped_.load(std::memory_order_acquire)) return;
        if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
            Fail();
            return;
        }
        if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame) return;
        const int w = frame->size().width(), h = frame->size().height();
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || frame->stride() < w * 4) {
            Fail();
            return;
        }
        auto output = VideoFrame::create(w, h, VideoBufferType::I420);
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        auto* y = output.data();
        auto* u = y + w * h;
        auto* v = u + cw * ch;
        // DesktopFrame BGRA bytes are libyuv's little-endian ARGB input.
        if (libyuv::ARGBToI420(frame->data(), frame->stride(), y, w, u, cw, v, cw, w, h) != 0) {
            Fail();
            return;
        }
        last_frame_ = std::chrono::steady_clock::now();
        if (!stopped_.load(std::memory_order_acquire) && frame_) frame_(output);
    }
    std::atomic<bool> stopped_{true};
    std::thread worker_;
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    FrameCallback frame_;
    EndCallback ended_;
    std::chrono::steady_clock::time_point last_frame_;
};
} // namespace

std::vector<DesktopSource> EnumerateDesktopSources() {
    ComScope com;
    std::vector<DesktopSource> result;
    for (auto kind : {DesktopSourceKind::Screen, DesktopSourceKind::Window}) {
        auto capturer = MakeCapturer(kind);
        webrtc::DesktopCapturer::SourceList sources;
        if (!capturer || !capturer->GetSourceList(&sources)) continue;
        for (const auto& source : sources) result.push_back({kind, source.id, source.title});
    }
    return result;
}

std::unique_ptr<IDesktopCapture> CreateDesktopCapture() { return std::make_unique<DesktopCapture>(); }

std::optional<ScreenBinding> ResolveScreenBinding(
        const DesktopSource &source, std::uint64_t sourceEpoch,
        std::string shareSessionId) {
    if (source.kind != DesktopSourceKind::Screen || sourceEpoch == 0 ||
        shareSessionId.empty()) return std::nullopt;
    webrtc::DesktopCapturer::SourceList screens;
    std::vector<std::string> deviceNames;
    if (!webrtc::GetScreenList(&screens, &deviceNames) ||
        screens.size() != deviceNames.size()) return std::nullopt;
    std::optional<std::size_t> matched;
    for (std::size_t index = 0; index < screens.size(); ++index) {
        if (screens[index].id != source.id) continue;
        if (matched) return std::nullopt;
        matched = index;
    }
    if (!matched) return std::nullopt;

    HMONITOR monitor = nullptr;
    std::wstring deviceKey;
    if (!webrtc::GetHmonitorFromDeviceIndex(source.id, &monitor) || !monitor ||
        !webrtc::IsMonitorValid(monitor) ||
        !webrtc::IsScreenValid(source.id, &deviceKey)) return std::nullopt;
    const auto rect = webrtc::GetScreenRect(source.id, deviceKey);
    if (rect.is_empty()) return std::nullopt;
    const int width = rect.width();
    const int height = rect.height();
    if (width < 320 || height < 180 || width > 16384 || height > 16384) return std::nullopt;
    const double scale = std::min(1.0, 4096.0 / std::max(width, height));

    ScreenBinding result;
    result.share_session_id = std::move(shareSessionId);
    result.source_epoch = sourceEpoch;
    result.source_id = source.id;
    result.display_name = deviceNames[*matched];
    result.device_key = std::move(deviceKey);
    result.physical_x = rect.left();
    result.physical_y = rect.top();
    result.physical_width = width;
    result.physical_height = height;
    result.canonical_width = std::max(320, static_cast<int>(std::lround(width * scale)));
    result.canonical_height = std::max(180, static_cast<int>(std::lround(height * scale)));
    return result;
}

bool ValidateScreenBinding(const ScreenBinding &binding) {
    if (binding.source_epoch == 0 || binding.share_session_id.empty() ||
        binding.device_key.empty()) return false;
    HMONITOR monitor = nullptr;
    std::wstring currentKey;
    if (!webrtc::GetHmonitorFromDeviceIndex(binding.source_id, &monitor) || !monitor ||
        !webrtc::IsMonitorValid(monitor) ||
        !webrtc::IsScreenValid(binding.source_id, &currentKey) ||
        currentKey != binding.device_key) return false;
    const auto rect = webrtc::GetScreenRect(binding.source_id, binding.device_key);
    return !rect.is_empty() &&
        rect.left() == binding.physical_x && rect.top() == binding.physical_y &&
        rect.width() == binding.physical_width && rect.height() == binding.physical_height;
}
} // namespace livekit
