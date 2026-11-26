#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "render/latest_video_frame_core.h"

namespace webrtc {
class I420Buffer;
}

namespace livekit::render {

// A capability for one CPU allocation created by this module. A format tag or
// a caller-supplied boolean cannot establish a decoder buffer's lifetime rules.
// Decoder output uses its separately audited identity registry instead.
class CpuI420BufferProvenance final {
public:
    using Ptr = std::shared_ptr<const CpuI420BufferProvenance>;

    struct MutablePlanes {
        int width;
        int height;
        int chroma_width;
        int chroma_height;
        uint8_t* y;
        uint8_t* u;
        uint8_t* v;
        int stride_y;
        int stride_u;
        int stride_v;
    };
    using Initializer = std::function<void(const MutablePlanes&)>;

    // Allocates and zero-initializes ordinary CPU I420 storage, then invokes
    // initialize synchronously, before publishing it. The initializer must not
    // retain any plane pointer or modify these pixels after it returns.
    // Zero strides select tightly packed planes; invalid geometry returns null.
    static Ptr Create(int width, int height, Initializer initialize = {},
                      int stride_y = 0, int stride_u = 0, int stride_v = 0);

    ~CpuI420BufferProvenance();
    CpuI420BufferProvenance(const CpuI420BufferProvenance&) = delete;
    CpuI420BufferProvenance& operator=(const CpuI420BufferProvenance&) = delete;

    // Only use this read-only buffer to construct a VideoFrame. Do not cast it
    // back to a mutable buffer or retain a writer after Create returns.
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer() const;
    size_t retained_bytes() const noexcept { return retained_bytes_; }

private:
    friend class RetainedVideoFrame;
    CpuI420BufferProvenance(webrtc::scoped_refptr<webrtc::I420Buffer> buffer,
                            size_t retained_bytes);

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer_;
    const size_t retained_bytes_;
};

// Scheduler input. The renderer receives immutable planes with independent ownership.
class RetainedVideoFrame final : public FrameInput {
public:
    using Ptr = std::shared_ptr<const RetainedVideoFrame>;

    // Accept the controlled allocation or registered VP8/HEVC decoder output.
    // Reject other inputs without conversion or pixel access.
    static Ptr TryRetain(const webrtc::VideoFrame& frame,
                         RenderFrameMetadata metadata = {},
                         const CpuI420BufferProvenance::Ptr& provenance = {});

    OwnedI420Frame::Ptr Materialize() const override;
    // Charge frames retained by asynchronous consumers until their final release,
    // independently of the scheduler's input budget and shutdown barrier.
    static OwnedI420Frame::Ptr TryShareForRender(const Ptr&, const std::shared_ptr<size_t>& binding_count);
    int width() const noexcept { return frame_.width(); }
    int height() const noexcept { return frame_.height(); }
    size_t retained_bytes() const noexcept { return retained_bytes_; }
    size_t RetainedBytes() const noexcept override { return retained_bytes(); }
    const RenderFrameMetadata& render_metadata() const noexcept { return metadata_; }

    // The complete WebRTC frame is retained so metadata not currently consumed
    // by OwnedI420Frame (e.g. update rect/RTP timestamp) is not silently lost.
    const webrtc::VideoFrame& native_frame() const noexcept { return frame_; }

private:
    RetainedVideoFrame(const webrtc::VideoFrame& frame,
                       RenderFrameMetadata metadata,
                       size_t retained_bytes, CpuI420BufferProvenance::Ptr provenance = {});

    const webrtc::VideoFrame frame_;
    const RenderFrameMetadata metadata_;
    const size_t retained_bytes_;
    const CpuI420BufferProvenance::Ptr provenance_;
};

} // namespace livekit::render
