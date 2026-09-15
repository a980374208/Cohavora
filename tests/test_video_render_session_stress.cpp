#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

} // namespace

int main() {
    constexpr int kTrackCount = 9;
    constexpr int kFramesPerTrack = 2000;

    std::atomic<uint64_t> dx11_delivered{0};
    livekit::render::VideoRenderSession session({}, kTrackCount);
    session.UseDx11Backend(
        [&dx11_delivered](const std::string&, livekit::render::OwnedI420Frame::Ptr) {
            dx11_delivered.fetch_add(1, std::memory_order_relaxed);
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
    const uint64_t delivered_before_deactivate = dx11_delivered.load(std::memory_order_relaxed);
    if (!Expect(stats.router.submitted == static_cast<uint64_t>(kTrackCount * kFramesPerTrack),
                "all active Track callbacks must reach the bounded Router") ||
        !Expect(stats.attached_track_count == kTrackCount &&
                    stats.router.dropped_capacity == 0 &&
                    stats.rejected_track_attachments == 0,
                "nine streams must remain within the configured subscription and slot bounds") ||
        !Expect(delivered_before_deactivate >= kTrackCount &&
                    delivered_before_deactivate <= stats.router.submitted &&
                    stats.delivered_to_dx11 == delivered_before_deactivate,
                "DX11 consumption statistics must remain bounded by submitted frames")) {
        return 1;
    }

    session.Deactivate();
    for (const auto& track : tracks) {
        track->notifyI420VideoFrame(MakeFrame(235));
    }
    session.RenderLatestFrames();
    if (!Expect(!session.active() &&
                    dx11_delivered.load(std::memory_order_relaxed) == delivered_before_deactivate,
                "deactivation must detach every producer before late frames can render")) {
        return 1;
    }

    std::cout << "[VideoRenderSessionStressTest] PASS: "
              << stats.router.submitted << " submitted, "
              << stats.router.replaced_before_render << " replaced, "
              << delivered_before_deactivate << " rendered" << std::endl;
    return 0;
}
