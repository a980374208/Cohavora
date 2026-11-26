#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace livekit::data_track {

inline constexpr size_t kMaxPacketsPerFrame = 128;

enum class DtpFrameMarker : uint8_t {
    Inter = 0,
    Final = 1,
    Start = 2,
    Single = 3,
};

struct DtpE2eeExtension {
    uint8_t key_index = 0;
    std::array<uint8_t, 12> iv{};

    bool operator==(const DtpE2eeExtension& other) const = default;
};

struct DtpExtensions {
    std::optional<uint64_t> user_timestamp;
    std::optional<DtpE2eeExtension> e2ee;

    bool empty() const noexcept {
        return !user_timestamp.has_value() && !e2ee.has_value();
    }

    size_t PayloadSerializedLength() const noexcept;

    bool operator==(const DtpExtensions& other) const = default;
};

struct DtpHeader {
    static constexpr uint8_t kSupportedVersion = 0;
    static constexpr size_t kBaseHeaderLen = 12;
    static constexpr size_t kExtWordsIndicatorSize = 2;
    static constexpr uint8_t kExtTagPadding = 0;
    static constexpr uint8_t kExtTagE2ee = 1;
    static constexpr uint8_t kExtLenE2ee = 13;
    static constexpr uint8_t kExtTagUserTimestamp = 2;
    static constexpr uint8_t kExtLenUserTimestamp = 8;

    DtpFrameMarker marker = DtpFrameMarker::Single;
    uint16_t track_handle = 0;
    uint16_t sequence = 0;
    uint16_t frame_number = 0;
    uint32_t timestamp = 0;
    DtpExtensions extensions;

    size_t ExtensionWords() const noexcept;
    size_t PaddingLength() const noexcept;
    size_t SerializedLength() const noexcept;

    bool operator==(const DtpHeader& other) const = default;
};

enum class DtpDeserializeError {
    None = 0,
    TooShort,
    HeaderOverrun,
    MissingExtWords,
    UnsupportedVersion,
    InvalidTrackHandle,
    MalformedExt,
};

const char* ToString(DtpDeserializeError err) noexcept;

struct DtpPacket {
    DtpHeader header;
    std::vector<uint8_t> payload;

    size_t SerializedLength() const noexcept {
        return header.SerializedLength() + payload.size();
    }

    bool SerializeInto(std::vector<uint8_t>& out, std::string* error = nullptr) const;
    std::vector<uint8_t> Serialize() const;

    static bool Deserialize(const uint8_t* data,
                            size_t size,
                            DtpPacket& out,
                            DtpDeserializeError* error = nullptr);
    static bool Deserialize(const std::vector<uint8_t>& raw,
                            DtpPacket& out,
                            DtpDeserializeError* error = nullptr) {
        return Deserialize(raw.data(), raw.size(), out, error);
    }
};

/// Monotonic 90kHz RTP-compatible wrap-around clock.
class DtpClock90kHz {
public:
    using Clock = std::chrono::steady_clock;

    DtpClock90kHz();
    explicit DtpClock90kHz(uint32_t base_ticks,
                           Clock::time_point epoch = Clock::now());

    uint32_t Now();
    uint32_t At(Clock::time_point instant);

private:
    static bool IsBefore(uint32_t a, uint32_t b) noexcept {
        return static_cast<int32_t>(a - b) < 0;
    }

    Clock::time_point epoch_;
    uint32_t base_;
    uint32_t prev_;
};

/// Splits an application-level payload + extensions into MTU-sized DTP packets.
class DtpPacketizer {
public:
    static constexpr size_t kDefaultTransportMtu = 16'000;

    explicit DtpPacketizer(uint16_t track_handle,
                           size_t mtu_size = kDefaultTransportMtu);
    DtpPacketizer(uint16_t track_handle,
                  size_t mtu_size,
                  uint32_t clock_base,
                  DtpClock90kHz::Clock::time_point epoch = DtpClock90kHz::Clock::now());

    uint16_t track_handle() const noexcept { return track_handle_; }
    size_t mtu_size() const noexcept { return mtu_size_; }

    static size_t MaxFramePayloadBytes(const DtpExtensions& extensions,
                                       size_t mtu_size = kDefaultTransportMtu) noexcept;

    /// Empty payload returns true with `out_packets` cleared (no wire packets emitted).
    bool Packetize(const uint8_t* data,
                   size_t size,
                   const DtpExtensions& extensions,
                   std::vector<DtpPacket>& out_packets,
                   std::string* error = nullptr);

    bool Packetize(const std::vector<uint8_t>& payload,
                   const DtpExtensions& extensions,
                   std::vector<DtpPacket>& out_packets,
                   std::string* error = nullptr) {
        return Packetize(payload.data(), payload.size(), extensions, out_packets, error);
    }

private:
    static DtpFrameMarker MarkerForChunk(size_t index, size_t packet_count) noexcept;

    uint16_t track_handle_;
    size_t mtu_size_;
    uint16_t next_sequence_ = 0;
    uint16_t next_frame_number_ = 0;
    DtpClock90kHz clock_;
};

enum class DtpDepacketizerDropReason {
    None = 0,
    Interrupted,
    UnknownFrame,
    BufferFull,
    Incomplete,
};

struct DtpDepacketizerDropError {
    uint16_t frame_number = 0;
    DtpDepacketizerDropReason reason = DtpDepacketizerDropReason::None;
    uint16_t new_frame_number = 0; // Set when reason == Interrupted
    uint16_t received_packets = 0; // Set when reason == Incomplete
    uint16_t expected_packets = 0; // Set when reason == Incomplete

    std::string Describe() const;
    bool operator==(const DtpDepacketizerDropError& other) const = default;
};

struct DtpDepacketizedFrame {
    std::vector<uint8_t> payload;
    DtpExtensions extensions;
};

struct DtpDepacketizerPushOptions {
    size_t max_partial_frames = 1;
};

struct DtpDepacketizerPushResult {
    std::optional<DtpDepacketizedFrame> frame;
    std::optional<DtpDepacketizerDropError> drop_error;
};

/// Reassembles incoming DTP packets into complete frames, evicting oldest stale
/// partial frames when concurrent partial frame count reaches `max_partial_frames`.
class DtpDepacketizer {
public:
    static constexpr size_t kMaxBufferPackets = kMaxPacketsPerFrame;

    DtpDepacketizerPushResult Push(DtpPacket packet,
                                   DtpDepacketizerPushOptions options = {});

    void Reset();
    size_t ActivePartialCount() const noexcept { return partials_.size(); }

private:
    struct PartialFrame {
        uint16_t start_sequence = 0;
        DtpExtensions extensions;
        std::map<uint16_t, std::vector<uint8_t>> payloads;
        std::list<uint16_t>::iterator order_it;
    };

    DtpDepacketizerPushResult FrameFromSingle(DtpPacket&& packet, size_t max_partials);
    DtpDepacketizerPushResult BeginPartial(DtpPacket&& packet, size_t max_partials);
    DtpDepacketizerPushResult PushToPartial(DtpPacket&& packet);
    std::optional<DtpDepacketizerDropError> EvictOldest(uint16_t new_frame_number);
    static DtpDepacketizerPushResult Finalize(uint16_t frame_number,
                                              PartialFrame& partial,
                                              uint16_t end_sequence);

    std::list<uint16_t> partial_order_;
    std::unordered_map<uint16_t, PartialFrame> partials_;
};

} // namespace livekit::data_track
