#pragma once

#include "stats.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
#include "../rtc/linux/linux_encoder_capability_types.h"
#endif

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
enum class CodecSenderEncoder { Unknown, Amf, Nvenc, Qsv, X265, OpenH264, Libvpx, Other
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    , Vaapi
#endif
};
enum class CodecSenderMapping { Missing, Matched, Ambiguous };

#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
enum class LinuxVideoEncoderBackend { Unknown, Auto, Software, Amf, Nvenc, Qsv, Vaapi };
enum class LinuxVideoEncoderState {
    Unknown, AwaitingOutput, HardwareActive, SoftwareActive, SoftwareFallback,
    BackendMismatch, MixedOutput, PartialOutput
};

inline LinuxVideoEncoderBackend LinuxVideoEncoderBackendFromText(std::string_view value) noexcept {
    if (value == "auto") return LinuxVideoEncoderBackend::Auto;
    if (value == "software") return LinuxVideoEncoderBackend::Software;
    if (value == "amf") return LinuxVideoEncoderBackend::Amf;
    if (value == "nvenc") return LinuxVideoEncoderBackend::Nvenc;
    if (value == "qsv") return LinuxVideoEncoderBackend::Qsv;
    if (value == "vaapi") return LinuxVideoEncoderBackend::Vaapi;
    return LinuxVideoEncoderBackend::Unknown;
}
inline std::string_view LinuxVideoEncoderBackendText(LinuxVideoEncoderBackend value) noexcept {
    switch (value) {
    case LinuxVideoEncoderBackend::Auto: return "auto";
    case LinuxVideoEncoderBackend::Software: return "software";
    case LinuxVideoEncoderBackend::Amf: return "amf";
    case LinuxVideoEncoderBackend::Nvenc: return "nvenc";
    case LinuxVideoEncoderBackend::Qsv: return "qsv";
    case LinuxVideoEncoderBackend::Vaapi: return "vaapi";
    default: return "unknown";
    }
}
inline std::string_view LinuxVideoEncoderStateText(LinuxVideoEncoderState value) noexcept {
    switch (value) {
    case LinuxVideoEncoderState::AwaitingOutput: return "awaiting_output";
    case LinuxVideoEncoderState::HardwareActive: return "hardware_active";
    case LinuxVideoEncoderState::SoftwareActive: return "software_active";
    case LinuxVideoEncoderState::SoftwareFallback: return "software_fallback";
    case LinuxVideoEncoderState::BackendMismatch: return "backend_mismatch";
    case LinuxVideoEncoderState::MixedOutput: return "mixed_output";
    case LinuxVideoEncoderState::PartialOutput: return "partial_output";
    default: return "unknown";
    }
}
#endif

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
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    case CodecSenderEncoder::Vaapi: return "vaapi";
#endif
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
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    if (value == "NVENC-CUDA-Linux") return {CodecSenderEncoder::Nvenc, CodecSenderCodec::H264, true, value};
    if (value == "QSV-VAAPI-Linux") return {CodecSenderEncoder::Qsv, CodecSenderCodec::H264, true, value};
    if (value == "VAAPI-Linux") return {CodecSenderEncoder::Vaapi, CodecSenderCodec::H264, true, value};
#endif
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
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    LinuxEncoderCapabilityReason capability_reason = LinuxEncoderCapabilityReason::None;
#endif
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

#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
struct LinuxVideoEncoderObservation {
    CodecSenderSource source = CodecSenderSource::Unknown;
    LinuxVideoEncoderBackend requested_backend = LinuxVideoEncoderBackend::Unknown;
    CodecSenderEncoder encoder = CodecSenderEncoder::Unknown;
    LinuxVideoEncoderState state = LinuxVideoEncoderState::AwaitingOutput;
    std::uint64_t room_generation = 0;
    std::uint64_t publication_epoch = 0;
    LinuxEncoderCapabilityReason capability_reason = LinuxEncoderCapabilityReason::None;
};
struct LinuxVideoEncoderSnapshot {
    bool overflow = false;
    std::size_t row_count = 0;
    std::array<LinuxVideoEncoderObservation, CodecSenderSnapshot::kCapacity> rows{};
};
static_assert(std::is_trivially_copyable_v<LinuxVideoEncoderSnapshot>);

inline LinuxVideoEncoderState ObserveLinuxVideoEncoder(
        LinuxVideoEncoderBackend requested, const CodecSenderRow& row) noexcept {
    if (!row.frames_encoded || *row.frames_encoded == 0) return LinuxVideoEncoderState::AwaitingOutput;
    if (!row.hardware.has_value()) return LinuxVideoEncoderState::Unknown;
    if (!*row.hardware) {
        return requested == LinuxVideoEncoderBackend::Software || requested == LinuxVideoEncoderBackend::Unknown
            ? LinuxVideoEncoderState::SoftwareActive : LinuxVideoEncoderState::SoftwareFallback;
    }
    if (row.codec == CodecSenderCodec::Unknown || row.codec == CodecSenderCodec::Other) {
        return LinuxVideoEncoderState::Unknown;
    }
    if (row.implementation_name_length > row.implementation_name.size()) return LinuxVideoEncoderState::Unknown;
    const auto implementation = ClassifyCodecSenderImplementation(std::string_view(
        row.implementation_name.data(), row.implementation_name_length));
    if (implementation.codec != CodecSenderCodec::Unknown && implementation.codec != row.codec) {
        return LinuxVideoEncoderState::BackendMismatch;
    }
    const bool matches = requested == LinuxVideoEncoderBackend::Auto ||
        (requested == LinuxVideoEncoderBackend::Nvenc && row.encoder == CodecSenderEncoder::Nvenc) ||
        (requested == LinuxVideoEncoderBackend::Qsv && row.encoder == CodecSenderEncoder::Qsv) ||
        (requested == LinuxVideoEncoderBackend::Vaapi && row.encoder == CodecSenderEncoder::Vaapi) ||
        (requested == LinuxVideoEncoderBackend::Amf && row.encoder == CodecSenderEncoder::Amf);
    return matches ? LinuxVideoEncoderState::HardwareActive : LinuxVideoEncoderState::BackendMismatch;
}

struct LinuxVideoEncoderProgressRow {
    CodecSenderToken<96> rtp_id;
    std::array<char, 64> implementation_name{};
    std::uint8_t implementation_name_length = 0;
    std::uint32_t pc_index = 0;
    CodecSenderCodec codec = CodecSenderCodec::Unknown;
    std::optional<std::uint64_t> frames_encoded;
    bool output_observed = false;
};
struct LinuxVideoEncoderProgress {
    CodecSenderToken<96> publication_sid;
    std::uint64_t sequence = 0;
    std::uint64_t room_generation = 0;
    std::uint64_t publication_epoch = 0;
    std::size_t row_count = 0;
    std::array<LinuxVideoEncoderProgressRow, CodecSenderSnapshot::kCapacity> rows{};
    LinuxVideoEncoderObservation observation;
};

inline LinuxVideoEncoderObservation BuildLinuxVideoEncoderObservation(
        const CodecSenderSnapshot& sender, std::string_view publication_sid,
        LinuxVideoEncoderBackend requested, CodecSenderSource source,
        std::uint64_t generation, std::uint64_t epoch,
        LinuxVideoEncoderProgress& previous,
        LinuxEncoderCapabilityReason reason = LinuxEncoderCapabilityReason::None) noexcept {
    LinuxVideoEncoderObservation result;
    result.source = source;
    result.requested_backend = requested;
    result.room_generation = generation;
    result.publication_epoch = epoch;
    result.capability_reason = reason;
    if (generation == 0 || epoch == 0 || publication_sid.empty() || sender.sequence == 0) return result;
    if (previous.room_generation != generation || previous.publication_epoch != epoch ||
        previous.publication_sid.view() != publication_sid ||
        previous.observation.requested_backend != requested) previous = {};
    // Snapshot publication is independent of stats completion; never count the
    // same stats sample twice or infer current output from an old cumulative count.
    if (previous.sequence >= sender.sequence) return previous.observation;
    LinuxVideoEncoderProgress current;
    current.publication_sid.assign(publication_sid);
    current.sequence = sender.sequence;
    current.room_generation = generation;
    current.publication_epoch = epoch;
    bool has_output = false;
    bool awaiting_output = false;
    for (std::size_t index = 0; index < sender.row_count && index < sender.rows.size(); ++index) {
        const auto& row = sender.rows[index];
        if (row.mapping != CodecSenderMapping::Matched || row.track_sid.view() != publication_sid ||
            row.room_generation != generation || row.publication_epoch != epoch ||
            row.rtp_id.view().empty()) continue;
        auto& progress = current.rows[current.row_count++];
        progress.rtp_id = row.rtp_id;
        progress.pc_index = row.pc_index;
        progress.codec = row.codec;
        progress.implementation_name = row.implementation_name;
        progress.implementation_name_length = row.implementation_name_length;
        progress.frames_encoded = row.frames_encoded;
        for (std::size_t old_index = 0; old_index < previous.row_count; ++old_index) {
            const auto& old = previous.rows[old_index];
            if (old.pc_index != progress.pc_index || old.codec != progress.codec ||
                old.rtp_id.view() != progress.rtp_id.view() ||
                old.implementation_name_length != progress.implementation_name_length ||
                old.implementation_name != progress.implementation_name) continue;
            if (progress.frames_encoded && old.frames_encoded && *progress.frames_encoded >= *old.frames_encoded) {
                progress.output_observed = old.output_observed || *progress.frames_encoded > *old.frames_encoded;
            }
            break;
        }
        if (!progress.output_observed) {
            awaiting_output = true;
            continue;
        }
        const auto status = ObserveLinuxVideoEncoder(requested, row);
        if (!has_output) {
            result.state = status;
            result.encoder = row.encoder;
            has_output = true;
        } else {
            if (result.state != status) result.state = LinuxVideoEncoderState::MixedOutput;
            if (result.encoder != row.encoder) result.encoder = CodecSenderEncoder::Other;
        }
    }
    if (has_output && awaiting_output) result.state = LinuxVideoEncoderState::PartialOutput;
    if (result.state == LinuxVideoEncoderState::SoftwareFallback && result.capability_reason == LinuxEncoderCapabilityReason::None)
        result.capability_reason = LinuxEncoderCapabilityReason::UnknownUnavailable;
    current.observation = result;
    previous = current;
    return result;
}
#endif

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
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    std::string_view requested_backend;
#endif
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
                const bool software_fallback = row.hardware.has_value() && !*row.hardware
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                    && publication->requested_backend != "software"
#endif
                    ;
                if (publication->reported_codec_fallback ||
                    software_fallback ||
                    (codec_known && !publication->requested_codec.empty() && publication->requested_codec != "auto" &&
                     observed != publication->requested_codec) ||
                    (codec_known && implementation.codec != CodecSenderCodec::Unknown && row.codec != implementation.codec)) {
                    row.fallback = true;
                } else if (codec_known && row.hardware.has_value() &&
                           (*row.hardware
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                            || publication->requested_backend == "software"
#endif
                           ) &&
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
