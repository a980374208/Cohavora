#pragma once
#include "remote_control.h"
#include <set>

namespace livekit::remote_control {
// Used under the native input owner's mutex. Authorization outlives a pause;
// each admission transition changes the token so old queued/network input dies.
class InputArbitration {
public:
    static constexpr uint64_t LocalQuietMs = 1000;
    InputStatus status() const { return status_; }
    bool accepts(uint64_t epoch) const { return !status_.paused && epoch == status_.epoch; }
    bool locallyHeld(unsigned token) const { return localHeld_.contains(token); }
    void reset() { remotePaused_ = false; suspend(); }
    void pause(bool value) { remotePaused_ = value; suspend(); }
    void localActivity(uint64_t now, unsigned token = 0, bool down = false) {
        if (token) {
            if (down) localHeld_.insert(token); else localHeld_.erase(token);
        }
        quietUntil_ = now + LocalQuietMs;
        suspend();
    }
    void poll(uint64_t now, bool remoteKeysReleased) {
        if (status_.paused && !remotePaused_ && localHeld_.empty() && now >= quietUntil_ && remoteKeysReleased) {
            ++status_.epoch;
            status_.paused = false;
        }
    }
private:
    void suspend() {
        if (!status_.paused) ++status_.epoch;
        status_.paused = true;
    }
    InputStatus status_;
    bool remotePaused_ = false;
    uint64_t quietUntil_ = 0;
    std::set<unsigned> localHeld_;
};
}
