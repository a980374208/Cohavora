#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace livekit {

// The native cryptor returns the same owned frame on success. Dropped frames
// never complete. Bound this diagnostic ledger independently of that behavior:
// eviction loses evidence, never media, and never turns an unknown into proof.
class MediaFrameEvidence final {
public:
    void Admit(const void* frame, uint64_t epoch, bool cryptographic) {
        std::lock_guard lock(mutex_);
        // A dropped frame's address can be reused by the allocator.
        for (auto& entry : entries_) if (entry.frame == frame) entry = {};
        if (!frame || !cryptographic || !epoch || (epoch & 1)) return;
        entries_[next_] = {frame, epoch};
        next_ = (next_ + 1) % entries_.size();
    }
    bool Complete(const void* frame, uint64_t current_epoch) {
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_) {
            if (!frame || entry.frame != frame) continue;
            const auto admitted_epoch = entry.epoch;
            entry = {};
            if (!current_epoch || (current_epoch & 1) || admitted_epoch != current_epoch) return false;
            protected_epoch_.store(current_epoch, std::memory_order_release);
            return true;
        }
        return false;
    }
    uint64_t protected_epoch() const { return protected_epoch_.load(std::memory_order_acquire); }
private:
    struct Entry { const void* frame = nullptr; uint64_t epoch = 0; };
    std::mutex mutex_;
    std::array<Entry, 128> entries_{};
    std::size_t next_ = 0;
    std::atomic<uint64_t> protected_epoch_{0};
};

} // namespace livekit
