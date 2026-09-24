#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <map>

#include "core/track.h"
#include "render/owned_i420_frame.h"
#include "render/video_render_session.h"

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[VideoRenderSessionStressTest] FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

livekit::render::OwnedI420Frame::Ptr MakeFrame(uint8_t y_value) {
    const uint8_t y[] = {y_value, y_value, y_value, y_value};
    const uint8_t u[] = {128};
    const uint8_t v[] = {128};
    return livekit::render::OwnedI420Frame::CopyFromPlanes(2, 2, y, 2, u, 1, v, 1);
}

struct RenderLeaseFixture {
    std::shared_ptr<livekit::MembershipState> participant_state;
    std::shared_ptr<livekit::TrackMembershipState> track_state;
    std::shared_ptr<livekit::MediaBindingState> binding_state;
    livekit::render::VideoRenderSession::RemoteTrackSelection selection;
};

RenderLeaseFixture MakeSelection(const std::string& sid,
                                 const std::string& identity,
                                 uint64_t serial) {
    RenderLeaseFixture result;
    livekit::ParticipantKey participant_key{
        31, serial, "PA_" + identity, identity};
    result.participant_state =
        std::make_shared<livekit::MembershipState>(participant_key);
    livekit::TrackKey track_key{participant_key, serial, sid};
    result.track_state =
        std::make_shared<livekit::TrackMembershipState>(track_key);
    livekit::MediaBindingKey binding_key{track_key, serial};
    result.binding_state =
        std::make_shared<livekit::MediaBindingState>(binding_key);
    result.binding_state->active.store(true, std::memory_order_release);
    result.selection.key = track_key;
    result.selection.ticket = result.track_state;
    result.selection.media_binding_key = binding_key;
    result.selection.media_binding_ticket = result.binding_state;
    result.selection.track = std::make_shared<livekit::Track>(
        sid, sid, livekit::TrackKind::Video, livekit::TrackSource::Camera);
    result.selection.identity = identity;
    result.selection.render_key = sid;
    return result;
}

} // namespace

int main() {
    {
        std::map<std::string, int> delivered;
        livekit::render::VideoRenderSession leased({}, 2);
        leased.UseGpuBackend([&](const std::string& key,
                                 livekit::render::VideoRenderFrame::Ptr frame) {
            delivered[key] = frame->view().planes[0].data[0];
        });
        auto a = MakeSelection("TR_LEASE_A", "lease-a", 1);
        auto b = MakeSelection("TR_LEASE_B", "lease-b", 2);
        auto c = MakeSelection("TR_LEASE_C", "lease-c", 3);
        auto applied = leased.ApplySelection(
            9, 1, {a.selection, b.selection});
        if (!Expect(applied.attached == 2 && applied.tracks.size() == 2 &&
                        applied.tracks[0].second ==
                            livekit::render::VideoRenderSession::AttachResult::Attached &&
                        applied.tracks[1].second ==
                            livekit::render::VideoRenderSession::AttachResult::Attached,
                    "the first accepted plan must attach both render leases")) return 1;
        a.selection.track->notifyI420VideoFrame(MakeFrame(30));
        b.selection.track->notifyI420VideoFrame(MakeFrame(60));
        leased.RenderLatestFrames();
        if (!Expect(delivered.size() == 2,
                    "both leases from the first plan must render")) return 1;

        delivered.clear();
        applied = leased.ApplySelection(9, 2, {b.selection, c.selection});
        if (!Expect(applied.attached == 2 &&
                        leased.statistics().attached_track_count == 2 &&
                        leased.statistics().rejected_track_attachments == 0,
                    "selection changes must release A before admitting C")) return 1;
        a.selection.track->notifyI420VideoFrame(MakeFrame(90));
        b.selection.track->notifyI420VideoFrame(MakeFrame(120));
        c.selection.track->notifyI420VideoFrame(MakeFrame(150));
        leased.RenderLatestFrames();
        if (!Expect(delivered.size() == 2 && !delivered.count("TR_LEASE_A") &&
                        delivered["TR_LEASE_B"] == 120 &&
                        delivered["TR_LEASE_C"] == 150,
                    "a retired page lease must reject late frames")) return 1;

        delivered.clear();
        b.binding_state->active.store(false, std::memory_order_release);
        b.selection.track->notifyI420VideoFrame(MakeFrame(180));
        c.selection.track->notifyI420VideoFrame(MakeFrame(210));
        leased.RenderLatestFrames();
        if (!Expect(delivered.size() == 1 && delivered["TR_LEASE_C"] == 210,
                    "an inactive media binding ticket must reject frames before render")) return 1;

        const auto stale = leased.ApplySelection(9, 1, {a.selection});
        if (!Expect(stale.tracks.size() == 1 &&
                        stale.tracks.front().second ==
                            livekit::render::VideoRenderSession::AttachResult::StaleSelection &&
                        stale.attached == 2,
                    "an old policy revision must not replace the current selection")) return 1;
    }

    // A producer remains alive while the UI retires/rebinds its local mailbox.
    // Delivery is UI-owned; the worker never captures the session or a canvas.
    {
        auto source = std::make_shared<livekit::VideoSource>(4, 4);
        std::atomic<bool> started{false};
        std::atomic<bool> done{false};
        int delivered = 0;
        livekit::render::VideoRenderSession local({});
        local.AttachLocalSource(source);
        local.UseGpuBackend([&](const std::string&, livekit::render::VideoRenderFrame::Ptr) { ++delivered; });
        std::thread capture([&] {
            auto frame = livekit::VideoFrame::create(4, 4, livekit::VideoBufferType::RGBA);
            started.store(true, std::memory_order_release);
            for (int i = 0; i < 4000; ++i) source->captureFrame(frame, i);
            done.store(true, std::memory_order_release);
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 100; ++i) {
            local.RenderLatestFrames();
            local.DetachLocalSource();
            local.AttachLocalSource(source);
        }
        local.Deactivate();
        const int before = delivered;
        capture.join();
        local.RenderLatestFrames();
        if (!Expect(done.load() && delivered == before && !local.active(),
                    "concurrent local detach/deactivate must reject all late frames")) return 1;
    }
    {
        livekit::render::VideoRenderSession share_session({}, 2);
        std::map<std::string, int> luminance;
        share_session.UseGpuBackend([&](const std::string& key, livekit::render::VideoRenderFrame::Ptr frame) {
            luminance[key] = frame->view().planes[0].data[0];
        });
        auto camera = std::make_shared<livekit::Track>("TR_CAMERA", "camera",
            livekit::TrackKind::Video, livekit::TrackSource::Camera);
        auto screen = std::make_shared<livekit::Track>("TR_SCREEN", "screen",
            livekit::TrackKind::Video, livekit::TrackSource::ScreenShareVideo);
        share_session.AttachRemoteTrack(camera, "same-participant", "camera-view");
        share_session.AttachRemoteTrack(screen, "same-participant", "screen-view");
        camera->notifyI420VideoFrame(MakeFrame(30));
        screen->notifyI420VideoFrame(MakeFrame(200));
        share_session.RenderLatestFrames();
        if (!Expect(luminance.size() == 2 && luminance["camera-view"] == 30 && luminance["screen-view"] == 200,
                    "camera and screen must render independently for the same participant")) return 1;
        share_session.RemoveTrack("TR_SCREEN");
        luminance.clear();
        camera->notifyI420VideoFrame(MakeFrame(40));
        screen->notifyI420VideoFrame(MakeFrame(220));
        share_session.RenderLatestFrames();
        if (!Expect(luminance.size() == 1 && luminance["camera-view"] == 40,
                    "screen stop must preserve camera and reject late screen frames")) return 1;
        share_session.RemoveTracksForIdentity("same-participant");
        if (!Expect(share_session.statistics().attached_track_count == 0, "departure removes all participant tracks")) return 1;
    }
    constexpr int kTrackCount = 9;
    constexpr int kFramesPerTrack = 2000;

    std::atomic<uint64_t> gpu_delivered{0};
    livekit::render::VideoRenderSession session({}, kTrackCount);
    session.UseGpuBackend(
        [&gpu_delivered](const std::string&, livekit::render::VideoRenderFrame::Ptr) {
            gpu_delivered.fetch_add(1, std::memory_order_relaxed);
        });

    std::vector<std::shared_ptr<livekit::Track>> tracks;
    tracks.reserve(kTrackCount);
    for (int index = 0; index != kTrackCount; ++index) {
        auto track = std::make_shared<livekit::Track>(
            "TR_STRESS_" + std::to_string(index), "stress", livekit::TrackKind::Video);
        session.AttachRemoteTrack(track, "participant-" + std::to_string(index));
        tracks.push_back(std::move(track));
    }

    std::atomic<int> ready{0};
    std::atomic<int> finished{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    producers.reserve(kTrackCount);
    for (int index = 0; index != kTrackCount; ++index) {
        producers.emplace_back([&, index]() {
            const auto frame = MakeFrame(static_cast<uint8_t>(16 + index));
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (!frame) {
                finished.fetch_add(1, std::memory_order_release);
                return;
            }
            for (int frame_index = 0; frame_index != kFramesPerTrack; ++frame_index) {
                tracks[index]->notifyI420VideoFrame(frame);
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    while (ready.load(std::memory_order_acquire) != kTrackCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) != kTrackCount) {
        session.RenderLatestFrames();
        std::this_thread::yield();
    }
    for (auto& producer : producers) {
        producer.join();
    }

    // Drain each per-track latest slot after producers stop.  The exact number
    // delivered while producers run is deliberately not asserted: latest-wins
    // semantics make it scheduler-dependent.
    session.RenderLatestFrames();
    session.RenderLatestFrames();
    const auto stats = session.statistics();
    const uint64_t delivered_before_deactivate = gpu_delivered.load(std::memory_order_relaxed);
    if (!Expect(stats.router.submitted == static_cast<uint64_t>(kTrackCount * kFramesPerTrack),
                "all active Track callbacks must reach the bounded Router") ||
        !Expect(stats.attached_track_count == kTrackCount &&
                    stats.router.dropped_capacity == 0 &&
                    stats.rejected_track_attachments == 0,
                "nine streams must remain within the configured subscription and slot bounds") ||
        !Expect(delivered_before_deactivate >= kTrackCount &&
                    delivered_before_deactivate <= stats.router.submitted &&
                    stats.delivered_to_gpu == delivered_before_deactivate,
                "DX11 consumption statistics must remain bounded by submitted frames")) {
        return 1;
    }

    session.Deactivate();
    for (const auto& track : tracks) {
        track->notifyI420VideoFrame(MakeFrame(235));
    }
    session.RenderLatestFrames();
    if (!Expect(!session.active() &&
                    gpu_delivered.load(std::memory_order_relaxed) == delivered_before_deactivate,
                "deactivation must detach every producer before late frames can render")) {
        return 1;
    }

    std::cout << "[VideoRenderSessionStressTest] PASS: "
              << stats.router.submitted << " submitted, "
              << stats.router.replaced_before_render << " replaced, "
              << delivered_before_deactivate << " rendered" << std::endl;
    return 0;
}
