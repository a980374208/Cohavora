#include "video_render_router.h"

#include <thread>
#include <utility>

namespace livekit::render {

VideoRenderRouter::VideoRenderRouter(uint64_t generation, size_t max_active_tracks)
    : configured_generation_(generation),
      max_active_tracks_(max_active_tracks),
      active_generation_(generation) {}

VideoRenderRouter::~VideoRenderRouter() {
    Deactivate(configured_generation_);
}

bool VideoRenderRouter::StartLazyWorker(const std::shared_ptr<LatestFrameSlot>& slot) {
    if (slot->lazy_worker_started) return true;

    // Only local screen preview uses lazy producers. Allow the current preview
    // and one retiring predecessor; a stuck driver must not accumulate detached
    // workers/native frames when the user repeatedly restarts sharing. The tiny
    // admission counter lives for the process, including late worker retirement.
    static auto* const budget = new std::atomic<size_t>(0);
    auto count = budget->load(std::memory_order_relaxed);
    do {
        if (count >= 2) return false;
    } while (!budget->compare_exchange_weak(count, count + 1, std::memory_order_acq_rel));

    slot->lazy_worker_started = true;
    try {
        std::thread([slot, lifetime = budget] {
            RunLazyWorker(slot);
            lifetime->fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    } catch (...) {
        slot->lazy_worker_started = false;
        budget->fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void VideoRenderRouter::RunLazyWorker(std::shared_ptr<LatestFrameSlot> slot) {
    for (;;) {
        std::unique_ptr<LazyFrameProducer> producer;
        uint64_t epoch = 0;
        bool materialize = false;
        {
            std::unique_lock lock(slot->mutex);
            slot->changed.wait(lock, [&] {
                return slot->closed || slot->retired_producer ||
                    (slot->lazy_producer && (slot->discard_pending ||
                     (slot->requested && !slot->frame)));
            });
            if (slot->retired_producer) {
                producer = std::move(slot->retired_producer);
            } else if (slot->lazy_producer) {
                producer = std::move(slot->lazy_producer);
                materialize = !slot->closed && !slot->discard_pending && slot->requested;
                epoch = slot->epoch;
                slot->discard_pending = false;
            } else if (slot->closed) {
                break;
            }
        }

        OwnedI420Frame::Ptr frame;
        if (materialize) {
            try { frame = (*producer)(); } catch (...) { /* Drop a failed preview. */ }
        }
        // A producer can retain a D3D texture or perform slow final release.
        // Both materialization and retirement happen outside the metadata lock.
        producer.reset();
        if (frame) {
            std::lock_guard lock(slot->mutex);
            if (!slot->closed && slot->epoch == epoch && slot->requested && !slot->frame) {
                slot->frame = std::move(frame);
                slot->requested = false;
            }
        }
    }
}

void VideoRenderRouter::CloseSlot(const std::shared_ptr<LatestFrameSlot>& slot) {
    {
        std::lock_guard lock(slot->mutex);
        slot->closed = true;
        ++slot->epoch;
        slot->requested = false;
        slot->frame.reset();
    }
    // The self-contained worker owns its State until its active call and all
    // pending native owners retire. The UI never joins or destroys those owners.
    slot->changed.notify_one();
}

bool VideoRenderRouter::Submit(const std::string& track_id,
                               uint64_t generation,
                               OwnedI420Frame::Ptr frame) {
    if (track_id.empty() || !frame) {
        dropped_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto slot = FindSlotForSubmit(track_id, generation);
    if (!slot) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->closed) return false;
        if (slot->frame || slot->lazy_producer) {
            replaced_before_render_.fetch_add(1, std::memory_order_relaxed);
        }
        ++slot->epoch;
        slot->requested = false;
        slot->discard_pending = true;
        slot->frame = std::move(frame);
    }
    slot->changed.notify_one();
    submitted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VideoRenderRouter::SubmitLazy(const std::string& track_id,
                                   uint64_t generation,
                                   LazyFrameProducer producer) {
    if (track_id.empty() || !producer) {
        dropped_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto slot = FindSlotForSubmit(track_id, generation);
    if (!slot) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->closed) return false;
        if ((slot->lazy_producer && slot->retired_producer) || !StartLazyWorker(slot)) {
            dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto pending = std::make_unique<LazyFrameProducer>(std::move(producer));
        if (slot->frame || slot->lazy_producer) {
            replaced_before_render_.fetch_add(1, std::memory_order_relaxed);
        }
        // Never retire an accepted GPU owner on the capture/UI caller. If the
        // retirement slot is occupied, admission above drops the new input.
        if (slot->lazy_producer) slot->retired_producer = std::move(slot->lazy_producer);
        slot->lazy_producer = std::move(pending);
        slot->discard_pending = false;
    }
    slot->changed.notify_one();
    submitted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VideoRenderRouter::RegisterTrackBinding(const std::string& track_id,
                                              uint64_t generation,
                                              uint64_t binding_generation) {
    if (track_id.empty() || binding_generation == 0) {
        dropped_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::shared_ptr<LatestFrameSlot> slot;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (active_generation_.load(std::memory_order_relaxed) != generation) {
            rejected_generation_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto existing = tracks_.find(track_id);
        if (existing != tracks_.end()) {
            slot = existing->second;
        } else {
            if (tracks_.size() >= max_active_tracks_) {
                dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            slot = std::make_shared<LatestFrameSlot>();
            tracks_.emplace(track_id, slot);
        }
    }

    // Clear a frame left by the preceding Track while publishing the new
    // binding identity under the same lock used by SubmitBound.
    std::lock_guard<std::mutex> lock(slot->mutex);
    if (slot->closed) return false;
    ++slot->epoch;
    slot->requested = false;
    slot->discard_pending = true;
    slot->frame.reset();
    slot->binding_generation = binding_generation;
    slot->changed.notify_one();
    return true;
}

bool VideoRenderRouter::SubmitBound(const std::string& track_id,
                                    uint64_t generation,
                                    uint64_t binding_generation,
                                    OwnedI420Frame::Ptr frame) {
    if (track_id.empty() || binding_generation == 0 || !frame) {
        dropped_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::shared_ptr<LatestFrameSlot> slot;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (active_generation_.load(std::memory_order_relaxed) != generation) {
            rejected_generation_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto it = tracks_.find(track_id);
        if (it == tracks_.end()) {
            rejected_binding_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slot = it->second;
    }

    std::lock_guard<std::mutex> lock(slot->mutex);
    if (slot->closed || slot->binding_generation != binding_generation) {
        rejected_binding_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (slot->frame || slot->lazy_producer) {
        replaced_before_render_.fetch_add(1, std::memory_order_relaxed);
    }
    ++slot->epoch;
    slot->requested = false;
    slot->discard_pending = true;
    slot->frame = std::move(frame);
    slot->changed.notify_one();
    submitted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VideoRenderRouter::RemoveTrack(const std::string& track_id, uint64_t generation) {
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> lock(tracks_mutex_);
    if (active_generation_.load(std::memory_order_relaxed) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return false;
    CloseSlot(it->second);
    tracks_.erase(it);
    return true;
}

OwnedI420Frame::Ptr VideoRenderRouter::TakeLatest(const std::string& track_id, uint64_t generation) {
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    std::shared_ptr<LatestFrameSlot> slot;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (active_generation_.load(std::memory_order_relaxed) != generation) {
            rejected_generation_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        const auto it = tracks_.find(track_id);
        if (it == tracks_.end()) {
            return nullptr;
        }
        slot = it->second;
    }

    OwnedI420Frame::Ptr frame;
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->closed) return nullptr;
        frame = std::exchange(slot->frame, nullptr);
        // One outstanding credit, even when the UI polls during a slow Map.
        // No producer or capture owner ever reaches the consuming UI thread.
        slot->requested = true;
    }
    slot->changed.notify_one();
    return frame;
}

void VideoRenderRouter::DiscardLatest(const std::string& track_id, uint64_t generation) {
    if (active_generation_.load(std::memory_order_acquire) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::shared_ptr<LatestFrameSlot> slot;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (active_generation_.load(std::memory_order_relaxed) != generation) {
            rejected_generation_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto it = tracks_.find(track_id);
        if (it == tracks_.end()) {
            return;
        }
        slot = it->second;
    }

    std::lock_guard<std::mutex> lock(slot->mutex);
    ++slot->epoch;
    slot->requested = false;
    slot->discard_pending = true;
    slot->frame.reset();
    slot->changed.notify_one();
}

void VideoRenderRouter::Deactivate(uint64_t generation) {
    uint64_t expected = generation;
    if (!active_generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        if (expected != 0) {
            rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(tracks_mutex_);
    for (const auto& [id, slot] : tracks_) CloseSlot(slot);
    tracks_.clear();
}

size_t VideoRenderRouter::active_track_count() const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return tracks_.size();
}

VideoRenderRouter::Statistics VideoRenderRouter::statistics() const noexcept {
    return {
        submitted_.load(std::memory_order_relaxed),
        replaced_before_render_.load(std::memory_order_relaxed),
        rejected_generation_.load(std::memory_order_relaxed),
        rejected_binding_.load(std::memory_order_relaxed),
        dropped_invalid_.load(std::memory_order_relaxed),
        dropped_capacity_.load(std::memory_order_relaxed),
    };
}

std::shared_ptr<VideoRenderRouter::LatestFrameSlot> VideoRenderRouter::FindSlotForSubmit(
    const std::string& track_id,
    uint64_t generation) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    if (active_generation_.load(std::memory_order_relaxed) != generation) {
        rejected_generation_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const auto existing = tracks_.find(track_id);
    if (existing != tracks_.end()) {
        return existing->second;
    }
    if (tracks_.size() >= max_active_tracks_) {
        dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    auto slot = std::make_shared<LatestFrameSlot>();
    tracks_.emplace(track_id, slot);
    return slot;
}

} // namespace livekit::render
