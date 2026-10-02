#include "owned_i420_frame.h"

#include <array>
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

std::shared_ptr<OwnedI420Frame::StorageBlock> OwnedI420Frame::AcquireStorage(size_t bytes) {
    // Cache pixels, never frames, tracks or session owners. Only this producer
    // thread can acquire a cache slot. A count of one means every published
    // frame holding this private block has been destroyed, including frames
    // retained by consumers through weak_ptr locks. No shared mutable frame
    // can be overwritten. Outstanding blocks outlive the producer safely.
    struct Cache {
        std::array<std::shared_ptr<StorageBlock>, 8> blocks;
        size_t bytes = 0;
    };
    constexpr size_t kMaximumCachedBytes = 8 * 1024 * 1024;
    thread_local Cache cache;
    for (const auto& block : cache.blocks)
        if (block && block->size == bytes && block.use_count() == 1) return block;

    if (bytes <= kMaximumCachedBytes) {
        for (auto& block : cache.blocks) {
            if (block && block.use_count() != 1) continue;
            const auto previous = block ? block->size : 0;
            if (cache.bytes - previous > kMaximumCachedBytes - bytes) continue;
            auto replacement = std::make_shared<StorageBlock>(bytes);
            block = std::move(replacement);
            cache.bytes = cache.bytes - previous + bytes;
            return block;
        }
    }
    // Retained frames, large sizes and cache exhaustion preserve full delivery.
    // Their storage is released with the immutable frame rather than cached.
    return std::make_shared<StorageBlock>(bytes);
}

OwnedI420Frame::OwnedI420Frame(int width,
                               int height,
                               int chroma_width,
                               int chroma_height,
                               size_t y_size,
                               size_t u_size,
                               int64_t timestamp_us,
                               VideoRotation rotation,
                               RenderColorSpace color_space,
                               RenderFrameMetadata render_metadata)
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
      render_metadata_(std::move(render_metadata)),
      // All bytes are filled by CopyFromPlanes before the immutable frame is
      // published. Avoid clearing a full decoder frame immediately before copy.
      storage_(AcquireStorage(y_size + u_size + u_size)) {}

OwnedI420Frame::Ptr OwnedI420Frame::CopyFrom(
        const webrtc::VideoFrame& frame,
        RenderFrameMetadata render_metadata) {
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
                          ToRenderColorSpace(frame.color_space()),
                          std::move(render_metadata));
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
                                                    RenderColorSpace color_space,
                                                    RenderFrameMetadata render_metadata) {
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
                                                                       color_space,
                                                                       std::move(render_metadata)));

    const auto copy_plane = [](uint8_t* destination, const uint8_t* source,
                               int row_bytes, int rows, int source_stride) {
        if (source_stride == row_bytes) {
            std::memcpy(destination, source, static_cast<size_t>(row_bytes) * rows);
        } else {
            for (int row = 0; row < rows; ++row)
                std::memcpy(destination + static_cast<size_t>(row) * row_bytes,
                            source + static_cast<size_t>(row) * source_stride,
                            static_cast<size_t>(row_bytes));
        }
    };
    copy_plane(result->storage_->bytes.get(), data_y, width, height, stride_y);
    copy_plane(result->storage_->bytes.get() + result->u_offset_, data_u, chroma_width, chroma_height, stride_u);
    copy_plane(result->storage_->bytes.get() + result->v_offset_, data_v, chroma_width, chroma_height, stride_v);
    return result;
}

} // namespace livekit::render
