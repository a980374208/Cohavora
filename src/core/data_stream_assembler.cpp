#include "data_stream_assembler.h"

#include <limits>
#include <utility>

namespace livekit {

IncomingDataStreamAssembler::IncomingDataStreamAssembler()
    : IncomingDataStreamAssembler(Limits{}) {}

IncomingDataStreamAssembler::IncomingDataStreamAssembler(Limits limits)
    : limits_(limits) {}

bool IncomingDataStreamAssembler::Begin(
    std::string stream_id,
    std::string topic,
    uint64_t total_length,
    std::string sender_identity,
    std::string sender_sid,
    TimePoint now) {
    if (stream_id.empty() || total_length == 0 ||
        total_length > limits_.max_stream_size ||
        total_length > std::numeric_limits<size_t>::max()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    MaybePurgeExpiredLocked(now);

    if (auto existing = streams_.find(stream_id); existing != streams_.end()) {
        EraseLocked(existing);
    }
    if (streams_.size() >= limits_.max_streams) {
        return false;
    }

    StreamState stream;
    stream.topic = std::move(topic);
    stream.total_length = static_cast<size_t>(total_length);
    stream.sender_identity = std::move(sender_identity);
    stream.sender_sid = std::move(sender_sid);
    stream.started_at = now;
    streams_.emplace(std::move(stream_id), std::move(stream));
    return true;
}

std::optional<AssembledDataStream> IncomingDataStreamAssembler::AddChunk(
    const std::string& stream_id,
    uint64_t chunk_index,
    std::span<const uint8_t> content,
    TimePoint now) {
    if (content.empty()) {
        return std::nullopt;
    }

    std::lock_guard lock(mutex_);
    MaybePurgeExpiredLocked(now);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return std::nullopt;
    }

    auto& stream = it->second;
    if (stream.chunks.contains(chunk_index)) {
        return std::nullopt;
    }
    if (stream.chunks.size() >= limits_.max_chunks_per_stream ||
        content.size() > stream.total_length - stream.received_bytes ||
        content.size() > limits_.max_buffered_bytes - buffered_bytes_) {
        EraseLocked(it);
        return std::nullopt;
    }

    stream.chunks.emplace(
        chunk_index,
        std::vector<uint8_t>(content.begin(), content.end()));
    stream.received_bytes += content.size();
    buffered_bytes_ += content.size();

    if (!IsCompleteLocked(stream)) {
        return std::nullopt;
    }
    return AssembleAndEraseLocked(it);
}

std::optional<AssembledDataStream> IncomingDataStreamAssembler::Finish(
    const std::string& stream_id,
    TimePoint now) {
    std::lock_guard lock(mutex_);
    MaybePurgeExpiredLocked(now);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return std::nullopt;
    }
    if (!IsCompleteLocked(it->second)) {
        EraseLocked(it);
        return std::nullopt;
    }
    return AssembleAndEraseLocked(it);
}

size_t IncomingDataStreamAssembler::PurgeExpired(TimePoint now) {
    std::lock_guard lock(mutex_);
    return PurgeExpiredLocked(now);
}

size_t IncomingDataStreamAssembler::active_streams() const {
    std::lock_guard lock(mutex_);
    return streams_.size();
}

size_t IncomingDataStreamAssembler::buffered_bytes() const {
    std::lock_guard lock(mutex_);
    return buffered_bytes_;
}

void IncomingDataStreamAssembler::MaybePurgeExpiredLocked(TimePoint now) {
    if (next_cleanup_ == TimePoint{} || now >= next_cleanup_) {
        PurgeExpiredLocked(now);
    }
}

size_t IncomingDataStreamAssembler::PurgeExpiredLocked(TimePoint now) {
    size_t removed = 0;
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (now - it->second.started_at >= limits_.stream_ttl) {
            buffered_bytes_ -= it->second.received_bytes;
            it = streams_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    next_cleanup_ = now + limits_.cleanup_interval;
    return removed;
}

bool IncomingDataStreamAssembler::IsCompleteLocked(const StreamState& stream) const {
    if (stream.received_bytes != stream.total_length || stream.chunks.empty()) {
        return false;
    }

    uint64_t expected_index = 0;
    for (const auto& [index, chunk] : stream.chunks) {
        if (index != expected_index || chunk.empty()) {
            return false;
        }
        ++expected_index;
    }
    return true;
}

std::optional<AssembledDataStream>
IncomingDataStreamAssembler::AssembleAndEraseLocked(StreamMap::iterator it) {
    AssembledDataStream result;
    result.payload.reserve(it->second.total_length);
    for (auto& [index, chunk] : it->second.chunks) {
        result.payload.insert(result.payload.end(), chunk.begin(), chunk.end());
    }
    result.topic = std::move(it->second.topic);
    result.sender_identity = std::move(it->second.sender_identity);
    result.sender_sid = std::move(it->second.sender_sid);
    EraseLocked(it);
    return result;
}

void IncomingDataStreamAssembler::EraseLocked(StreamMap::iterator it) {
    buffered_bytes_ -= it->second.received_bytes;
    streams_.erase(it);
}

} // namespace livekit
