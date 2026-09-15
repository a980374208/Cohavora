#include "owned_i420_frame.h"

#include <cstring>
#include <limits>
#include <optional>

#include "api/video/color_space.h"
#include "api/video/video_frame.h"

namespace livekit::render {
namespace {

bool CheckedMultiply(size_t lhs, size_t rhs, size_t* result) {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

RenderColorSpace ToRenderColorSpace(const std::optional<webrtc::ColorSpace>& color_space) {
    if (!color_space) {
        return {};
    }

    RenderColorSpace result;
    switch (color_space->matrix()) {
    case webrtc::ColorSpace::MatrixID::kBT709:
        result.matrix = RenderColorMatrix::Bt709;
        break;
    case webrtc::ColorSpace::MatrixID::kBT2020_NCL:
    case webrtc::ColorSpace::MatrixID::kBT2020_CL:
        result.matrix = RenderColorMatrix::Bt2020;
        break;
    case webrtc::ColorSpace::MatrixID::kFCC:
    case webrtc::ColorSpace::MatrixID::kBT470BG:
    case webrtc::ColorSpace::MatrixID::kSMPTE170M:
    case webrtc::ColorSpace::MatrixID::kSMPTE240M:
        result.matrix = RenderColorMatrix::Bt601;
        break;
    default:
        break;
    }

    switch (color_space->range()) {
    case webrtc::ColorSpace::RangeID::kLimited:
        result.range = RenderColorRange::Limited;
        break;
    case webrtc::ColorSpace::RangeID::kFull:
        result.range = RenderColorRange::Full;
        break;
    default:
        break;
    }
    return result;
}

VideoRotation ToVideoRotation(webrtc::VideoRotation rotation) {
    switch (rotation) {
    case webrtc::kVideoRotation_90:
        return VideoRotation::VIDEO_ROTATION_90;
    case webrtc::kVideoRotation_180:
        return VideoRotation::VIDEO_ROTATION_180;
    case webrtc::kVideoRotation_270:
        return VideoRotation::VIDEO_ROTATION_270;
    case webrtc::kVideoRotation_0:
    default:
        return VideoRotation::VIDEO_ROTATION_0;
    }
}

} // namespace

OwnedI420Frame::OwnedI420Frame(int width,
                               int height,
                               int chroma_width,
                               int chroma_height,
                               size_t y_size,
                               size_t u_size,
                               int64_t timestamp_us,
                               VideoRotation rotation,
                               RenderColorSpace color_space)
    : width_(width),
      height_(height),
      chroma_width_(chroma_width),
      chroma_height_(chroma_height),
      stride_y_(width),
      stride_u_(chroma_width),
      stride_v_(chroma_width),
      u_offset_(y_size),
      v_offset_(y_size + u_size),
      timestamp_us_(timestamp_us),
      rotation_(rotation),
      color_space_(color_space),
      storage_(y_size + u_size + u_size) {}

OwnedI420Frame::Ptr OwnedI420Frame::CopyFrom(const webrtc::VideoFrame& frame) {
    const auto buffer = frame.video_frame_buffer();
    if (!buffer) {
        return nullptr;
    }

    const auto i420_buffer = buffer->ToI420();
    if (!i420_buffer) {
        return nullptr;
    }

    return CopyFromPlanes(frame.width(),
                          frame.height(),
                          i420_buffer->DataY(),
                          i420_buffer->StrideY(),
                          i420_buffer->DataU(),
                          i420_buffer->StrideU(),
                          i420_buffer->DataV(),
                          i420_buffer->StrideV(),
                          frame.timestamp_us(),
                          ToVideoRotation(frame.rotation()),
                          ToRenderColorSpace(frame.color_space()));
}

OwnedI420Frame::Ptr OwnedI420Frame::CopyFromPlanes(int width,
                                                    int height,
                                                    const uint8_t* data_y,
                                                    int stride_y,
                                                    const uint8_t* data_u,
                                                    int stride_u,
                                                    const uint8_t* data_v,
                                                    int stride_v,
                                                    int64_t timestamp_us,
                                                    VideoRotation rotation,
                                                    RenderColorSpace color_space) {
    if (width <= 0 || height <= 0 || !data_y || !data_u || !data_v) {
        return nullptr;
    }

    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    if (stride_y < width || stride_u < chroma_width || stride_v < chroma_width) {
        return nullptr;
    }

    size_t y_size = 0;
    size_t chroma_size = 0;
    if (!CheckedMultiply(static_cast<size_t>(width), static_cast<size_t>(height), &y_size) ||
        !CheckedMultiply(static_cast<size_t>(chroma_width), static_cast<size_t>(chroma_height), &chroma_size) ||
        y_size > std::numeric_limits<size_t>::max() - chroma_size ||
        y_size + chroma_size > std::numeric_limits<size_t>::max() - chroma_size) {
        return nullptr;
    }

    auto result = std::shared_ptr<OwnedI420Frame>(new OwnedI420Frame(width,
                                                                       height,
                                                                       chroma_width,
                                                                       chroma_height,
                                                                       y_size,
                                                                       chroma_size,
                                                                       timestamp_us,
                                                                       rotation,
                                                                       color_space));

    for (int row = 0; row < height; ++row) {
        std::memcpy(result->storage_.data() + static_cast<size_t>(row) * result->stride_y_,
                    data_y + static_cast<size_t>(row) * stride_y,
                    static_cast<size_t>(width));
    }
    for (int row = 0; row < chroma_height; ++row) {
        std::memcpy(result->storage_.data() + result->u_offset_ + static_cast<size_t>(row) * result->stride_u_,
                    data_u + static_cast<size_t>(row) * stride_u,
                    static_cast<size_t>(chroma_width));
        std::memcpy(result->storage_.data() + result->v_offset_ + static_cast<size_t>(row) * result->stride_v_,
                    data_v + static_cast<size_t>(row) * stride_v,
                    static_cast<size_t>(chroma_width));
    }
    return result;
}

} // namespace livekit::render
