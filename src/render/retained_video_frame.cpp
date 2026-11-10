#include "retained_video_frame.h"
#include "rtc/retained_vp8_decoder.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "api/video/i420_buffer.h"

namespace livekit::render {

CpuI420BufferProvenance::CpuI420BufferProvenance(
        webrtc::scoped_refptr<webrtc::I420Buffer> buffer,
        size_t retained_bytes)
    : buffer_(std::move(buffer)), retained_bytes_(retained_bytes) {}

CpuI420BufferProvenance::~CpuI420BufferProvenance() = default;

CpuI420BufferProvenance::Ptr CpuI420BufferProvenance::Create(
        int width, int height, Initializer initialize,
        int stride_y, int stride_u, int stride_v) {
    // Keep both ceil-chroma arithmetic and the SDK's signed allocation
    // arithmetic representable before calling into its checked allocator.
    if (width <= 0 || height <= 0 ||
        width == std::numeric_limits<int>::max() ||
        height == std::numeric_limits<int>::max()) return {};
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    if (stride_y == 0) stride_y = width;
    if (stride_u == 0) stride_u = chroma_width;
    if (stride_v == 0) stride_v = chroma_width;
    if (stride_y < width || stride_u < chroma_width || stride_v < chroma_width) return {};

    const auto y_bytes = uint64_t(stride_y) * uint64_t(height);
    const auto u_bytes = uint64_t(stride_u) * uint64_t(chroma_height);
    const auto v_bytes = uint64_t(stride_v) * uint64_t(chroma_height);
    const auto total_bytes = y_bytes + u_bytes + v_bytes;
    if (total_bytes > uint64_t(std::numeric_limits<int>::max())) return {};

    auto buffer = webrtc::I420Buffer::Create(width, height, stride_y, stride_u, stride_v);
    if (!buffer) return {};
    std::fill_n(buffer->MutableDataY(), size_t(y_bytes), uint8_t{});
    std::fill_n(buffer->MutableDataU(), size_t(u_bytes), uint8_t{});
    std::fill_n(buffer->MutableDataV(), size_t(v_bytes), uint8_t{});
    if (initialize) {
        initialize({width, height, chroma_width, chroma_height,
            buffer->MutableDataY(), buffer->MutableDataU(), buffer->MutableDataV(),
            stride_y, stride_u, stride_v});
    }
    return Ptr(new CpuI420BufferProvenance(std::move(buffer), size_t(total_bytes)));
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
CpuI420BufferProvenance::frame_buffer() const {
    return buffer_;
}

RetainedVideoFrame::RetainedVideoFrame(
        const webrtc::VideoFrame& frame, RenderFrameMetadata metadata,
        size_t retained_bytes, CpuI420BufferProvenance::Ptr provenance)
    : frame_(frame), metadata_(std::move(metadata)), retained_bytes_(retained_bytes),
      provenance_(std::move(provenance)) {}

RetainedVideoFrame::Ptr RetainedVideoFrame::TryRetain(
        const webrtc::VideoFrame& frame, RenderFrameMetadata metadata,
        const CpuI420BufferProvenance::Ptr& provenance) {
    // Compare ownership identity before invoking any virtual pixel/format
    // operation on an untrusted native or external decoder buffer.
    const auto buffer = frame.video_frame_buffer();
    if (!provenance) {
        const auto bytes = CertifiedVp8RetainedBytes(buffer.get());
        return bytes ? Ptr(new RetainedVideoFrame(frame, std::move(metadata), *bytes)) : Ptr{};
    }
    if (!buffer || buffer.get() != provenance->buffer_.get()) return {};
    return Ptr(new RetainedVideoFrame(frame, std::move(metadata), provenance->retained_bytes(), provenance));
}

OwnedI420Frame::Ptr RetainedVideoFrame::Materialize() const {
    // The exact CPU allocation is established above. Its I420 conversion is
    // trivial; OwnedI420Frame remains the only independently owned output.
    return OwnedI420Frame::CopyFrom(frame_, metadata_);
}

} // namespace livekit::render
