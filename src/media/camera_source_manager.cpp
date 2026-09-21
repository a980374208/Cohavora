#include "camera_source_manager.h"
#include "dshow_enumerator.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <spdlog/spdlog.h>

namespace livekit {

namespace {

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
    std::shared_ptr<ICameraCapturer> old_cap;
    std::shared_ptr<ICameraCapturer> new_cap;
    std::shared_ptr<VideoSource> relay_source;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (probing_capturer_) {
            probing_capturer_->Stop();
            probing_capturer_.reset();
        }
        old_cap = active_capturer_;
        active_capturer_.reset();

        current_config_ = config;
        active_device_path_ = config.device_path;
        switch_state_ = CameraSwitchState::Idle;
        switch_generation_++;

        new_cap = factory_();
        if (!new_cap) {
            spdlog::error("[CameraSourceManager] Failed to instantiate capturer from factory");
            return false;
        }

        relay_source = std::make_shared<VideoSource>(config.width, config.height);
        std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();
        relay_source->addSink([weak_self](const VideoFrame& frame, const VideoCaptureOptions& options) {
            if (auto self = weak_self.lock()) {
                if (auto out = self->GetOutputSource()) {
                    out->captureFrame(frame, options);
                }
            }
        });

        if (!new_cap->Init(config, relay_source) || !new_cap->Start()) {
            spdlog::error("[CameraSourceManager] Failed to initialize or start capturer for device: {}", config.device_path);
            return false;
        }

        active_capturer_ = new_cap;
    }

    if (old_cap) {
        old_cap->Stop();
    }
    spdlog::info("[CameraSourceManager] Successfully started active camera: {}", config.device_path);
    return true;
}

void CameraSourceManager::Stop() {
    std::shared_ptr<ICameraCapturer> act;
    std::shared_ptr<ICameraCapturer> prb;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        switch_generation_++;
        switch_state_ = CameraSwitchState::Idle;
        act = std::move(active_capturer_);
        prb = std::move(probing_capturer_);
    }

    if (act) {
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

    std::vector<CameraResolution> resolutions;
    for (const auto& capability : selected_device->capabilities) {
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
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    if (target_config.width <= 0 || target_config.height <= 0 || target_config.fps <= 0) {
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
    std::string immediate_error;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (SameCaptureConfig(target_config, current_config_) &&
            active_capturer_ && active_capturer_->IsRunning()) {
            spdlog::info("[CameraSourceManager] Target camera configuration is already active: {} {}x{}@{}",
                         target_config.device_path, target_config.width, target_config.height,
                         target_config.fps);
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
                spdlog::error("[CameraSourceManager] Factory returned null capturer during switch");
                has_immediate_result = true;
                immediate_error = "Factory failed to create capturer";
            } else {
                auto probe_source = std::make_shared<VideoSource>(probe_config.width, probe_config.height);
                auto first_frame_handled = std::make_shared<std::atomic<bool>>(false);
                std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();

                probe_source->addSink([weak_self, gen, new_probe, probe_config, callback, first_frame_handled]
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
                            gen, new_probe, probe_config, frame, options, callback);
                    }
                });

                if (!new_probe->Init(probe_config, probe_source)) {
                    switch_state_ = CameraSwitchState::Aborted;
                    spdlog::error("[CameraSourceManager] Failed to initialize probe capturer for: {} {}x{}@{}",
                                  probe_config.device_path, probe_config.width,
                                  probe_config.height, probe_config.fps);
                    has_immediate_result = true;
                    immediate_error = "Failed to initialize replacement camera configuration";
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
        return;
    }
    if (!probe_started) {
        spdlog::error("[CameraSourceManager] Failed to start probe capturer for: {} {}x{}@{}",
                      probe_config.device_path, probe_config.width,
                      probe_config.height, probe_config.fps);
        new_probe->Stop();
        if (active_to_resume && !active_to_resume->Start()) {
            spdlog::error("[CameraSourceManager] Failed to restore previous camera after reconfiguration failure");
        }
        DeliverSwitchResult(
            std::move(callback), false, "Failed to start replacement camera configuration");
        return;
    }

    spdlog::info("[CameraSourceManager] Probing replacement camera: {} {}x{}@{} (timeout: {}ms, gen: {})",
                 probe_config.device_path, probe_config.width, probe_config.height,
                 probe_config.fps, timeout_ms, gen);

    std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();
    ScheduleTimeout(timeout_ms, [weak_self, gen, callback]() mutable {
        if (auto self = weak_self.lock()) {
            self->HandleProbeTimeout(gen, std::move(callback));
        }
    });
}

void CameraSourceManager::HandleProbeFrameReceived(uint64_t generation,
                                                   std::shared_ptr<ICameraCapturer> probe_capturer,
                                                   const DShowCaptureConfig& target_config,
                                                   const VideoFrame& frame,
                                                   const VideoCaptureOptions& options,
                                                   SwitchCallback callback) {
    std::shared_ptr<ICameraCapturer> old_active;
    std::shared_ptr<VideoSource> out_src;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (switch_generation_.load() != generation || switch_state_ != CameraSwitchState::Probing) {
            spdlog::warn("[CameraSourceManager] Ignore stale probe frame (gen: {}, cur: {})",
                         generation, switch_generation_.load());
            return;
        }

        spdlog::info("[CameraSourceManager] Verified first usable frame from target: {} {}x{}@{} (gen: {}). Committing switch.",
                     target_config.device_path, target_config.width, target_config.height,
                     target_config.fps, generation);

        old_active = std::move(active_capturer_);
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
        ScheduleCleanup([old_active]() {
            old_active->Stop();
        });
    }

    DeliverSwitchResult(std::move(callback), true, "");
}

void CameraSourceManager::HandleProbeTimeout(uint64_t generation, SwitchCallback callback) {
    std::shared_ptr<ICameraCapturer> timed_out_probe;
    std::shared_ptr<ICameraCapturer> active_to_resume;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (switch_generation_.load() != generation || switch_state_ != CameraSwitchState::Probing) {
            return; // 已经完成提交或已被新代际取代
        }

        spdlog::warn("[CameraSourceManager] Camera switch timed out (gen: {}). Rolling back to active capturer.",
                     generation);

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
        spdlog::error("[CameraSourceManager] Failed to restore previous camera after probe timeout");
    }

    DeliverSwitchResult(
        std::move(callback),
        false,
        "Timeout waiting for first usable frame from target camera");
}

void CameraSourceManager::DeliverSwitchResult(
        SwitchCallback callback,
        bool success,
        const std::string& error_message) {
    if (before_terminal_delivery_for_test_) {
        before_terminal_delivery_for_test_();
    }
    if (callback) {
        callback(success, error_message);
    }
}

void CameraSourceManager::ScheduleTimeout(int timeout_ms, ScheduledTask task) {
    if (timeout_scheduler_for_test_) {
        timeout_scheduler_for_test_(timeout_ms, std::move(task));
        return;
    }
    std::thread([timeout_ms, task = std::move(task)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        task();
    }).detach();
}

void CameraSourceManager::ScheduleCleanup(ScheduledTask task) {
    if (cleanup_scheduler_for_test_) {
        cleanup_scheduler_for_test_(std::move(task));
        return;
    }
    std::thread([task = std::move(task)]() mutable {
        task();
    }).detach();
}

} // namespace livekit
