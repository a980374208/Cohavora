#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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

/// Reader for incoming text streams.
class TextStreamReader {
public:
    explicit TextStreamReader(TextStreamInfo info);
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
    bool is_closed() const;
    const std::string& close_reason() const;

    /// Called by Room when a new chunk arrives
    void OnChunkUpdate(const std::string& text);

    /// Called by Room when the stream is closed
    void OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs);

private:
    TextStreamInfo info_;
    std::deque<std::string> queue_;
    bool closed_ = false;
    std::string close_reason_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

/// Reader for incoming byte streams.
class ByteStreamReader {
public:
    explicit ByteStreamReader(ByteStreamInfo info);
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
    size_t received_bytes() const;
    bool is_closed() const;
    const std::string& close_reason() const;

    /// Called by Room when a new chunk arrives
    void OnChunkUpdate(const uint8_t* data, size_t size);

    /// Called by Room when the stream is closed
    void OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs);

private:
    ByteStreamInfo info_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t received_bytes_ = 0;
    bool closed_ = false;
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
    bool is_closed() const noexcept { return closed_; }

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

    void EnsureHeaderSent();
    void SendChunk(const uint8_t* data, size_t size);
    void SendTrailer(const std::string& reason, const std::map<std::string, std::string>& attributes);

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

    bool closed_ = false;
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
