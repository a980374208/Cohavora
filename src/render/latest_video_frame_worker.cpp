#include "latest_video_frame_worker.h"

#include <stdexcept>
#include <utility>

namespace livekit::render {

LatestVideoFrameWorker::LatestVideoFrameWorker(std::shared_ptr<LatestVideoFrameCore> core)
    : core_(std::move(core)), wake_(std::make_shared<WakeState>()) {
    if (!core_) throw std::invalid_argument("latest video core is required");
    core_->SetWakeCallback([weak = std::weak_ptr<WakeState>(wake_)] {
        if (const auto wake = weak.lock()) {
            {
                std::lock_guard lock(wake->mutex);
                ++wake->revision;
            }
            wake->changed.notify_one();
        }
    });
    try {
        thread_ = std::thread([this] { Run(); });
    } catch (...) {
        core_->SetWakeCallback({});
        throw;
    }
}

LatestVideoFrameWorker::~LatestVideoFrameWorker() {
    StopAndDrain();
}

void LatestVideoFrameWorker::BeginClose() {
    // Closing the gate does not run any frame destructor on the control thread.
    // The worker performs retirement after finishing its current conversion.
    core_->CloseAdmission();
    {
        std::lock_guard lock(wake_->mutex);
        wake_->stopping = true;
        ++wake_->revision;
    }
    wake_->changed.notify_one();
}

void LatestVideoFrameWorker::StopAndDrain() {
    // This owner must never be destroyed by a materializer running on itself.
    // A conversion has no access to the scheduler owner in the supported path.
    std::lock_guard stop_lock(stop_mutex_);
    if (thread_.get_id() == std::this_thread::get_id()) std::terminate();
    BeginClose();
    if (thread_.joinable()) thread_.join();
    core_->SetWakeCallback({});
    core_->Drain();
}

void LatestVideoFrameWorker::Run() noexcept {
    try {
        for (;;) {
            uint64_t revision;
            {
                std::lock_guard lock(wake_->mutex);
                if (wake_->stopping) break;
                revision = wake_->revision;
            }
            const auto now = LatestVideoFrameCore::Clock::now();
            if (auto work = core_->TryStart(now)) {
                OwnedI420Frame::Ptr frame;
                try { frame = work.Materialize(); }
                catch (...) { /* A failed frame does not strand its request. */ }
                work.Complete(std::move(frame), LatestVideoFrameCore::Clock::now());
                continue;
            }
            const auto deadline = core_->NextWakeup(LatestVideoFrameCore::Clock::now());
            std::unique_lock lock(wake_->mutex);
            const auto changed = [&] { return wake_->stopping || wake_->revision != revision; };
            if (deadline) wake_->changed.wait_until(lock, *deadline, changed);
            else wake_->changed.wait(lock, changed);
        }
    } catch (...) {
        failed_.store(true, std::memory_order_release);
    }
    core_->BeginClose();
}

} // namespace livekit::render
