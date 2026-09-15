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

} // namespace

int main() {
    livekit::render::QtCpuVideoRenderer renderer;

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
        [&delivered, &delivered_identity, &delivered_image](const std::string& identity, const QImage& image) {
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
    int dx11_delivered = 0;
    std::string dx11_identity;
    livekit::render::OwnedI420Frame::Ptr dx11_frame;
    livekit::render::VideoRenderSession backend_session(
        [&cpu_delivered](const std::string&, const QImage&) {
            ++cpu_delivered;
        });
    auto backend_track = std::make_shared<livekit::Track>("TR_DX11", "dx11", livekit::TrackKind::Video);
    backend_session.AttachRemoteTrack(backend_track, "participant-b");
    backend_session.UseDx11Backend(
        [&dx11_delivered, &dx11_identity, &dx11_frame](const std::string& identity,
                                                        livekit::render::OwnedI420Frame::Ptr frame) {
            ++dx11_delivered;
            dx11_identity = identity;
            dx11_frame = std::move(frame);
        });
    backend_track->notifyI420VideoFrame(red);
    backend_session.RenderLatestFrames();
    if (!Expect(dx11_delivered == 1 && cpu_delivered == 0 && dx11_identity == "participant-b",
                "DX11 backend must consume I420 directly without a QImage callback") ||
        !Expect(dx11_frame == red, "DX11 callback must receive the owned Router frame")) {
        return 1;
    }

    backend_session.UseQtCpuBackend();
    backend_track->notifyI420VideoFrame(white);
    backend_session.RenderLatestFrames();
    if (!Expect(cpu_delivered == 1 && dx11_delivered == 1,
                "backend switch must make CPU and DX11 consumption mutually exclusive")) {
        return 1;
    }

    // Full reconnect may recreate Track while the SFU preserves its SID.  The
    // old subscription must be cancelled; otherwise the replacement will
    // never deliver frames because the SID is already present in the session.
    backend_session.UseDx11Backend(
        [&dx11_delivered, &dx11_identity, &dx11_frame](const std::string& identity,
                                                        livekit::render::OwnedI420Frame::Ptr frame) {
            ++dx11_delivered;
            dx11_identity = identity;
            dx11_frame = std::move(frame);
        });
    auto replacement_track = std::make_shared<livekit::Track>(
        "TR_DX11", "dx11-after-reconnect", livekit::TrackKind::Video);
    backend_session.AttachRemoteTrack(replacement_track, "participant-b");
    backend_track->notifyI420VideoFrame(black);
    backend_session.RenderLatestFrames();
    if (!Expect(dx11_delivered == 1,
                "replaced Track must cancel the old same-SID subscription")) {
        return 1;
    }
    replacement_track->notifyI420VideoFrame(black);
    backend_session.RenderLatestFrames();
    const auto reconnect_stats = backend_session.statistics();
    if (!Expect(dx11_delivered == 2 && dx11_identity == "participant-b" && dx11_frame == black,
                "replacement Track with the same SID must render after reconnect") ||
        !Expect(reconnect_stats.delivered_to_dx11 == 2 &&
                    reconnect_stats.attached_track_count == 1 &&
                    reconnect_stats.backend == livekit::render::VideoRenderSession::Backend::Dx11,
                "session diagnostics must describe the active backend and delivered work")) {
        return 1;
    }

    int capped_delivered = 0;
    livekit::render::VideoRenderSession capped_session({}, 1);
    capped_session.UseDx11Backend(
        [&capped_delivered](const std::string&, livekit::render::OwnedI420Frame::Ptr) {
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
