#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "render/owned_i420_frame.h"
#include "../core/video_capacity.h"

namespace livekit::render {

// Thread-safe latest-frame router. It has no Qt, Room, Participant or D3D
// dependency: producers only replace a per-track slot, and the selected
// backend consumes at most one frame per track on its render tick.
class VideoRenderRouter final {
public:
    using LazyFrameProducer = std::function<OwnedI420Frame::Ptr()>;

    struct Statistics {
        uint64_t submitted = 0;
        uint64_t replaced_before_render = 0;
        uint64_t rejected_generation = 0;
        uint64_t rejected_binding = 0;
        uint64_t dropped_invalid = 0;
        uint64_t dropped_capacity = 0;
    };

    explicit VideoRenderRouter(uint64_t generation,
                               size_t max_active_tracks = kMaximumRemoteVideoTracks);
    ~VideoRenderRouter();

    VideoRenderRouter(const VideoRenderRouter&) = delete;
    VideoRenderRouter& operator=(const VideoRenderRouter&) = delete;

    bool Submit(const std::string& track_id, uint64_t generation, OwnedI420Frame::Ptr frame);
    // TakeLatest only requests conversion and consumes ready CPU output. A
    // background worker evaluates accepted producers and retires their captures.
    // DiscardLatest cancels demand and invalidates an in-flight result. Hidden
    // previews retain at most the latest pending input, without materializing it.
    // Admission is bounded; false means this preview frame should be dropped.
    bool SubmitLazy(const std::string& track_id, uint64_t generation, LazyFrameProducer producer);
    // Bound submissions are used by VideoRenderSession. Registering a new
    // binding atomically invalidates frames from a replaced Track with the
    // same SID, including callbacks already in flight during a reconnect.
    bool RegisterTrackBinding(const std::string& track_id,
                              uint64_t generation,
                              uint64_t binding_generation);
    bool SubmitBound(const std::string& track_id,
                     uint64_t generation,
                     uint64_t binding_generation,
                     OwnedI420Frame::Ptr frame);
    bool RemoveTrack(const std::string& track_id, uint64_t generation);
    OwnedI420Frame::Ptr TakeLatest(const std::string& track_id, uint64_t generation);
    void DiscardLatest(const std::string& track_id, uint64_t generation);
    void Deactivate(uint64_t generation);

    uint64_t generation() const noexcept { return configured_generation_; }
    bool active() const noexcept { return active_generation_.load(std::memory_order_acquire) == configured_generation_; }
    size_t active_track_count() const;
    Statistics statistics() const noexcept;

private:
    struct LatestFrameSlot {
        std::mutex mutex;
        std::condition_variable changed;
        OwnedI420Frame::Ptr frame;
        std::unique_ptr<LazyFrameProducer> lazy_producer;
        std::unique_ptr<LazyFrameProducer> retired_producer;
        uint64_t binding_generation = 0;
        uint64_t epoch = 0;
        bool closed = false;
        bool lazy_worker_started = false;
        bool requested = false;
        bool discard_pending = false;
    };

    // Caller holds slot->mutex. No worker operation or capture destruction may
    // hold that lock, so UI invalidation never waits for GPU work or retirement.
    static bool StartLazyWorker(const std::shared_ptr<LatestFrameSlot>& slot);
    static void RunLazyWorker(std::shared_ptr<LatestFrameSlot> slot);
    static void CloseSlot(const std::shared_ptr<LatestFrameSlot>& slot);

    std::shared_ptr<LatestFrameSlot> FindSlotForSubmit(const std::string& track_id, uint64_t generation);

    const uint64_t configured_generation_;
    const size_t max_active_tracks_;
    std::atomic<uint64_t> active_generation_;

    mutable std::mutex tracks_mutex_;
    std::unordered_map<std::string, std::shared_ptr<LatestFrameSlot>> tracks_;

    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> replaced_before_render_{0};
    std::atomic<uint64_t> rejected_generation_{0};
    std::atomic<uint64_t> rejected_binding_{0};
    std::atomic<uint64_t> dropped_invalid_{0};
    std::atomic<uint64_t> dropped_capacity_{0};
};

} // namespace livekit::render
