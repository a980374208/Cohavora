#include "video_frame.h"
#include <stdexcept>

namespace livekit {

static std::size_t CalculateBufferSize(int width, int height, VideoBufferType type) {
    if (width <= 0 || height <= 0) return 0;
    switch (type) {
        case VideoBufferType::RGBA:
        case VideoBufferType::ABGR:
        case VideoBufferType::ARGB:
        case VideoBufferType::BGRA:
            return static_cast<std::size_t>(width * height * 4);
        case VideoBufferType::RGB24:
            return static_cast<std::size_t>(width * height * 3);
        case VideoBufferType::I420:
            return static_cast<std::size_t>(width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2));
        case VideoBufferType::NV12:
            return static_cast<std::size_t>(width * height + ((width + 1) / 2) * ((height + 1) / 2) * 2);
        default:
            return static_cast<std::size_t>(width * height * 4);
    }
}

VideoFrame::VideoFrame()
    : width_(0), height_(0), type_(VideoBufferType::RGBA) {}

VideoFrame::VideoFrame(int width, int height, VideoBufferType type, std::vector<std::uint8_t> data)
    : width_(width), height_(height), type_(type), data_(std::move(data)) {
    std::size_t expected = CalculateBufferSize(width, height, type);
    if (data_.size() != expected) {
        throw std::invalid_argument("VideoFrame data size does not match expected size for given format and dimensions");
    }
}

VideoFrame VideoFrame::create(int width, int height, VideoBufferType type) {
    std::size_t size = CalculateBufferSize(width, height, type);
    std::vector<std::uint8_t> buffer(size, 0);
    return VideoFrame(width, height, type, std::move(buffer));
}

std::vector<VideoPlaneInfo> VideoFrame::planeInfos() const {
    std::vector<VideoPlaneInfo> planes;
    if (data_.empty() || width_ <= 0 || height_ <= 0) return planes;

    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(data_.data());
    if (type_ == VideoBufferType::RGBA || type_ == VideoBufferType::BGRA || type_ == VideoBufferType::ARGB || type_ == VideoBufferType::ABGR) {
        VideoPlaneInfo info;
        info.data_ptr = base;
        info.stride = static_cast<std::uint32_t>(width_ * 4);
        info.size = static_cast<std::uint32_t>(data_.size());
        planes.push_back(info);
    } else if (type_ == VideoBufferType::I420) {
        std::uint32_t y_stride = static_cast<std::uint32_t>(width_);
        std::uint32_t y_size = y_stride * height_;
        std::uint32_t uv_stride = static_cast<std::uint32_t>((width_ + 1) / 2);
        std::uint32_t uv_size = uv_stride * ((height_ + 1) / 2);

        // Y plane
        planes.push_back({base, y_stride, y_size});
        // U plane
        planes.push_back({base + y_size, uv_stride, uv_size});
        // V plane
        planes.push_back({base + y_size + uv_size, uv_stride, uv_size});
    } else {
        VideoPlaneInfo info;
        info.data_ptr = base;
        info.stride = static_cast<std::uint32_t>(width_);
        info.size = static_cast<std::uint32_t>(data_.size());
        planes.push_back(info);
    }
    return planes;
}

} // namespace livekit
