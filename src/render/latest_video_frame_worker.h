#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "render/latest_video_frame_core.h"

namespace livekit::render {

// Exclusive scheduler for one core. A mutation only wakes this single worker;
// it never enqueues one task per incoming frame. No Qt, Room or device owner.
// Call BeginClose from the control owner, and StopAndDrain from its cleanup
// thread while the native buffer prerequisites are still alive.
class LatestVideoFrameWorker final {
public:
    explicit LatestVideoFrameWorker(std::shared_ptr<LatestVideoFrameCore> core);
    ~LatestVideoFrameWorker();
    LatestVideoFrameWorker(const LatestVideoFrameWorker&) = delete;
    LatestVideoFrameWorker& operator=(const LatestVideoFrameWorker&) = delete;

    void BeginClose();
    void StopAndDrain();
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

private:
    struct WakeState {
        std::mutex mutex;
        std::condition_variable changed;
        uint64_t revision = 0;
        bool stopping = false;
    };
    void Run() noexcept;
    std::shared_ptr<LatestVideoFrameCore> core_;
    std::shared_ptr<WakeState> wake_;
    std::mutex stop_mutex_;
    std::thread thread_;
    std::atomic<bool> failed_{false};
};

} // namespace livekit::render
