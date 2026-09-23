#pragma once

#include "e2e_media_marker.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace livekit::telemetry {

inline constexpr std::string_view kE2eMeasurementTopic =
    "cohavora.telemetry.e2e.v1";
inline constexpr std::uint32_t kE2eMeasurementWireVersion = 1;
inline constexpr std::uint32_t kE2eMeasurementProtocolVersion = 1;
inline constexpr std::size_t kE2eMeasurementMaxWireBytes = 4096;

enum class E2eMessageKind {
    Capabilities,
    ClockRequest,
    ClockResponse,
    MediaProbe,
    MediaAck,
};

struct E2eCapabilities {
    std::vector<std::uint32_t> protocol_versions;
    bool clock_sync = false;
    bool media_ack = false;
    bool frame_identity = false;
};

struct E2eMessage {
    E2eMessageKind kind = E2eMessageKind::Capabilities;
    std::string session_id;
    // Opaque measurement peer ids, not user identities.
    std::string sender_peer_id;
    std::string destination_peer_id;
    std::uint64_t sequence = 0;
    std::uint32_t protocol_version = 0;
    std::string nonce;
    E2eCapabilities capabilities;
    std::int64_t t0_local_send_us = 0;
    std::int64_t t1_remote_receive_us = 0;
    std::int64_t t2_remote_send_us = 0;
    std::string track_id;
    std::uint64_t probe_id = 0;
    std::int64_t remote_decode_us = 0;
    std::int64_t remote_render_submit_us = 0;
    std::string measurement_point;
    // Optional decoded frame dimensions. Version-1 peers that predate S8c
    // omit both fields; a partial pair is invalid.
    std::uint32_t remote_frame_width = 0;
    std::uint32_t remote_frame_height = 0;
};

[[nodiscard]] std::optional<std::string> EncodeE2eMessage(
    const E2eMessage& message);
[[nodiscard]] std::optional<E2eMessage> DecodeE2eMessage(
    std::string_view topic,
    std::string_view bytes);

enum class E2eMeasurementMode {
    Unsupported,
    ClockRoundTripOnly,
    MediaCorrelated,
};

struct E2eNegotiation {
    E2eMeasurementMode mode = E2eMeasurementMode::Unsupported;
    std::uint32_t protocol_version = 0;
    bool clock_sync = false;
    bool media_correlation = false;
    std::string reason = "peer_no_measurement_protocol";
};

[[nodiscard]] E2eNegotiation NegotiateE2eCapabilities(
    const E2eCapabilities& local,
    const std::optional<E2eCapabilities>& remote);

struct ClockExchange {
    std::uint64_t sequence = 0;
    std::int64_t t0_local_send_us = 0;
    std::int64_t t1_remote_receive_us = 0;
    std::int64_t t2_remote_send_us = 0;
    std::int64_t t3_local_receive_us = 0;
};

struct ClockCalibrationPolicy {
    std::size_t maximum_samples = 32;
    std::size_t minimum_samples = 3;
    std::int64_t maximum_age_us = 30'000'000;
    std::int64_t maximum_uncertainty_us = 10'000;
    double maximum_absolute_drift_ppm = 1'000.0;
    std::int64_t timestamp_resolution_us = 1;
};

enum class ClockCalibrationState {
    WarmingUp,
    Valid,
    Uncertain,
    Stale,
    Invalid,
};

struct ClockMappedTime {
    std::int64_t local_time_us = 0;
    std::int64_t lower_bound_us = 0;
    std::int64_t upper_bound_us = 0;
};

struct ClockMapping {
    ClockCalibrationState state = ClockCalibrationState::WarmingUp;
    std::string reason = "insufficient_clock_samples";
    std::size_t accepted_samples = 0;
    std::size_t rejected_samples = 0;
    std::int64_t reference_local_us = 0;
    double offset_us = 0.0;
    double drift_ppm = 0.0;
    std::int64_t uncertainty_us = 0;
    std::int64_t age_us = 0;
    std::int64_t best_round_trip_us = 0;

    [[nodiscard]] std::optional<ClockMappedTime> RemoteToLocal(
        std::int64_t remote_time_us) const;
};

class ClockCalibrator final {
public:
    explicit ClockCalibrator(ClockCalibrationPolicy policy = {});

    [[nodiscard]] bool AddExchange(const ClockExchange& exchange);
    [[nodiscard]] ClockMapping Evaluate(std::int64_t now_local_us) const;
    void Reset();

private:
    struct Sample {
        std::uint64_t sequence = 0;
        long double local_midpoint_us = 0.0;
        long double offset_us = 0.0;
        std::int64_t uncertainty_us = 0;
        std::int64_t round_trip_us = 0;
    };

    ClockCalibrationPolicy policy_;
    std::vector<Sample> samples_;
    std::deque<std::uint64_t> recent_sequences_;
    std::unordered_set<std::uint64_t> recent_sequence_set_;
    std::size_t rejected_samples_ = 0;
};

enum class E2eActionStatus {
    Accepted,
    NotReady,
    Unsupported,
    InvalidContext,
    InvalidMessage,
    DuplicateOrUnknown,
    CapacityExceeded,
    Retired,
};

struct E2eMediaProbeTimes {
    std::int64_t publication_start_us = 0;
    std::int64_t capture_us = 0;
    std::int64_t marker_send_us = 0;
};

struct E2eMediaMeasurement {
    std::string track_id;
    std::uint64_t probe_id = 0;
    std::uint64_t sequence = 0;
    std::int64_t ack_round_trip_us = 0;
    std::optional<std::int64_t> publication_to_decode_us;
    std::optional<std::int64_t> capture_to_render_submit_us;
    std::optional<std::uint32_t> remote_frame_width;
    std::optional<std::uint32_t> remote_frame_height;
    ClockCalibrationState clock_state = ClockCalibrationState::WarmingUp;
    std::int64_t uncertainty_us = 0;
    std::string one_way_reason = "clock_mapping_unavailable";
    std::string measurement_point;
};

struct E2eActionResult {
    E2eActionStatus status = E2eActionStatus::InvalidMessage;
    std::string reason = "invalid_measurement_message";
    std::optional<E2eMessage> message;
    std::optional<E2eMediaMeasurement> media_measurement;

    [[nodiscard]] bool accepted() const noexcept {
        return status == E2eActionStatus::Accepted;
    }
};

struct E2eMeasurementSessionConfig {
    std::string session_id;
    std::string local_peer_id;
    std::string remote_peer_id;
    E2eCapabilities local_capabilities;
    ClockCalibrationPolicy clock_policy;
    std::size_t maximum_pending_probes = 16;
    std::size_t maximum_pending_media_probes = 16;
};

// Mutable protocol state is deliberately lock-free and must be invoked by one
// serialized session owner (for example, the Room/session strand).
class E2eMeasurementSession final {
public:
    explicit E2eMeasurementSession(E2eMeasurementSessionConfig config);

    [[nodiscard]] E2eMessage BuildCapabilities(
        std::uint64_t sequence) const;
    [[nodiscard]] E2eActionResult AcceptCapabilities(
        const E2eMessage& message);
    void MarkPeerUnavailable();

    [[nodiscard]] E2eActionResult BeginClockProbe(
        std::string nonce,
        std::uint64_t sequence,
        std::int64_t local_send_us);
    [[nodiscard]] E2eActionResult AnswerClockProbe(
        const E2eMessage& request,
        std::int64_t local_receive_us,
        std::int64_t local_send_us) const;
    [[nodiscard]] E2eActionResult AcceptClockResponse(
        const E2eMessage& response,
        std::int64_t local_receive_us);

    [[nodiscard]] E2eActionResult BeginMediaProbe(
        std::string track_id,
        std::string nonce,
        std::uint64_t probe_id,
        std::uint64_t sequence,
        E2eMediaProbeTimes times);
    [[nodiscard]] E2eActionResult AcceptMediaProbe(
        const E2eMessage& announcement);
    [[nodiscard]] E2eActionResult ObserveDecodedMediaMarker(
        std::string_view track_id,
        const E2eMediaMarker& marker,
        std::int64_t remote_decode_us,
        std::uint64_t render_token);
    [[nodiscard]] E2eActionResult BuildMediaAckAfterRenderSubmit(
        std::string_view track_id,
        std::uint64_t render_token,
        std::int64_t remote_render_submit_us,
        std::string measurement_point,
        std::uint32_t remote_frame_width = 0,
        std::uint32_t remote_frame_height = 0);
    [[nodiscard]] E2eActionResult AcceptMediaAck(
        const E2eMessage& acknowledgement,
        std::int64_t local_receive_us);
    void CancelMediaTrack(std::string_view track_id);

    [[nodiscard]] ClockMapping clock_mapping(
        std::int64_t now_local_us) const;
    [[nodiscard]] const E2eNegotiation& negotiation() const noexcept;
    [[nodiscard]] std::size_t pending_probe_count() const noexcept;
    [[nodiscard]] std::size_t pending_media_probe_count() const noexcept;
    [[nodiscard]] std::size_t pending_incoming_media_probe_count() const noexcept;
    void Retire();

private:
    struct PendingProbe {
        std::uint64_t sequence = 0;
        std::int64_t local_send_us = 0;
    };

    struct PendingMediaProbe {
        std::string track_id;
        std::string nonce;
        std::uint64_t sequence = 0;
        E2eMediaProbeTimes times;
    };

    struct IncomingMediaProbe {
        std::string track_id;
        std::string nonce;
        std::uint64_t sequence = 0;
        bool decoded = false;
        std::int64_t remote_decode_us = 0;
        std::uint64_t render_token = 0;
    };

    [[nodiscard]] E2eActionResult ValidateIncoming(
        const E2eMessage& message,
        E2eMessageKind expected_kind) const;
    [[nodiscard]] bool ClockReady() const noexcept;
    [[nodiscard]] bool MediaReady() const noexcept;
    void ClearMediaState();

    E2eMeasurementSessionConfig config_;
    E2eNegotiation negotiation_;
    ClockCalibrator calibrator_;
    std::unordered_map<std::string, PendingProbe> pending_probes_;
    std::unordered_map<std::uint64_t, PendingMediaProbe> pending_media_probes_;
    std::unordered_map<std::uint64_t, IncomingMediaProbe> incoming_media_probes_;
    std::deque<std::uint64_t> completed_outgoing_media_probes_;
    std::unordered_set<std::uint64_t> completed_outgoing_media_probe_set_;
    std::deque<std::uint64_t> completed_incoming_media_probes_;
    std::unordered_set<std::uint64_t> completed_incoming_media_probe_set_;
    bool retired_ = false;
};

} // namespace livekit::telemetry
