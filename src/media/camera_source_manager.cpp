#include "camera_source_manager.h"
#include "dshow_enumerator.h"
#include "telemetry/diagnostic_pipeline.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <tuple>

namespace livekit {

struct CameraSourceManager::InitialCaptureAttempt final {
    // Device calls are serialized separately from manager state. In particular,
    // Stop must not race a late Init/Start into restarting a retired graph.
    std::mutex lifecycle_mutex;
    std::condition_variable ready;
    std::shared_ptr<ICameraCapturer> capturer;
    bool stopped = false; // lifecycle_mutex
    bool cancelled = false; // state_mutex_
    bool first_frame_delivery_pending = false; // state_mutex_
    std::optional<VideoFrame> first_frame; // state_mutex_
    VideoCaptureOptions first_frame_options; // state_mutex_
};

struct CameraSourceManager::DeviceSwitchSpan final {
    DeviceSwitchSpan()
        : started_at(std::chrono::steady_clock::now()),
          operation_id(diagnostic::NewCorrelationId()) {
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::DeviceSwitchStarted;
        event.thread_role = diagnostic::ThreadRole::Media;
        event.media_kind = diagnostic::MediaKind::Video;
        event.context.operation_id = operation_id;
        diagnostic::EmitBusinessEvent(event);
    }

    ~DeviceSwitchSpan() {
        Finish(diagnostic::Outcome::Cancelled,
               diagnostic::DeviceSwitchReason::Superseded);
    }

    void Finish(diagnostic::Outcome outcome,
                diagnostic::DeviceSwitchReason reason) noexcept {
        bool expected = false;
        if (!terminal.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) return;
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::DeviceSwitchTerminal;
        event.thread_role = diagnostic::ThreadRole::Media;
        event.media_kind = diagnostic::MediaKind::Video;
        event.context.operation_id = operation_id;
        event.outcome = outcome;
        event.device_switch_reason = reason;
        event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        diagnostic::EmitBusinessEvent(event);
    }

    std::chrono::steady_clock::time_point started_at;
    diagnostic::OpaqueId operation_id;
    std::atomic<bool> terminal{false};
};

namespace {

void EmitCaptureTerminal(diagnostic::Outcome outcome,
                         diagnostic::DeviceSwitchReason reason,
                         diagnostic::OpaqueId parent = {}) noexcept {
    diagnostic::Event event;
    event.kind = diagnostic::EventKind::DeviceCaptureTerminal;
    event.thread_role = diagnostic::ThreadRole::Media;
    event.media_kind = diagnostic::MediaKind::Video;
    event.context.parent_operation_id = parent;
    event.outcome = outcome;
    event.device_switch_reason = reason;
    diagnostic::EmitBusinessEvent(event);
}

constexpr int kFullHdWidth = 1920;
constexpr int kFullHdHeight = 1080;
constexpr std::int64_t kFullHdPixels =
    static_cast<std::int64_t>(kFullHdWidth) * kFullHdHeight;

std::int64_t PixelCount(const CameraResolution& resolution) {
    return static_cast<std::int64_t>(resolution.width) * resolution.height;
}

bool SameCaptureConfig(const DShowCaptureConfig& lhs, const DShowCaptureConfig& rhs) {
    return lhs.device_path == rhs.device_path &&
        lhs.width == rhs.width &&
        lhs.height == rhs.height &&
        lhs.fps == rhs.fps &&
        lhs.preferred_format == rhs.preferred_format &&
        lhs.output_format == rhs.output_format &&
        lhs.flip_vertically == rhs.flip_vertically &&
        lhs.auto_reconnect == rhs.auto_reconnect;
}

} // namespace

// =========================================================================
// DShowCameraCapturer
// =========================================================================

DShowCameraCapturer::DShowCameraCapturer(std::shared_ptr<DShowVideoCapture> capture)
    : capture_(std::move(capture)) {
    if (!capture_) {
        capture_ = DShowVideoCapture::Create();
    }
}

DShowCameraCapturer::~DShowCameraCapturer() {
    Stop();
}

bool DShowCameraCapturer::Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source) {
    if (!capture_) return false;
    return capture_->Init(config, std::move(video_source));
}

bool DShowCameraCapturer::Start() {
    if (!capture_) return false;
    return capture_->Start();
}

void DShowCameraCapturer::Stop() {
    if (capture_) {
        capture_->Stop();
    }
}

bool DShowCameraCapturer::IsRunning() const noexcept {
    return capture_ ? capture_->IsRunning() : false;
}

DShowCaptureConfig DShowCameraCapturer::GetConfig() const {
    return capture_ ? capture_->GetConfig() : DShowCaptureConfig{};
}

std::string DShowCameraCapturer::GetDevicePath() const {
    return capture_ ? capture_->GetConfig().device_path : std::string{};
}

// =========================================================================
// CameraSourceManager
// =========================================================================

std::shared_ptr<CameraSourceManager> CameraSourceManager::Create(
    std::shared_ptr<VideoSource> output_source,
    CapturerFactory factory) {
    return std::make_shared<CameraSourceManager>(std::move(output_source), std::move(factory));
}

CameraSourceManager::CameraSourceManager(std::shared_ptr<VideoSource> output_source, CapturerFactory factory)
    : output_source_(std::move(output_source)), factory_(std::move(factory)) {
    if (!factory_) {
        factory_ = []() -> std::shared_ptr<ICameraCapturer> {
            return std::make_shared<DShowCameraCapturer>();
        };
    }
}

CameraSourceManager::~CameraSourceManager() {
    Stop();
}

void CameraSourceManager::SetOutputSource(std::shared_ptr<VideoSource> output_source) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    output_source_ = std::move(output_source);
}

std::shared_ptr<VideoSource> CameraSourceManager::GetOutputSource() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return output_source_;
}

bool CameraSourceManager::Start(const DShowCaptureConfig& config) {
    return StartInitialCapture(config, std::nullopt) == CameraStartResult::Ready;
}

CameraStartResult CameraSourceManager::StartAndWaitForFirstFrame(
        const DShowCaptureConfig& config, int timeout_ms) {
    return StartInitialCapture(config, timeout_ms > 0 ? timeout_ms : 3000);
}

void CameraSourceManager::RetireInitialCapture(
        const std::shared_ptr<InitialCaptureAttempt>& attempt) {
    if (!attempt) return;
    std::lock_guard lock(attempt->lifecycle_mutex);
    if (!attempt->stopped && attempt->capturer) {
        attempt->stopped = true;
        attempt->capturer->Stop();
    }
}

CameraStartResult CameraSourceManager::StartInitialCapture(
        const DShowCaptureConfig& config,
        std::optional<int> first_frame_timeout_ms) {
    auto attempt = std::make_shared<InitialCaptureAttempt>();
    std::shared_ptr<InitialCaptureAttempt> old_initial;
    std::shared_ptr<InitialCaptureAttempt> old_active_initial;
    std::shared_ptr<ICameraCapturer> old_active;
    std::shared_ptr<ICameraCapturer> old_probe;
    uint64_t generation = 0;
    {
        std::lock_guard lock(state_mutex_);
        old_initial = std::move(initial_capture_);
        old_active_initial = std::move(active_initial_capture_);
        if (old_initial) old_initial->cancelled = true;
        if (old_active_initial) old_active_initial->cancelled = true;
        old_active = std::move(active_capturer_);
        old_probe = std::move(probing_capturer_);
        generation = ++switch_generation_;
        initial_capture_ = attempt;
        current_config_ = config;
        active_device_path_ = config.device_path;
        switch_state_ = CameraSwitchState::Idle;
    }
    if (old_initial) old_initial->ready.notify_all();
    RetireInitialCapture(old_initial);
    RetireInitialCapture(old_active_initial);
    if (old_active && !old_active_initial) old_active->Stop();
    if (old_probe) old_probe->Stop();

    std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();
    std::weak_ptr<InitialCaptureAttempt> weak_attempt = attempt;
    VideoSource::FrameSink relay = [weak_self, weak_attempt, generation](
            const VideoFrame& frame, const VideoCaptureOptions& options) {
        if (frame.width() <= 0 || frame.height() <= 0 || frame.dataSize() == 0) return;
        const auto self = weak_self.lock();
        const auto current = weak_attempt.lock();
        if (!self || !current) return;
        std::shared_ptr<VideoSource> out;
        bool first_frame_received = false;
        {
            std::lock_guard lock(self->state_mutex_);
            if (current->cancelled) return;
            if (self->active_initial_capture_ == current &&
                self->active_capturer_ == current->capturer) {
                // The worker must finish delivering the cached first frame
                // before a newer driver callback can reach the output. Drop
                // callbacks in this short commit window without waiting on it.
                if (current->first_frame_delivery_pending) return;
                // The active camera remains valid while a replacement probes.
                out = self->output_source_;
            } else if (self->initial_capture_ == current &&
                       self->switch_generation_.load() == generation) {
                if (!current->first_frame) {
                    current->first_frame = frame;
                    current->first_frame_options = options;
                    first_frame_received = true;
                }
            }
        }
        if (first_frame_received) current->ready.notify_all();
        if (out) out->captureFrame(frame, options);
    };

    bool graph_started = false;
    auto failure_reason = diagnostic::DeviceSwitchReason::Superseded;
    {
        std::lock_guard lifecycle_lock(attempt->lifecycle_mutex);
        const auto still_owned = [&] {
            std::lock_guard lock(state_mutex_);
            return !attempt->cancelled && initial_capture_ == attempt &&
                switch_generation_.load() == generation;
        };
        if (still_owned()) {
            attempt->capturer = factory_();
            if (!attempt->capturer) {
                failure_reason = diagnostic::DeviceSwitchReason::FactoryFailed;
            } else {
                auto source = std::make_shared<VideoSource>(config.width, config.height);
                source->addSink(relay);
                if (!attempt->capturer->Init(config, std::move(source))) {
                    failure_reason = diagnostic::DeviceSwitchReason::InitFailed;
                } else if (still_owned()) {
                    graph_started = attempt->capturer->Start();
                    failure_reason = diagnostic::DeviceSwitchReason::StartFailed;
                }
            }
        }
    }

    std::optional<VideoFrame> first_frame;
    VideoCaptureOptions first_frame_options;
    bool success = false;
    {
        std::unique_lock lock(state_mutex_);
        const auto owns_attempt = [&] {
            return !attempt->cancelled && initial_capture_ == attempt &&
                switch_generation_.load() == generation;
        };
        if (graph_started && owns_attempt() && first_frame_timeout_ms) {
            const bool ready = attempt->ready.wait_for(lock,
                std::chrono::milliseconds(*first_frame_timeout_ms), [&] {
                    return !owns_attempt() || attempt->first_frame.has_value();
                });
            if (!ready) failure_reason = diagnostic::DeviceSwitchReason::ProbeTimeout;
        }
        if (!owns_attempt()) {
            failure_reason = diagnostic::DeviceSwitchReason::Superseded;
        } else if (graph_started &&
                   (!first_frame_timeout_ms || attempt->first_frame)) {
            active_capturer_ = attempt->capturer;
            active_initial_capture_ = attempt;
            initial_capture_.reset();
            first_frame = std::move(attempt->first_frame);
            first_frame_options = attempt->first_frame_options;
            attempt->first_frame_delivery_pending = first_frame.has_value();
            success = true;
        } else {
            attempt->cancelled = true;
            initial_capture_.reset();
            switch_state_ = CameraSwitchState::Aborted;
        }
    }
    if (!success) RetireInitialCapture(attempt);
    if (success && first_frame) {
        std::shared_ptr<VideoSource> out;
        {
            std::lock_guard lock(state_mutex_);
            if (!attempt->cancelled && active_initial_capture_ == attempt &&
                active_capturer_ == attempt->capturer) {
                out = output_source_;
            }
        }
        if (out) out->captureFrame(*first_frame, first_frame_options);
        {
            std::lock_guard lock(state_mutex_);
            attempt->first_frame_delivery_pending = false;
        }
    }
    const auto outcome = success ? diagnostic::Outcome::Success
        : failure_reason == diagnostic::DeviceSwitchReason::Superseded
            ? diagnostic::Outcome::Cancelled
        : failure_reason == diagnostic::DeviceSwitchReason::ProbeTimeout
            ? diagnostic::Outcome::Timeout : diagnostic::Outcome::Failure;
    EmitCaptureTerminal(outcome, success
        ? diagnostic::DeviceSwitchReason::Completed : failure_reason);
    if (success) return CameraStartResult::Ready;
    switch (failure_reason) {
    case diagnostic::DeviceSwitchReason::FactoryFailed:
        return CameraStartResult::FactoryFailed;
    case diagnostic::DeviceSwitchReason::InitFailed:
        return CameraStartResult::InitFailed;
    case diagnostic::DeviceSwitchReason::StartFailed:
        return CameraStartResult::StartFailed;
    case diagnostic::DeviceSwitchReason::ProbeTimeout:
        return CameraStartResult::FirstFrameTimeout;
    default:
        return CameraStartResult::Cancelled;
    }
}

void CameraSourceManager::Stop() {
    std::shared_ptr<ICameraCapturer> act;
    std::shared_ptr<ICameraCapturer> prb;
    std::shared_ptr<InitialCaptureAttempt> initial;
    std::shared_ptr<InitialCaptureAttempt> active_initial;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        switch_generation_++;
        switch_state_ = CameraSwitchState::Idle;
        act = std::move(active_capturer_);
        prb = std::move(probing_capturer_);
        initial = std::move(initial_capture_);
        active_initial = std::move(active_initial_capture_);
        if (initial) initial->cancelled = true;
        if (active_initial) active_initial->cancelled = true;
    }

    if (initial) initial->ready.notify_all();
    RetireInitialCapture(initial);
    RetireInitialCapture(active_initial);
    if (act && !active_initial) {
        act->Stop();
    }
    if (prb) {
        prb->Stop();
    }
}

bool CameraSourceManager::IsRunning() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_capturer_ && active_capturer_->IsRunning();
}

std::string CameraSourceManager::GetActiveDevicePath() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_device_path_;
}

DShowCaptureConfig CameraSourceManager::GetActiveConfig() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_config_;
}

CameraSwitchState CameraSourceManager::GetSwitchState() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return switch_state_;
}

std::vector<CameraResolution> CameraSourceManager::GetSupportedResolutions(
        const std::string& device_path) {
    const auto devices = DShowEnumerator::EnumerateVideoDevices();
    const DShowDeviceInfo* selected_device = nullptr;
    for (const auto& device : devices) {
        if ((!device_path.empty() &&
             (device.path == device_path || device.name == device_path)) ||
            (device_path.empty() && device.is_default)) {
            selected_device = &device;
            break;
        }
    }
    if (!selected_device && device_path.empty() && !devices.empty()) {
        selected_device = &devices.front();
    }
    if (!selected_device) {
        return {};
    }

    return GetSupportedResolutions(*selected_device);
}

std::vector<CameraResolution> CameraSourceManager::GetSupportedResolutions(
        const DShowDeviceInfo& device) {
    std::vector<CameraResolution> resolutions;
    for (const auto& capability : device.capabilities) {
        if (capability.width <= 0 || capability.height <= 0) {
            continue;
        }
        const int max_fps = (std::max)({0, capability.min_fps, capability.max_fps});
        const auto existing = std::find_if(
            resolutions.begin(), resolutions.end(),
            [&capability](const CameraResolution& resolution) {
                return resolution.width == capability.width &&
                    resolution.height == capability.height;
            });
        if (existing != resolutions.end()) {
            existing->max_fps = (std::max)(existing->max_fps, max_fps);
        } else {
            resolutions.push_back({capability.width, capability.height, max_fps});
        }
    }

    std::sort(resolutions.begin(), resolutions.end(),
              [](const CameraResolution& lhs, const CameraResolution& rhs) {
        return std::tuple{PixelCount(lhs), lhs.width, lhs.height, lhs.max_fps} >
            std::tuple{PixelCount(rhs), rhs.width, rhs.height, rhs.max_fps};
    });
    return resolutions;
}

std::optional<CameraResolution> CameraSourceManager::SelectDefaultResolution(
        const std::vector<CameraResolution>& resolutions) {
    const auto valid = [](const CameraResolution& resolution) {
        return resolution.width > 0 && resolution.height > 0;
    };
    const auto highest = std::max_element(
        resolutions.begin(), resolutions.end(),
        [&valid](const CameraResolution& lhs, const CameraResolution& rhs) {
            if (!valid(lhs)) return valid(rhs);
            if (!valid(rhs)) return false;
            return std::tuple{PixelCount(lhs), lhs.width, lhs.height, lhs.max_fps} <
                std::tuple{PixelCount(rhs), rhs.width, rhs.height, rhs.max_fps};
        });
    if (highest == resolutions.end() || !valid(*highest)) {
        return std::nullopt;
    }
    if (PixelCount(*highest) < kFullHdPixels) {
        return *highest;
    }

    const CameraResolution* closest = nullptr;
    std::tuple<std::int64_t, std::int64_t, std::int64_t, int> closest_rank;
    for (const auto& resolution : resolutions) {
        if (!valid(resolution)) {
            continue;
        }
        const int target_width = resolution.width >= resolution.height
            ? kFullHdWidth
            : kFullHdHeight;
        const int target_height = resolution.width >= resolution.height
            ? kFullHdHeight
            : kFullHdWidth;
        const auto width_delta = static_cast<std::int64_t>(resolution.width) - target_width;
        const auto height_delta = static_cast<std::int64_t>(resolution.height) - target_height;
        const auto pixel_delta = PixelCount(resolution) >= kFullHdPixels
            ? PixelCount(resolution) - kFullHdPixels
            : kFullHdPixels - PixelCount(resolution);
        const auto rank = std::tuple{
            width_delta * width_delta + height_delta * height_delta,
            pixel_delta,
            PixelCount(resolution),
            -resolution.max_fps};
        if (!closest || rank < closest_rank) {
            closest = &resolution;
            closest_rank = rank;
        }
    }
    return closest ? std::optional<CameraResolution>(*closest) : std::nullopt;
}

void CameraSourceManager::SwitchDeviceAsync(const std::string& target_device_path,
                                            int timeout_ms,
                                            SwitchCallback callback) {
    auto target_config = GetActiveConfig();
    target_config.device_path = target_device_path;
    ReconfigureAsync(target_config, timeout_ms, std::move(callback));
}

void CameraSourceManager::ReconfigureAsync(const DShowCaptureConfig& target_config,
                                           int timeout_ms,
                                           SwitchCallback callback) {
    std::shared_ptr<InitialCaptureAttempt> initial;
    {
        std::lock_guard lock(state_mutex_);
        initial = std::move(initial_capture_);
        if (initial) {
            initial->cancelled = true;
            ++switch_generation_;
        }
    }
    if (initial) initial->ready.notify_all();
    RetireInitialCapture(initial);

    auto switch_span = std::make_shared<DeviceSwitchSpan>();
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    if (target_config.width <= 0 || target_config.height <= 0 || target_config.fps <= 0) {
        switch_span->Finish(diagnostic::Outcome::Failure,
            diagnostic::DeviceSwitchReason::InvalidConfiguration);
        DeliverSwitchResult(std::move(callback), false, "Invalid camera capture configuration");
        return;
    }

    std::shared_ptr<ICameraCapturer> old_probe;
    std::shared_ptr<ICameraCapturer> new_probe;
    std::shared_ptr<ICameraCapturer> active_to_pause;
    uint64_t gen = 0;
    DShowCaptureConfig probe_config;
    bool has_immediate_result = false;
    bool immediate_success = false;
    auto immediate_reason = diagnostic::DeviceSwitchReason::AlreadyActive;
    std::string immediate_error;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (SameCaptureConfig(target_config, current_config_) &&
            active_capturer_ && active_capturer_->IsRunning()) {
            if (probing_capturer_) {
                ++switch_generation_;
                old_probe = std::move(probing_capturer_);
                switch_state_ = CameraSwitchState::Idle;
            }
            has_immediate_result = true;
            immediate_success = true;
        } else {
            gen = ++switch_generation_;
            switch_state_ = CameraSwitchState::Probing;

            if (probing_capturer_) {
                old_probe = std::move(probing_capturer_);
            }

            probe_config = target_config;

            new_probe = factory_();
            if (!new_probe) {
                switch_state_ = CameraSwitchState::Aborted;
                has_immediate_result = true;
                immediate_error = "Factory failed to create capturer";
                immediate_reason = diagnostic::DeviceSwitchReason::FactoryFailed;
            } else {
                auto probe_source = std::make_shared<VideoSource>(probe_config.width, probe_config.height);
                auto first_frame_handled = std::make_shared<std::atomic<bool>>(false);
                std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();

                probe_source->addSink([weak_self, gen, new_probe, probe_config,
                                       callback, switch_span, first_frame_handled]
                                      (const VideoFrame& frame, const VideoCaptureOptions& options) {
                    if (first_frame_handled->exchange(true)) {
                        if (auto self = weak_self.lock()) {
                            std::shared_ptr<VideoSource> out;
                            {
                                std::lock_guard<std::mutex> lock(self->state_mutex_);
                                if (self->switch_generation_.load() == gen &&
                                    self->active_capturer_ == new_probe) {
                                    out = self->output_source_;
                                }
                            }
                            if (out) {
                                out->captureFrame(frame, options);
                            }
                        }
                        return;
                    }
                    if (auto self = weak_self.lock()) {
                        self->HandleProbeFrameReceived(
                            gen, new_probe, probe_config, frame, options,
                            callback, switch_span);
                    }
                });

                if (!new_probe->Init(probe_config, probe_source)) {
                    switch_state_ = CameraSwitchState::Aborted;
                    has_immediate_result = true;
                    immediate_error = "Failed to initialize replacement camera configuration";
                    immediate_reason = diagnostic::DeviceSwitchReason::InitFailed;
                } else {
                    probing_capturer_ = new_probe;
                    if (active_capturer_ && active_capturer_->IsRunning() &&
                        target_config.device_path == active_device_path_ &&
                        !SameCaptureConfig(target_config, current_config_)) {
                        active_to_pause = active_capturer_;
                    }
                }
            }
        }
    }

    if (old_probe) {
        old_probe->Stop();
    }

    if (has_immediate_result) {
        switch_span->Finish(immediate_success ? diagnostic::Outcome::Success
            : diagnostic::Outcome::Failure, immediate_reason);
        DeliverSwitchResult(std::move(callback), immediate_success, immediate_error);
        return;
    }

    bool probe_started = new_probe->Start();
    if (!probe_started && active_to_pause) {
        // Most physical cameras are exclusive. Retry after releasing the old
        // graph, and restore it if the new configuration cannot be started.
        active_to_pause->Stop();
        probe_started = new_probe->Start();
    }

    bool owns_probe = false;
    bool committed_during_start = false;
    std::shared_ptr<ICameraCapturer> active_to_resume;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        owns_probe = switch_generation_.load() == gen &&
            switch_state_ == CameraSwitchState::Probing &&
            probing_capturer_ == new_probe;
        committed_during_start = switch_generation_.load() == gen &&
            switch_state_ == CameraSwitchState::Committed &&
            active_capturer_ == new_probe;
        if (!probe_started && owns_probe) {
            probing_capturer_.reset();
            switch_state_ = CameraSwitchState::Aborted;
            if (active_capturer_ && !active_capturer_->IsRunning()) {
                active_to_resume = active_capturer_;
            }
        }
    }

    if (committed_during_start) {
        return;
    }
    if (!owns_probe) {
        if (probe_started) {
            new_probe->Stop();
        }
        switch_span->Finish(diagnostic::Outcome::Cancelled,
            diagnostic::DeviceSwitchReason::Superseded);
        return;
    }
    if (!probe_started) {
        new_probe->Stop();
        if (active_to_resume && !active_to_resume->Start()) {
            EmitCaptureTerminal(diagnostic::Outcome::Failure,
                diagnostic::DeviceSwitchReason::StartFailed, switch_span->operation_id);
        }
        switch_span->Finish(diagnostic::Outcome::Failure,
            diagnostic::DeviceSwitchReason::StartFailed);
        DeliverSwitchResult(
            std::move(callback), false, "Failed to start replacement camera configuration");
        return;
    }


    std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();
    ScheduleTimeout(timeout_ms, [weak_self, gen, callback, switch_span]() mutable {
        if (auto self = weak_self.lock()) {
            self->HandleProbeTimeout(gen, std::move(callback), switch_span);
        }
    });
}

void CameraSourceManager::HandleProbeFrameReceived(uint64_t generation,
                                                   std::shared_ptr<ICameraCapturer> probe_capturer,
                                                   const DShowCaptureConfig& target_config,
                                                   const VideoFrame& frame,
                                                   const VideoCaptureOptions& options,
                                                   SwitchCallback callback,
                                                   std::shared_ptr<DeviceSwitchSpan> switch_span) {
    std::shared_ptr<ICameraCapturer> old_active;
    std::shared_ptr<InitialCaptureAttempt> old_initial;
    std::shared_ptr<VideoSource> out_src;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (switch_generation_.load() != generation || switch_state_ != CameraSwitchState::Probing) {
            switch_span->Finish(diagnostic::Outcome::Cancelled,
                diagnostic::DeviceSwitchReason::Superseded);
            return;
        }


        old_active = std::move(active_capturer_);
        old_initial = std::move(active_initial_capture_);
        if (old_initial) old_initial->cancelled = true;
        active_capturer_ = std::move(probe_capturer);
        probing_capturer_.reset();
        active_device_path_ = target_config.device_path;
        current_config_ = target_config;
        switch_state_ = CameraSwitchState::Committed;

        out_src = output_source_;
    }

    // 交付首帧到真实推流与预览
    if (out_src) {
        out_src->captureFrame(frame, options);
    }

    // 异步安全释放旧设备
    if (old_active) {
        ScheduleCleanup([old_active, old_initial]() {
            if (old_initial) RetireInitialCapture(old_initial);
            else old_active->Stop();
        });
    }

    switch_span->Finish(diagnostic::Outcome::Success,
        diagnostic::DeviceSwitchReason::Completed);
    DeliverSwitchResult(std::move(callback), true, "");
}

void CameraSourceManager::HandleProbeTimeout(uint64_t generation,
                                             SwitchCallback callback,
                                             std::shared_ptr<DeviceSwitchSpan> switch_span) {
    std::shared_ptr<ICameraCapturer> timed_out_probe;
    std::shared_ptr<ICameraCapturer> active_to_resume;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (switch_generation_.load() != generation || switch_state_ != CameraSwitchState::Probing) {
            switch_span->Finish(diagnostic::Outcome::Cancelled,
                diagnostic::DeviceSwitchReason::Superseded);
            return; // 已经完成提交或已被新代际取代
        }


        switch_state_ = CameraSwitchState::Aborted;
        timed_out_probe = std::move(probing_capturer_);
        if (active_capturer_ && !active_capturer_->IsRunning()) {
            active_to_resume = active_capturer_;
        }
    }

    if (timed_out_probe && active_to_resume) {
        // Release an exclusive device before rebuilding the previous graph.
        timed_out_probe->Stop();
    } else if (timed_out_probe) {
        ScheduleCleanup([timed_out_probe]() {
            timed_out_probe->Stop();
        });
    }
    if (active_to_resume && !active_to_resume->Start()) {
        EmitCaptureTerminal(diagnostic::Outcome::Failure,
            diagnostic::DeviceSwitchReason::StartFailed, switch_span->operation_id);
    }

    switch_span->Finish(diagnostic::Outcome::Timeout,
        diagnostic::DeviceSwitchReason::ProbeTimeout);

    DeliverSwitchResult(
        std::move(callback),
        false,
        "Timeout waiting for first usable frame from target camera");
}

void CameraSourceManager::DeliverSwitchResult(
        SwitchCallback callback,
        bool success,
        const std::string& error_message) {
    if (before_terminal_delivery_hook_) {
        before_terminal_delivery_hook_();
    }
    if (callback) {
        callback(success, error_message);
    }
}

void CameraSourceManager::ScheduleTimeout(int timeout_ms, ScheduledTask task) {
    if (timeout_scheduler_override_) {
        timeout_scheduler_override_(timeout_ms, std::move(task));
        return;
    }
    std::thread([timeout_ms, task = std::move(task)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        task();
    }).detach();
}

void CameraSourceManager::ScheduleCleanup(ScheduledTask task) {
    if (cleanup_scheduler_override_) {
        cleanup_scheduler_override_(std::move(task));
        return;
    }
    std::thread([task = std::move(task)]() mutable {
        task();
    }).detach();
}

} // namespace livekit
