#pragma once

#include <chrono>

#include "render/api/render_backend_api.h"
#include "render/owned_i420_frame.h"

namespace livekit::render {
class NativeScreenPreview;

// Host-owned immutable frame. The ABI view only borrows this object's storage.
// Remote I420 retains its original owner without copying/converting the pixels.
// Local frames keep YUV as YUV; packed RGB variants normalize to RGBA bytes.
class VideoRenderFrame final {
public:
    using Ptr = std::shared_ptr<const VideoRenderFrame>;
    static Ptr FromI420(OwnedI420Frame::Ptr frame);
    static Ptr FromCapture(const VideoFrame& frame, const VideoCaptureOptions& options = VideoCaptureOptions{});
    // For screen producers that transfer an immutable snapshot. Mapping a
    // native buffer is worker-only; the CPU result owns only mapped pixels.
    static Ptr FromScreenPixels(const VideoFrame& frame);
    OwnedI420Frame::Ptr ToOwnedI420() const;
    const std::shared_ptr<const NativeScreenPreview>& nativePreview() const noexcept { return native_preview_; }
    static Ptr CopyFrom(const VideoFrame& frame, const VideoCaptureOptions& options = {});
    const lk_render_frame_view& view() const noexcept { return view_; }
    const OwnedI420Frame::Ptr& i420Owner() const noexcept { return i420_; }
    RenderColorSpace colorSpace() const noexcept;
    const RenderFrameMetadata& renderMetadata() const noexcept;
    const RenderFrameMetadata::Ptr& presentationMetadata() const noexcept { return presentation_; }
    void SetRenderExpected(
        bool expected,
        RenderExpectationReason reason = RenderExpectationReason::SurfaceVisible,
        std::chrono::steady_clock::time_point source_time =
            std::chrono::steady_clock::now()) const;
    void NotifyRendered(
        const char* measurement_point,
        std::chrono::steady_clock::time_point source_time =
            std::chrono::steady_clock::now()) const;
    void NotifyRenderStage(
        const char* measurement_point,
        std::chrono::microseconds duration,
        std::chrono::steady_clock::time_point source_time =
            std::chrono::steady_clock::now()) const;
    VideoRenderFrame(const VideoRenderFrame&) = delete;
    VideoRenderFrame& operator=(const VideoRenderFrame&) = delete;
private:
    friend class NativeScreenPreview;
    VideoRenderFrame() = default;
#ifndef LK_RENDER_MODULE_BUILD
    static Ptr FromNV12(webrtc::scoped_refptr<const webrtc::NV12BufferInterface>,
                        const VideoCaptureOptions&, RenderColorSpace);
#endif
    lk_render_frame_view view_{};
    OwnedI420Frame::Ptr i420_;
    RenderFrameMetadata::Ptr presentation_;
    std::shared_ptr<const void> pixel_owner_;
    std::shared_ptr<const NativeScreenPreview> native_preview_;
    std::vector<uint8_t> storage_;
};

} // namespace livekit::render
