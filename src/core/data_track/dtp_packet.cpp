#include "data_track/dtp_packet.h"

#include <algorithm>
#include <limits>
#include <random>
#include <sstream>

namespace livekit::data_track {

namespace {

constexpr uint8_t kVersionShift = 5;
constexpr uint8_t kVersionMask = 0x07;
constexpr uint8_t kFrameMarkerShift = 3;
constexpr uint8_t kFrameMarkerMask = 0x03;
constexpr uint8_t kExtFlagShift = 2;
constexpr uint8_t kExtFlagMask = 0x01;

inline void WriteU16Be(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void WriteU32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void WriteU64Be(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

inline uint16_t ReadU16Be(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t ReadU32Be(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t ReadU64Be(const uint8_t* p) noexcept {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(p[i]);
    }
    return value;
}

uint32_t RandomSeed32() {
    std::random_device rd;
    return static_cast<uint32_t>(rd());
}

DtpFrameMarker MarkerFromWireBits(uint8_t bits) noexcept {
    switch (bits & kFrameMarkerMask) {
        case 0x02: return DtpFrameMarker::Start;
        case 0x01: return DtpFrameMarker::Final;
        case 0x03: return DtpFrameMarker::Single;
        default:   return DtpFrameMarker::Inter;
    }
}

uint8_t WireBitsFromMarker(DtpFrameMarker marker) noexcept {
    switch (marker) {
        case DtpFrameMarker::Inter:  return 0x00;
        case DtpFrameMarker::Final:  return 0x01;
        case DtpFrameMarker::Start:  return 0x02;
        case DtpFrameMarker::Single: return 0x03;
    }
    return 0x00;
}

} // namespace

size_t DtpExtensions::PayloadSerializedLength() const noexcept {
    size_t len = 0;
    if (e2ee.has_value()) {
        len += 2 + DtpHeader::kExtLenE2ee;
    }
    if (user_timestamp.has_value()) {
        len += 2 + DtpHeader::kExtLenUserTimestamp;
    }
    return len;
}

size_t DtpHeader::ExtensionWords() const noexcept {
    const size_t ext_len = extensions.PayloadSerializedLength();
    if (ext_len == 0) {
        return 0;
    }
    return (kExtWordsIndicatorSize + ext_len + 3) / 4;
}

size_t DtpHeader::PaddingLength() const noexcept {
    const size_t words = ExtensionWords();
    if (words == 0) {
        return 0;
    }
    const size_t ext_len = extensions.PayloadSerializedLength();
    return (words * 4) - kExtWordsIndicatorSize - ext_len;
}

size_t DtpHeader::SerializedLength() const noexcept {
    return kBaseHeaderLen + (4 * ExtensionWords());
}

const char* ToString(DtpDeserializeError err) noexcept {
    switch (err) {
        case DtpDeserializeError::None:               return "none";
        case DtpDeserializeError::TooShort:           return "too short to contain base header";
        case DtpDeserializeError::HeaderOverrun:      return "extension header exceeds packet length";
        case DtpDeserializeError::MissingExtWords:    return "missing extension word indicator";
        case DtpDeserializeError::UnsupportedVersion: return "unsupported packet version";
        case DtpDeserializeError::InvalidTrackHandle: return "invalid track handle (0 is reserved)";
        case DtpDeserializeError::MalformedExt:       return "malformed header extension";
    }
    return "unknown";
}

bool DtpPacket::SerializeInto(std::vector<uint8_t>& out, std::string* error) const {
    if (header.track_handle == 0) {
        if (error) *error = "track_handle 0 is reserved";
        return false;
    }
    const size_t ext_words = header.ExtensionWords();
    if (ext_words > 0x10000) {
        if (error) *error = "extension header length overflow";
        return false;
    }
    const bool has_ext = ext_words > 0;
    out.reserve(out.size() + SerializedLength());

    uint8_t initial = static_cast<uint8_t>((DtpHeader::kSupportedVersion & kVersionMask) << kVersionShift);
    initial |= static_cast<uint8_t>((WireBitsFromMarker(header.marker) & kFrameMarkerMask) << kFrameMarkerShift);
    if (has_ext) {
        initial |= static_cast<uint8_t>(1u << kExtFlagShift);
    }

    out.push_back(initial);
    out.push_back(0); // Reserved
    WriteU16Be(out, header.track_handle);
    WriteU16Be(out, header.sequence);
    WriteU16Be(out, header.frame_number);
    WriteU32Be(out, header.timestamp);

    if (has_ext) {
        // Spec encodes extension word count minus 1
        WriteU16Be(out, static_cast<uint16_t>(ext_words - 1));
        if (header.extensions.e2ee.has_value()) {
            const auto& e2ee = *header.extensions.e2ee;
            out.push_back(DtpHeader::kExtTagE2ee);
            out.push_back(DtpHeader::kExtLenE2ee);
            out.push_back(e2ee.key_index);
            out.insert(out.end(), e2ee.iv.begin(), e2ee.iv.end());
        }
        if (header.extensions.user_timestamp.has_value()) {
            out.push_back(DtpHeader::kExtTagUserTimestamp);
            out.push_back(DtpHeader::kExtLenUserTimestamp);
            WriteU64Be(out, *header.extensions.user_timestamp);
        }
        const size_t padding = header.PaddingLength();
        out.insert(out.end(), padding, 0u);
    }

    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

std::vector<uint8_t> DtpPacket::Serialize() const {
    std::vector<uint8_t> out;
    if (!SerializeInto(out)) {
        return {};
    }
    return out;
}

bool DtpPacket::Deserialize(const uint8_t* data,
                            size_t size,
                            DtpPacket& out,
                            DtpDeserializeError* error) {
    auto fail = [&](DtpDeserializeError err) {
        if (error) *error = err;
        return false;
    };

    if (!data || size < DtpHeader::kBaseHeaderLen) {
        return fail(DtpDeserializeError::TooShort);
    }

    size_t offset = 0;
    const uint8_t initial = data[offset++];
    const uint8_t version = static_cast<uint8_t>((initial >> kVersionShift) & kVersionMask);
    if (version > DtpHeader::kSupportedVersion) {
        return fail(DtpDeserializeError::UnsupportedVersion);
    }
    const DtpFrameMarker marker =
        MarkerFromWireBits(static_cast<uint8_t>((initial >> kFrameMarkerShift) & kFrameMarkerMask));
    const bool ext_flag = ((initial >> kExtFlagShift) & kExtFlagMask) != 0;
    ++offset; // Skip reserved low byte

    const uint16_t track_handle = ReadU16Be(data + offset);
    offset += 2;
    if (track_handle == 0) {
        return fail(DtpDeserializeError::InvalidTrackHandle);
    }
    const uint16_t sequence = ReadU16Be(data + offset);
    offset += 2;
    const uint16_t frame_number = ReadU16Be(data + offset);
    offset += 2;
    const uint32_t timestamp = ReadU32Be(data + offset);
    offset += 4;

    DtpExtensions extensions;
    if (ext_flag) {
        if (size - offset < DtpHeader::kExtWordsIndicatorSize) {
            return fail(DtpDeserializeError::MissingExtWords);
        }
        const uint16_t ext_words_minus_1 = ReadU16Be(data + offset);
        offset += 2;
        const size_t ext_block_bytes =
            (4u * (static_cast<size_t>(ext_words_minus_1) + 1u)) - DtpHeader::kExtWordsIndicatorSize;
        if (ext_block_bytes > size - offset) {
            return fail(DtpDeserializeError::HeaderOverrun);
        }

        size_t ext_pos = offset;
        const size_t ext_end = offset + ext_block_bytes;
        while (ext_pos < ext_end) {
            const uint8_t tag = data[ext_pos++];
            if (tag == DtpHeader::kExtTagPadding) {
                continue;
            }
            if (ext_pos >= ext_end) {
                return fail(DtpDeserializeError::MalformedExt);
            }
            const size_t ext_len = static_cast<size_t>(data[ext_pos++]);
            if (ext_end - ext_pos < ext_len) {
                return fail(DtpDeserializeError::MalformedExt);
            }
            if (tag == DtpHeader::kExtTagE2ee && ext_len >= DtpHeader::kExtLenE2ee) {
                DtpE2eeExtension ext;
                ext.key_index = data[ext_pos];
                std::copy_n(data + ext_pos + 1, ext.iv.size(), ext.iv.begin());
                extensions.e2ee = ext;
            } else if (tag == DtpHeader::kExtTagUserTimestamp &&
                       ext_len >= DtpHeader::kExtLenUserTimestamp) {
                extensions.user_timestamp = ReadU64Be(data + ext_pos);
            }
            // Known extensions longer than current version skip trailing bytes;
            // unknown/under-sized extensions are skipped forward-compatibly.
            ext_pos += ext_len;
        }
        offset = ext_end;
    }

    out.header = DtpHeader{
        marker,
        track_handle,
        sequence,
        frame_number,
        timestamp,
        extensions,
    };
    out.payload.assign(data + offset, data + size);
    if (error) *error = DtpDeserializeError::None;
    return true;
}

DtpClock90kHz::DtpClock90kHz()
    : DtpClock90kHz(RandomSeed32(), Clock::now()) {}

DtpClock90kHz::DtpClock90kHz(uint32_t base_ticks, Clock::time_point epoch)
    : epoch_(epoch), base_(base_ticks), prev_(base_ticks) {}

uint32_t DtpClock90kHz::Now() {
    return At(Clock::now());
}

uint32_t DtpClock90kHz::At(Clock::time_point instant) {
    uint64_t ticks = 0;
    if (instant > epoch_) {
        const auto nanos = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(instant - epoch_).count());
        // round(nanos * 90_000 / 1e9) split cleanly to avoid uint64 overflow:
        // nanos = q * 1e9 + r
        const uint64_t secs = nanos / 1'000'000'000ULL;
        const uint64_t rem_ns = nanos % 1'000'000'000ULL;
        ticks = secs * 90'000ULL + ((rem_ns * 90'000ULL + 500'000'000ULL) / 1'000'000'000ULL);
    }
    uint32_t ts = base_ + static_cast<uint32_t>(ticks);
    if (IsBefore(ts, prev_)) {
        ts = prev_;
    }
    prev_ = ts;
    return ts;
}

DtpPacketizer::DtpPacketizer(uint16_t track_handle, size_t mtu_size)
    : track_handle_(track_handle),
      mtu_size_(mtu_size) {}

DtpPacketizer::DtpPacketizer(uint16_t track_handle,
                             size_t mtu_size,
                             uint32_t clock_base,
                             DtpClock90kHz::Clock::time_point epoch)
    : track_handle_(track_handle),
      mtu_size_(mtu_size),
      clock_(clock_base, epoch) {}

DtpFrameMarker DtpPacketizer::MarkerForChunk(size_t index, size_t packet_count) noexcept {
    if (packet_count <= 1) {
        return DtpFrameMarker::Single;
    }
    if (index == 0) {
        return DtpFrameMarker::Start;
    }
    if (index + 1 == packet_count) {
        return DtpFrameMarker::Final;
    }
    return DtpFrameMarker::Inter;
}

size_t DtpPacketizer::MaxFramePayloadBytes(const DtpExtensions& extensions, size_t mtu_size) noexcept {
    DtpHeader header;
    header.extensions = extensions;
    const size_t header_len = header.SerializedLength();
    if (mtu_size <= header_len) return 0;
    const size_t payload_per_packet = mtu_size - header_len;
    constexpr size_t max_size = (std::numeric_limits<size_t>::max)();
    return payload_per_packet > max_size / kMaxPacketsPerFrame
        ? max_size : payload_per_packet * kMaxPacketsPerFrame;
}

bool DtpPacketizer::Packetize(const uint8_t* data,
                              size_t size,
                              const DtpExtensions& extensions,
                              std::vector<DtpPacket>& out_packets,
                              std::string* error) {
    out_packets.clear();
    if (track_handle_ == 0) {
        if (error) *error = "invalid track handle 0";
        return false;
    }

    DtpHeader prototype;
    prototype.track_handle = track_handle_;
    prototype.extensions = extensions;
    const size_t header_len = prototype.SerializedLength();
    if (mtu_size_ <= header_len) {
        if (error) *error = "MTU is too short to fit DTP header";
        return false;
    }
    if (size == 0 || !data) {
        return true;
    }

    const size_t max_payload_size = mtu_size_ - header_len;
    const size_t packet_count = 1 + (size - 1) / max_payload_size;
    if (packet_count > kMaxPacketsPerFrame) {
        if (error) *error = "DTP frame exceeds the fragment limit";
        return false;
    }
    const uint16_t frame_num = next_frame_number_++;
    const uint32_t ts = clock_.Now();

    out_packets.reserve(packet_count);
    for (size_t i = 0; i < packet_count; ++i) {
        const size_t offset = i * max_payload_size;
        const size_t chunk_len = std::min(max_payload_size, size - offset);
        DtpPacket pkt;
        pkt.header = DtpHeader{
            MarkerForChunk(i, packet_count),
            track_handle_,
            next_sequence_++,
            frame_num,
            ts,
            extensions,
        };
        pkt.payload.assign(data + offset, data + offset + chunk_len);
        out_packets.push_back(std::move(pkt));
    }
    return true;
}

std::string DtpDepacketizerDropError::Describe() const {
    std::ostringstream ss;
    ss << "Frame " << frame_number << " dropped: ";
    switch (reason) {
        case DtpDepacketizerDropReason::None:
            ss << "none";
            break;
        case DtpDepacketizerDropReason::Interrupted:
            ss << "interrupted by new frame " << new_frame_number;
            break;
        case DtpDepacketizerDropReason::UnknownFrame:
            ss << "unknown frame";
            break;
        case DtpDepacketizerDropReason::BufferFull:
            ss << "buffer full";
            break;
        case DtpDepacketizerDropReason::Incomplete:
            ss << "incomplete (" << received_packets << "/" << expected_packets << ")";
            break;
    }
    return ss.str();
}

void DtpDepacketizer::Reset() {
    partials_.clear();
    partial_order_.clear();
}

std::optional<DtpDepacketizerDropError> DtpDepacketizer::EvictOldest(uint16_t new_frame_number) {
    if (partial_order_.empty()) {
        return std::nullopt;
    }
    const uint16_t oldest = partial_order_.front();
    partial_order_.pop_front();
    partials_.erase(oldest);
    DtpDepacketizerDropError err;
    err.frame_number = oldest;
    err.reason = DtpDepacketizerDropReason::Interrupted;
    err.new_frame_number = new_frame_number;
    return err;
}

DtpDepacketizerPushResult DtpDepacketizer::Push(DtpPacket packet,
                                                DtpDepacketizerPushOptions options) {
    const size_t max_partials = std::max<size_t>(1, options.max_partial_frames);
    switch (packet.header.marker) {
        case DtpFrameMarker::Single:
            return FrameFromSingle(std::move(packet), max_partials);
        case DtpFrameMarker::Start:
            return BeginPartial(std::move(packet), max_partials);
        case DtpFrameMarker::Inter:
        case DtpFrameMarker::Final:
            return PushToPartial(std::move(packet));
    }
    return {};
}

DtpDepacketizerPushResult DtpDepacketizer::FrameFromSingle(DtpPacket&& packet, size_t max_partials) {
    DtpDepacketizerPushResult result;
    if (partials_.size() >= max_partials) {
        result.drop_error = EvictOldest(packet.header.frame_number);
    }
    result.frame = DtpDepacketizedFrame{
        std::move(packet.payload),
        packet.header.extensions,
    };
    return result;
}

DtpDepacketizerPushResult DtpDepacketizer::BeginPartial(DtpPacket&& packet, size_t max_partials) {
    DtpDepacketizerPushResult result;
    const uint16_t frame_number = packet.header.frame_number;

    // Duplicate Start for an already-tracked frame replaces its prior partial cleanly.
    if (auto existing = partials_.find(frame_number); existing != partials_.end()) {
        partial_order_.erase(existing->second.order_it);
        partials_.erase(existing);
    }

    while (partials_.size() >= max_partials) {
        auto evicted = EvictOldest(frame_number);
        if (!evicted.has_value()) {
            break;
        }
        if (!result.drop_error.has_value()) {
            result.drop_error = evicted;
        }
    }

    partial_order_.push_back(frame_number);
    auto order_it = std::prev(partial_order_.end());

    PartialFrame partial;
    partial.start_sequence = packet.header.sequence;
    partial.extensions = packet.header.extensions;
    partial.payloads.emplace(packet.header.sequence, std::move(packet.payload));
    partial.order_it = order_it;
    partials_.emplace(frame_number, std::move(partial));
    return result;
}

DtpDepacketizerPushResult DtpDepacketizer::PushToPartial(DtpPacket&& packet) {
    const uint16_t frame_number = packet.header.frame_number;
    const auto it = partials_.find(frame_number);
    if (it == partials_.end()) {
        DtpDepacketizerPushResult res;
        DtpDepacketizerDropError err;
        err.frame_number = frame_number;
        err.reason = DtpDepacketizerDropReason::UnknownFrame;
        res.drop_error = err;
        return res;
    }

    auto& partial = it->second;
    if (partial.payloads.size() >= kMaxBufferPackets) {
        partial_order_.erase(partial.order_it);
        partials_.erase(it);
        DtpDepacketizerPushResult res;
        DtpDepacketizerDropError err;
        err.frame_number = frame_number;
        err.reason = DtpDepacketizerDropReason::BufferFull;
        res.drop_error = err;
        return res;
    }

    partial.payloads[packet.header.sequence] = std::move(packet.payload);

    if (packet.header.marker == DtpFrameMarker::Final) {
        PartialFrame completed = std::move(partial);
        partial_order_.erase(completed.order_it);
        partials_.erase(it);
        return Finalize(frame_number, completed, packet.header.sequence);
    }
    return {};
}

DtpDepacketizerPushResult DtpDepacketizer::Finalize(uint16_t frame_number,
                                                    PartialFrame& partial,
                                                    uint16_t end_sequence) {
    const uint16_t received = static_cast<uint16_t>(partial.payloads.size());
    size_t total_len = 0;
    for (const auto& [seq, bytes] : partial.payloads) {
        (void)seq;
        total_len += bytes.size();
    }

    std::vector<uint8_t> assembled;
    assembled.reserve(total_len);

    uint16_t seq = partial.start_sequence;
    while (true) {
        const auto chunk_it = partial.payloads.find(seq);
        if (chunk_it == partial.payloads.end()) {
            DtpDepacketizerPushResult res;
            DtpDepacketizerDropError err;
            err.frame_number = frame_number;
            err.reason = DtpDepacketizerDropReason::Incomplete;
            err.received_packets = received;
            err.expected_packets = static_cast<uint16_t>(
                static_cast<uint16_t>(end_sequence - partial.start_sequence) + 1u);
            res.drop_error = err;
            return res;
        }
        assembled.insert(assembled.end(), chunk_it->second.begin(), chunk_it->second.end());
        partial.payloads.erase(chunk_it);
        if (seq == end_sequence) {
            break;
        }
        seq = static_cast<uint16_t>(seq + 1u);
    }

    DtpDepacketizerPushResult res;
    res.frame = DtpDepacketizedFrame{
        std::move(assembled),
        partial.extensions,
    };
    return res;
}

} // namespace livekit::data_track
