#include "data_stream.h"

#include <chrono>
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
                                   std::string sender_identity)
    : publisher_(std::move(publisher)),
      stream_id_(stream_id.empty() ? GenerateStreamId() : std::move(stream_id)),
      mime_type_(std::move(mime_type)),
      topic_(std::move(topic)),
      timestamp_ms_(CurrentTimestampMs()),
      total_size_(total_size),
      attributes_(std::move(attributes)),
      destination_identities_(std::move(destination_identities)),
      sender_identity_(std::move(sender_identity)) {}

BaseStreamWriter::~BaseStreamWriter() {
    if (!closed_) {
        Close();
    }
}

void BaseStreamWriter::EnsureHeaderSent() {
    if (header_sent_) return;

    proto::DataPacket packet;
    auto* header = packet.mutable_stream_header();
    header->set_stream_id(stream_id_);
    header->set_timestamp(timestamp_ms_);
    header->set_topic(topic_);
    header->set_mime_type(mime_type_);
    if (total_size_.has_value()) {
        header->set_total_length(static_cast<uint64_t>(total_size_.value()));
    }
    for (const auto& [k, v] : attributes_) {
        (*header->mutable_attributes())[k] = v;
    }

    FillContentHeader(header);

    if (publisher_) {
        publisher_(packet, true);
    }
    header_sent_ = true;
}

void BaseStreamWriter::SendChunk(const uint8_t* data, size_t size) {
    EnsureHeaderSent();

    proto::DataPacket packet;
    auto* chunk = packet.mutable_stream_chunk();
    chunk->set_stream_id(stream_id_);
    chunk->set_chunk_index(next_chunk_index_++);
    if (data && size > 0) {
        chunk->set_content(data, size);
    }

    if (publisher_) {
        publisher_(packet, true);
    }
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

    if (publisher_) {
        publisher_(packet, true);
    }
}

void BaseStreamWriter::Close(const std::string& reason, const std::map<std::string, std::string>& attributes) {
    std::lock_guard lock(write_mutex_);
    if (closed_) return;
    closed_ = true;
    SendTrailer(reason, attributes);
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
                       std::move(destination_identities), std::move(sender_identity)),
      reply_to_id_(std::move(reply_to_id)) {
    info_.stream_id = stream_id_;
    info_.topic = topic_;
    info_.mime_type = mime_type_;
    info_.timestamp = timestamp_ms_;
    info_.total_length = total_size_;
    info_.attributes = attributes_;
    info_.reply_to_stream_id = reply_to_id_;
    info_.sender_identity = sender_identity_;
}

void TextStreamWriter::FillContentHeader(proto::DataStream::Header* header) {
    auto* th = header->mutable_text_header();
    th->set_operation_type(proto::DataStream::OperationType::DataStream_OperationType_CREATE);
    if (!reply_to_id_.empty()) {
        th->set_reply_to_stream_id(reply_to_id_);
    }
}

void TextStreamWriter::Write(const std::string& text) {
    std::lock_guard lock(write_mutex_);
    if (closed_ || text.empty()) return;

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
                       std::move(destination_identities), std::move(sender_identity)) {
    info_.stream_id = stream_id_;
    info_.name = std::move(name);
    info_.topic = topic_;
    info_.mime_type = mime_type_;
    info_.timestamp = timestamp_ms_;
    info_.total_length = total_size_;
    info_.attributes = attributes_;
    info_.sender_identity = sender_identity_;
}

void ByteStreamWriter::FillContentHeader(proto::DataStream::Header* header) {
    auto* bh = header->mutable_byte_header();
    if (!info_.name.empty()) {
        bh->set_name(info_.name);
    }
}

void ByteStreamWriter::Write(const std::vector<uint8_t>& data) {
    Write(data.data(), data.size());
}

void ByteStreamWriter::Write(const uint8_t* data, size_t size) {
    std::lock_guard lock(write_mutex_);
    if (closed_ || !data || size == 0) return;

    size_t offset = 0;
    while (offset < size) {
        const size_t chunk_size = std::min(size - offset, kStreamChunkSize);
        SendChunk(data + offset, chunk_size);
        offset += chunk_size;
    }
}

} // namespace livekit
