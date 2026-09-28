#pragma once

#include <atomic>
#include <cstdint>

namespace OpenMeeting {

// Coordinators are recreated when the meeting window closes, while diagnostic
// and telemetry stores live for the entire process. A generation must therefore
// remain unique across coordinator lifetimes, not just within one window.
inline std::uint64_t NextMeetingSessionGeneration() {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace OpenMeeting
