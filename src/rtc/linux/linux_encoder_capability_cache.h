#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace livekit::detail {

// Snapshot must provide generation, revision, ready and probing. The whole
// owned value is committed together; flags can never describe another device.
template <typename Snapshot>
class LinuxEncoderCapabilityCache final {
public:
    struct Ticket {
        std::uint64_t generation = 0;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    using Merge = void (*)(Snapshot&, const Snapshot&);
    explicit LinuxEncoderCapabilityCache(Snapshot initial, Merge merge = nullptr)
        : snapshot_(std::move(initial)), merge_(merge) {
        snapshot_.generation = generation_;
    }
    Snapshot Read() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
    std::optional<Ticket> Begin() {
        std::lock_guard lock(mutex_);
        if (sealed_ || snapshot_.ready || snapshot_.probing) return std::nullopt;
        active_ = std::make_shared<std::atomic<bool>>(false);
        snapshot_.probing = true;
        snapshot_.revision = ++revision_;
        return Ticket{generation_, active_};
    }
    bool Commit(const Ticket& ticket, Snapshot completed) {
        std::lock_guard lock(mutex_);
        if (sealed_ || ticket.generation != generation_ || ticket.cancelled != active_ ||
            !active_ || active_->load(std::memory_order_acquire)) return false;
        completed.generation = generation_;
        completed.revision = ++revision_;
        completed.ready = true;
        completed.probing = false;
        if (merge_) merge_(completed, snapshot_);
        snapshot_ = std::move(completed);
        active_.reset();
        return true;
    }
    bool Invalidate(Snapshot pending) {
        std::lock_guard lock(mutex_);
        if (sealed_) return false;
        Replace(std::move(pending), false);
        return true;
    }
    void Seal(Snapshot retired) {
        std::lock_guard lock(mutex_);
        if (sealed_) return;
        Replace(std::move(retired), true);
        sealed_ = true;
    }

private:
    void Replace(Snapshot snapshot, bool ready) {
        if (active_) active_->store(true, std::memory_order_release);
        active_.reset();
        snapshot.generation = ++generation_;
        snapshot.revision = ++revision_;
        snapshot.probing = false;
        snapshot.ready = ready;
        if (merge_) merge_(snapshot, snapshot_);
        snapshot_ = std::move(snapshot);
    }

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::uint64_t generation_ = 1;
    std::uint64_t revision_ = 0;
    std::shared_ptr<std::atomic<bool>> active_;
    bool sealed_ = false;
    Merge merge_ = nullptr;
};

} // namespace livekit::detail
