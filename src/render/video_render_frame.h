#pragma once

#include <chrono>

#include "render/api/render_backend_api.h"
#include "render/owned_i420_frame.h"

namespace livekit::render {

// Host-owned immutable frame. The ABI view only borrows this object's storage.
// Remote I420 retains its original owner without copying/converting the pixels.
// Local frames keep YUV as YUV; packed RGB variants normalize to RGBA bytes.
class VideoRenderFrame final {
public:
    using Ptr = std::shared_ptr<const VideoRenderFrame>;
    static Ptr FromI420(OwnedI420Frame::Ptr frame);
    static Ptr CopyFrom(const VideoFrame& frame, const VideoCaptureOptions& options = {});
    const lk_render_frame_view& view() const noexcept { return view_; }
    const OwnedI420Frame::Ptr& i420Owner() const noexcept { return i420_; }
    RenderColorSpace colorSpace() const noexcept;
    const RenderFrameMetadata& renderMetadata() const noexcept;
    void SetRenderExpected(
        bool expected,
        RenderExpectationReason reason = RenderExpectationReason::SurfaceVisible,
        std::chrono::steady_clock::time_point source_time =
            std::chrono::steady_clock::now()) const;
    void NotifyRendered(
        const char* measurement_point,
        std::chrono::steady_clock::time_point source_time =
            std::chrono::steady_clock::now()) const;
    VideoRenderFrame(const VideoRenderFrame&) = delete;
    VideoRenderFrame& operator=(const VideoRenderFrame&) = delete;
private:
    VideoRenderFrame() = default;
    lk_render_frame_view view_{};
    OwnedI420Frame::Ptr i420_;
    std::vector<uint8_t> storage_;
};

} // namespace livekit::render
