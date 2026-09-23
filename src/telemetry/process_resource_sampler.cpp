#include "process_resource_sampler.h"

#include <algorithm>
#include <limits>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#endif

namespace livekit::telemetry {

namespace {

#if defined(_WIN32)

std::uint64_t FileTimeTo100ns(const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::optional<std::uint32_t> CountProcessThreads(DWORD process_id) noexcept {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t count = 0;
    if (::Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == process_id) ++count;
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot, &entry));
    } else {
        ::CloseHandle(snapshot);
        return std::nullopt;
    }
    ::CloseHandle(snapshot);
    return count;
}

#endif

} // namespace

std::optional<double> ProcessResourceSampler::ComputeNormalizedCpuPercent(
    std::uint64_t process_time_delta_100ns,
    std::chrono::nanoseconds wall_time_delta,
    std::uint32_t logical_processor_count) noexcept {
    if (wall_time_delta <= std::chrono::nanoseconds::zero() ||
        logical_processor_count == 0) {
        return std::nullopt;
    }
    const long double wall_100ns =
        static_cast<long double>(wall_time_delta.count()) / 100.0L;
    if (wall_100ns <= 0.0L) return std::nullopt;
    const long double normalized =
        100.0L * static_cast<long double>(process_time_delta_100ns) /
        (wall_100ns * static_cast<long double>(logical_processor_count));
    return static_cast<double>((std::clamp)(normalized, 0.0L, 100.0L));
}

ProcessResourceSample ProcessResourceSampler::Sample() {
    ProcessResourceSample sample;
    const auto started_at = Clock::now();
    sample.captured_at = started_at;
    sample.gpu_availability = Availability::Unsupported;
    sample.gpu_reason = "gpu_process_provider_not_configured";

#if defined(_WIN32)
    const HANDLE process = ::GetCurrentProcess();
    if (logical_processor_count_ == 0) {
        logical_processor_count_ = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (logical_processor_count_ == 0) {
            logical_processor_count_ = (std::max)(1u, std::thread::hardware_concurrency());
        }
    }
    sample.logical_processor_count = logical_processor_count_;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        const auto process_time = FileTimeTo100ns(kernel) + FileTimeTo100ns(user);
        if (has_cpu_baseline_ && process_time >= previous_process_time_100ns_) {
            const auto cpu = ComputeNormalizedCpuPercent(
                process_time - previous_process_time_100ns_,
                started_at - previous_wall_time_, logical_processor_count_);
            if (cpu) {
                sample.cpu_availability = Availability::Valid;
                sample.cpu_reason = "process_times_window_valid";
                sample.cpu_percent = *cpu;
            } else {
                sample.cpu_availability = Availability::Invalid;
                sample.cpu_reason = "process_times_invalid_window";
            }
        } else if (has_cpu_baseline_) {
            sample.cpu_availability = Availability::Invalid;
            sample.cpu_reason = "process_times_counter_reset";
        } else {
            sample.cpu_availability = Availability::WarmingUp;
            sample.cpu_reason = "process_times_baseline_warming_up";
        }
        previous_process_time_100ns_ = process_time;
        previous_wall_time_ = started_at;
        has_cpu_baseline_ = true;
    } else {
        sample.cpu_availability = Availability::Invalid;
        sample.cpu_reason = "process_times_query_failed";
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (::K32GetProcessMemoryInfo(process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        sample.memory_availability = Availability::Valid;
        sample.memory_reason = "process_memory_query_valid";
        sample.working_set_bytes = memory.WorkingSetSize;
        sample.peak_working_set_bytes = memory.PeakWorkingSetSize;
        sample.private_bytes = memory.PrivateUsage;
    } else {
        sample.memory_availability = Availability::Invalid;
        sample.memory_reason = "process_memory_query_failed";
    }

    DWORD handle_count = 0;
    if (::GetProcessHandleCount(process, &handle_count)) {
        sample.handle_count_availability = Availability::Valid;
        sample.handle_count_reason = "process_handle_count_valid";
        sample.handle_count = handle_count;
    } else {
        sample.handle_count_availability = Availability::Invalid;
        sample.handle_count_reason = "process_handle_count_query_failed";
    }

    const auto thread_refresh_due = thread_count_sampled_at_ == Clock::time_point{} ||
        started_at - thread_count_sampled_at_ >= std::chrono::seconds(5);
    if (thread_refresh_due) {
        if (const auto count = CountProcessThreads(::GetCurrentProcessId())) {
            cached_thread_count_ = *count;
            thread_count_sampled_at_ = started_at;
        }
    }
    if (thread_count_sampled_at_ != Clock::time_point{}) {
        sample.thread_count_availability = Availability::Valid;
        sample.thread_count_reason = thread_refresh_due
            ? "process_thread_count_valid"
            : "process_thread_count_cached";
        sample.thread_count = cached_thread_count_;
        sample.thread_count_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            started_at - thread_count_sampled_at_).count();
    } else {
        sample.thread_count_availability = Availability::Invalid;
        sample.thread_count_reason = "process_thread_count_query_failed";
    }
#else
    sample.cpu_availability = Availability::Unsupported;
    sample.cpu_reason = "process_cpu_sampler_unsupported_platform";
    sample.memory_availability = Availability::Unsupported;
    sample.memory_reason = "process_memory_sampler_unsupported_platform";
    sample.thread_count_availability = Availability::Unsupported;
    sample.thread_count_reason = "process_thread_sampler_unsupported_platform";
    sample.handle_count_availability = Availability::Unsupported;
    sample.handle_count_reason = "process_handle_sampler_unsupported_platform";
#endif

    const bool any_primary_value =
        sample.cpu_availability == Availability::Valid ||
        sample.cpu_availability == Availability::WarmingUp ||
        sample.memory_availability == Availability::Valid ||
        sample.thread_count_availability == Availability::Valid ||
        sample.handle_count_availability == Availability::Valid;
    const bool all_primary_values =
        (sample.cpu_availability == Availability::Valid ||
         sample.cpu_availability == Availability::WarmingUp) &&
        sample.memory_availability == Availability::Valid &&
        sample.thread_count_availability == Availability::Valid &&
        sample.handle_count_availability == Availability::Valid;
    sample.availability = any_primary_value
        ? Availability::Valid : Availability::Unsupported;
    sample.reason = all_primary_values
        ? "process_resource_sample_valid"
        : any_primary_value
            ? "process_resource_sample_partial"
            : "process_resource_sampler_unsupported";
    sample.capture_duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - started_at).count();
    return sample;
}

} // namespace livekit::telemetry
