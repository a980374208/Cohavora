#include "video_frame.h"
#if defined(_WIN32)
#include "../media/d3d11_nv12_frame_buffer.h"
#endif
#include "libyuv/planar_functions.h"
#include "rtc_base/ref_counted_object.h"
#include <cstring>
#include <stdexcept>

namespace livekit {

static std::size_t CalculateBufferSize(int width, int height, VideoBufferType type) {
    if (width <= 0 || height <= 0) return 0;
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t cw = (w + 1) / 2;
    const std::size_t ch = (h + 1) / 2;
    switch (type) {
        case VideoBufferType::RGBA:
        case VideoBufferType::ABGR:
        case VideoBufferType::ARGB:
        case VideoBufferType::BGRA:
            return w * h * 4;
        case VideoBufferType::RGB24:
            return w * h * 3;
        case VideoBufferType::I420:
            return w * h + 2 * cw * ch;
        case VideoBufferType::NV12:
            return w * h + cw * ch * 2;
        default:
            return w * h * 4;
    }
}

namespace {

bool HasExclusiveNV12Ref(const webrtc::scoped_refptr<webrtc::NV12Buffer>& buffer) noexcept {
    if (!buffer) return false;
    return static_cast<const webrtc::RefCountedObject<webrtc::NV12Buffer>*>(
               buffer.get())
        ->HasOneRef();
}

} // namespace

VideoFrame::VideoFrame()
    : width_(0), height_(0), type_(VideoBufferType::RGBA), size_(0) {}

VideoFrame::VideoFrame(int width, int height, VideoBufferType type, std::vector<std::uint8_t> data)
    : width_(width), height_(height), type_(type), size_(data.size()) {
    std::size_t expected = CalculateBufferSize(width, height, type);
    if (size_ != expected) {
        throw std::invalid_argument("VideoFrame data size does not match expected size for given format and dimensions");
    }
    if (size_ == 0) {
        return;
    }
    if (type_ == VideoBufferType::NV12) {
        nv12_buffer_ = webrtc::NV12Buffer::Create(width_, height_);
        std::memcpy(nv12_buffer_->MutableDataY(), data.data(), size_);
    } else {
        shared_data_ = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
    }
}

VideoFrame::VideoFrame(webrtc::scoped_refptr<webrtc::NV12Buffer> nv12_buffer)
    : width_(nv12_buffer ? nv12_buffer->width() : 0),
      height_(nv12_buffer ? nv12_buffer->height() : 0),
      type_(VideoBufferType::NV12),
      size_(CalculateBufferSize(width_, height_, VideoBufferType::NV12)),
      nv12_buffer_(std::move(nv12_buffer)) {}

VideoFrame::VideoFrame(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer,
                       VideoBufferType reported_type)
    : width_(native_buffer ? native_buffer->width() : 0),
      height_(native_buffer ? native_buffer->height() : 0),
      type_(reported_type),
      size_(CalculateBufferSize(width_, height_, reported_type)),
      native_buffer_(std::move(native_buffer)) {}

VideoFrame VideoFrame::create(int width, int height, VideoBufferType type) {
    VideoFrame frame = createUninit(width, height, type);
    if (frame.nv12_buffer_) {
        frame.nv12_buffer_->InitializeData();
    } else if (frame.shared_data_ && !frame.shared_data_->empty()) {
        std::memset(frame.shared_data_->data(), 0, frame.shared_data_->size());
    }
    return frame;
}

VideoFrame VideoFrame::createUninit(int width, int height, VideoBufferType type) {
    std::size_t size = CalculateBufferSize(width, height, type);
    if (size == 0) {
        return VideoFrame();
    }
    if (type == VideoBufferType::NV12) {
        return VideoFrame(webrtc::NV12Buffer::Create(width, height));
    }
    VideoFrame frame;
    frame.width_ = width;
    frame.height_ = height;
    frame.type_ = type;
    frame.size_ = size;
    frame.shared_data_ = std::make_shared<std::vector<std::uint8_t>>(size);
    return frame;
}

std::uint8_t* VideoFrame::data() noexcept {
    if (!nv12_buffer_ && !shared_data_ && native_buffer_) {
        (void)static_cast<const VideoFrame*>(this)->data();
    }
    if (nv12_buffer_) {
        if (!HasExclusiveNV12Ref(nv12_buffer_)) {
            auto copy = webrtc::NV12Buffer::Create(width_, height_);
            std::memcpy(copy->MutableDataY(), nv12_buffer_->DataY(), size_);
            nv12_buffer_ = std::move(copy);
        }
        return nv12_buffer_->MutableDataY();
    }
    if (shared_data_) {
        if (shared_data_.use_count() > 1) {
            shared_data_ = std::make_shared<std::vector<std::uint8_t>>(*shared_data_);
        }
        return shared_data_->data();
    }
    return nullptr;
}

const std::uint8_t* VideoFrame::data() const noexcept {
    if (nv12_buffer_) {
        return nv12_buffer_->DataY();
    }
    if (shared_data_) {
        return shared_data_->data();
    }
    if (native_buffer_) {
#if defined(_WIN32)
        if (const auto* d3d_nv12 = D3d11Nv12FrameBuffer::FromVideoFrameBuffer(native_buffer_.get())) {
            auto mapped = d3d_nv12->ToNV12();
            if (mapped) {
                auto copy = webrtc::NV12Buffer::Create(width_, height_);
                libyuv::NV12Copy(
                    mapped->DataY(), mapped->StrideY(),
                    mapped->DataUV(), mapped->StrideUV(),
                    copy->MutableDataY(), copy->StrideY(),
                    copy->MutableDataUV(), copy->StrideUV(),
                    width_, height_);
                nv12_buffer_ = std::move(copy);
                return nv12_buffer_->DataY();
            }
        }
#endif
        if (native_buffer_->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
            const auto* nv12 = native_buffer_->GetNV12();
            if (nv12) {
                auto copy = webrtc::NV12Buffer::Create(width_, height_);
                libyuv::NV12Copy(
                    nv12->DataY(), nv12->StrideY(),
                    nv12->DataUV(), nv12->StrideUV(),
                    copy->MutableDataY(), copy->StrideY(),
                    copy->MutableDataUV(), copy->StrideUV(),
                    width_, height_);
                nv12_buffer_ = std::move(copy);
                return nv12_buffer_->DataY();
            }
        }
        mapped_i420_ = native_buffer_->ToI420();
        if (mapped_i420_) {
            nv12_buffer_ = webrtc::NV12Buffer::Copy(*mapped_i420_);
            return nv12_buffer_ ? nv12_buffer_->DataY() : nullptr;
        }
    }
    return nullptr;
}

std::vector<VideoPlaneInfo> VideoFrame::planeInfos() const {
    std::vector<VideoPlaneInfo> planes;
    const std::uint8_t* raw = data();
    if (!raw || size_ == 0 || width_ <= 0 || height_ <= 0) return planes;

    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(raw);
    if (type_ == VideoBufferType::RGBA || type_ == VideoBufferType::BGRA || type_ == VideoBufferType::ARGB || type_ == VideoBufferType::ABGR) {
        VideoPlaneInfo info;
        info.data_ptr = base;
        info.stride = static_cast<std::uint32_t>(width_ * 4);
        info.size = static_cast<std::uint32_t>(size_);
        planes.push_back(info);
    } else if (type_ == VideoBufferType::I420) {
        std::uint32_t y_stride = static_cast<std::uint32_t>(width_);
        std::uint32_t y_size = y_stride * static_cast<std::uint32_t>(height_);
        std::uint32_t uv_stride = static_cast<std::uint32_t>((width_ + 1) / 2);
        std::uint32_t uv_size = uv_stride * static_cast<std::uint32_t>((height_ + 1) / 2);

        // Y plane
        planes.push_back({base, y_stride, y_size});
        // U plane
        planes.push_back({base + y_size, uv_stride, uv_size});
        // V plane
        planes.push_back({base + y_size + uv_size, uv_stride, uv_size});
    } else if (type_ == VideoBufferType::NV12) {
        std::uint32_t y_stride = nv12_buffer_
            ? static_cast<std::uint32_t>(nv12_buffer_->StrideY())
            : static_cast<std::uint32_t>(width_);
        std::uint32_t y_size = y_stride * static_cast<std::uint32_t>(height_);
        std::uint32_t uv_stride = nv12_buffer_
            ? static_cast<std::uint32_t>(nv12_buffer_->StrideUV())
            : static_cast<std::uint32_t>(((width_ + 1) / 2) * 2);
        std::uint32_t uv_size = uv_stride * static_cast<std::uint32_t>((height_ + 1) / 2);
        std::uintptr_t uv_base = nv12_buffer_
            ? reinterpret_cast<std::uintptr_t>(nv12_buffer_->DataUV())
            : (base + y_size);

        // Y plane
        planes.push_back({base, y_stride, y_size});
        // Interleaved UV plane
        planes.push_back({uv_base, uv_stride, uv_size});
    } else {
        VideoPlaneInfo info;
        info.data_ptr = base;
        info.stride = static_cast<std::uint32_t>(width_);
        info.size = static_cast<std::uint32_t>(size_);
        planes.push_back(info);
    }
    return planes;
}

} // namespace livekit
