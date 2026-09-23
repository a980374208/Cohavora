#include "e2e_measurement.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kMessageNames[] = {
    "capabilities", "clock_request", "clock_response", "media_probe",
    "media_ack"};

bool Identifier(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == ':' || c == '.';
        });
}

std::string Counter(std::uint64_t value) {
    return std::to_string(value);
}

std::string Timestamp(std::int64_t value) {
    return std::to_string(value);
}

bool ValidMediaIdentity(const E2eMessage& message) {
    return message.protocol_version != 0 && Identifier(message.nonce) &&
        Identifier(message.track_id) && message.probe_id != 0 &&
        message.sequence != 0 &&
        message.sequence <= std::numeric_limits<std::uint32_t>::max();
}

template <typename Integer>
Integer ParseInteger(const Json& value, bool positive) {
    if (!value.is_string()) throw std::invalid_argument("integer_type");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 20) {
        throw std::invalid_argument("integer_size");
    }
    Integer result = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (positive && result <= 0) || (!positive && result < 0)) {
        throw std::invalid_argument("integer_value");
    }
    return result;
}

bool ValidCapabilities(const E2eCapabilities& capabilities) {
    if (capabilities.protocol_versions.empty() ||
        capabilities.protocol_versions.size() > 8) {
        return false;
    }
    auto versions = capabilities.protocol_versions;
    if (std::any_of(versions.begin(), versions.end(),
            [](std::uint32_t value) { return value == 0; })) {
        return false;
    }
    std::sort(versions.begin(), versions.end());
    return std::adjacent_find(versions.begin(), versions.end()) == versions.end();
}

Json CapabilitiesJson(const E2eCapabilities& capabilities) {
    Json versions = Json::array();
    for (const auto version : capabilities.protocol_versions) {
        versions.push_back(version);
    }
    return Json{
        {"versions", std::move(versions)},
        {"clockSync", capabilities.clock_sync},
        {"mediaAck", capabilities.media_ack},
        {"frameIdentity", capabilities.frame_identity},
    };
}

E2eCapabilities ParseCapabilities(const Json& value) {
    if (!value.is_object() || value.size() != 4 ||
        !value.at("versions").is_array() ||
        value.at("versions").size() > 8) {
        throw std::invalid_argument("capabilities_shape");
    }
    E2eCapabilities result;
    for (const auto& version : value.at("versions")) {
        if (!version.is_number_unsigned()) {
            throw std::invalid_argument("capability_version_type");
        }
        result.protocol_versions.push_back(version.get<std::uint32_t>());
    }
    result.clock_sync = value.at("clockSync").get<bool>();
    result.media_ack = value.at("mediaAck").get<bool>();
    result.frame_identity = value.at("frameIdentity").get<bool>();
    if (!ValidCapabilities(result)) {
        throw std::invalid_argument("capabilities_value");
    }
    return result;
}

E2eActionResult Action(E2eActionStatus status, std::string reason) {
    E2eActionResult result;
    result.status = status;
    result.reason = std::move(reason);
    return result;
}

void RememberProbe(std::deque<std::uint64_t>& order,
                   std::unordered_set<std::uint64_t>& values,
                   std::uint64_t probe_id,
                   std::size_t capacity) {
    if (probe_id == 0 || values.contains(probe_id)) return;
    order.push_back(probe_id);
    values.insert(probe_id);
    const auto bounded_capacity = std::max<std::size_t>(capacity * 2, 2);
    while (order.size() > bounded_capacity) {
        values.erase(order.front());
        order.pop_front();
    }
}

bool ValidPolicy(const ClockCalibrationPolicy& policy) {
    return policy.maximum_samples >= 2 &&
        policy.minimum_samples >= 2 &&
        policy.minimum_samples <= policy.maximum_samples &&
        policy.maximum_age_us > 0 &&
        policy.maximum_uncertainty_us > 0 &&
        std::isfinite(policy.maximum_absolute_drift_ppm) &&
        policy.maximum_absolute_drift_ppm > 0.0 &&
        policy.timestamp_resolution_us >= 0;
}

std::int64_t SaturatingCeil(long double value) {
    if (!std::isfinite(value) ||
        value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= 0.0L) return 0;
    return static_cast<std::int64_t>(std::ceil(value));
}

} // namespace

std::optional<std::string> EncodeE2eMessage(const E2eMessage& message) {
    const auto kind = static_cast<std::size_t>(message.kind);
    if (kind >= std::size(kMessageNames) ||
        !Identifier(message.session_id) ||
        !Identifier(message.sender_peer_id) ||
        !Identifier(message.destination_peer_id) ||
        message.sequence == 0) {
        return std::nullopt;
    }

    Json value{
        {"wireVersion", kE2eMeasurementWireVersion},
        {"kind", kMessageNames[kind]},
        {"session", message.session_id},
        {"sender", message.sender_peer_id},
        {"destination", message.destination_peer_id},
        {"sequence", Counter(message.sequence)},
    };

    switch (message.kind) {
    case E2eMessageKind::Capabilities:
        if (!ValidCapabilities(message.capabilities)) return std::nullopt;
        value["capabilities"] = CapabilitiesJson(message.capabilities);
        break;
    case E2eMessageKind::ClockRequest:
        if (message.protocol_version == 0 || !Identifier(message.nonce) ||
            message.t0_local_send_us < 0) {
            return std::nullopt;
        }
        value["protocolVersion"] = message.protocol_version;
        value["nonce"] = message.nonce;
        value["t0Us"] = Timestamp(message.t0_local_send_us);
        break;
    case E2eMessageKind::ClockResponse:
        if (message.protocol_version == 0 || !Identifier(message.nonce) ||
            message.t0_local_send_us < 0 ||
            message.t1_remote_receive_us < 0 ||
            message.t2_remote_send_us < message.t1_remote_receive_us) {
            return std::nullopt;
        }
        value["protocolVersion"] = message.protocol_version;
        value["nonce"] = message.nonce;
        value["t0Us"] = Timestamp(message.t0_local_send_us);
        value["t1Us"] = Timestamp(message.t1_remote_receive_us);
        value["t2Us"] = Timestamp(message.t2_remote_send_us);
        break;
    case E2eMessageKind::MediaProbe:
        if (!ValidMediaIdentity(message)) return std::nullopt;
        value["protocolVersion"] = message.protocol_version;
        value["nonce"] = message.nonce;
        value["track"] = message.track_id;
        value["probeId"] = Counter(message.probe_id);
        break;
    case E2eMessageKind::MediaAck:
        if (!ValidMediaIdentity(message) ||
            message.remote_decode_us < 0 ||
            message.remote_render_submit_us < message.remote_decode_us ||
            !Identifier(message.measurement_point) ||
            ((message.remote_frame_width == 0) !=
                (message.remote_frame_height == 0))) {
            return std::nullopt;
        }
        value["protocolVersion"] = message.protocol_version;
        value["nonce"] = message.nonce;
        value["track"] = message.track_id;
        value["probeId"] = Counter(message.probe_id);
        value["decodeUs"] = Timestamp(message.remote_decode_us);
        value["renderSubmitUs"] = Timestamp(message.remote_render_submit_us);
        value["measurementPoint"] = message.measurement_point;
        if (message.remote_frame_width != 0) {
            value["frameWidth"] = message.remote_frame_width;
            value["frameHeight"] = message.remote_frame_height;
        }
        break;
    }

    auto bytes = value.dump();
    if (bytes.size() > kE2eMeasurementMaxWireBytes) return std::nullopt;
    return bytes;
}

std::optional<E2eMessage> DecodeE2eMessage(
        std::string_view topic,
        std::string_view bytes) {
    if (topic != kE2eMeasurementTopic || bytes.empty() ||
        bytes.size() > kE2eMeasurementMaxWireBytes) {
        return std::nullopt;
    }
    try {
        const auto value = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) throw std::invalid_argument("depth");
            return true;
        });
        if (!value.is_object() || value.size() > 15 ||
            value.at("wireVersion").get<std::uint32_t>() !=
                kE2eMeasurementWireVersion) {
            return std::nullopt;
        }
        const auto name = value.at("kind").get<std::string>();
        const auto found = std::find(
            std::begin(kMessageNames), std::end(kMessageNames), name);
        if (found == std::end(kMessageNames)) return std::nullopt;

        E2eMessage result;
        result.kind = static_cast<E2eMessageKind>(found - std::begin(kMessageNames));
        result.session_id = value.at("session").get<std::string>();
        result.sender_peer_id = value.at("sender").get<std::string>();
        result.destination_peer_id = value.at("destination").get<std::string>();
        result.sequence = ParseInteger<std::uint64_t>(value.at("sequence"), true);
        if (!Identifier(result.session_id) ||
            !Identifier(result.sender_peer_id) ||
            !Identifier(result.destination_peer_id)) {
            return std::nullopt;
        }

        switch (result.kind) {
        case E2eMessageKind::Capabilities:
            if (value.size() != 7) return std::nullopt;
            result.capabilities = ParseCapabilities(value.at("capabilities"));
            break;
        case E2eMessageKind::ClockRequest:
            if (value.size() != 9) return std::nullopt;
            result.protocol_version = value.at("protocolVersion").get<std::uint32_t>();
            result.nonce = value.at("nonce").get<std::string>();
            result.t0_local_send_us =
                ParseInteger<std::int64_t>(value.at("t0Us"), false);
            if (result.protocol_version == 0 || !Identifier(result.nonce)) {
                return std::nullopt;
            }
            break;
        case E2eMessageKind::ClockResponse:
            if (value.size() != 11) return std::nullopt;
            result.protocol_version = value.at("protocolVersion").get<std::uint32_t>();
            result.nonce = value.at("nonce").get<std::string>();
            result.t0_local_send_us =
                ParseInteger<std::int64_t>(value.at("t0Us"), false);
            result.t1_remote_receive_us =
                ParseInteger<std::int64_t>(value.at("t1Us"), false);
            result.t2_remote_send_us =
                ParseInteger<std::int64_t>(value.at("t2Us"), false);
            if (result.protocol_version == 0 || !Identifier(result.nonce) ||
                result.t2_remote_send_us < result.t1_remote_receive_us) {
                return std::nullopt;
            }
            break;
        case E2eMessageKind::MediaProbe:
            if (value.size() != 10) return std::nullopt;
            result.protocol_version = value.at("protocolVersion").get<std::uint32_t>();
            result.nonce = value.at("nonce").get<std::string>();
            result.track_id = value.at("track").get<std::string>();
            result.probe_id = ParseInteger<std::uint64_t>(
                value.at("probeId"), true);
            if (!ValidMediaIdentity(result)) return std::nullopt;
            break;
        case E2eMessageKind::MediaAck:
            if (value.size() != 13 && value.size() != 15) return std::nullopt;
            result.protocol_version = value.at("protocolVersion").get<std::uint32_t>();
            result.nonce = value.at("nonce").get<std::string>();
            result.track_id = value.at("track").get<std::string>();
            result.probe_id = ParseInteger<std::uint64_t>(
                value.at("probeId"), true);
            result.remote_decode_us = ParseInteger<std::int64_t>(
                value.at("decodeUs"), false);
            result.remote_render_submit_us = ParseInteger<std::int64_t>(
                value.at("renderSubmitUs"), false);
            result.measurement_point =
                value.at("measurementPoint").get<std::string>();
            if (value.contains("frameWidth") != value.contains("frameHeight")) {
                return std::nullopt;
            }
            if (value.contains("frameWidth")) {
                result.remote_frame_width =
                    value.at("frameWidth").get<std::uint32_t>();
                result.remote_frame_height =
                    value.at("frameHeight").get<std::uint32_t>();
                if (result.remote_frame_width == 0 ||
                    result.remote_frame_height == 0) {
                    return std::nullopt;
                }
            }
            if (!ValidMediaIdentity(result) ||
                result.remote_render_submit_us < result.remote_decode_us ||
                !Identifier(result.measurement_point)) {
                return std::nullopt;
            }
            break;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

E2eNegotiation NegotiateE2eCapabilities(
        const E2eCapabilities& local,
        const std::optional<E2eCapabilities>& remote) {
    E2eNegotiation result;
    if (!remote) return result;
    if (!ValidCapabilities(local) || !ValidCapabilities(*remote)) {
        result.reason = "invalid_capabilities";
        return result;
    }

    for (const auto local_version : local.protocol_versions) {
        if (std::find(remote->protocol_versions.begin(),
                remote->protocol_versions.end(), local_version) !=
            remote->protocol_versions.end()) {
            result.protocol_version = std::max(
                result.protocol_version, local_version);
        }
    }
    if (result.protocol_version == 0) {
        result.reason = "protocol_version_mismatch";
        return result;
    }
    result.clock_sync = local.clock_sync && remote->clock_sync;
    if (!result.clock_sync) {
        result.reason = "clock_sync_unsupported";
        return result;
    }

    result.media_correlation = local.media_ack && remote->media_ack &&
        local.frame_identity && remote->frame_identity;
    if (result.media_correlation) {
        result.mode = E2eMeasurementMode::MediaCorrelated;
        result.reason = "media_correlation_ready";
    } else {
        result.mode = E2eMeasurementMode::ClockRoundTripOnly;
        result.reason = "media_frame_identity_unavailable";
    }
    return result;
}

ClockCalibrator::ClockCalibrator(ClockCalibrationPolicy policy)
    : policy_(std::move(policy)) {
    if (!ValidPolicy(policy_)) {
        policy_ = ClockCalibrationPolicy{};
    }
}

bool ClockCalibrator::AddExchange(const ClockExchange& exchange) {
    const bool valid = exchange.sequence != 0 &&
        exchange.t0_local_send_us >= 0 &&
        exchange.t3_local_receive_us >= exchange.t0_local_send_us &&
        exchange.t1_remote_receive_us >= 0 &&
        exchange.t2_remote_send_us >= exchange.t1_remote_receive_us &&
        recent_sequence_set_.find(exchange.sequence) == recent_sequence_set_.end();
    if (!valid) {
        ++rejected_samples_;
        return false;
    }

    const auto local_elapsed =
        exchange.t3_local_receive_us - exchange.t0_local_send_us;
    const auto remote_processing =
        exchange.t2_remote_send_us - exchange.t1_remote_receive_us;
    if (remote_processing > local_elapsed) {
        ++rejected_samples_;
        return false;
    }

    const auto round_trip = local_elapsed - remote_processing;
    const long double local_midpoint =
        (static_cast<long double>(exchange.t0_local_send_us) +
            static_cast<long double>(exchange.t3_local_receive_us)) / 2.0L;
    const long double remote_midpoint =
        (static_cast<long double>(exchange.t1_remote_receive_us) +
            static_cast<long double>(exchange.t2_remote_send_us)) / 2.0L;
    Sample sample;
    sample.sequence = exchange.sequence;
    sample.local_midpoint_us = local_midpoint;
    sample.offset_us = remote_midpoint - local_midpoint;
    sample.round_trip_us = round_trip;
    sample.uncertainty_us = round_trip / 2 +
        (round_trip % 2 != 0 ? 1 : 0) + policy_.timestamp_resolution_us;

    samples_.push_back(sample);
    std::sort(samples_.begin(), samples_.end(), [](const Sample& a, const Sample& b) {
        return a.local_midpoint_us < b.local_midpoint_us;
    });
    if (samples_.size() > policy_.maximum_samples) samples_.erase(samples_.begin());

    recent_sequences_.push_back(exchange.sequence);
    recent_sequence_set_.insert(exchange.sequence);
    const auto sequence_capacity = policy_.maximum_samples * 2;
    while (recent_sequences_.size() > sequence_capacity) {
        recent_sequence_set_.erase(recent_sequences_.front());
        recent_sequences_.pop_front();
    }
    return true;
}

ClockMapping ClockCalibrator::Evaluate(std::int64_t now_local_us) const {
    ClockMapping result;
    result.accepted_samples = samples_.size();
    result.rejected_samples = rejected_samples_;
    if (samples_.empty()) return result;

    const auto& latest = samples_.back();
    result.reference_local_us = static_cast<std::int64_t>(
        std::llround(latest.local_midpoint_us));
    if (now_local_us < result.reference_local_us) {
        result.state = ClockCalibrationState::Invalid;
        result.reason = "local_clock_before_reference";
        return result;
    }
    result.age_us = now_local_us - result.reference_local_us;
    result.best_round_trip_us = std::min_element(
        samples_.begin(), samples_.end(), [](const Sample& a, const Sample& b) {
            return a.round_trip_us < b.round_trip_us;
        })->round_trip_us;
    if (samples_.size() < policy_.minimum_samples) return result;
    if (result.age_us > policy_.maximum_age_us) {
        result.state = ClockCalibrationState::Stale;
        result.reason = "clock_mapping_stale";
        return result;
    }

    long double mean_x = 0.0L;
    long double mean_y = 0.0L;
    for (const auto& sample : samples_) {
        mean_x += sample.local_midpoint_us;
        mean_y += sample.offset_us;
    }
    mean_x /= static_cast<long double>(samples_.size());
    mean_y /= static_cast<long double>(samples_.size());

    long double covariance = 0.0L;
    long double variance = 0.0L;
    for (const auto& sample : samples_) {
        const auto dx = sample.local_midpoint_us - mean_x;
        covariance += dx * (sample.offset_us - mean_y);
        variance += dx * dx;
    }
    if (variance <= 0.0L) return result;

    const auto slope = covariance / variance;
    result.drift_ppm = static_cast<double>(slope * 1'000'000.0L);
    result.offset_us = static_cast<double>(
        mean_y + slope * (latest.local_midpoint_us - mean_x));
    if (!std::isfinite(result.drift_ppm) ||
        !std::isfinite(result.offset_us) ||
        std::abs(result.drift_ppm) > policy_.maximum_absolute_drift_ppm) {
        result.state = ClockCalibrationState::Invalid;
        result.reason = "clock_drift_out_of_range";
        return result;
    }

    long double uncertainty = 0.0L;
    for (const auto& sample : samples_) {
        const auto predicted = mean_y + slope * (sample.local_midpoint_us - mean_x);
        uncertainty = std::max(
            uncertainty,
            std::abs(sample.offset_us - predicted) + sample.uncertainty_us);
    }
    result.uncertainty_us = SaturatingCeil(uncertainty);
    if (result.uncertainty_us > policy_.maximum_uncertainty_us) {
        result.state = ClockCalibrationState::Uncertain;
        result.reason = "clock_uncertainty_exceeds_limit";
        return result;
    }

    result.state = ClockCalibrationState::Valid;
    result.reason = "clock_mapping_valid";
    return result;
}

void ClockCalibrator::Reset() {
    samples_.clear();
    recent_sequences_.clear();
    recent_sequence_set_.clear();
    rejected_samples_ = 0;
}

std::optional<ClockMappedTime> ClockMapping::RemoteToLocal(
        std::int64_t remote_time_us) const {
    if (state != ClockCalibrationState::Valid || remote_time_us < 0) {
        return std::nullopt;
    }
    const long double slope = static_cast<long double>(drift_ppm) / 1'000'000.0L;
    const long double denominator = 1.0L + slope;
    if (denominator <= 0.0L) return std::nullopt;
    const long double remote_reference =
        static_cast<long double>(reference_local_us) + offset_us;
    const long double local = static_cast<long double>(reference_local_us) +
        (static_cast<long double>(remote_time_us) - remote_reference) /
            denominator;
    if (!std::isfinite(local) ||
        local < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        local > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    ClockMappedTime result;
    result.local_time_us = static_cast<std::int64_t>(std::llround(local));
    result.lower_bound_us = result.local_time_us - uncertainty_us;
    result.upper_bound_us = result.local_time_us + uncertainty_us;
    return result;
}

E2eMeasurementSession::E2eMeasurementSession(
        E2eMeasurementSessionConfig config)
    : config_(std::move(config)),
      calibrator_(config_.clock_policy) {
    if (!Identifier(config_.session_id) ||
        !Identifier(config_.local_peer_id) ||
        !Identifier(config_.remote_peer_id) ||
        config_.local_peer_id == config_.remote_peer_id ||
        !ValidCapabilities(config_.local_capabilities)) {
        retired_ = true;
    }
    if (config_.maximum_pending_probes == 0 ||
        config_.maximum_pending_probes > 256) {
        config_.maximum_pending_probes = 16;
    }
    if (config_.maximum_pending_media_probes == 0 ||
        config_.maximum_pending_media_probes > 256) {
        config_.maximum_pending_media_probes = 16;
    }
}

E2eMessage E2eMeasurementSession::BuildCapabilities(
        std::uint64_t sequence) const {
    E2eMessage message;
    message.kind = E2eMessageKind::Capabilities;
    message.session_id = config_.session_id;
    message.sender_peer_id = config_.local_peer_id;
    message.destination_peer_id = config_.remote_peer_id;
    message.sequence = sequence;
    message.capabilities = config_.local_capabilities;
    return message;
}

E2eActionResult E2eMeasurementSession::ValidateIncoming(
        const E2eMessage& message,
        E2eMessageKind expected_kind) const {
    if (retired_) return Action(E2eActionStatus::Retired, "measurement_session_retired");
    if (message.kind != expected_kind) {
        return Action(E2eActionStatus::InvalidMessage, "measurement_message_kind_mismatch");
    }
    if (message.session_id != config_.session_id ||
        message.sender_peer_id != config_.remote_peer_id ||
        message.destination_peer_id != config_.local_peer_id) {
        return Action(E2eActionStatus::InvalidContext, "measurement_context_mismatch");
    }
    return Action(E2eActionStatus::Accepted, "measurement_context_valid");
}

E2eActionResult E2eMeasurementSession::AcceptCapabilities(
        const E2eMessage& message) {
    auto validation = ValidateIncoming(message, E2eMessageKind::Capabilities);
    if (!validation.accepted()) return validation;
    negotiation_ = NegotiateE2eCapabilities(
        config_.local_capabilities, message.capabilities);
    if (negotiation_.mode == E2eMeasurementMode::Unsupported) {
        pending_probes_.clear();
        ClearMediaState();
        return Action(E2eActionStatus::Unsupported, negotiation_.reason);
    }
    if (!negotiation_.media_correlation) ClearMediaState();
    return Action(E2eActionStatus::Accepted, negotiation_.reason);
}

void E2eMeasurementSession::MarkPeerUnavailable() {
    negotiation_ = NegotiateE2eCapabilities(
        config_.local_capabilities, std::nullopt);
    pending_probes_.clear();
    ClearMediaState();
    calibrator_.Reset();
}

bool E2eMeasurementSession::ClockReady() const noexcept {
    return negotiation_.clock_sync &&
        negotiation_.mode != E2eMeasurementMode::Unsupported;
}

bool E2eMeasurementSession::MediaReady() const noexcept {
    return negotiation_.mode == E2eMeasurementMode::MediaCorrelated &&
        negotiation_.media_correlation;
}

void E2eMeasurementSession::ClearMediaState() {
    for (const auto& [probe_id, probe] : pending_media_probes_) {
        (void)probe;
        RememberProbe(completed_outgoing_media_probes_,
            completed_outgoing_media_probe_set_, probe_id,
            config_.maximum_pending_media_probes);
    }
    for (const auto& [probe_id, probe] : incoming_media_probes_) {
        (void)probe;
        RememberProbe(completed_incoming_media_probes_,
            completed_incoming_media_probe_set_, probe_id,
            config_.maximum_pending_media_probes);
    }
    pending_media_probes_.clear();
    incoming_media_probes_.clear();
}

E2eActionResult E2eMeasurementSession::BeginClockProbe(
        std::string nonce,
        std::uint64_t sequence,
        std::int64_t local_send_us) {
    if (retired_) return Action(E2eActionStatus::Retired, "measurement_session_retired");
    if (!ClockReady()) return Action(E2eActionStatus::NotReady, "clock_sync_not_ready");
    if (!Identifier(nonce) || sequence == 0 || local_send_us < 0) {
        return Action(E2eActionStatus::InvalidMessage, "invalid_clock_probe");
    }
    if (pending_probes_.find(nonce) != pending_probes_.end() ||
        std::any_of(pending_probes_.begin(), pending_probes_.end(),
            [sequence](const auto& entry) {
                return entry.second.sequence == sequence;
            })) {
        return Action(E2eActionStatus::DuplicateOrUnknown, "duplicate_clock_probe");
    }
    if (pending_probes_.size() >= config_.maximum_pending_probes) {
        return Action(E2eActionStatus::CapacityExceeded, "clock_probe_capacity_reached");
    }

    pending_probes_.emplace(nonce, PendingProbe{sequence, local_send_us});
    E2eActionResult result = Action(E2eActionStatus::Accepted, "clock_probe_started");
    E2eMessage request;
    request.kind = E2eMessageKind::ClockRequest;
    request.session_id = config_.session_id;
    request.sender_peer_id = config_.local_peer_id;
    request.destination_peer_id = config_.remote_peer_id;
    request.sequence = sequence;
    request.protocol_version = negotiation_.protocol_version;
    request.nonce = std::move(nonce);
    request.t0_local_send_us = local_send_us;
    result.message = std::move(request);
    return result;
}

E2eActionResult E2eMeasurementSession::AnswerClockProbe(
        const E2eMessage& request,
        std::int64_t local_receive_us,
        std::int64_t local_send_us) const {
    auto validation = ValidateIncoming(request, E2eMessageKind::ClockRequest);
    if (!validation.accepted()) return validation;
    if (!ClockReady()) return Action(E2eActionStatus::NotReady, "clock_sync_not_ready");
    if (request.protocol_version != negotiation_.protocol_version ||
        !Identifier(request.nonce) || request.sequence == 0 ||
        request.t0_local_send_us < 0 || local_receive_us < 0 ||
        local_send_us < local_receive_us) {
        return Action(E2eActionStatus::InvalidMessage, "invalid_clock_request");
    }

    E2eActionResult result = Action(E2eActionStatus::Accepted, "clock_response_ready");
    E2eMessage response;
    response.kind = E2eMessageKind::ClockResponse;
    response.session_id = config_.session_id;
    response.sender_peer_id = config_.local_peer_id;
    response.destination_peer_id = config_.remote_peer_id;
    response.sequence = request.sequence;
    response.protocol_version = negotiation_.protocol_version;
    response.nonce = request.nonce;
    response.t0_local_send_us = request.t0_local_send_us;
    response.t1_remote_receive_us = local_receive_us;
    response.t2_remote_send_us = local_send_us;
    result.message = std::move(response);
    return result;
}

E2eActionResult E2eMeasurementSession::AcceptClockResponse(
        const E2eMessage& response,
        std::int64_t local_receive_us) {
    auto validation = ValidateIncoming(response, E2eMessageKind::ClockResponse);
    if (!validation.accepted()) return validation;
    if (!ClockReady()) return Action(E2eActionStatus::NotReady, "clock_sync_not_ready");
    const auto pending = pending_probes_.find(response.nonce);
    if (pending == pending_probes_.end()) {
        return Action(E2eActionStatus::DuplicateOrUnknown, "unknown_clock_response");
    }
    if (response.protocol_version != negotiation_.protocol_version ||
        response.sequence != pending->second.sequence ||
        response.t0_local_send_us != pending->second.local_send_us ||
        local_receive_us < pending->second.local_send_us) {
        return Action(E2eActionStatus::InvalidMessage, "clock_response_mismatch");
    }
    ClockExchange exchange;
    exchange.sequence = response.sequence;
    exchange.t0_local_send_us = pending->second.local_send_us;
    exchange.t1_remote_receive_us = response.t1_remote_receive_us;
    exchange.t2_remote_send_us = response.t2_remote_send_us;
    exchange.t3_local_receive_us = local_receive_us;
    if (!calibrator_.AddExchange(exchange)) {
        return Action(E2eActionStatus::InvalidMessage, "invalid_clock_exchange");
    }
    pending_probes_.erase(pending);
    return Action(E2eActionStatus::Accepted, "clock_response_accepted");
}

E2eActionResult E2eMeasurementSession::BeginMediaProbe(
        std::string track_id,
        std::string nonce,
        std::uint64_t probe_id,
        std::uint64_t sequence,
        E2eMediaProbeTimes times) {
    if (retired_) {
        return Action(E2eActionStatus::Retired, "measurement_session_retired");
    }
    if (!MediaReady()) {
        return Action(E2eActionStatus::NotReady, "media_correlation_not_ready");
    }
    if (!Identifier(track_id) || !Identifier(nonce) || probe_id == 0 ||
        sequence == 0 ||
        sequence > std::numeric_limits<std::uint32_t>::max() ||
        times.publication_start_us < 0 ||
        times.capture_us < times.publication_start_us ||
        times.marker_send_us < times.capture_us) {
        return Action(E2eActionStatus::InvalidMessage, "invalid_media_probe");
    }
    if (pending_media_probes_.contains(probe_id) ||
        completed_outgoing_media_probe_set_.contains(probe_id) ||
        std::any_of(pending_media_probes_.begin(), pending_media_probes_.end(),
            [&](const auto& entry) {
                return entry.second.sequence == sequence ||
                    entry.second.nonce == nonce;
            })) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "duplicate_media_probe");
    }
    if (pending_media_probes_.size() >=
        config_.maximum_pending_media_probes) {
        return Action(E2eActionStatus::CapacityExceeded,
            "media_probe_capacity_reached");
    }

    PendingMediaProbe pending;
    pending.track_id = track_id;
    pending.nonce = nonce;
    pending.sequence = sequence;
    pending.times = times;
    pending_media_probes_.emplace(probe_id, std::move(pending));

    E2eActionResult result = Action(
        E2eActionStatus::Accepted, "media_probe_started");
    E2eMessage announcement;
    announcement.kind = E2eMessageKind::MediaProbe;
    announcement.session_id = config_.session_id;
    announcement.sender_peer_id = config_.local_peer_id;
    announcement.destination_peer_id = config_.remote_peer_id;
    announcement.sequence = sequence;
    announcement.protocol_version = negotiation_.protocol_version;
    announcement.nonce = std::move(nonce);
    announcement.track_id = std::move(track_id);
    announcement.probe_id = probe_id;
    result.message = std::move(announcement);
    return result;
}

E2eActionResult E2eMeasurementSession::AcceptMediaProbe(
        const E2eMessage& announcement) {
    auto validation = ValidateIncoming(
        announcement, E2eMessageKind::MediaProbe);
    if (!validation.accepted()) return validation;
    if (!MediaReady()) {
        return Action(E2eActionStatus::NotReady, "media_correlation_not_ready");
    }
    if (!ValidMediaIdentity(announcement) ||
        announcement.protocol_version != negotiation_.protocol_version) {
        return Action(E2eActionStatus::InvalidMessage,
            "invalid_media_probe_announcement");
    }
    if (incoming_media_probes_.contains(announcement.probe_id) ||
        completed_incoming_media_probe_set_.contains(announcement.probe_id) ||
        std::any_of(incoming_media_probes_.begin(), incoming_media_probes_.end(),
            [&](const auto& entry) {
                return entry.second.sequence == announcement.sequence ||
                    entry.second.nonce == announcement.nonce;
            })) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "duplicate_media_probe_announcement");
    }
    if (incoming_media_probes_.size() >=
        config_.maximum_pending_media_probes) {
        return Action(E2eActionStatus::CapacityExceeded,
            "incoming_media_probe_capacity_reached");
    }

    IncomingMediaProbe pending;
    pending.track_id = announcement.track_id;
    pending.nonce = announcement.nonce;
    pending.sequence = announcement.sequence;
    incoming_media_probes_.emplace(
        announcement.probe_id, std::move(pending));
    return Action(E2eActionStatus::Accepted,
        "media_probe_announcement_accepted");
}

E2eActionResult E2eMeasurementSession::ObserveDecodedMediaMarker(
        std::string_view track_id,
        const E2eMediaMarker& marker,
        std::int64_t remote_decode_us,
        std::uint64_t render_token) {
    if (retired_) {
        return Action(E2eActionStatus::Retired, "measurement_session_retired");
    }
    if (!MediaReady()) {
        return Action(E2eActionStatus::NotReady, "media_correlation_not_ready");
    }
    if (!Identifier(track_id) || marker.probe_id == 0 ||
        marker.sequence == 0 || remote_decode_us < 0 || render_token == 0) {
        return Action(E2eActionStatus::InvalidMessage,
            "invalid_decoded_media_marker");
    }
    if (completed_incoming_media_probe_set_.contains(marker.probe_id)) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "completed_media_marker");
    }
    const auto found = incoming_media_probes_.find(marker.probe_id);
    if (found == incoming_media_probes_.end()) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "unknown_media_marker");
    }
    auto& pending = found->second;
    if (pending.track_id != track_id || pending.sequence != marker.sequence) {
        return Action(E2eActionStatus::InvalidMessage,
            "media_marker_context_mismatch");
    }
    if (pending.decoded ||
        std::any_of(incoming_media_probes_.begin(), incoming_media_probes_.end(),
            [&](const auto& entry) {
                return entry.second.decoded &&
                    entry.second.track_id == track_id &&
                    entry.second.render_token == render_token;
            })) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "duplicate_decoded_media_marker");
    }
    pending.decoded = true;
    pending.remote_decode_us = remote_decode_us;
    pending.render_token = render_token;
    return Action(E2eActionStatus::Accepted,
        "decoded_media_marker_accepted");
}

E2eActionResult E2eMeasurementSession::BuildMediaAckAfterRenderSubmit(
        std::string_view track_id,
        std::uint64_t render_token,
        std::int64_t remote_render_submit_us,
        std::string measurement_point,
        std::uint32_t remote_frame_width,
        std::uint32_t remote_frame_height) {
    if (retired_) {
        return Action(E2eActionStatus::Retired, "measurement_session_retired");
    }
    if (!MediaReady()) {
        return Action(E2eActionStatus::NotReady, "media_correlation_not_ready");
    }
    if (!Identifier(track_id) || render_token == 0 ||
        remote_render_submit_us < 0 || !Identifier(measurement_point) ||
        ((remote_frame_width == 0) != (remote_frame_height == 0))) {
        return Action(E2eActionStatus::InvalidMessage,
            "invalid_render_submit");
    }
    const auto found = std::find_if(
        incoming_media_probes_.begin(), incoming_media_probes_.end(),
        [&](const auto& entry) {
            return entry.second.decoded &&
                entry.second.track_id == track_id &&
                entry.second.render_token == render_token;
        });
    if (found == incoming_media_probes_.end()) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "render_submit_without_decoded_marker");
    }
    if (remote_render_submit_us < found->second.remote_decode_us) {
        return Action(E2eActionStatus::InvalidMessage,
            "render_submit_precedes_decode");
    }

    E2eMessage acknowledgement;
    acknowledgement.kind = E2eMessageKind::MediaAck;
    acknowledgement.session_id = config_.session_id;
    acknowledgement.sender_peer_id = config_.local_peer_id;
    acknowledgement.destination_peer_id = config_.remote_peer_id;
    acknowledgement.sequence = found->second.sequence;
    acknowledgement.protocol_version = negotiation_.protocol_version;
    acknowledgement.nonce = found->second.nonce;
    acknowledgement.track_id = found->second.track_id;
    acknowledgement.probe_id = found->first;
    acknowledgement.remote_decode_us = found->second.remote_decode_us;
    acknowledgement.remote_render_submit_us = remote_render_submit_us;
    acknowledgement.measurement_point = std::move(measurement_point);
    acknowledgement.remote_frame_width = remote_frame_width;
    acknowledgement.remote_frame_height = remote_frame_height;

    const auto completed_probe_id = found->first;
    incoming_media_probes_.erase(found);
    RememberProbe(completed_incoming_media_probes_,
        completed_incoming_media_probe_set_, completed_probe_id,
        config_.maximum_pending_media_probes);
    E2eActionResult result = Action(
        E2eActionStatus::Accepted, "media_ack_ready_after_render_submit");
    result.message = std::move(acknowledgement);
    return result;
}

E2eActionResult E2eMeasurementSession::AcceptMediaAck(
        const E2eMessage& acknowledgement,
        std::int64_t local_receive_us) {
    auto validation = ValidateIncoming(
        acknowledgement, E2eMessageKind::MediaAck);
    if (!validation.accepted()) return validation;
    if (!MediaReady()) {
        return Action(E2eActionStatus::NotReady, "media_correlation_not_ready");
    }
    if (completed_outgoing_media_probe_set_.contains(
            acknowledgement.probe_id)) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "duplicate_media_ack");
    }
    const auto found = pending_media_probes_.find(acknowledgement.probe_id);
    if (found == pending_media_probes_.end()) {
        return Action(E2eActionStatus::DuplicateOrUnknown,
            "unknown_media_ack");
    }
    const auto& pending = found->second;
    if (!ValidMediaIdentity(acknowledgement) ||
        acknowledgement.protocol_version != negotiation_.protocol_version ||
        acknowledgement.track_id != pending.track_id ||
        acknowledgement.nonce != pending.nonce ||
        acknowledgement.sequence != pending.sequence ||
        acknowledgement.remote_decode_us < 0 ||
        acknowledgement.remote_render_submit_us <
            acknowledgement.remote_decode_us ||
        !Identifier(acknowledgement.measurement_point) ||
        local_receive_us < pending.times.marker_send_us) {
        return Action(E2eActionStatus::InvalidMessage,
            "media_ack_mismatch");
    }

    E2eMediaMeasurement measurement;
    measurement.track_id = pending.track_id;
    measurement.probe_id = acknowledgement.probe_id;
    measurement.sequence = acknowledgement.sequence;
    measurement.ack_round_trip_us =
        local_receive_us - pending.times.marker_send_us;
    measurement.measurement_point = acknowledgement.measurement_point;
    if (acknowledgement.remote_frame_width != 0) {
        measurement.remote_frame_width = acknowledgement.remote_frame_width;
        measurement.remote_frame_height = acknowledgement.remote_frame_height;
    }
    const auto mapping = calibrator_.Evaluate(local_receive_us);
    measurement.clock_state = mapping.state;
    measurement.uncertainty_us = mapping.uncertainty_us;
    measurement.one_way_reason = mapping.reason;
    if (mapping.state == ClockCalibrationState::Valid) {
        const auto decode_local = mapping.RemoteToLocal(
            acknowledgement.remote_decode_us);
        const auto render_local = mapping.RemoteToLocal(
            acknowledgement.remote_render_submit_us);
        if (decode_local && render_local &&
            decode_local->local_time_us >= pending.times.publication_start_us &&
            render_local->local_time_us >= pending.times.capture_us &&
            render_local->local_time_us <= local_receive_us) {
            measurement.publication_to_decode_us =
                decode_local->local_time_us - pending.times.publication_start_us;
            measurement.capture_to_render_submit_us =
                render_local->local_time_us - pending.times.capture_us;
            measurement.one_way_reason = "clock_mapped_media_measurements_valid";
        } else {
            measurement.one_way_reason =
                "mapped_media_time_precedes_local_origin";
        }
    }

    const auto completed_probe_id = found->first;
    pending_media_probes_.erase(found);
    RememberProbe(completed_outgoing_media_probes_,
        completed_outgoing_media_probe_set_, completed_probe_id,
        config_.maximum_pending_media_probes);
    E2eActionResult result = Action(
        E2eActionStatus::Accepted, "media_ack_accepted");
    result.media_measurement = std::move(measurement);
    return result;
}

void E2eMeasurementSession::CancelMediaTrack(std::string_view track_id) {
    if (track_id.empty()) return;
    for (auto it = pending_media_probes_.begin();
         it != pending_media_probes_.end();) {
        if (it->second.track_id != track_id) {
            ++it;
            continue;
        }
        RememberProbe(completed_outgoing_media_probes_,
            completed_outgoing_media_probe_set_, it->first,
            config_.maximum_pending_media_probes);
        it = pending_media_probes_.erase(it);
    }
    for (auto it = incoming_media_probes_.begin();
         it != incoming_media_probes_.end();) {
        if (it->second.track_id != track_id) {
            ++it;
            continue;
        }
        RememberProbe(completed_incoming_media_probes_,
            completed_incoming_media_probe_set_, it->first,
            config_.maximum_pending_media_probes);
        it = incoming_media_probes_.erase(it);
    }
}

ClockMapping E2eMeasurementSession::clock_mapping(
        std::int64_t now_local_us) const {
    if (!ClockReady()) {
        ClockMapping result;
        result.state = ClockCalibrationState::Invalid;
        result.reason = negotiation_.reason;
        return result;
    }
    return calibrator_.Evaluate(now_local_us);
}

const E2eNegotiation& E2eMeasurementSession::negotiation() const noexcept {
    return negotiation_;
}

std::size_t E2eMeasurementSession::pending_probe_count() const noexcept {
    return pending_probes_.size();
}

std::size_t E2eMeasurementSession::pending_media_probe_count() const noexcept {
    return pending_media_probes_.size();
}

std::size_t E2eMeasurementSession::pending_incoming_media_probe_count() const noexcept {
    return incoming_media_probes_.size();
}

void E2eMeasurementSession::Retire() {
    retired_ = true;
    pending_probes_.clear();
    ClearMediaState();
    calibrator_.Reset();
    negotiation_ = {};
    negotiation_.reason = "measurement_session_retired";
}

} // namespace livekit::telemetry
