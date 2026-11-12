#include "camera_enumerator.h"
#ifdef _WIN32
#include "dshow_enumerator.h"
namespace livekit {
std::vector<CameraDeviceInfo> CameraEnumerator::EnumerateVideoDevices() {
    return DShowEnumerator::EnumerateVideoDevices();
}
std::vector<CameraCapability> CameraEnumerator::GetDeviceCapabilities(const std::string& path) {
    return DShowEnumerator::GetDeviceCapabilities(path);
}
}
#else
#include "linux_camera_capture.h"
#include "modules/video_capture/video_capture_factory.h"
namespace livekit {
std::vector<CameraCapability> CameraEnumerator::GetDeviceCapabilities(const std::string& path) {
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    std::vector<CameraCapability> result;
    if (!info) return result;
    const int count = info->NumberOfCapabilities(path.c_str());
    for (int i = 0; i < count; ++i) {
        webrtc::VideoCaptureCapability value;
        if (info->GetCapability(path.c_str(), i, value) != 0 ||
            value.width <= 0 || value.height <= 0 || value.maxFPS <= 0) continue;
        CameraCapability item;
        item.width = value.width;
        item.height = value.height;
        item.max_fps = value.maxFPS;
        switch (value.videoType) {
        case webrtc::VideoType::kI420: item.format = CameraPixelFormat::I420; break;
        case webrtc::VideoType::kYUY2: item.format = CameraPixelFormat::YUY2; break;
        case webrtc::VideoType::kNV12: item.format = CameraPixelFormat::NV12; break;
        case webrtc::VideoType::kMJPEG: item.format = CameraPixelFormat::MJPEG; break;
        default: item.format = CameraPixelFormat::Unknown; break;
        }
        result.push_back(item);
    }
    return result;
}
std::vector<CameraDeviceInfo> CameraEnumerator::EnumerateVideoDevices() {
    std::vector<CameraDeviceInfo> result;
    for (const auto& device : media::LinuxCameraCapture::Enumerate()) {
        result.push_back({device.name, device.id, result.empty(), GetDeviceCapabilities(device.id)});
    }
    return result;
}
}
#endif
