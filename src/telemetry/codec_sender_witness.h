#pragma once

#include "stats.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace livekit::telemetry {

// This value-only bounded observation is separate from aggregate media metrics.
// Tokens contain opaque RTC/publication identifiers, never SDP, URLs or accounts.
template<std::size_t Capacity>
struct CodecSenderToken {
    std::array<char, Capacity> bytes{};
    std::uint16_t length = 0;
    std::string_view view() const noexcept {
        return length <= Capacity ? std::string_view(bytes.data(), length) : std::string_view{};
    }
    bool assign(std::string_view value) noexcept {
        length = 0;
        if (value.empty() || value.size() > Capacity) return false;
        for (const auto c : value) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
        std::copy(value.begin(), value.end(), bytes.begin());
        length = static_cast<std::uint16_t>(value.size());
        return true;
    }
};

enum class CodecSenderSource { Unknown, Camera, ScreenShare };
enum class CodecSenderCodec { Unknown, H264, H265, Vp8, Vp9, Av1, Other };
enum class CodecSenderEncoder { Unknown, Amf, Nvenc, Qsv, X265, OpenH264, Libvpx, Other };
enum class CodecSenderMapping { Missing, Matched, Ambiguous };

inline std::string_view CodecSenderCodecText(CodecSenderCodec value) noexcept {
    switch (value) {
    case CodecSenderCodec::H264: return "h264";
    case CodecSenderCodec::H265: return "h265";
    case CodecSenderCodec::Vp8: return "vp8";
    case CodecSenderCodec::Vp9: return "vp9";
    case CodecSenderCodec::Av1: return "av1";
    case CodecSenderCodec::Other: return "other";
    default: return {};
    }
}
inline std::string_view CodecSenderEncoderText(CodecSenderEncoder value) noexcept {
    switch (value) {
    case CodecSenderEncoder::Amf: return "amf";
    case CodecSenderEncoder::Nvenc: return "nvenc";
    case CodecSenderEncoder::Qsv: return "qsv";
    case CodecSenderEncoder::X265: return "x265";
    case CodecSenderEncoder::OpenH264: return "openh264";
    case CodecSenderEncoder::Libvpx: return "libvpx";
    case CodecSenderEncoder::Other: return "other";
    default: return {};
    }
}
inline CodecSenderCodec CodecSenderMime(std::string_view value) noexcept {
    if (value == "video/H265" || value == "video/h265") return CodecSenderCodec::H265;
    if (value == "video/H264" || value == "video/h264") return CodecSenderCodec::H264;
    if (value == "video/VP8" || value == "video/vp8") return CodecSenderCodec::Vp8;
    if (value == "video/VP9" || value == "video/vp9") return CodecSenderCodec::Vp9;
    if (value == "video/AV1" || value == "video/av1") return CodecSenderCodec::Av1;
    return value.empty() ? CodecSenderCodec::Unknown : CodecSenderCodec::Other;
}

struct CodecSenderImplementation {
    CodecSenderEncoder encoder = CodecSenderEncoder::Unknown;
    CodecSenderCodec codec = CodecSenderCodec::Unknown;
    std::optional<bool> hardware;
    // Only exact names audited against GetEncoderInfo are retained here.
    std::string_view name;
};
inline CodecSenderImplementation ClassifyCodecSenderImplementation(std::string_view value) noexcept {
    if (value == "AMF H264 / D3D11") return {CodecSenderEncoder::Amf, CodecSenderCodec::H264, true, value};
    if (value == "AMF H265 / D3D11") return {CodecSenderEncoder::Amf, CodecSenderCodec::H265, true, value};
    if (value == "NVENC H264 / D3D11") return {CodecSenderEncoder::Nvenc, CodecSenderCodec::H264, true, value};
    if (value == "NVENC H265 / D3D11") return {CodecSenderEncoder::Nvenc, CodecSenderCodec::H265, true, value};
    if (value == "QSV H264 / oneVPL D3D11") return {CodecSenderEncoder::Qsv, CodecSenderCodec::H264, true, value};
    if (value == "QSV H265 / oneVPL D3D11") return {CodecSenderEncoder::Qsv, CodecSenderCodec::H265, true, value};
    if (value == "x265 H265") return {CodecSenderEncoder::X265, CodecSenderCodec::H265, false, value};
    if (value == "OpenH264") return {CodecSenderEncoder::OpenH264, CodecSenderCodec::H264, false, value};
    if (value == "libvpx") return {CodecSenderEncoder::Libvpx, CodecSenderCodec::Unknown, false, value};
    return {value.empty() ? CodecSenderEncoder::Unknown : CodecSenderEncoder::Other,
        CodecSenderCodec::Unknown, std::nullopt, value.empty() ? std::string_view{} : "other"};
}

struct CodecSenderRow {
    CodecSenderToken<96> track_sid;
    CodecSenderToken<96> rtp_id;
    CodecSenderToken<96> codec_id;
    CodecSenderToken<48> mid;
    std::array<char, 64> implementation_name{};
    std::uint8_t implementation_name_length = 0;
    CodecSenderSource source = CodecSenderSource::Unknown;
    CodecSenderCodec codec = CodecSenderCodec::Unknown;
    CodecSenderEncoder encoder = CodecSenderEncoder::Unknown;
    CodecSenderMapping mapping = CodecSenderMapping::Missing;
    std::uint32_t pc_index = 0;
    std::uint64_t room_generation = 0;
    std::uint64_t publication_epoch = 0;
    std::optional<bool> hardware;
    std::optional<bool> fallback;
    std::optional<std::uint64_t> frames_encoded;
    std::optional<std::uint64_t> frames_sent;
    std::optional<std::uint64_t> bytes_sent;
    std::optional<std::uint64_t> packets_sent;
};
struct CodecSenderSnapshot {
    static constexpr std::size_t kCapacity = 16;
    std::uint64_t sequence = 0;
    std::int64_t capture_unix_ms = 0;
    std::int64_t stats_timestamp_ms = 0;
    bool overflow = false;
    std::size_t row_count = 0;
    std::array<CodecSenderRow, kCapacity> rows{};
};
static_assert(std::is_trivially_copyable_v<CodecSenderSnapshot>);

// Views are consumed synchronously on the room strand and never retained.
struct CodecSenderPublicationView {
    std::string_view track_sid;
    std::string_view rtc_track_id;
    std::span<const std::string> sender_track_ids;
    CodecSenderSource source = CodecSenderSource::Unknown;
    std::uint64_t room_generation = 0;
    std::uint64_t publication_epoch = 0;
    std::string_view requested_codec;
    bool reported_codec_fallback = false;
};

inline CodecSenderSnapshot BuildCodecSenderSnapshot(
        const RoomStatsReport& report, std::span<const CodecSenderPublicationView> publications,
        std::uint64_t sequence, std::int64_t capture_unix_ms) {
    CodecSenderSnapshot result;
    result.sequence = sequence;
    result.capture_unix_ms = capture_unix_ms;
    result.stats_timestamp_ms = report.timestamp_ms;
    std::uint32_t pc_index = 0;
    for (const auto& pc : report.reports) {
        for (const auto& stream : pc.outbound_rtp) {
            if (!stream.kind_available || stream.kind != "video") continue;
            const CodecStats* codec = nullptr;
            if (stream.codec_id_available) {
                std::size_t matches = 0;
                for (const auto& candidate : pc.codecs) {
                    if (candidate.id == stream.codec_id) { codec = &candidate; ++matches; }
                }
                if (matches != 1) codec = nullptr;
            }
            if (codec && codec->mime_type_available &&
                (codec->mime_type == "video/rtx" || codec->mime_type == "video/red" ||
                 codec->mime_type == "video/ulpfec" || codec->mime_type == "video/flexfec-03")) continue;
            if (result.row_count == result.rows.size()) { result.overflow = true; continue; }
            auto& row = result.rows[result.row_count++];
            row.pc_index = pc_index;
            row.rtp_id.assign(stream.id);
            if (stream.mid_available) row.mid.assign(stream.mid);
            if (stream.codec_id_available) row.codec_id.assign(stream.codec_id);
            if (codec && codec->mime_type_available) row.codec = CodecSenderMime(codec->mime_type);
            if (stream.frames_encoded_available) row.frames_encoded = stream.frames_encoded;
            if (stream.frames_sent_available) row.frames_sent = stream.frames_sent;
            if (stream.bytes_sent_available) row.bytes_sent = stream.bytes_sent;
            if (stream.packets_sent_available) row.packets_sent = stream.packets_sent;
            CodecSenderImplementation implementation;
            if (stream.encoder_implementation_available) {
                implementation = ClassifyCodecSenderImplementation(stream.encoder_implementation);
                row.encoder = implementation.encoder;
                row.hardware = implementation.hardware;
                std::copy(implementation.name.begin(), implementation.name.end(), row.implementation_name.begin());
                row.implementation_name_length = static_cast<std::uint8_t>(implementation.name.size());
            }
            const CodecSenderPublicationView* publication = nullptr;
            bool ambiguous = false;
            if (stream.mid_available && pc.senders_available) {
                for (const auto& sender : pc.senders) {
                    if (!sender.mid_available || sender.mid != stream.mid || sender.kind != "video") continue;
                    for (const auto& candidate : publications) {
                        const bool matches = !sender.track_id.empty() &&
                            (sender.track_id == candidate.rtc_track_id ||
                             std::find(candidate.sender_track_ids.begin(), candidate.sender_track_ids.end(), sender.track_id) != candidate.sender_track_ids.end());
                        if (!matches) continue;
                        if (publication && publication != &candidate) ambiguous = true;
                        publication = &candidate;
                    }
                }
            }
            row.mapping = ambiguous ? CodecSenderMapping::Ambiguous :
                (publication ? CodecSenderMapping::Matched : CodecSenderMapping::Missing);
            if (publication && !ambiguous) {
                if (publication->track_sid.starts_with("TR_")) row.track_sid.assign(publication->track_sid);
                row.source = publication->source;
                row.room_generation = publication->room_generation;
                row.publication_epoch = publication->publication_epoch;
                const auto observed = CodecSenderCodecText(row.codec);
                const bool codec_known = row.codec != CodecSenderCodec::Unknown;
                if (publication->reported_codec_fallback ||
                    (row.hardware.has_value() && !*row.hardware) ||
                    (codec_known && !publication->requested_codec.empty() && publication->requested_codec != "auto" &&
                     observed != publication->requested_codec) ||
                    (codec_known && implementation.codec != CodecSenderCodec::Unknown && row.codec != implementation.codec)) {
                    row.fallback = true;
                } else if (codec_known && row.hardware.has_value() && *row.hardware &&
                           !publication->requested_codec.empty()) {
                    row.fallback = false;
                }
            }
        }
        ++pc_index;
    }
    return result;
}

} // namespace livekit::telemetry
