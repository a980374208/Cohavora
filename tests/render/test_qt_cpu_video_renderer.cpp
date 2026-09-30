#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <QtGui/QColor>

#include "core/track.h"
#include "render/owned_i420_frame.h"
#include "render/qt_cpu_video_renderer.h"
#include "render/video_render_session.h"

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[QtCpuVideoRendererTest] FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

livekit::render::OwnedI420Frame::Ptr MakeFrame(uint8_t y_value,
                                               uint8_t u_value,
                                               uint8_t v_value) {
    const uint8_t y[] = {y_value, y_value, y_value, y_value};
    const uint8_t u[] = {u_value};
    const uint8_t v[] = {v_value};
    return livekit::render::OwnedI420Frame::CopyFromPlanes(2, 2, y, 2, u, 1, v, 1);
}

bool LocalFrameAcceptance() {
    using namespace livekit;
    using namespace livekit::render;
    QtCpuVideoRenderer renderer;
    VideoCaptureOptions options;
    options.timestamp_us = 7654321;
    options.rotation = VideoRotation::VIDEO_ROTATION_90;
    auto nv12 = VideoFrame::create(3, 5, VideoBufferType::NV12);
    std::fill(nv12.data(), nv12.data() + 15, uint8_t(235));
    std::fill(nv12.data() + 15, nv12.data() + nv12.dataSize(), uint8_t(128));
    auto owned = VideoRenderFrame::CopyFrom(nv12, options);
    nv12.data()[0] = 16;
    if (!Expect(owned && owned->view().format == LK_RENDER_NV12 && owned->view().planes[1].stride_bytes == 4 &&
                owned->view().planes[1].height_samples == 3 && owned->view().timestamp_us == options.timestamp_us &&
                owned->view().planes[0].data[0] == 235, "odd NV12 planes and capture metadata must survive producer reuse")) return false;
    const auto rotated = renderer.Convert(*owned);
    if (!Expect(rotated.size() == QSize(5, 3) && rotated.pixelColor(2, 1).red() > 245,
                "local NV12 CPU conversion must preserve rotation")) return false;
    for (auto format : {VideoBufferType::RGBA, VideoBufferType::BGRA, VideoBufferType::ARGB, VideoBufferType::ABGR}) {
        std::vector<uint8_t> bytes;
        switch (format) {
        case VideoBufferType::RGBA: bytes = {240, 20, 10, 255}; break;
        case VideoBufferType::BGRA: bytes = {10, 20, 240, 255}; break;
        case VideoBufferType::ARGB: bytes = {255, 240, 20, 10}; break;
        case VideoBufferType::ABGR: bytes = {255, 10, 20, 240}; break;
        default: break;
        }
        auto packed = VideoRenderFrame::CopyFrom(VideoFrame(1, 1, format, bytes));
        if (!Expect(packed && packed->view().format == LK_RENDER_RGBA8 &&
                    renderer.Convert(*packed).pixelColor(0, 0) == QColor(240, 20, 10),
                    "packed format names must describe byte order, not alias all formats to RGBA")) return false;
    }
    if (!Expect(!VideoRenderFrame::CopyFrom(VideoFrame{}), "empty local frame rejected")) return false;
    options.rotation = static_cast<VideoRotation>(45);
    if (!Expect(!VideoRenderFrame::CopyFrom(nv12, options), "unsupported rotation rejected")) return false;

    auto first = std::make_shared<VideoSource>(3, 5);
    auto replacement = std::make_shared<VideoSource>(3, 5);
    int gpu = 0, cpu = 0;
    VideoRenderFrame::Ptr received;
    VideoRenderSession session([&](const std::string& key, const QImage& image,
                                   VideoRenderFrame::Ptr) {
        if (key == "local" && !image.isNull()) ++cpu;
    });
    session.AttachLocalSource(first);
    session.UseGpuBackend([&](const std::string& key, VideoRenderFrame::Ptr frame) {
        if (key == "local") { ++gpu; received = std::move(frame); }
    });
    for (int i = 1; i <= 100; ++i) first->captureFrame(nv12, i);
    session.AttachLocalSource(first); // Idempotent layout refresh must preserve pending latest frame.
    session.RenderLatestFrames(); session.RenderLatestFrames();
    if (!Expect(gpu == 1 && cpu == 0 && received->view().timestamp_us == 100,
                "local mailbox must retain exactly the latest frame and select one backend")) return false;
    first->captureFrame(nv12, 101);
    session.UseQtCpuBackend(); session.RenderLatestFrames();
    if (!Expect(gpu == 1 && cpu == 1, "pending local frame must follow the newly selected CPU backend")) return false;
    first->captureFrame(nv12, 102);
    session.AttachLocalSource(replacement);
    first->captureFrame(nv12, 103);
    session.RenderLatestFrames();
    if (!Expect(cpu == 1, "rebinding must discard old queued frames and disconnect old producers")) return false;
    replacement->captureFrame(nv12, 104); session.RenderLatestFrames();
    if (!Expect(cpu == 2, "replacement local source must deliver")) return false;
    session.Deactivate();
    replacement->captureFrame(nv12, 105); session.RenderLatestFrames();
    if (!Expect(cpu == 2 && gpu == 1, "deactivated local source cannot revive rendering")) return false;
    return true;
}

} // namespace

int main() {
    if (!LocalFrameAcceptance()) return 1;
    livekit::render::QtCpuVideoRenderer renderer;
    const uint8_t rotatedY[] = {16, 16, 235, 235, 16, 16, 235, 235};
    const uint8_t rotatedUV[] = {128, 128};
    for (auto rotation : {livekit::VideoRotation::VIDEO_ROTATION_90, livekit::VideoRotation::VIDEO_ROTATION_270}) {
        auto frame = livekit::render::OwnedI420Frame::CopyFromPlanes(
            4, 2, rotatedY, 4, rotatedUV, 2, rotatedUV, 2, 0, rotation);
        const auto image = renderer.Convert(*frame);
        if (!Expect(image.size() == QSize(2, 4), "CPU fallback must preserve the rotated display aspect ratio")) return 1;
        const bool clockwise = rotation == livekit::VideoRotation::VIDEO_ROTATION_90;
        if (!Expect((image.pixelColor(0, 0).red() < 10) == clockwise &&
                    (image.pixelColor(0, 3).red() > 245) == clockwise,
                    "CPU fallback must rotate pixels along with the display dimensions")) return 1;
    }

    auto black = MakeFrame(16, 128, 128);
    auto white = MakeFrame(235, 128, 128);
    auto red = MakeFrame(82, 90, 240);
    if (!Expect(black && white && red, "test I420 frames must be created")) {
        return 1;
    }

    const QColor black_pixel(renderer.Convert(*black).pixelColor(0, 0));
    const QColor white_pixel(renderer.Convert(*white).pixelColor(0, 0));
    const QColor red_pixel(renderer.Convert(*red).pixelColor(0, 0));
    if (!Expect(black_pixel.red() <= 3 && black_pixel.green() <= 3 && black_pixel.blue() <= 3,
                "BT.601 limited black must remain black") ||
        !Expect(white_pixel.red() >= 250 && white_pixel.green() >= 250 && white_pixel.blue() >= 250,
                "BT.601 limited white must remain white") ||
        !Expect(red_pixel.red() >= 245 && red_pixel.green() <= 12 && red_pixel.blue() <= 12,
                "BT.601 red sample must preserve U/V ordering")) {
        return 1;
    }

    int delivered = 0;
    std::string delivered_identity;
    QImage delivered_image;
    livekit::render::VideoRenderSession session(
        [&delivered, &delivered_identity, &delivered_image](
                const std::string& identity, const QImage& image,
                livekit::render::VideoRenderFrame::Ptr) {
            ++delivered;
            delivered_identity = identity;
            delivered_image = image;
        });
    auto track = std::make_shared<livekit::Track>("TR_RENDER", "render", livekit::TrackKind::Video);
    session.AttachRemoteTrack(track, "participant-a");
    track->notifyI420VideoFrame(red);
    session.RenderLatestFrames();
    if (!Expect(delivered == 1 && delivered_identity == "participant-a",
                "session must render the router's latest frame for its track") ||
        !Expect(!delivered_image.isNull() && delivered_image.width() == 2 && delivered_image.height() == 2,
                "session must deliver an owned QImage")) {
        return 1;
    }

    session.Deactivate();
    track->notifyI420VideoFrame(white);
    session.RenderLatestFrames();
    if (!Expect(delivered == 1 && !session.active(),
                "Deactivate must cancel subscriptions and reject late frames")) {
        return 1;
    }

    int cpu_delivered = 0;
    int gpu_delivered = 0;
    std::string gpu_identity;
    livekit::render::VideoRenderFrame::Ptr gpu_frame;
    livekit::render::VideoRenderSession backend_session(
        [&cpu_delivered](const std::string&, const QImage&,
                         livekit::render::VideoRenderFrame::Ptr) {
            ++cpu_delivered;
        });
    auto backend_track = std::make_shared<livekit::Track>("TR_DX11", "dx11", livekit::TrackKind::Video);
    backend_session.AttachRemoteTrack(backend_track, "participant-b");
    backend_session.UseGpuBackend(
        [&gpu_delivered, &gpu_identity, &gpu_frame](const std::string& identity,
                                                        livekit::render::VideoRenderFrame::Ptr frame) {
            ++gpu_delivered;
            gpu_identity = identity;
            gpu_frame = std::move(frame);
        });
    backend_track->notifyI420VideoFrame(red);
    backend_session.RenderLatestFrames();
    if (!Expect(gpu_delivered == 1 && cpu_delivered == 0 && gpu_identity == "participant-b",
                "DX11 backend must consume I420 directly without a QImage callback") ||
        !Expect(gpu_frame && gpu_frame->i420Owner() == red, "DX11 callback must receive the owned Router frame")) {
        return 1;
    }

    backend_session.UseQtCpuBackend();
    backend_track->notifyI420VideoFrame(white);
    backend_session.RenderLatestFrames();
    if (!Expect(cpu_delivered == 1 && gpu_delivered == 1,
                "backend switch must make CPU and DX11 consumption mutually exclusive")) {
        return 1;
    }

    // Full reconnect may recreate Track while the SFU preserves its SID.  The
    // old subscription must be cancelled; otherwise the replacement will
    // never deliver frames because the SID is already present in the session.
    backend_session.UseGpuBackend(
        [&gpu_delivered, &gpu_identity, &gpu_frame](const std::string& identity,
                                                        livekit::render::VideoRenderFrame::Ptr frame) {
            ++gpu_delivered;
            gpu_identity = identity;
            gpu_frame = std::move(frame);
        });
    auto replacement_track = std::make_shared<livekit::Track>(
        "TR_DX11", "dx11-after-reconnect", livekit::TrackKind::Video);
    backend_session.AttachRemoteTrack(replacement_track, "participant-b");
    backend_track->notifyI420VideoFrame(black);
    backend_session.RenderLatestFrames();
    if (!Expect(gpu_delivered == 1,
                "replaced Track must cancel the old same-SID subscription")) {
        return 1;
    }
    replacement_track->notifyI420VideoFrame(black);
    backend_session.RenderLatestFrames();
    const auto reconnect_stats = backend_session.statistics();
    if (!Expect(gpu_delivered == 2 && gpu_identity == "participant-b" && gpu_frame && gpu_frame->i420Owner() == black,
                "replacement Track with the same SID must render after reconnect") ||
        !Expect(reconnect_stats.delivered_to_gpu == 2 &&
                    reconnect_stats.attached_track_count == 1 &&
                    reconnect_stats.backend == livekit::render::VideoRenderSession::Backend::Gpu,
                "session diagnostics must describe the active backend and delivered work")) {
        return 1;
    }

    int capped_delivered = 0;
    livekit::render::VideoRenderSession capped_session({}, 1);
    capped_session.UseGpuBackend(
        [&capped_delivered](const std::string&, livekit::render::VideoRenderFrame::Ptr) {
            ++capped_delivered;
        });
    auto first_capped_track = std::make_shared<livekit::Track>(
        "TR_CAP_1", "first", livekit::TrackKind::Video);
    auto second_capped_track = std::make_shared<livekit::Track>(
        "TR_CAP_2", "second", livekit::TrackKind::Video);
    capped_session.AttachRemoteTrack(first_capped_track, "participant-c");
    capped_session.AttachRemoteTrack(second_capped_track, "participant-d");
    first_capped_track->notifyI420VideoFrame(white);
    second_capped_track->notifyI420VideoFrame(black);
    capped_session.RenderLatestFrames();
    const auto capped_stats = capped_session.statistics();
    if (!Expect(capped_delivered == 1 && capped_stats.attached_track_count == 1 &&
                    capped_stats.rejected_track_attachments == 1,
                "session must bound Track subscriptions before idle tracks consume memory")) {
        return 1;
    }

    std::cout << "[QtCpuVideoRendererTest] PASS" << std::endl;
    return 0;
}
