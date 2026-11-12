#include "linux_camera_capture.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include "api/video/video_frame_buffer.h"
#include "modules/video_capture/video_capture_factory.h"
#include "../rtc/video_frame.h"

namespace livekit::media {
namespace {

std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> DeviceInfo() {
    return std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
}

VideoRotation ToRotation(webrtc::VideoRotation rotation) {
    switch (rotation) {
    case webrtc::kVideoRotation_90: return VideoRotation::VIDEO_ROTATION_90;
    case webrtc::kVideoRotation_180: return VideoRotation::VIDEO_ROTATION_180;
    case webrtc::kVideoRotation_270: return VideoRotation::VIDEO_ROTATION_270;
    case webrtc::kVideoRotation_0:
    default: return VideoRotation::VIDEO_ROTATION_0;
    }
}

} // namespace

LinuxCameraCapture::~LinuxCameraCapture() {
    Stop();
}

std::vector<CameraDevice> LinuxCameraCapture::Enumerate() {
    std::vector<CameraDevice> devices;
    auto info = DeviceInfo();
    if (!info) return devices;

    const auto count = info->NumberOfDevices();
    devices.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        char name[webrtc::kVideoCaptureDeviceNameLength] = {};
        char id[webrtc::kVideoCaptureUniqueNameLength] = {};
        if (info->GetDeviceName(index, name, sizeof(name), id, sizeof(id)) != 0) {
            continue;
        }
        name[sizeof(name) - 1] = '\0';
        id[sizeof(id) - 1] = '\0';
        if (*id) devices.push_back({id, *name ? name : id});
    }
    return devices;
}

bool LinuxCameraCapture::Start(const std::string& device_id,
                               std::shared_ptr<VideoSource> source) {
    return Start(device_id, std::move(source), 640, 480, 30);
}

bool LinuxCameraCapture::Start(const std::string& device_id,
                               std::shared_ptr<VideoSource> source,
                               int width, int height, int fps) {
    if (device_id.empty() || !source || width <= 0 || height <= 0 || fps <= 0) return false;
    std::lock_guard lock(operation_mutex_);
    if (running_ && device_id_ == device_id && source_ == source &&
        width_ == width && height_ == height && fps_ == fps &&
        module_ && module_->CaptureStarted()) return true;

    const auto previous_id = device_id_;
    auto previous_source = source_;
    const bool was_running = running_;
    const int previous_width = width_, previous_height = height_, previous_fps = fps_;
    StopLocked();
    if (StartLocked(device_id, std::move(source), width, height, fps)) return true;
    if (was_running) StartLocked(previous_id, std::move(previous_source),
                                 previous_width, previous_height, previous_fps);
    return false;
}

bool LinuxCameraCapture::StartLocked(const std::string& device_id,
                                     std::shared_ptr<VideoSource> source,
                                     int width, int height, int fps) {
    auto info = DeviceInfo();
    if (!info) return false;

    webrtc::VideoCaptureCapability requested;
    requested.width = width;
    requested.height = height;
    requested.maxFPS = fps;
    webrtc::VideoCaptureCapability selected;
    if (info->GetBestMatchedCapability(device_id.c_str(), requested, selected) < 0 ||
        selected.width <= 0 || selected.height <= 0) {
        return false;
    }

    auto module = webrtc::VideoCaptureFactory::Create(device_id.c_str());
    if (!module) return false;
    {
        std::lock_guard lock(callback_mutex_);
        source_ = std::move(source);
        active_ = true;
    }
    module->RegisterCaptureDataCallback(this);
    if (module->StartCapture(selected) != 0 || !module->CaptureStarted()) {
        {
            std::lock_guard lock(callback_mutex_);
            active_ = false;
        }
        module->DeRegisterCaptureDataCallback();
        module->StopCapture();
        std::unique_lock lock(callback_mutex_);
        callbacks_drained_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
        source_.reset();
        return false;
    }

    module_ = std::move(module);
    device_id_ = device_id;
    width_ = width;
    height_ = height;
    fps_ = fps;
    running_ = true;
    return true;
}

void LinuxCameraCapture::Stop() {
    std::lock_guard lock(operation_mutex_);
    StopLocked();
}

void LinuxCameraCapture::StopLocked() {
    {
        std::lock_guard lock(callback_mutex_);
        active_ = false;
    }
    if (module_) {
        module_->DeRegisterCaptureDataCallback();
        module_->StopCapture();
        module_ = nullptr;
    }
    std::unique_lock lock(callback_mutex_);
    callbacks_drained_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    source_.reset();
    device_id_.clear();
    running_ = false;
}

bool LinuxCameraCapture::Running() const {
    std::lock_guard lock(operation_mutex_);
    return running_ && module_ && module_->CaptureStarted();
}

void LinuxCameraCapture::OnFrame(const webrtc::VideoFrame& frame) {
    std::shared_ptr<VideoSource> source;
    {
        std::lock_guard lock(callback_mutex_);
        if (!active_ || !source_) return;
        ++callbacks_in_flight_;
        source = source_;
    }

    try {
        const auto buffer = frame.video_frame_buffer();
        const auto i420 = buffer ? buffer->ToI420() : nullptr;
        if (i420) {
            const int width = i420->width();
            const int height = i420->height();
            constexpr int kMaximumPixels = 3840 * 2160;
            if (width > 0 && height > 0 &&
                i420->StrideY() >= width &&
                i420->StrideU() >= (width + 1) / 2 &&
                i420->StrideV() >= (width + 1) / 2 &&
                static_cast<int64_t>(width) * height <= kMaximumPixels) {
                const int chroma_width = (width + 1) / 2;
                const int chroma_height = (height + 1) / 2;
                const size_t y_size = static_cast<size_t>(width) * height;
                const size_t chroma_size = static_cast<size_t>(chroma_width) * chroma_height;
                std::vector<uint8_t> pixels(y_size + 2 * chroma_size);
                const auto copy_plane = [](uint8_t* output, const uint8_t* input,
                                           int row_bytes, int rows, int stride) {
                    for (int row = 0; row < rows; ++row) {
                        std::memcpy(output + static_cast<size_t>(row) * row_bytes,
                                    input + static_cast<size_t>(row) * stride,
                                    static_cast<size_t>(row_bytes));
                    }
                };
                copy_plane(pixels.data(), i420->DataY(), width, height, i420->StrideY());
                copy_plane(pixels.data() + y_size, i420->DataU(),
                           chroma_width, chroma_height, i420->StrideU());
                copy_plane(pixels.data() + y_size + chroma_size, i420->DataV(),
                           chroma_width, chroma_height, i420->StrideV());
                source->captureFrame(
                    VideoFrame(width, height, VideoBufferType::I420, std::move(pixels)),
                    frame.timestamp_us(), ToRotation(frame.rotation()));
            }
        }
    } catch (...) {
        // A capture callback must not unwind into WebRTC's device thread.
    }

    {
        std::lock_guard lock(callback_mutex_);
        --callbacks_in_flight_;
        if (callbacks_in_flight_ == 0) callbacks_drained_.notify_all();
    }
}

} // namespace livekit::media
