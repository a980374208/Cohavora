#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api/scoped_refptr.h"
#include "modules/video_capture/video_capture.h"
#include "../rtc/video_source.h"

namespace livekit::media {

struct CameraDevice {
    std::string id;
    std::string name;
};

class LinuxCameraCapture final : private webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    LinuxCameraCapture() = default;
    ~LinuxCameraCapture() override;

    LinuxCameraCapture(const LinuxCameraCapture&) = delete;
    LinuxCameraCapture& operator=(const LinuxCameraCapture&) = delete;

    static std::vector<CameraDevice> Enumerate();

    // A failed switch restores the previously running device when possible.
    bool Start(const std::string& device_id, std::shared_ptr<VideoSource> source);
    bool Start(const std::string& device_id, std::shared_ptr<VideoSource> source,
               int width, int height, int fps);
    void Stop();
    bool Running() const;

private:
    void OnFrame(const webrtc::VideoFrame& frame) override;
    bool StartLocked(const std::string& device_id, std::shared_ptr<VideoSource> source,
                     int width, int height, int fps);
    void StopLocked();

    mutable std::mutex operation_mutex_;
    std::mutex callback_mutex_;
    std::condition_variable callbacks_drained_;
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> module_;
    std::shared_ptr<VideoSource> source_;
    std::string device_id_;
    size_t callbacks_in_flight_ = 0;
    bool active_ = false;
    bool running_ = false;
    int width_ = 640, height_ = 480, fps_ = 30;
};

} // namespace livekit::media
