#include "desktop_capture.h"

#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include "modules/desktop_capture/win/wgc_capturer_win.h"
#include "modules/desktop_capture/win/screen_capturer_win_directx.h"
#include "screen_capture_fallback.h"
#include "libyuv/convert.h"
#include "libyuv/scale.h"
#include <windows.h>
#include <objbase.h>
#include "wgc_window_capture.h"
#include <roapi.h>
#pragma comment(lib, "runtimeobject.lib")
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
std::atomic<std::uint32_t> observed_backend{0};
std::atomic<std::uint64_t> observed_frames{0};
enum class CaptureFailure {
    None, SourceUnavailable, WindowClosed, FrameTimeout, BackendPermanent,
    InvalidFrame, UnsupportedQuality, Conversion, Scaling, WorkerException,
    ConversionException
};
enum class BindingFailure { None, InvalidBinding, MonitorUnavailable, MonitorInvalid,
    ScreenUnavailable, DeviceChanged, EmptyGeometry, GeometryChanged };
std::atomic<std::uint64_t> observed_failures{0}, observed_binding_failures{0};
std::atomic<CaptureFailure> observed_failure{CaptureFailure::None};
std::atomic<BindingFailure> observed_binding_failure{BindingFailure::None};

const char* FailureName(CaptureFailure failure) {
    switch (failure) {
    case CaptureFailure::None: return "none";
    case CaptureFailure::SourceUnavailable: return "source_unavailable";
    case CaptureFailure::WindowClosed: return "window_closed";
    case CaptureFailure::FrameTimeout: return "window_frame_timeout";
    case CaptureFailure::BackendPermanent: return "backend_permanent";
    case CaptureFailure::InvalidFrame: return "invalid_frame";
    case CaptureFailure::UnsupportedQuality: return "unsupported_quality";
    case CaptureFailure::Conversion: return "conversion_failed";
    case CaptureFailure::Scaling: return "scaling_failed";
    case CaptureFailure::WorkerException: return "capture_worker_exception";
    case CaptureFailure::ConversionException: return "conversion_worker_exception";
    }
    return "unknown";
}
const char* FailureName(BindingFailure failure) {
    switch (failure) {
    case BindingFailure::None: return "none";
    case BindingFailure::InvalidBinding: return "invalid_binding";
    case BindingFailure::MonitorUnavailable: return "monitor_unavailable";
    case BindingFailure::MonitorInvalid: return "monitor_invalid";
    case BindingFailure::ScreenUnavailable: return "screen_unavailable";
    case BindingFailure::DeviceChanged: return "device_changed";
    case BindingFailure::EmptyGeometry: return "empty_geometry";
    case BindingFailure::GeometryChanged: return "geometry_changed";
    }
    return "unknown";
}
ScreenBindingStatus RejectBinding(BindingFailure failure) {
    observed_binding_failure.store(failure, std::memory_order_relaxed);
    observed_binding_failures.fetch_add(1, std::memory_order_release);
    return failure == BindingFailure::GeometryChanged
        ? ScreenBindingStatus::GeometryChanged : ScreenBindingStatus::Unavailable;
}

webrtc::DesktopCaptureOptions Options(bool allow_wgc_window = false) {
    auto options = webrtc::DesktopCaptureOptions::CreateDefault();
    // A session shutdown may wait for the worker; never enumerate or send
    // synchronous window messages back into this process's waiting Qt thread.
    options.set_enumerate_current_process_windows(false);
    options.set_allow_cropping_window_capturer(false);
    options.set_allow_directx_capturer(false);
    options.set_allow_wgc_screen_capturer(false);
    // Enumeration and unsupported capture use GDI. Window WGC is owned below.
    options.set_allow_wgc_window_capturer(allow_wgc_window);
    options.set_wgc_require_border(true);
    options.set_disable_effects(false);
    options.set_prefer_cursor_embedded(true);
    return options;
}

auto MakeCapturer(DesktopSourceKind kind, bool allow_wgc_window = false) {
    return kind == DesktopSourceKind::Screen
        ? webrtc::DesktopCapturer::CreateScreenCapturer(Options())
        : webrtc::DesktopCapturer::CreateWindowCapturer(Options(allow_wgc_window));
}

class ComScope {
public:
    ComScope() : result_(RoInitialize(RO_INIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(result_)) RoUninitialize(); }
private:
    HRESULT result_;
};

// Inject an error only after a real backend has delivered frames. The normal
// fallback owner still retires the backend outside its callback stack.
class FailingCaptureForProbe final : public webrtc::DesktopCapturer,
                                    private webrtc::DesktopCapturer::Callback {
public:
    FailingCaptureForProbe(std::unique_ptr<webrtc::DesktopCapturer> capture, unsigned frames)
        : capture_(std::move(capture)), limit_(frames) {}
    bool GetSourceList(SourceList* sources) override { return capture_->GetSourceList(sources); }
    bool SelectSource(SourceId source) override { return capture_->SelectSource(source); }
    void Start(webrtc::DesktopCapturer::Callback* callback) override { callback_ = callback; capture_->Start(this); }
    void SetMaxFrameRate(uint32_t rate) override { capture_->SetMaxFrameRate(rate); }
    void CaptureFrame() override {
        if (delivered_ >= limit_) callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
        else capture_->CaptureFrame();
    }
private:
    void OnCaptureResult(Result result, std::unique_ptr<webrtc::DesktopFrame> frame) override {
        if (result == Result::SUCCESS && frame) ++delivered_;
        callback_->OnCaptureResult(result, std::move(frame));
    }
    std::unique_ptr<webrtc::DesktopCapturer> capture_;
    webrtc::DesktopCapturer::Callback* callback_ = nullptr;
    unsigned limit_, delivered_ = 0;
};

std::unique_ptr<webrtc::DesktopCapturer> InjectCaptureFailure(
        std::unique_ptr<webrtc::DesktopCapturer> capture, unsigned frames) {
    if (capture && frames) return std::make_unique<FailingCaptureForProbe>(std::move(capture), frames);
    return capture;
}

class DesktopCapture final : public IDesktopCapture, private webrtc::DesktopCapturer::Callback {
public:
    explicit DesktopCapture(DesktopCaptureProbeOptions probe) : probe_(std::move(probe)) {}
    ~DesktopCapture() override { Stop(); }
    bool SetQuality(ScreenShareQuality quality, uint64_t revision, QualityCallback applied) override {
        if (!quality.valid()) return false;
        {
            std::lock_guard lock(wait_mutex_);
            if (revision < requested_revision_) return false;
            if (source_width_ > 0 && !ResolveScreenShareProfile(source_width_, source_height_, quality, revision)) return false;
            requested_quality_ = quality;
            requested_revision_ = revision;
            requested_callback_ = std::move(applied);
            ++mailbox_sequence_;
        }
        wake_.notify_all();
        return true;
    }
    void Start(DesktopSource source, FrameCallback frame, EndCallback ended) override {
        Stop();
        {
            std::lock_guard lock(wait_mutex_);
            source_width_ = source_height_ = 0;
        }
        frame_ = std::move(frame);
        ended_ = std::move(ended);
        backend_seen_ = false;
        backend_id_ = 0;
        stopped_ = false;
        last_frame_ns_.store(NowNs(), std::memory_order_relaxed);
        converter_ = std::thread([this] { ConvertLoop(); });
        worker_ = std::thread([this, source = std::move(source)] {
            ComScope com;
            try {
                const bool window = source.kind == DesktopSourceKind::Window;
                const bool wgc_supported = !probe_.simulate_wgc_unsupported &&
                    (!window || probe_.allow_wgc_window) &&
                    (window && probe_.use_legacy_wgc_window
                        ? webrtc::IsWgcSupported(webrtc::CaptureType::kWindow)
                        : IsOwnedWgcSupported(source.kind));
                auto capturer = window && wgc_supported &&
                    !probe_.use_legacy_wgc_window
                    ? CreateOwnedWgcWindowCapturer([this](auto phase) { Notify(phase); })
                    : (window ? MakeCapturer(source.kind, wgc_supported) : nullptr);
                if (!window) {
                    capturer = std::make_unique<ScreenCaptureFallback>(
                        std::vector<ScreenCaptureFallback::Backend>{
                            {[this]() -> std::unique_ptr<webrtc::DesktopCapturer> {
                                if (probe_.simulate_dxgi_unsupported ||
                                    !webrtc::ScreenCapturerWinDirectx::IsSupported()) return nullptr;
                                return InjectCaptureFailure(
                                    std::make_unique<webrtc::ScreenCapturerWinDirectx>(Options()),
                                    probe_.simulate_dxgi_failure_after_frames);
                            }},
                            {[this, wgc_supported]() -> std::unique_ptr<webrtc::DesktopCapturer> {
                                return wgc_supported
                                    ? InjectCaptureFailure(CreateOwnedWgcScreenCapturer(
                                        [this](auto phase) { Notify(phase); }), probe_.simulate_wgc_failure_after_frames) : nullptr;
                            }, true},
                            {[] { return MakeCapturer(DesktopSourceKind::Screen); }, true}
                        });
                }
                if (capturer) Notify(DesktopCaptureProbePhase::Created);
                if (!capturer || !capturer->SelectSource(source.id)) {
                    capturer.reset();
                    Notify(DesktopCaptureProbePhase::Destroyed);
                    Fail(CaptureFailure::SourceUnavailable);
                    return;
                }
                capturer->Start(this);
                Notify(DesktopCaptureProbePhase::Started);
                ScreenCaptureDeadline deadline;
                uint64_t applied_sequence = ~uint64_t{0};
                while (!stopped_.load(std::memory_order_acquire)) {
                    bool changed = false;
                    {
                        std::lock_guard lock(wait_mutex_);
                        if (applied_sequence != mailbox_sequence_) {
                            applied_sequence = mailbox_sequence_;
                            active_quality_ = requested_quality_;
                            active_revision_ = requested_revision_;
                            active_callback_ = requested_callback_;
                            changed = true;
                        }
                    }
                    if (changed) {
                        capturer->SetMaxFrameRate(active_quality_.fps);
                        deadline.Reset(std::chrono::steady_clock::now(), active_quality_.fps);
                    }
                    // WGC may keep returning its cached last frame after the
                    // target closes; successful CaptureFrame alone is not an
                    // ended signal. Observe the selected window's lifetime.
                    if (source.kind == DesktopSourceKind::Window &&
                        !IsWindow(reinterpret_cast<HWND>(source.id))) {
                        Fail(CaptureFailure::WindowClosed);
                        break;
                    }
                    // Plan the next slot before capture/conversion. Computing
                    // it after an overrun adds an entire idle period to work
                    // that has already exceeded the requested interval.
                    const auto capture_started = std::chrono::steady_clock::now();
                    const auto next_capture = deadline.Advance(capture_started);
                    backend_time_us_ = 0;
                    capturer->CaptureFrame();
                    if (window && std::chrono::nanoseconds(NowNs() - last_frame_ns_.load(std::memory_order_relaxed)) > 5s)
                        Fail(CaptureFailure::FrameTimeout);
                    const auto capture_finished = probe_.on_capture_timing ? std::chrono::steady_clock::now()
                                                                          : capture_started;
                    {
                        std::unique_lock lock(wait_mutex_);
                        wake_.wait_until(lock, next_capture,
                            [this, applied_sequence] { return stopped_.load() || mailbox_sequence_ != applied_sequence; });
                    }
                    if (probe_.on_capture_timing) probe_.on_capture_timing(
                        std::chrono::duration_cast<std::chrono::microseconds>(capture_finished-capture_started).count(),
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-capture_finished).count(),
                        backend_time_us_);
                }
                capturer.reset();
                Notify(DesktopCaptureProbePhase::Destroyed);
            } catch (...) {
                Fail(CaptureFailure::WorkerException);
            }
        });
    }
    void Stop() override {
        stopped_.store(true, std::memory_order_release);
        wake_.notify_all();
        conversion_wake_.notify_all();
        const bool joined = worker_.joinable() || converter_.joinable();
        if (worker_.joinable()) worker_.join();
        if (converter_.joinable()) converter_.join();
        {
            std::lock_guard lock(conversion_mutex_);
            pending_.reset();
        }
        if (joined) Notify(DesktopCaptureProbePhase::Joined);
        frame_ = {};
        ended_ = {};
        converted_.reset();
        scaled_.reset();
        last_applied_.reset();
    }
private:
    struct PendingFrame {
        std::unique_ptr<webrtc::DesktopFrame> frame;
        ScreenShareFrameProfile profile;
        QualityCallback applied;
    };
    static std::int64_t NowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void ConvertLoop() noexcept {
        try {
            while (!stopped_.load(std::memory_order_acquire)) {
                std::optional<PendingFrame> pending;
                {
                    std::unique_lock lock(conversion_mutex_);
                    conversion_wake_.wait(lock, [this] { return stopped_.load() || pending_.has_value(); });
                    if (stopped_.load()) break;
                    pending = std::move(pending_);
                    pending_.reset();
                }
                const auto started = probe_.on_conversion_timing ? NowNs() : 0;
                ConvertAndDeliver(*pending);
                if (probe_.on_conversion_timing) probe_.on_conversion_timing((NowNs()-started)/1000);
            }
        } catch (...) { Fail(CaptureFailure::ConversionException); }
    }
    void Notify(DesktopCaptureProbePhase phase, std::uint32_t capturer_id = 0) noexcept {
        if (!probe_.on_event) return;
        try { probe_.on_event({phase, capturer_id, GetCurrentThreadId()}); }
        catch (...) {}
    }
    void Fail(CaptureFailure failure) {
        if (!stopped_.exchange(true)) {
            observed_failure.store(failure, std::memory_order_relaxed);
            observed_failures.fetch_add(1, std::memory_order_release);
            if (ended_) ended_();
        }
        wake_.notify_all();
        conversion_wake_.notify_all();
    }
    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override {
        if (stopped_.load(std::memory_order_acquire)) return;
        if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
            Fail(CaptureFailure::BackendPermanent);
            return;
        }
        if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame) return;
        if (probe_.on_capture_timing) backend_time_us_ = std::max<int64_t>(0,frame->capture_time_ms()) * 1000;
        observed_backend.store(frame->capturer_id(), std::memory_order_relaxed);
        observed_frames.fetch_add(1, std::memory_order_relaxed);
        if (!backend_seen_ || frame->capturer_id() != backend_id_) {
            backend_seen_ = true;
            backend_id_ = frame->capturer_id();
            Notify(DesktopCaptureProbePhase::BackendFrame, backend_id_);
        }
        const int w = frame->size().width(), h = frame->size().height();
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || frame->stride() < w * 4) {
            Fail(CaptureFailure::InvalidFrame);
            return;
        }
        {
            std::lock_guard lock(wait_mutex_);
            source_width_ = w; source_height_ = h;
        }
        const auto profile = ResolveScreenShareProfile(w, h, active_quality_, active_revision_);
        if (!profile) {
            if (w >= 2 && h >= 2) Fail(CaptureFailure::UnsupportedQuality);
            return;
        }
        {
            std::lock_guard lock(conversion_mutex_);
            if (stopped_.load()) return;
            // One in-flight frame plus one newest pending frame. The backend
            // retains its device owner and never waits for conversion/delivery.
            pending_ = PendingFrame{std::move(frame), *profile, active_callback_};
        }
        conversion_wake_.notify_one();
    }
    void ConvertAndDeliver(const PendingFrame& pending) {
        const auto& frame = pending.frame;
        const auto& profile = pending.profile;
        const int w = frame->size().width(), h = frame->size().height();
        if (!converted_ || converted_->width() != w || converted_->height() != h)
            converted_ = VideoFrame::create(w, h, VideoBufferType::I420);
        auto& output = *converted_;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        auto* y = output.data();
        auto* u = y + w * h;
        auto* v = u + cw * ch;
        // DesktopFrame BGRA bytes are libyuv's little-endian ARGB input.
        if (libyuv::ARGBToI420(frame->data(), frame->stride(), y, w, u, cw, v, cw, w, h) != 0) {
            Fail(CaptureFailure::Conversion);
            return;
        }
        const VideoFrame* delivered = &output;
        if (profile.width != w || profile.height != h) {
            const int dw = profile.width, dh = profile.height;
            if (!scaled_ || scaled_->width() != dw || scaled_->height() != dh)
                scaled_ = VideoFrame::create(dw, dh, VideoBufferType::I420);
            auto* dy = scaled_->data();
            auto* du = dy + dw * dh;
            auto* dv = du + (dw / 2) * (dh / 2);
            if (libyuv::I420Scale(y,w,u,cw,v,cw,w,h,
                    dy,dw,du,dw/2,dv,dw/2,dw,dh,libyuv::kFilterBox) != 0) {
                Fail(CaptureFailure::Scaling);
                return;
            }
            delivered = &*scaled_;
        } else scaled_.reset();
        last_frame_ns_.store(NowNs(), std::memory_order_relaxed);
        if (!stopped_.load(std::memory_order_acquire) && frame_) {
            frame_(*delivered);
            if (!stopped_.load(std::memory_order_acquire) && (!last_applied_ || *last_applied_ != profile)) {
                last_applied_ = profile;
                if (pending.applied) pending.applied(profile);
            }
        }
    }
    std::atomic<bool> stopped_{true};
    std::thread worker_, converter_;
    std::mutex conversion_mutex_;
    std::condition_variable conversion_wake_;
    std::optional<PendingFrame> pending_; // conversion_mutex_, at most one
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    ScreenShareQuality requested_quality_, active_quality_;
    uint64_t requested_revision_ = 0, active_revision_ = 0, mailbox_sequence_ = 0;
    int source_width_ = 0, source_height_ = 0; // wait_mutex_
    QualityCallback requested_callback_, active_callback_;
    std::optional<ScreenShareFrameProfile> last_applied_; // converter only
    std::optional<VideoFrame> converted_, scaled_; // converter-owned reusable buffers
    FrameCallback frame_;
    EndCallback ended_;
    std::atomic<std::int64_t> last_frame_ns_{0};
    DesktopCaptureProbeOptions probe_;
    std::uint32_t backend_id_ = 0;
    bool backend_seen_ = false;
    std::uint64_t backend_time_us_ = 0;
};
} // namespace

DesktopCaptureObservation ObserveDesktopCapture() {
    using namespace webrtc::DesktopCapturerId;
    const auto id = observed_backend.load(std::memory_order_relaxed);
    const char* backend = "unknown";
    if (id == kWgcCapturerWin) backend = "wgc";
    else if (id == kScreenCapturerWinDirectx) backend = "dxgi";
    else if (id == kWindowCapturerWinGdi || id == kScreenCapturerWinGdi) backend = "gdi";
    const auto failures = observed_failures.load(std::memory_order_acquire);
    const auto binding_failures = observed_binding_failures.load(std::memory_order_acquire);
    return {backend, observed_frames.load(std::memory_order_relaxed),
        failures, FailureName(observed_failure.load(std::memory_order_relaxed)),
        binding_failures, FailureName(observed_binding_failure.load(std::memory_order_relaxed))};
}

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

std::unique_ptr<IDesktopCapture> CreateDesktopCapture() {
    return CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions{});
}

std::unique_ptr<IDesktopCapture> CreateDesktopCaptureForProbe(DesktopCaptureProbeOptions probe) {
    return std::make_unique<DesktopCapture>(std::move(probe));
}

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

ScreenBindingStatus CheckScreenBinding(const ScreenBinding &binding) {
    if (binding.source_epoch == 0 || binding.share_session_id.empty() ||
        binding.device_key.empty()) return RejectBinding(BindingFailure::InvalidBinding);
    HMONITOR monitor = nullptr;
    std::wstring currentKey;
    if (!webrtc::GetHmonitorFromDeviceIndex(binding.source_id, &monitor) || !monitor)
        return RejectBinding(BindingFailure::MonitorUnavailable);
    if (!webrtc::IsMonitorValid(monitor)) return RejectBinding(BindingFailure::MonitorInvalid);
    if (!webrtc::IsScreenValid(binding.source_id, &currentKey)) return RejectBinding(BindingFailure::ScreenUnavailable);
    if (currentKey != binding.device_key) return RejectBinding(BindingFailure::DeviceChanged);
    const auto rect = webrtc::GetScreenRect(binding.source_id, binding.device_key);
    if (rect.is_empty()) return RejectBinding(BindingFailure::EmptyGeometry);
    if (rect.left() != binding.physical_x || rect.top() != binding.physical_y ||
        rect.width() != binding.physical_width || rect.height() != binding.physical_height)
        return RejectBinding(BindingFailure::GeometryChanged);
    return ScreenBindingStatus::Valid;
}
bool ValidateScreenBinding(const ScreenBinding &binding) {
    return CheckScreenBinding(binding) == ScreenBindingStatus::Valid;
}
} // namespace livekit
