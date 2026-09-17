#pragma once

#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include "dshow_types.h"
#include "dshow_capture.h"
#include "video_source.h"

class CameraOwnerTestAccess;

namespace livekit {

enum class CameraSwitchState {
    Idle = 0,
    Probing,
    Committed,
    Aborted
};

// 抽象捕获器接口，便于测试与多后端扩展
class ICameraCapturer {
public:
    virtual ~ICameraCapturer() = default;
    virtual bool Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const noexcept = 0;
    virtual DShowCaptureConfig GetConfig() const = 0;
    virtual std::string GetDevicePath() const = 0;
};

// 默认 DirectShow 捕获器包装
class DShowCameraCapturer : public ICameraCapturer {
public:
    explicit DShowCameraCapturer(std::shared_ptr<DShowVideoCapture> capture = nullptr);
    ~DShowCameraCapturer() override;

    bool Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const noexcept override;
    DShowCaptureConfig GetConfig() const override;
    std::string GetDevicePath() const override;

private:
    std::shared_ptr<DShowVideoCapture> capture_;
};

using CapturerFactory = std::function<std::shared_ptr<ICameraCapturer>()>;

class CameraSourceManager : public std::enable_shared_from_this<CameraSourceManager> {
public:
    using SwitchCallback = std::function<void(bool success, const std::string& error_message)>;

    static std::shared_ptr<CameraSourceManager> Create(
        std::shared_ptr<VideoSource> output_source = nullptr,
        CapturerFactory factory = nullptr);

    CameraSourceManager(std::shared_ptr<VideoSource> output_source, CapturerFactory factory);
    ~CameraSourceManager();

    CameraSourceManager(const CameraSourceManager&) = delete;
    CameraSourceManager& operator=(const CameraSourceManager&) = delete;

    void SetOutputSource(std::shared_ptr<VideoSource> output_source);
    std::shared_ptr<VideoSource> GetOutputSource() const;

    // 启动初始采集设备
    bool Start(const DShowCaptureConfig& config);

    // 停止当前所有捕获器
    void Stop();

    // 活跃状态查询
    bool IsRunning() const;
    std::string GetActiveDevicePath() const;
    DShowCaptureConfig GetActiveConfig() const;
    CameraSwitchState GetSwitchState() const;

    // 活跃摄像头平滑热切换 (带首帧验证与超时回滚)
    void SwitchDeviceAsync(const std::string& target_device_path,
                           int timeout_ms,
                           SwitchCallback callback);

private:
    friend class ::CameraOwnerTestAccess;

    using ScheduledTask = std::function<void()>;
    using TimeoutScheduler = std::function<void(int timeout_ms, ScheduledTask task)>;
    using CleanupScheduler = std::function<void(ScheduledTask task)>;

    void HandleProbeFrameReceived(uint64_t generation,
                                  std::shared_ptr<ICameraCapturer> probe_capturer,
                                  const std::string& target_device_path,
                                  const VideoFrame& frame,
                                  const VideoCaptureOptions& options,
                                  SwitchCallback callback);

    void HandleProbeTimeout(uint64_t generation,
                            SwitchCallback callback);
    void DeliverSwitchResult(SwitchCallback callback,
                             bool success,
                             const std::string& error_message);
    void ScheduleTimeout(int timeout_ms, ScheduledTask task);
    void ScheduleCleanup(ScheduledTask task);

    mutable std::mutex state_mutex_;
    std::shared_ptr<VideoSource> output_source_;
    CapturerFactory factory_;

    std::shared_ptr<ICameraCapturer> active_capturer_;
    std::shared_ptr<ICameraCapturer> probing_capturer_;
    std::string active_device_path_;
    DShowCaptureConfig current_config_;

    CameraSwitchState switch_state_{CameraSwitchState::Idle};
    std::atomic<uint64_t> switch_generation_{0};

    // Default-empty seams are installed only by the dedicated deterministic
    // regression. Production keeps the existing detached scheduling behavior.
    TimeoutScheduler timeout_scheduler_for_test_;
    CleanupScheduler cleanup_scheduler_for_test_;
    ScheduledTask before_terminal_delivery_for_test_;
};

} // namespace livekit
