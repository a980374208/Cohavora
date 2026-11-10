#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace livekit {

// Per-subscription admission. Zero fps preserves every update (notably the
// last update of a screen share). No decoder, Qt or session ownership here.
class VideoFrameAdmission final {
public:
    using Clock = std::chrono::steady_clock;
    enum class Rejection { None, Inactive, Rate, Busy };

private:
    struct State {
        std::mutex mutex;
        bool active = true;
        bool in_flight = false;
        uint32_t fps = 0;
        uint64_t revision = 1;
        std::optional<Clock::time_point> next;
    };

public:
    class Permit {
    public:
        Permit(Permit&& other) noexcept
            : state_(std::exchange(other.state_, {})), revision_(other.revision_),
              time_(other.time_), reserved_(other.reserved_) {}
        Permit& operator=(Permit&&) = delete;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        ~Permit() { Finish(false); }
        bool Commit() { return Finish(true); }

    private:
        friend class VideoFrameAdmission;
        Permit(std::shared_ptr<State> state, uint64_t revision,
               Clock::time_point time, bool reserved)
            : state_(std::move(state)), revision_(revision), time_(time), reserved_(reserved) {}
        bool Finish(bool copied) {
            auto state = std::exchange(state_, {});
            if (!state) return false;
            std::lock_guard lock(state->mutex);
            if (!state->active || state->revision != revision_) return false;
            if (reserved_) {
                state->in_flight = false;
                if (copied) {
                    const auto period = std::chrono::nanoseconds(1000000000LL / state->fps);
                    // Preserve phase under small callback jitter; never accumulate
                    // catch-up credits after an idle or stalled producer.
                    state->next = state->next && time_ < *state->next + period
                        ? *state->next + period : time_ + period;
                }
            }
            return copied;
        }
        std::shared_ptr<State> state_;
        uint64_t revision_;
        Clock::time_point time_;
        bool reserved_;
    };

    explicit VideoFrameAdmission(uint32_t fps = 0) { state_->fps = fps; }

    std::optional<Permit> TryAcquire(Clock::time_point time, Rejection& reason) {
        std::lock_guard lock(state_->mutex);
        reason = Rejection::None;
        if (!state_->active) { reason = Rejection::Inactive; return {}; }
        if (state_->fps) {
            if (state_->in_flight) { reason = Rejection::Busy; return {}; }
            const auto period = std::chrono::nanoseconds(1000000000LL / state_->fps);
            const auto tolerance = std::min(period / 10, std::chrono::nanoseconds(2000000));
            if (state_->next && time + tolerance < *state_->next) {
                reason = Rejection::Rate;
                return {};
            }
            state_->in_flight = true;
        }
        return Permit(state_, state_->revision, time, state_->fps != 0);
    }

    void SetMaxFps(uint32_t fps) {
        std::lock_guard lock(state_->mutex);
        if (state_->fps == fps || !state_->active) return;
        state_->fps = fps;
        ++state_->revision;
        state_->in_flight = false;
        state_->next.reset();
    }
    void Deactivate() {
        std::lock_guard lock(state_->mutex);
        state_->active = false;
        ++state_->revision;
        state_->in_flight = false;
    }
    bool active() const {
        std::lock_guard lock(state_->mutex);
        return state_->active;
    }

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace livekit
