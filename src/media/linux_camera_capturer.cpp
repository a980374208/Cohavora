#include "camera_source_manager.h"
#include "camera_enumerator.h"
#include "linux_camera_capture.h"

namespace livekit {
namespace {
class LinuxCameraCapturer final : public ICameraCapturer {
public:
    bool Init(const CameraCaptureConfig& config, std::shared_ptr<VideoSource> source) override {
        if (capture_.Running() || !source || config.width <= 0 || config.height <= 0 || config.fps <= 0) return false;
        config_ = config;
        if (config_.device_path.empty()) {
            const auto devices = media::LinuxCameraCapture::Enumerate();
            if (devices.empty()) return false;
            config_.device_path = devices.front().id;
        }
        source_ = std::move(source);
        return true;
    }
    bool Start() override {
        return capture_.Start(config_.device_path, source_, config_.width, config_.height, config_.fps);
    }
    void Stop() override { capture_.Stop(); }
    bool IsRunning() const noexcept override { return capture_.Running(); }
    CameraCaptureConfig GetConfig() const override { return config_; }
    std::string GetDevicePath() const override { return config_.device_path; }
private:
    media::LinuxCameraCapture capture_;
    CameraCaptureConfig config_;
    std::shared_ptr<VideoSource> source_;
};
}
std::shared_ptr<ICameraCapturer> CreatePlatformCameraCapturer() {
    return std::make_shared<LinuxCameraCapturer>();
}
}
