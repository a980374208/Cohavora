#include "video_render_router.h"

#include <utility>

namespace livekit::render {

VideoRenderRouter::VideoRenderRouter(uint64_t generation, size_t max_active_tracks)
    : configured_generation_(generation),
      max_active_tracks_(max_active_tracks),
      active_generation_(generation) {}

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
        if (slot->frame) {
            replaced_before_render_.fetch_add(1, std::memory_order_relaxed);
        }
        slot->frame = std::move(frame);
    }
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
    slot->frame.reset();
    slot->binding_generation = binding_generation;
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
    if (slot->binding_generation != binding_generation) {
        rejected_binding_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (slot->frame) {
        replaced_before_render_.fetch_add(1, std::memory_order_relaxed);
    }
    slot->frame = std::move(frame);
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
    return tracks_.erase(track_id) != 0;
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

    std::lock_guard<std::mutex> lock(slot->mutex);
    return std::exchange(slot->frame, nullptr);
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
