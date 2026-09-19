#include "data_stream.h"
#include "operation.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <random>
#include <sstream>
#include <iomanip>

namespace livekit {

namespace {

std::string GenerateStreamId(const std::string& prefix = "st_") {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t val = dist(rng);
    std::ostringstream oss;
    oss << prefix << std::hex << std::setfill('0') << std::setw(16) << val;
    return oss.str();
}

int64_t CurrentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

proto::DataStream::Header MakeTextContentHeader(const std::string& reply_to_id) {
    proto::DataStream::Header header;
    auto* text = header.mutable_text_header();
    text->set_operation_type(proto::DataStream::OperationType::DataStream_OperationType_CREATE);
    if (!reply_to_id.empty()) {
        text->set_reply_to_stream_id(reply_to_id);
    }
    return header;
}

proto::DataStream::Header MakeByteContentHeader(const std::string& name) {
    proto::DataStream::Header header;
    auto* bytes = header.mutable_byte_header();
    if (!name.empty()) {
        bytes->set_name(name);
    }
    return header;
}

} // namespace

// =========================================================================
// TextStreamReader Implementation
// =========================================================================

TextStreamReader::TextStreamReader(TextStreamInfo info)
    : info_(std::move(info)) {}

TextStreamReader::~TextStreamReader() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

bool TextStreamReader::ReadNext(std::string& out) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this]() {
        return !queue_.empty() || closed_;
    });

    if (!queue_.empty()) {
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    return false;
}

std::string TextStreamReader::ReadAll() {
    std::string result;
    std::string chunk;
    while (ReadNext(chunk)) {
        result.append(chunk);
    }
    return result;
}

bool TextStreamReader::HasAvailableChunk() const {
    std::lock_guard lock(mutex_);
    return !queue_.empty();
}

bool TextStreamReader::is_closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

const std::string& TextStreamReader::close_reason() const {
    std::lock_guard lock(mutex_);
    return close_reason_;
}

void TextStreamReader::OnChunkUpdate(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    queue_.push_back(text);
    cv_.notify_one();
}

void TextStreamReader::OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs) {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    closed_ = true;
    close_reason_ = reason;
    for (const auto& [k, v] : trailer_attrs) {
        info_.attributes[k] = v;
    }
    cv_.notify_all();
}

// =========================================================================
// ByteStreamReader Implementation
// =========================================================================

ByteStreamReader::ByteStreamReader(ByteStreamInfo info)
    : info_(std::move(info)) {}

ByteStreamReader::~ByteStreamReader() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

bool ByteStreamReader::ReadNext(std::vector<uint8_t>& out) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this]() {
        return !queue_.empty() || closed_;
    });

    if (!queue_.empty()) {
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    return false;
}

std::vector<uint8_t> ByteStreamReader::ReadAll() {
    std::vector<uint8_t> result;
    if (info_.total_length.has_value()) {
        result.reserve(info_.total_length.value());
    }
    std::vector<uint8_t> chunk;
    while (ReadNext(chunk)) {
        result.insert(result.end(), chunk.begin(), chunk.end());
    }
    return result;
}

bool ByteStreamReader::HasAvailableChunk() const {
    std::lock_guard lock(mutex_);
    return !queue_.empty();
}

size_t ByteStreamReader::received_bytes() const {
    std::lock_guard lock(mutex_);
    return received_bytes_;
}

bool ByteStreamReader::is_closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

const std::string& ByteStreamReader::close_reason() const {
    std::lock_guard lock(mutex_);
    return close_reason_;
}

void ByteStreamReader::OnChunkUpdate(const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (closed_ || !data || size == 0) return;
    queue_.emplace_back(data, data + size);
    received_bytes_ += size;
    cv_.notify_one();
}

void ByteStreamReader::OnStreamClose(const std::string& reason, const std::map<std::string, std::string>& trailer_attrs) {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    closed_ = true;
    close_reason_ = reason;
    for (const auto& [k, v] : trailer_attrs) {
        info_.attributes[k] = v;
    }
    cv_.notify_all();
}

// =========================================================================
// BaseStreamWriter Implementation
// =========================================================================

BaseStreamWriter::BaseStreamWriter(StreamPacketPublisher publisher,
                                   std::string topic,
                                   std::map<std::string, std::string> attributes,
                                   std::string stream_id,
                                   std::optional<std::size_t> total_size,
                                   std::string mime_type,
                                   std::vector<std::string> destination_identities,
                                   std::string sender_identity,
                                   proto::DataStream::Header content_header)
    : publisher_(std::move(publisher)),
      stream_id_(stream_id.empty() ? GenerateStreamId() : std::move(stream_id)),
      mime_type_(std::move(mime_type)),
      topic_(std::move(topic)),
      timestamp_ms_(CurrentTimestampMs()),
      total_size_(total_size),
      attributes_(std::move(attributes)),
      destination_identities_(std::move(destination_identities)),
      sender_identity_(std::move(sender_identity)),
      header_(std::move(content_header)) {
    header_.set_stream_id(stream_id_);
    header_.set_timestamp(timestamp_ms_);
    header_.set_topic(topic_);
    header_.set_mime_type(mime_type_);
    if (total_size_.has_value()) {
        header_.set_total_length(static_cast<uint64_t>(total_size_.value()));
    }
    for (const auto& [key, value] : attributes_) {
        (*header_.mutable_attributes())[key] = value;
    }
}

BaseStreamWriter::~BaseStreamWriter() noexcept {
    if (state_.load(std::memory_order_acquire) != State::Open) return;
    try {
        Close();
    } catch (const std::exception&) {
        // Destruction cannot return an error. Explicit Write/Close/Cancel keep
        // propagating it; this diagnostic must not allocate or throw instead.
        std::fputs("Stream writer finalization failed during destruction (exception).\n", stderr);
        std::fflush(stderr);
    } catch (...) {
        std::fputs("Stream writer finalization failed during destruction (unknown exception).\n", stderr);
        std::fflush(stderr);
    }
}

void BaseStreamWriter::EnsureOpenOrRethrowLocked() {
    if (state_.load(std::memory_order_relaxed) != State::Failed) return;
    std::rethrow_exception(first_failure_);
}

[[noreturn]] void BaseStreamWriter::FailLocked(std::exception_ptr error) {
    if (state_.load(std::memory_order_relaxed) != State::Failed) {
        first_failure_ = std::move(error);
        state_.store(State::Failed, std::memory_order_release);
    }
    std::rethrow_exception(first_failure_);
}

void BaseStreamWriter::PublishPacket(const proto::DataPacket& packet,
                                     const char* stage) {
    if (!publisher_) {
        FailLocked(std::make_exception_ptr(OperationError(
            OperationKind::SendData,
            OperationErrorCode::DataChannelUnavailable,
            stage,
            "stream publisher is unavailable")));
    }

    bool accepted = false;
    try {
        proto::DataPacket routed_packet = packet;
        for (const auto& identity : destination_identities_) {
            routed_packet.add_destination_identities(identity);
        }
        accepted = publisher_(routed_packet, true);
    } catch (...) {
        FailLocked(std::current_exception());
    }
    if (!accepted) {
        FailLocked(std::make_exception_ptr(OperationError(
            OperationKind::SendData,
            OperationErrorCode::DataChannelRejected,
            stage,
            "stream packet was rejected")));
    }
}

void BaseStreamWriter::EnsureHeaderSent() {
    if (header_sent_) return;

    proto::DataPacket packet;
    *packet.mutable_stream_header() = header_;

    PublishPacket(packet, "header");
    header_sent_ = true;
}

void BaseStreamWriter::SendChunk(const uint8_t* data, size_t size) {
    EnsureHeaderSent();

    proto::DataPacket packet;
    auto* chunk = packet.mutable_stream_chunk();
    chunk->set_stream_id(stream_id_);
    chunk->set_chunk_index(next_chunk_index_);
    if (data && size > 0) {
        chunk->set_content(data, size);
    }

    PublishPacket(packet, "chunk");
    ++next_chunk_index_;
}

void BaseStreamWriter::SendTrailer(const std::string& reason, const std::map<std::string, std::string>& attributes) {
    EnsureHeaderSent();

    proto::DataPacket packet;
    auto* trailer = packet.mutable_stream_trailer();
    trailer->set_stream_id(stream_id_);
    if (!reason.empty()) {
        trailer->set_reason(reason);
    }
    for (const auto& [k, v] : attributes) {
        (*trailer->mutable_attributes())[k] = v;
    }

    PublishPacket(packet, "trailer");
}

void BaseStreamWriter::Close(const std::string& reason, const std::map<std::string, std::string>& attributes) {
    std::lock_guard lock(write_mutex_);
    EnsureOpenOrRethrowLocked();
    if (state_.load(std::memory_order_relaxed) == State::Closed) return;
    SendTrailer(reason, attributes);
    state_.store(State::Closed, std::memory_order_release);
}

void BaseStreamWriter::Cancel(const std::string& reason) {
    Close(reason.empty() ? "cancelled" : reason);
}

// =========================================================================
// TextStreamWriter Implementation
// =========================================================================

TextStreamWriter::TextStreamWriter(StreamPacketPublisher publisher,
                                   std::string topic,
                                   std::map<std::string, std::string> attributes,
                                   std::string stream_id,
                                   std::optional<std::size_t> total_size,
                                   std::string reply_to_id,
                                   std::vector<std::string> destination_identities,
                                   std::string sender_identity)
    : BaseStreamWriter(std::move(publisher), std::move(topic), std::move(attributes),
                       std::move(stream_id), total_size, "text/plain",
                       std::move(destination_identities), std::move(sender_identity),
                       MakeTextContentHeader(reply_to_id)) {
    info_.stream_id = stream_id_;
    info_.topic = topic_;
    info_.mime_type = mime_type_;
    info_.timestamp = timestamp_ms_;
    info_.total_length = total_size_;
    info_.attributes = attributes_;
    info_.reply_to_stream_id = std::move(reply_to_id);
    info_.sender_identity = sender_identity_;
}

void TextStreamWriter::Write(const std::string& text) {
    std::lock_guard lock(write_mutex_);
    EnsureOpenOrRethrowLocked();
    if (state_.load(std::memory_order_relaxed) == State::Closed || text.empty()) return;

    size_t offset = 0;
    const size_t len = text.size();
    while (offset < len) {
        const size_t chunk_size = std::min(len - offset, kStreamChunkSize);
        SendChunk(reinterpret_cast<const uint8_t*>(text.data() + offset), chunk_size);
        offset += chunk_size;
    }
}

// =========================================================================
// ByteStreamWriter Implementation
// =========================================================================

ByteStreamWriter::ByteStreamWriter(StreamPacketPublisher publisher,
                                   std::string name,
                                   std::string topic,
                                   std::map<std::string, std::string> attributes,
                                   std::string stream_id,
                                   std::optional<std::size_t> total_size,
                                   std::string mime_type,
                                   std::vector<std::string> destination_identities,
                                   std::string sender_identity)
    : BaseStreamWriter(std::move(publisher), std::move(topic), std::move(attributes),
                       std::move(stream_id), total_size, std::move(mime_type),
                       std::move(destination_identities), std::move(sender_identity),
                       MakeByteContentHeader(name)) {
    info_.stream_id = stream_id_;
    info_.name = std::move(name);
    info_.topic = topic_;
    info_.mime_type = mime_type_;
    info_.timestamp = timestamp_ms_;
    info_.total_length = total_size_;
    info_.attributes = attributes_;
    info_.sender_identity = sender_identity_;
}

void ByteStreamWriter::Write(const std::vector<uint8_t>& data) {
    Write(data.data(), data.size());
}

void ByteStreamWriter::Write(const uint8_t* data, size_t size) {
    std::lock_guard lock(write_mutex_);
    EnsureOpenOrRethrowLocked();
    if (state_.load(std::memory_order_relaxed) == State::Closed || !data || size == 0) return;

    size_t offset = 0;
    while (offset < size) {
        const size_t chunk_size = std::min(size - offset, kStreamChunkSize);
        SendChunk(data + offset, chunk_size);
        offset += chunk_size;
    }
}

} // namespace livekit
