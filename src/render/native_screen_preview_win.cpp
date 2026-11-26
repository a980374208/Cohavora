#include "native_screen_preview_win.h"

namespace livekit::render {
namespace {
void CALLBACK ReleasePreview(PTP_CALLBACK_INSTANCE, void* preview, PTP_WORK) {
    delete static_cast<NativeScreenPreview*>(preview);
}
}
VideoRenderFrame::Ptr NativeScreenPreview::Create(const VideoFrame& frame) {
    const auto input = frame.videoFrameBuffer();
    const auto* native = D3d11Nv12FrameBuffer::FromVideoFrameBuffer(input.get());
    lk_render_dx11_texture_view view{};
    if (!native || !native->PreparePreview(view)) return {};
    auto result = std::shared_ptr<VideoRenderFrame>(new VideoRenderFrame);
    auto preview = std::unique_ptr<NativeScreenPreview>(new NativeScreenPreview(
        webrtc::scoped_refptr<const D3d11Nv12FrameBuffer>(native), view));
    // The last scene can outlive its window, Room and shutdown dispatcher.
    // Reserve cleanup on this worker before handing the texture to another thread.
    auto* work = CreateThreadpoolWork(ReleasePreview, preview.get(), nullptr);
    if (!work) return {};
    result->native_preview_.reset(preview.release(), [work](const NativeScreenPreview*) noexcept {
        SubmitThreadpoolWork(work);
        CloseThreadpoolWork(work);
    });
    auto& v = result->view_;
    v.struct_size = sizeof(v);
    v.width = view.width;
    v.height = view.height;
    v.format = LK_RENDER_NV12;
    v.color_matrix = LK_RENDER_MATRIX_BT709;
    v.color_range = LK_RENDER_RANGE_LIMITED;
    return result;
}

VideoRenderFrame::Ptr NativeScreenPreview::ReadPixels() const {
    return VideoRenderFrame::FromNV12(buffer_->ToNV12(), {},
        {RenderColorMatrix::Bt709, RenderColorRange::Limited});
}
} // namespace livekit::render
