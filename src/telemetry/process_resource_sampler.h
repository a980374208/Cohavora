#pragma once

#include "session_telemetry.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace livekit::telemetry {

class ProcessResourceSampler final {
public:
    using Clock = std::chrono::steady_clock;

    ProcessResourceSample Sample();

    static std::optional<double> ComputeNormalizedCpuPercent(
        std::uint64_t process_time_delta_100ns,
        std::chrono::nanoseconds wall_time_delta,
        std::uint32_t logical_processor_count) noexcept;

private:
    std::uint64_t previous_process_time_100ns_ = 0;
    Clock::time_point previous_wall_time_{};
    bool has_cpu_baseline_ = false;
    std::uint32_t logical_processor_count_ = 0;
    std::uint32_t cached_thread_count_ = 0;
    Clock::time_point thread_count_sampled_at_{};
};

} // namespace livekit::telemetry
