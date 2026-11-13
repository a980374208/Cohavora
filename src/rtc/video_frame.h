#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#ifndef LK_RENDER_MODULE_BUILD
#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame_buffer.h"
#endif

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
#ifndef LK_RENDER_MODULE_BUILD
    explicit VideoFrame(webrtc::scoped_refptr<webrtc::NV12Buffer> nv12_buffer);
    explicit VideoFrame(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer,
                        VideoBufferType reported_type = VideoBufferType::NV12);
#endif
    virtual ~VideoFrame() = default;

    static VideoFrame create(int width, int height, VideoBufferType type);
    static VideoFrame createUninit(int width, int height, VideoBufferType type);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    VideoBufferType type() const noexcept { return type_; }

    std::uint8_t* data() noexcept;
    const std::uint8_t* data() const noexcept;
    std::size_t dataSize() const noexcept { return size_; }

#ifndef LK_RENDER_MODULE_BUILD
    webrtc::scoped_refptr<webrtc::NV12BufferInterface> nv12Buffer() const noexcept {
        return nv12_buffer_;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> videoFrameBuffer() const noexcept {
        if (native_buffer_) return native_buffer_;
        return nv12_buffer_;
    }

    bool hasNativeBuffer() const noexcept {
        return static_cast<bool>(native_buffer_);
    }
#endif

    std::vector<VideoPlaneInfo> planeInfos() const;

private:
    int width_{0};
    int height_{0};
    VideoBufferType type_{VideoBufferType::RGBA};
    std::size_t size_{0};
#ifndef LK_RENDER_MODULE_BUILD
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer_;
    mutable webrtc::scoped_refptr<webrtc::NV12Buffer> nv12_buffer_;
    mutable webrtc::scoped_refptr<webrtc::I420BufferInterface> mapped_i420_;
#endif
    std::shared_ptr<std::vector<std::uint8_t>> shared_data_;
};

} // namespace livekit
