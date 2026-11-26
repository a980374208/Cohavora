#pragma once

#include <utility>

#include "media/d3d11_nv12_frame_buffer.h"
#include "render/video_render_frame.h"

namespace livekit::render {
class NativeScreenPreview final {
public:
    ~NativeScreenPreview() = default;
    static VideoRenderFrame::Ptr Create(const VideoFrame& frame);

    const lk_render_dx11_texture_view& view() const noexcept { return view_; }
    void RecordRead(const lk_render_dx11_retirement& ticket) const { buffer_->RecordPreviewRead(ticket); }
    VideoRenderFrame::Ptr ReadPixels() const;
private:
    NativeScreenPreview(webrtc::scoped_refptr<const D3d11Nv12FrameBuffer> buffer,
                        lk_render_dx11_texture_view view)
        : buffer_(std::move(buffer)), view_(view) {}
    webrtc::scoped_refptr<const D3d11Nv12FrameBuffer> buffer_;
    lk_render_dx11_texture_view view_;
};
} // namespace livekit::render
