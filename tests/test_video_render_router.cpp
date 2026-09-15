#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "render/owned_i420_frame.h"
#include "render/video_render_router.h"

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[VideoRenderRouterTest] FAILED: " << message << std::endl;
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
    using livekit::render::VideoRenderRouter;

    auto first = MakeFrame(16);
    auto second = MakeFrame(235);
    VideoRenderRouter router(7, 2);

    if (!Expect(!router.Submit("", 7, first), "empty track id must be rejected") ||
        !Expect(router.Submit("track-a", 7, first), "first frame must be accepted") ||
        !Expect(router.Submit("track-a", 7, second), "newer frame must replace the slot") ||
        !Expect(router.TakeLatest("track-a", 7) == second, "TakeLatest must return only the newest frame") ||
        !Expect(router.TakeLatest("track-a", 7) == nullptr, "taking a slot must clear it") ||
        !Expect(router.Submit("track-b", 7, first), "second track must be accepted") ||
        !Expect(router.RemoveTrack("track-b", 7), "RemoveTrack must remove its slot") ||
        !Expect(router.TakeLatest("track-b", 7) == nullptr, "removed slot must not retain a frame") ||
        !Expect(!router.Submit("track-a", 8, first), "wrong generation must be rejected")) {
        return 1;
    }

    const auto stats = router.statistics();
    if (!Expect(stats.submitted == 3, "submitted counter must count accepted frames") ||
        !Expect(stats.replaced_before_render == 1, "replacement counter must record latest-wins drops") ||
        !Expect(stats.dropped_invalid == 1, "invalid counter must record invalid input") ||
        !Expect(stats.rejected_generation == 1, "generation counter must record stale input")) {
        return 1;
    }

    VideoRenderRouter capacity_router(11, 1);
    if (!Expect(capacity_router.Submit("only-track", 11, first), "first capacity slot must be accepted") ||
        !Expect(!capacity_router.Submit("overflow-track", 11, second), "router capacity must be bounded") ||
        !Expect(capacity_router.statistics().dropped_capacity == 1, "capacity drop must be counted")) {
        return 1;
    }

    VideoRenderRouter binding_router(13, 1);
    if (!Expect(binding_router.RegisterTrackBinding("reconnect-track", 13, 1),
                "first Track binding must register") ||
        !Expect(binding_router.SubmitBound("reconnect-track", 13, 1, first),
                "frame from the first Track binding must be accepted") ||
        !Expect(binding_router.RegisterTrackBinding("reconnect-track", 13, 2),
                "replacement Track binding must register") ||
        !Expect(!binding_router.SubmitBound("reconnect-track", 13, 1, second),
                "in-flight frame from replaced Track binding must be rejected") ||
        !Expect(binding_router.TakeLatest("reconnect-track", 13) == nullptr,
                "binding replacement must discard the preceding Track frame") ||
        !Expect(binding_router.SubmitBound("reconnect-track", 13, 2, second),
                "frame from replacement Track binding must be accepted") ||
        !Expect(binding_router.TakeLatest("reconnect-track", 13) == second,
                "replacement Track frame must be readable") ||
        !Expect(binding_router.statistics().rejected_binding == 1,
                "binding rejection must be observable")) {
        return 1;
    }

    VideoRenderRouter concurrent_router(19, 4);
    constexpr int kThreadCount = 4;
    constexpr int kSubmitsPerThread = 100;
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int index = 0; index < kThreadCount; ++index) {
        workers.emplace_back([&concurrent_router, first]() {
            for (int frame_index = 0; frame_index < kSubmitsPerThread; ++frame_index) {
                concurrent_router.Submit("concurrent-track", 19, first);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (!Expect(concurrent_router.TakeLatest("concurrent-track", 19) != nullptr,
                "concurrent Submit must leave one readable latest frame") ||
        !Expect(concurrent_router.statistics().submitted == kThreadCount * kSubmitsPerThread,
                "concurrent Submit must not lose accepted accounting")) {
        return 1;
    }

    router.Deactivate(7);
    if (!Expect(!router.active(), "Deactivate must make the router inactive") ||
        !Expect(!router.Submit("track-a", 7, first), "deactivated router must reject new input")) {
        return 1;
    }

    std::cout << "[VideoRenderRouterTest] PASS" << std::endl;
    return 0;
}
