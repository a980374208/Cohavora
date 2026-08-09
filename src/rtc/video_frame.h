#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace livekit {

enum class VideoBufferType {
    RGBA = 0,
    ABGR,
    ARGB,
    BGRA,
    RGB24,
    I420,
    I420A,
    I422,
    I444,
    I010,
    NV12
};

struct VideoPlaneInfo {
    std::uintptr_t data_ptr{0};
    std::uint32_t stride{0};
    std::uint32_t size{0};
};

class VideoFrame {
public:
    VideoFrame();
    VideoFrame(int width, int height, VideoBufferType type, std::vector<std::uint8_t> data);
    virtual ~VideoFrame() = default;

    static VideoFrame create(int width, int height, VideoBufferType type);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    VideoBufferType type() const noexcept { return type_; }

    std::uint8_t* data() noexcept { return data_.data(); }
    const std::uint8_t* data() const noexcept { return data_.data(); }
    std::size_t dataSize() const noexcept { return data_.size(); }

    std::vector<VideoPlaneInfo> planeInfos() const;

private:
    int width_{0};
    int height_{0};
    VideoBufferType type_{VideoBufferType::RGBA};
    std::vector<std::uint8_t> data_;
};

} // namespace livekit
