#include "video_render_frame.h"
#include <cstring>
#include <limits>

namespace livekit::render {
namespace {
// Keep host metadata numerically compatible with the frozen module ABI.
static_assert(uint32_t(RenderColorMatrix::Unspecified) == LK_RENDER_MATRIX_UNSPECIFIED &&
    uint32_t(RenderColorMatrix::Bt601) == LK_RENDER_MATRIX_BT601 &&
    uint32_t(RenderColorMatrix::Bt709) == LK_RENDER_MATRIX_BT709 &&
    uint32_t(RenderColorMatrix::Bt2020) == LK_RENDER_MATRIX_BT2020_NCL);
static_assert(uint32_t(RenderColorRange::Unspecified) == LK_RENDER_RANGE_UNSPECIFIED &&
    uint32_t(RenderColorRange::Limited) == LK_RENDER_RANGE_LIMITED &&
    uint32_t(RenderColorRange::Full) == LK_RENDER_RANGE_FULL);
bool ValidRotation(uint32_t rotation) {
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}
}

VideoRenderFrame::Ptr VideoRenderFrame::FromI420(OwnedI420Frame::Ptr frame) {
    if (!frame) return {};
    auto result = std::shared_ptr<VideoRenderFrame>(new VideoRenderFrame);
    result->i420_ = std::move(frame);
    const auto& source = *result->i420_;
    auto& v = result->view_;
    v.struct_size = sizeof(v); v.format = LK_RENDER_I420;
    v.width = source.width(); v.height = source.height(); v.plane_count = 3;
    v.rotation_degrees = static_cast<uint32_t>(source.rotation());
    v.timestamp_us = source.timestamp_us();
    v.color_matrix = static_cast<uint32_t>(source.color_space().matrix);
    v.color_range = static_cast<uint32_t>(source.color_space().range);
    v.planes[0] = {source.data_y(), uint64_t(source.stride_y()) * source.height(),
        uint32_t(source.stride_y()), v.width, v.height, 0};
    v.planes[1] = {source.data_u(), uint64_t(source.stride_u()) * source.chroma_height(),
        uint32_t(source.stride_u()), uint32_t(source.chroma_width()), uint32_t(source.chroma_height()), 0};
    v.planes[2] = {source.data_v(), uint64_t(source.stride_v()) * source.chroma_height(),
        uint32_t(source.stride_v()), uint32_t(source.chroma_width()), uint32_t(source.chroma_height()), 0};
    return result;
}

VideoRenderFrame::Ptr VideoRenderFrame::CopyFrom(const VideoFrame& frame, const VideoCaptureOptions& options) {
    if (frame.width() <= 0 || frame.height() <= 0 || !ValidRotation(uint32_t(options.rotation))) return {};
    const uint64_t w = frame.width(), h = frame.height(), cw = (w + 1) / 2, ch = (h + 1) / 2;
    // ABI stride and libyuv consumers use 32-bit row lengths. Validate before
    // any multiplication/copy, including malformed/default-constructed frames.
    if (w > uint64_t(std::numeric_limits<int>::max()) / 4) return {};
    uint32_t format = 0;
    uint64_t inputBytes = 0;
    switch (frame.type()) {
    case VideoBufferType::I420:
    case VideoBufferType::I420A: // Preserve the previous preview's opaque-YUV policy.
        format = LK_RENDER_I420; inputBytes = w*h + 2*cw*ch; break;
    case VideoBufferType::NV12: format = LK_RENDER_NV12; inputBytes = w*h + 2*cw*ch; break;
    case VideoBufferType::RGB24: format = LK_RENDER_RGBA8; inputBytes = w*h*3; break;
    case VideoBufferType::RGBA:
    case VideoBufferType::BGRA:
    case VideoBufferType::ARGB:
    case VideoBufferType::ABGR: format = LK_RENDER_RGBA8; inputBytes = w*h*4; break;
    default: return {};
    }
    if (!frame.data() || inputBytes > frame.dataSize()) return {};
    const uint64_t outputBytes = format == LK_RENDER_RGBA8 ? w*h*4 : inputBytes;
    if (outputBytes > std::numeric_limits<size_t>::max()) return {};
    auto result = std::shared_ptr<VideoRenderFrame>(new VideoRenderFrame);
    result->storage_.resize(size_t(outputBytes));
    auto* dst = result->storage_.data();
    const auto* src = frame.data();
    if (format != LK_RENDER_RGBA8 || frame.type() == VideoBufferType::RGBA) {
        std::memcpy(dst, src, size_t(outputBytes));
    } else {
        const size_t count = size_t(w*h);
        for (size_t i = 0; i < count; ++i) {
            auto* pixel = dst + i*4;
            switch (frame.type()) {
            case VideoBufferType::RGB24:
                pixel[0]=src[i*3]; pixel[1]=src[i*3+1]; pixel[2]=src[i*3+2]; pixel[3]=255; break;
            case VideoBufferType::BGRA:
                pixel[0]=src[i*4+2]; pixel[1]=src[i*4+1]; pixel[2]=src[i*4]; pixel[3]=src[i*4+3]; break;
            case VideoBufferType::ARGB:
                pixel[0]=src[i*4+1]; pixel[1]=src[i*4+2]; pixel[2]=src[i*4+3]; pixel[3]=src[i*4]; break;
            case VideoBufferType::ABGR:
                pixel[0]=src[i*4+3]; pixel[1]=src[i*4+2]; pixel[2]=src[i*4+1]; pixel[3]=src[i*4]; break;
            default: break;
            }
        }
    }
    auto& v = result->view_;
    v.struct_size = sizeof(v); v.format = format; v.width = uint32_t(w); v.height = uint32_t(h);
    v.rotation_degrees = uint32_t(options.rotation); v.timestamp_us = options.timestamp_us;
    v.color_matrix = LK_RENDER_MATRIX_BT601; v.color_range = LK_RENDER_RANGE_LIMITED;
    v.plane_count = format == LK_RENDER_I420 ? 3 : format == LK_RENDER_NV12 ? 2 : 1;
    v.planes[0] = {dst, format == LK_RENDER_RGBA8 ? outputBytes : w*h,
        uint32_t(format == LK_RENDER_RGBA8 ? w*4 : w), uint32_t(w), uint32_t(h), 0};
    if (format == LK_RENDER_I420) {
        v.planes[1] = {dst+w*h, cw*ch, uint32_t(cw), uint32_t(cw), uint32_t(ch), 0};
        v.planes[2] = {dst+w*h+cw*ch, cw*ch, uint32_t(cw), uint32_t(cw), uint32_t(ch), 0};
    } else if (format == LK_RENDER_NV12) {
        v.planes[1] = {dst+w*h, cw*ch*2, uint32_t(cw*2), uint32_t(cw), uint32_t(ch), 0};
    }
    return result;
}

RenderColorSpace VideoRenderFrame::colorSpace() const noexcept {
    return {static_cast<RenderColorMatrix>(view_.color_matrix), static_cast<RenderColorRange>(view_.color_range)};
}
} // namespace livekit::render
