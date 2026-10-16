#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace product_heap_diagnostic {

// Arithmetic/progress predicate for the VirtualQuery traversal.
inline bool RegionEnd(std::uintptr_t cursor, std::uintptr_t base,
                      std::uintptr_t size, std::uintptr_t& next) noexcept {
    if (size == 0 || base > cursor || size > (std::numeric_limits<std::uintptr_t>::max)() - base)
        return false;
    next = base + size;
    return next > cursor;
}

// Worker-owned, explicit opt-in. The caller must exclude join/export and other
// application activity during the intervention; no application locks are held.
class ReleaseGuard final {
public:
    using Clock = std::chrono::steady_clock;
    explicit ReleaseGuard(std::uint64_t generation = 0) noexcept : generation_(generation) {}
    bool Claim(std::uint64_t generation, bool complete, std::size_t cleanup_pending,
               Clock::time_point now) noexcept {
        if (claimed_ || generation_ == 0) return false;
        if (generation != generation_ || !complete || cleanup_pending != 0) {
            settled_ = false;
            return false;
        }
        if (!settled_) { settled_ = true; since_ = now; return false; }
        if (now < since_ || now - since_ < std::chrono::seconds(5)) return false;
        claimed_ = true; // An unavailable/failed attempt is also consumed.
        return true;
    }
private:
    std::uint64_t generation_{};
    bool settled_{};
    bool claimed_{};
    Clock::time_point since_{};
};

struct CounterStamp {
    std::int64_t utc_ms{};
    std::int64_t elapsed_us{};
    std::uint32_t error{};
    bool available{};
};
struct MemorySample {
    CounterStamp process;
    std::uint64_t private_usage_bytes{};
    CounterStamp heaps;
    std::uint32_t process_heap_count{};
    CounterStamp summary;
    std::uint64_t allocated_bytes{}, committed_bytes{}, reserved_bytes{}, maximum_reserve_bytes{};
    CounterStamp virtual_memory;
    std::uint64_t private_commit_bytes{}, private_commit_regions{};
    bool virtual_query_complete{};
    bool virtual_query_overflow{};
    bool virtual_query_budget_exhausted{};
    std::uint32_t virtual_query_count{};
};
struct Intervention {
    std::uint64_t generation{};
    bool platform_available{};
    bool attempted{};
    bool succeeded{};
    bool journal_precondition_failed{};
    CounterStamp operation;
    MemorySample before{}, after{};
};
static_assert(std::is_trivially_copyable_v<Intervention>);

enum class Action { Observe, Optimize };
struct PageRow {
    std::uint64_t base{}, allocation_base{}, size{};
    std::uint32_t state{}, type{}, protect{};
};
struct PageSample {
    CounterStamp stamp;
    PageRow rows[4096]{};
    std::uint64_t heap_handles[64]{};
    std::uint64_t default_heap{};
    std::uint32_t row_count{}, query_count{}, heap_count{}, heap_handles_count{};
    CounterStamp heap_stamp;
    bool complete{}, overflow{}, truncated{}, query_budget_exhausted{}, heap_handles_complete{}, heap_handles_truncated{};
};
struct PageJournal {
    std::uint64_t workspace_base{}, workspace_size{};
    PageSample before{}, after{};
};
static_assert(std::is_trivially_copyable_v<PageJournal>);
inline bool AppendPage(PageSample& sample, PageRow row) noexcept {
    if (sample.row_count >= 4096) { sample.truncated = true; return false; }
    sample.rows[sample.row_count++] = row;
    return true;
}
inline bool PagesReady(const PageSample& pages) noexcept {
    if (!pages.stamp.available || !pages.complete || pages.truncated || pages.overflow ||
        pages.query_budget_exhausted || !pages.heap_stamp.available ||
        !pages.heap_handles_complete || pages.heap_handles_truncated || pages.default_heap == 0 ||
        pages.heap_handles_count == 0 || pages.heap_handles_count > 64) return false;
    for (std::uint32_t i = 0; i < pages.heap_handles_count; ++i)
        if (pages.heap_handles[i] == pages.default_heap) return true;
    return false;
}
// Private VirtualAlloc storage is established and touched before either sample,
// retained through both sides, and explicitly identified in the journal.
class JournalWorkspace final {
public:
    explicit JournalWorkspace(bool enabled) noexcept {
#if defined(_WIN32)
        if (enabled) {
            data_ = static_cast<PageJournal*>(::VirtualAlloc(nullptr, sizeof(PageJournal),
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (data_) {
                // Start the POD object's lifetime and touch storage before sampling.
                data_ = ::new (static_cast<void*>(data_)) PageJournal{};
                data_->workspace_base = reinterpret_cast<std::uintptr_t>(data_);
                SYSTEM_INFO system{}; ::GetSystemInfo(&system);
                data_->workspace_size = (sizeof(PageJournal) + system.dwPageSize - 1) /
                    system.dwPageSize * system.dwPageSize;
            }
        }
#else
        (void)enabled;
#endif
    }
    ~JournalWorkspace() {
#if defined(_WIN32)
        if (data_) ::VirtualFree(data_, 0, MEM_RELEASE);
#endif
    }
    JournalWorkspace(const JournalWorkspace&) = delete;
    JournalWorkspace& operator=(const JournalWorkspace&) = delete;
    PageJournal* get() noexcept { return data_; }
private:
    PageJournal* data_{};
};

inline std::int64_t UtcMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline void Finish(CounterStamp& stamp, ReleaseGuard::Clock::time_point start) noexcept {
    stamp.elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        ReleaseGuard::Clock::now() - start).count();
}

inline void SampleHeapHandles(PageSample& result) noexcept {
#if defined(_WIN32)
    auto start = ReleaseGuard::Clock::now();
    result.heap_stamp.utc_ms = UtcMs();
    result.default_heap = reinterpret_cast<std::uintptr_t>(::GetProcessHeap());
    HANDLE handles[64]{};
    const DWORD count = ::GetProcessHeaps(64, handles);
    result.heap_count = count;
    result.heap_handles_truncated = count > 64;
    result.heap_handles_complete = count > 0 && count <= 64;
    result.heap_stamp.available = result.heap_handles_complete;
    if (count == 0) result.heap_stamp.error = ::GetLastError();
    // When capacity is insufficient the returned buffer is not promoted to a
    // complete list. Retain no potentially unspecified partial handle values.
    if (result.heap_handles_complete) {
        result.heap_handles_count = count;
        for (DWORD i = 0; i < count; ++i)
            result.heap_handles[i] = reinterpret_cast<std::uintptr_t>(handles[i]);
    }
    Finish(result.heap_stamp, start);
#else
    (void)result;
#endif
}

// Counter-only mode retains no addresses. Optional journal retains address
// metadata only, never payload. No dynamic allocation/JSON/IO during sampling.
// Counters have separate timestamps: this is not an atomic memory snapshot.
inline MemorySample Sample(PageSample* pages = nullptr) noexcept {
    MemorySample result{};
#if defined(_WIN32)
    if (pages) SampleHeapHandles(*pages);
    auto start = ReleaseGuard::Clock::now();
    result.process.utc_ms = UtcMs();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    result.process.available = ::K32GetProcessMemoryInfo(::GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
    if (result.process.available) result.private_usage_bytes = memory.PrivateUsage;
    else result.process.error = ::GetLastError();
    Finish(result.process, start);

    start = ReleaseGuard::Clock::now();
    result.heaps.utc_ms = UtcMs();
    result.process_heap_count = ::GetProcessHeaps(0, nullptr);
    result.heaps.available = result.process_heap_count != 0;
    if (!result.heaps.available) result.heaps.error = ::GetLastError();
    Finish(result.heaps, start);

    start = ReleaseGuard::Clock::now();
    result.summary.utc_ms = UtcMs();
    HEAP_SUMMARY summary{};
    summary.cb = sizeof(summary);
    result.summary.available = ::HeapSummary(::GetProcessHeap(), 0, &summary) != FALSE;
    if (result.summary.available) {
        result.allocated_bytes = summary.cbAllocated;
        result.committed_bytes = summary.cbCommitted;
        result.reserved_bytes = summary.cbReserved;
        result.maximum_reserve_bytes = summary.cbMaxReserve;
    } else result.summary.error = ::GetLastError();
    Finish(result.summary, start);

    start = ReleaseGuard::Clock::now();
    result.virtual_memory.utc_ms = UtcMs();
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    while (cursor <= maximum) {
        if (result.virtual_query_count == 65536) { result.virtual_query_budget_exhausted = true; break; }
        ++result.virtual_query_count;
        MEMORY_BASIC_INFORMATION region{};
        if (::VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region)) {
            result.virtual_memory.error = ::GetLastError();
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto size = static_cast<std::uintptr_t>(region.RegionSize);
        std::uintptr_t next{};
        if (!RegionEnd(cursor, base, size, next)) {
            result.virtual_query_overflow = true;
            result.virtual_memory.error = ERROR_ARITHMETIC_OVERFLOW;
            break;
        }
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE) {
            if (region.RegionSize > (std::numeric_limits<std::uint64_t>::max)() - result.private_commit_bytes ||
                result.private_commit_regions == (std::numeric_limits<std::uint64_t>::max)()) {
                result.virtual_query_overflow = true;
                result.virtual_memory.error = ERROR_ARITHMETIC_OVERFLOW;
                break;
            }
            result.private_commit_bytes += region.RegionSize;
            ++result.private_commit_regions;
        }
        if (pages && region.Type == MEM_PRIVATE && (region.State == MEM_COMMIT || region.State == MEM_RESERVE)) {
            if (!AppendPage(*pages, {base, reinterpret_cast<std::uintptr_t>(region.AllocationBase),
                    region.RegionSize, region.State, region.Type, region.Protect})) break;
        }
        cursor = next;
        if (cursor > maximum) result.virtual_query_complete = true;
    }
    result.virtual_memory.available = result.virtual_query_complete;
    Finish(result.virtual_memory, start);
    if (pages) {
        pages->stamp = result.virtual_memory;
        pages->query_count = result.virtual_query_count;
        pages->complete = result.virtual_query_complete;
        pages->overflow = result.virtual_query_overflow;
        pages->query_budget_exhausted = result.virtual_query_budget_exhausted;
    }
#endif
    return result;
}

// Call only after ReleaseGuard::Claim returns true. Serialize the returned POD
// afterwards; unavailable/partial counters must become null, never numeric zero.
// A successful call does not guarantee decommit. Neither result proves live
// object growth/leak repair or the formal-run root cause. Default heap only.
inline Intervention Run(std::uint64_t generation, Action action = Action::Optimize,
                        PageJournal* journal = nullptr) noexcept {
    Intervention result{};
    result.generation = generation;
    if (generation == 0) return result;
#if defined(_WIN32)
    result.platform_available = true;
    result.before = Sample(journal ? &journal->before : nullptr);
    if (journal) {
        if (!PagesReady(journal->before)) {
            result.journal_precondition_failed = true;
            result.operation.utc_ms = UtcMs();
            result.operation.error = ERROR_INVALID_DATA;
            // Observe also remains unavailable: no valid paired intervention.
            return result;
        }
    }
    result.operation.utc_ms = UtcMs();
    const auto start = ReleaseGuard::Clock::now();
    result.operation.available = true;
    if (action == Action::Optimize) {
        result.attempted = true;
        HEAP_OPTIMIZE_RESOURCES_INFORMATION information{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
        result.succeeded = ::HeapSetInformation(::GetProcessHeap(), HeapOptimizeResources,
            &information, sizeof(information)) != FALSE;
        if (!result.succeeded) result.operation.error = ::GetLastError();
    }
    Finish(result.operation, start);
    result.after = Sample(journal ? &journal->after : nullptr);
#endif
    return result;
}

} // namespace product_heap_diagnostic
