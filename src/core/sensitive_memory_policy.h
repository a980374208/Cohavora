#pragma once

#include <atomic>

namespace livekit {

// Process-lifetime latch: third-party/Qt copies may survive logical teardown.
// This governs our own dump writer, not OS/admin/debugger memory acquisition.
inline std::atomic<bool> sensitive_memory_used{false};

inline void MarkSensitiveMemoryUsed() noexcept {
    sensitive_memory_used.store(true, std::memory_order_release);
}

inline bool ApplicationMemoryDumpAllowed() noexcept {
    return !sensitive_memory_used.load(std::memory_order_acquire);
}

} // namespace livekit
