#include "camera_source_manager.h"
#include <iostream>
#include <spdlog/spdlog.h>

namespace livekit {

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

void CameraSourceManager::SwitchDeviceAsync(const std::string& target_device_path,
                                            int timeout_ms,
                                            SwitchCallback callback) {
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    std::shared_ptr<ICameraCapturer> old_probe;
    std::shared_ptr<ICameraCapturer> new_probe;
    uint64_t gen = 0;
    DShowCaptureConfig probe_config;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (target_device_path == active_device_path_ && active_capturer_ && active_capturer_->IsRunning()) {
            spdlog::info("[CameraSourceManager] Target camera is already active: {}", target_device_path);
            if (callback) {
                callback(true, "");
            }
            return;
        }

        gen = ++switch_generation_;
        switch_state_ = CameraSwitchState::Probing;

        if (probing_capturer_) {
            old_probe = std::move(probing_capturer_);
        }

        probe_config = current_config_;
        probe_config.device_path = target_device_path;

        new_probe = factory_();
        if (!new_probe) {
            switch_state_ = CameraSwitchState::Aborted;
            spdlog::error("[CameraSourceManager] Factory returned null capturer during switch");
            if (callback) {
                callback(false, "Factory failed to create capturer");
            }
            return;
        }

        auto probe_source = std::make_shared<VideoSource>(probe_config.width, probe_config.height);
        auto first_frame_handled = std::make_shared<std::atomic<bool>>(false);
        std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();

        probe_source->addSink([weak_self, gen, new_probe, target_device_path, callback, first_frame_handled]
                              (const VideoFrame& frame, const VideoCaptureOptions& options) {
            if (first_frame_handled->exchange(true)) {
                if (auto self = weak_self.lock()) {
                    if (auto out = self->GetOutputSource()) {
                        out->captureFrame(frame, options);
                    }
                }
                return;
            }
            if (auto self = weak_self.lock()) {
                self->HandleProbeFrameReceived(gen, new_probe, target_device_path, frame, options, callback);
            }
        });

        if (!new_probe->Init(probe_config, probe_source) || !new_probe->Start()) {
            switch_state_ = CameraSwitchState::Aborted;
            spdlog::error("[CameraSourceManager] Failed to init/start probe capturer for: {}", target_device_path);
            if (callback) {
                callback(false, "Failed to start replacement camera device");
            }
            return;
        }

        probing_capturer_ = new_probe;
    }

    if (old_probe) {
        old_probe->Stop();
    }

    spdlog::info("[CameraSourceManager] Probing replacement camera: {} (timeout: {}ms, gen: {})",
                 target_device_path, timeout_ms, gen);

    // 启动超时检测
    std::weak_ptr<CameraSourceManager> weak_self = shared_from_this();
    std::thread([weak_self, gen, timeout_ms, callback]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        if (auto self = weak_self.lock()) {
            self->HandleProbeTimeout(gen, callback);
        }
    }).detach();
}

void CameraSourceManager::HandleProbeFrameReceived(uint64_t generation,
                                                   std::shared_ptr<ICameraCapturer> probe_capturer,
                                                   const std::string& target_device_path,
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

        spdlog::info("[CameraSourceManager] Verified first usable frame from target: {} (gen: {}). Committing switch.",
                     target_device_path, generation);

        old_active = std::move(active_capturer_);
        active_capturer_ = std::move(probe_capturer);
        probing_capturer_.reset();
        active_device_path_ = target_device_path;
        current_config_.device_path = target_device_path;
        switch_state_ = CameraSwitchState::Committed;

        out_src = output_source_;
    }

    // 交付首帧到真实推流与预览
    if (out_src) {
        out_src->captureFrame(frame, options);
    }

    // 异步安全释放旧设备
    if (old_active) {
        std::thread([old_active]() {
            old_active->Stop();
        }).detach();
    }

    if (callback) {
        callback(true, "");
    }
}

void CameraSourceManager::HandleProbeTimeout(uint64_t generation, SwitchCallback callback) {
    std::shared_ptr<ICameraCapturer> timed_out_probe;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (switch_generation_.load() != generation || switch_state_ != CameraSwitchState::Probing) {
            return; // 已经完成提交或已被新代际取代
        }

        spdlog::warn("[CameraSourceManager] Camera switch timed out (gen: {}). Rolling back to active capturer.",
                     generation);

        switch_state_ = CameraSwitchState::Aborted;
        timed_out_probe = std::move(probing_capturer_);
    }

    if (timed_out_probe) {
        std::thread([timed_out_probe]() {
            timed_out_probe->Stop();
        }).detach();
    }

    if (callback) {
        callback(false, "Timeout waiting for first usable frame from target camera");
    }
}

} // namespace livekit
