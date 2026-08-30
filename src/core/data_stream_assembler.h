#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace livekit {

struct AssembledDataStream {
    std::vector<uint8_t> payload;
    std::string topic;
    std::string sender_identity;
    std::string sender_sid;
};

// Owns the lifecycle and memory limits of inbound chunked data streams.
// Keeping this state outside Room makes the hot data path independently
// testable and prevents untrusted stream metadata from growing memory without
// bounds.
class IncomingDataStreamAssembler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Limits {
        size_t max_streams = 64;
        size_t max_stream_size = 16 * 1024 * 1024;
        size_t max_buffered_bytes = 64 * 1024 * 1024;
        size_t max_chunks_per_stream = 4096;
        std::chrono::seconds stream_ttl{30};
        std::chrono::seconds cleanup_interval{1};
    };

    IncomingDataStreamAssembler();
    explicit IncomingDataStreamAssembler(Limits limits);

    bool Begin(std::string stream_id,
               std::string topic,
               uint64_t total_length,
               std::string sender_identity,
               std::string sender_sid,
               TimePoint now = Clock::now());

    std::optional<AssembledDataStream> AddChunk(
        const std::string& stream_id,
        uint64_t chunk_index,
        std::span<const uint8_t> content,
        TimePoint now = Clock::now());

    // A trailer finalizes a complete stream or discards an incomplete one.
    std::optional<AssembledDataStream> Finish(
        const std::string& stream_id,
        TimePoint now = Clock::now());

    size_t PurgeExpired(TimePoint now = Clock::now());
    size_t active_streams() const;
    size_t buffered_bytes() const;

private:
    struct StreamState {
        std::string topic;
        size_t total_length = 0;
        std::string sender_identity;
        std::string sender_sid;
        TimePoint started_at;
        std::map<uint64_t, std::vector<uint8_t>> chunks;
        size_t received_bytes = 0;
    };

    using StreamMap = std::unordered_map<std::string, StreamState>;

    void MaybePurgeExpiredLocked(TimePoint now);
    size_t PurgeExpiredLocked(TimePoint now);
    bool IsCompleteLocked(const StreamState& stream) const;
    std::optional<AssembledDataStream> AssembleAndEraseLocked(StreamMap::iterator stream);
    void EraseLocked(StreamMap::iterator stream);

    Limits limits_;
    mutable std::mutex mutex_;
    StreamMap streams_;
    size_t buffered_bytes_ = 0;
    TimePoint next_cleanup_{};
};

} // namespace livekit
