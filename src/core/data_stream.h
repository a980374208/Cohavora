#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "livekit_models.pb.h"

namespace livekit {

/// Chunk size for data streams (matches LiveKit standard kStreamChunkSize = 15 KB).
constexpr std::size_t kStreamChunkSize = 15'000;

/// Base metadata for any stream (text or bytes).
struct BaseStreamInfo {
    std::string stream_id;
    std::string mime_type;
    std::string topic;
    int64_t timestamp = 0;
    std::optional<std::size_t> total_length;
    std::map<std::string, std::string> attributes;
    std::string sender_identity;
    std::string sender_sid;
};

/// Metadata for a text stream.
struct TextStreamInfo : BaseStreamInfo {
    int32_t operation_type = 0;
    int32_t version = 0;
    std::string reply_to_stream_id;
    std::vector<std::string> attached_stream_ids;
    bool generated = false;
};

/// Metadata for a byte stream (file, image, binary payload).
struct ByteStreamInfo : BaseStreamInfo {
    std::string name;
};

// =========================================================================
// Readers (Incoming Streams)
// =========================================================================

inline constexpr char kDataStreamActiveReaderLimitExceeded[] =
    "data stream active reader limit exceeded";
inline constexpr char kDataStreamBufferLimitExceeded[] =
    "data stream buffer limit exceeded";
inline constexpr char kDataStreamChunkLimitExceeded[] =
    "data stream queued chunk limit exceeded";
inline constexpr char kDataStreamReadAllLimitExceeded[] =
    "data stream ReadAll limit exceeded";
inline constexpr char kDataStreamDeclaredLengthExceeded[] =
    "data stream declared length exceeded";
inline constexpr char kDataStreamLengthMismatch[] =
    "data stream length mismatch";
inline constexpr char kDataStreamAssemblyRejected[] =
    "data stream assembly rejected";
inline constexpr char kDataStreamExpired[] =
    "data stream expired";
inline constexpr char kDataStreamReplaced[] =
    "data stream replaced";

/// Shared accounting for Reader-owned inbound memory. A Room owns one budget
/// per native session; retained Readers keep that session's accounting alive.
class DataStreamReaderBudget {
public:
    struct Limits {
        size_t max_active_readers = 64;
        size_t max_buffered_bytes_per_reader = 16 * 1024 * 1024;
        size_t max_buffered_bytes = 64 * 1024 * 1024;
        size_t max_queued_chunks_per_reader = 4096;
        size_t max_read_all_bytes = 16 * 1024 * 1024;
        std::chrono::seconds stream_ttl{30};
    };

    DataStreamReaderBudget();
    explicit DataStreamReaderBudget(Limits limits);

    bool TryAcquireReader();
    void ReleaseReader();
    bool TryReserveBuffered(size_t bytes);
    void ReleaseBuffered(size_t bytes);

    size_t active_readers() const;
    size_t buffered_bytes() const;
    const Limits& limits() const noexcept { return limits_; }

private:
    Limits limits_;
    mutable std::mutex mutex_;
    size_t active_readers_ = 0;
    size_t buffered_bytes_ = 0;
};

/// Reader for incoming text streams.
class TextStreamReader {
public:
    explicit TextStreamReader(TextStreamInfo info);
    TextStreamReader(TextStreamInfo info,
                     std::shared_ptr<DataStreamReaderBudget> budget);
    ~TextStreamReader();

    TextStreamReader(const TextStreamReader&) = delete;
    TextStreamReader& operator=(const TextStreamReader&) = delete;

    /// Blocking read of next text chunk.
    /// Returns false when the stream has ended.
    bool ReadNext(std::string& out);

    /// Convenience: read entire stream into a single string.
    /// Blocks until the stream is closed.
    std::string ReadAll();

    /// Non-blocking check for available chunks
    bool HasAvailableChunk() const;

    const TextStreamInfo& info() const noexcept { return info_; }
    bool admitted() const noexcept { return admitted_; }
    size_t buffered_bytes() const;
    bool is_closed() const;
    bool is_failed() const;
    const std::string& close_reason() const;

    /// Called by Room when a new chunk arrives
    void OnChunkUpdate(const std::string& text);
    bool TryOnChunkUpdate(const std::string& text);

    /// Called by Room when the stream is closed
    void OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs);
    void OnStreamError(const std::string& reason);

private:
    void ReleaseActiveLocked();
    void FailLocked(const std::string& reason);
    void MarkReadAllLimitExceeded();

    TextStreamInfo info_;
    std::shared_ptr<DataStreamReaderBudget> budget_;
    std::deque<std::string> queue_;
    size_t buffered_bytes_ = 0;
    size_t received_bytes_ = 0;
    size_t queued_chunks_ = 0;
    bool admitted_ = false;
    bool active_registered_ = false;
    bool closed_ = false;
    bool failed_ = false;
    std::string close_reason_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

/// Reader for incoming byte streams.
class ByteStreamReader {
public:
    explicit ByteStreamReader(ByteStreamInfo info);
    ByteStreamReader(ByteStreamInfo info,
                     std::shared_ptr<DataStreamReaderBudget> budget);
    ~ByteStreamReader();

    ByteStreamReader(const ByteStreamReader&) = delete;
    ByteStreamReader& operator=(const ByteStreamReader&) = delete;

    /// Blocking read of next byte chunk.
    /// Returns false when the stream has ended.
    bool ReadNext(std::vector<uint8_t>& out);

    /// Convenience: read entire stream into a single vector.
    /// Blocks until the stream is closed.
    std::vector<uint8_t> ReadAll();

    /// Non-blocking check for available chunks
    bool HasAvailableChunk() const;

    const ByteStreamInfo& info() const noexcept { return info_; }
    bool admitted() const noexcept { return admitted_; }
    size_t buffered_bytes() const;
    size_t received_bytes() const;
    bool is_closed() const;
    bool is_failed() const;
    const std::string& close_reason() const;

    /// Called by Room when a new chunk arrives
    void OnChunkUpdate(const uint8_t* data, size_t size);
    bool TryOnChunkUpdate(const uint8_t* data, size_t size);

    /// Called by Room when the stream is closed
    void OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs);
    void OnStreamError(const std::string& reason);

private:
    void ReleaseActiveLocked();
    void FailLocked(const std::string& reason);
    void MarkReadAllLimitExceeded();

    ByteStreamInfo info_;
    std::shared_ptr<DataStreamReaderBudget> budget_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t buffered_bytes_ = 0;
    size_t received_bytes_ = 0;
    size_t queued_chunks_ = 0;
    bool admitted_ = false;
    bool active_registered_ = false;
    bool closed_ = false;
    bool failed_ = false;
    std::string close_reason_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

// =========================================================================
// Writers (Outgoing Streams)
// =========================================================================

/// Callback function to emit a proto::DataPacket through the underlying DataChannel.
using StreamPacketPublisher = std::function<bool(const proto::DataPacket& packet, bool reliable)>;

class BaseStreamWriter {
public:
    virtual ~BaseStreamWriter() noexcept;

    const std::string& stream_id() const noexcept { return stream_id_; }
    const std::string& topic() const noexcept { return topic_; }
    const std::string& mime_type() const noexcept { return mime_type_; }
    int64_t timestamp_ms() const noexcept { return timestamp_ms_; }
    bool is_closed() const noexcept {
        return state_.load(std::memory_order_acquire) != State::Open;
    }

    /// Closes the stream normally with optional reason and trailing attributes.
    void Close(const std::string& reason = "", const std::map<std::string, std::string>& attributes = {});

    /// Cancels the stream prematurely with an error reason.
    void Cancel(const std::string& reason = "cancelled");

protected:
    BaseStreamWriter(StreamPacketPublisher publisher,
                     std::string topic,
                     std::map<std::string, std::string> attributes,
                     std::string stream_id,
                     std::optional<std::size_t> total_size,
                     std::string mime_type,
                     std::vector<std::string> destination_identities,
                     std::string sender_identity,
                     proto::DataStream::Header content_header);

    enum class State : uint8_t {
        Open,
        Closed,
        Failed,
    };

    void EnsureOpenOrRethrowLocked();
    void EnsureHeaderSent();
    void SendChunk(const uint8_t* data, size_t size);
    void SendTrailer(const std::string& reason,
                     const std::map<std::string, std::string>& attributes);
    void PublishPacket(const proto::DataPacket& packet, const char* stage);
    [[noreturn]] void FailLocked(std::exception_ptr error);

    StreamPacketPublisher publisher_;
    std::string stream_id_;
    std::string mime_type_;
    std::string topic_;
    int64_t timestamp_ms_ = 0;
    std::optional<std::size_t> total_size_;
    std::map<std::string, std::string> attributes_;
    std::vector<std::string> destination_identities_;
    std::string sender_identity_;
    // Own every header field before writing starts. Base destruction must not
    // consult Text/Byte members or dispatch into an already destroyed subtype.
    proto::DataStream::Header header_;

    std::atomic<State> state_{State::Open};
    // Protected by write_mutex_. The first failure is stable for every later
    // explicit operation, including non-standard publisher exceptions.
    std::exception_ptr first_failure_;
    bool header_sent_ = false;
    uint64_t next_chunk_index_ = 0;
    std::mutex write_mutex_;
};

/// Writer for outgoing text streams.
class TextStreamWriter : public BaseStreamWriter {
public:
    TextStreamWriter(StreamPacketPublisher publisher,
                     std::string topic = "",
                     std::map<std::string, std::string> attributes = {},
                     std::string stream_id = "",
                     std::optional<std::size_t> total_size = std::nullopt,
                     std::string reply_to_id = "",
                     std::vector<std::string> destination_identities = {},
                     std::string sender_identity = "");

    /// Write a UTF-8 string to the stream.
    /// Data will be automatically split into chunks of at most kStreamChunkSize bytes.
    void Write(const std::string& text);

    const TextStreamInfo& info() const noexcept { return info_; }

private:
    TextStreamInfo info_;
};

/// Writer for outgoing byte streams (files, images, binary data).
class ByteStreamWriter : public BaseStreamWriter {
public:
    ByteStreamWriter(StreamPacketPublisher publisher,
                     std::string name,
                     std::string topic = "",
                     std::map<std::string, std::string> attributes = {},
                     std::string stream_id = "",
                     std::optional<std::size_t> total_size = std::nullopt,
                     std::string mime_type = "application/octet-stream",
                     std::vector<std::string> destination_identities = {},
                     std::string sender_identity = "");

    /// Write binary data to the stream in kStreamChunkSize chunks.
    void Write(const std::vector<uint8_t>& data);
    void Write(const uint8_t* data, size_t size);

    const ByteStreamInfo& info() const noexcept { return info_; }

private:
    ByteStreamInfo info_;
};

} // namespace livekit
