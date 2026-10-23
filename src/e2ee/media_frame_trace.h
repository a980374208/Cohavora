#pragma once
#include "telemetry/media_diagnostic_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace livekit {

// Opt-in diagnostic metadata only. Never store payload, keys or object addresses.
enum class MediaFrameTracePhase : uint8_t {
    BindingCreated, BindingMapped, Input, Output, CopiedForDelivery
};

struct MediaFrameTraceRecord {
    MediaFrameTracePhase phase = MediaFrameTracePhase::Input;
    uint64_t sequence = 0, binding_id = 0, generation = 0;
    uint64_t room_binding_serial = 0, install_epoch = 0;
    uint32_t rtp_timestamp = 0, ssrc = 0;
    int64_t timestamp_us = 0, steady_ns = 0;
    int32_t width = 0, height = 0;
    bool ssrc_available = false, sif_match = false, sfu_vp8_signature = false;
};

class BoundedMediaFrameTrace final {
public:
    static constexpr size_t kCapacity = 4096;
    struct Batch {
        std::vector<MediaFrameTraceRecord> records;
        uint64_t overflow = 0, contention = 0;
        bool incomplete() const { return overflow != 0 || contention != 0; }
    };

#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    // Media callbacks never wait for a reader or allocate. Any lost metadata
    // invalidates a complete correlation claim; it is never evidence of absence.
    void Record(MediaFrameTraceRecord record) noexcept {
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                contention_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (size_ == kCapacity) {
                overflow_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            record.sequence = ++sequence_;
            record.steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            records_[(head_ + size_) % kCapacity] = record;
            ++size_;
        } catch (...) {
            contention_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Strand/test thread only. Allocate before taking the short metadata lock;
    // serialization and I/O belong to the caller after this method returns.
    Batch Drain() {
        Batch batch;
        batch.records.reserve(kCapacity);
        {
            std::lock_guard lock(mutex_);
            for (size_t i = 0; i < size_; ++i)
                batch.records.push_back(records_[(head_ + i) % kCapacity]);
            head_ = (head_ + size_) % kCapacity;
            size_ = 0;
        }
        batch.overflow = overflow_.load(std::memory_order_relaxed);
        batch.contention = contention_.load(std::memory_order_relaxed);
        return batch;
    }

#else
    constexpr void Record(MediaFrameTraceRecord) noexcept {}
    Batch Drain() const { return {}; }
#endif

    // Exact bitstream from SFU revision 3cfbd1242618a61178f7a05b126d6c0c4cac3731.
    // This identifies bytes, not a producer or an authenticated encrypted frame.
    static bool MatchesSfuVp8BlankFrame(const uint8_t* bytes, size_t size) {
        static constexpr std::array<uint8_t, 31> signature{
            0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00,
            0x08, 0x00, 0x00, 0x47, 0x08, 0x85, 0x85, 0x88,
            0x85, 0x84, 0x88, 0x02, 0x02, 0x00, 0x0c, 0x0d,
            0x60, 0x00, 0xfe, 0xff, 0xab, 0x50, 0x80};
        return size == signature.size() && bytes &&
            std::equal(signature.begin(), signature.end(), bytes);
    }

private:
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    std::mutex mutex_;
    std::array<MediaFrameTraceRecord, kCapacity> records_{};
    size_t head_ = 0, size_ = 0;
    uint64_t sequence_ = 0;
    std::atomic<uint64_t> overflow_{0}, contention_{0};
#endif
};

} // namespace livekit
