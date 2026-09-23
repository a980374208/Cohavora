#include "src/telemetry/e2e_measurement.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace livekit::telemetry;

E2eCapabilities FullCapabilities() {
    return {{1}, true, true, true};
}

E2eMessage PeerCapabilities(
        const E2eCapabilities& capabilities,
        std::uint64_t sequence = 1) {
    E2eMessage message;
    message.kind = E2eMessageKind::Capabilities;
    message.session_id = "session-1";
    message.sender_peer_id = "peer-b";
    message.destination_peer_id = "peer-a";
    message.sequence = sequence;
    message.capabilities = capabilities;
    return message;
}

E2eMeasurementSession MakeSession(
        ClockCalibrationPolicy policy = {},
        std::size_t maximum_pending = 16) {
    E2eMeasurementSessionConfig config;
    config.session_id = "session-1";
    config.local_peer_id = "peer-a";
    config.remote_peer_id = "peer-b";
    config.local_capabilities = FullCapabilities();
    config.clock_policy = policy;
    config.maximum_pending_probes = maximum_pending;
    return E2eMeasurementSession(std::move(config));
}

std::int64_t RemoteClock(
        std::int64_t local_us,
        std::int64_t base_offset_us,
        double drift_ppm) {
    return static_cast<std::int64_t>(std::llround(
        static_cast<long double>(local_us) + base_offset_us +
        static_cast<long double>(local_us) * drift_ppm / 1'000'000.0L));
}

void WireCodecIsBoundedAndStrict() {
    auto original = PeerCapabilities(FullCapabilities());
    const auto encoded = EncodeE2eMessage(original);
    TEST_CHECK(encoded.has_value());
    TEST_CHECK(encoded->size() <= kE2eMeasurementMaxWireBytes);
    const auto decoded = DecodeE2eMessage(kE2eMeasurementTopic, *encoded);
    TEST_CHECK(decoded.has_value());
    TEST_CHECK(decoded->kind == E2eMessageKind::Capabilities);
    TEST_CHECK(decoded->session_id == "session-1");
    TEST_CHECK(decoded->capabilities.frame_identity);

    TEST_CHECK(!DecodeE2eMessage("unrelated.topic", *encoded));
    TEST_CHECK(!DecodeE2eMessage(
        kE2eMeasurementTopic,
        std::string(kE2eMeasurementMaxWireBytes + 1, 'x')));

    auto extra = nlohmann::json::parse(*encoded);
    extra["rawIdentity"] = "must-not-be-accepted";
    TEST_CHECK(!DecodeE2eMessage(kE2eMeasurementTopic, extra.dump()));

    auto bad_version = nlohmann::json::parse(*encoded);
    bad_version["wireVersion"] = 2;
    TEST_CHECK(!DecodeE2eMessage(kE2eMeasurementTopic, bad_version.dump()));

    original.sender_peer_id = "peer b";
    TEST_CHECK(!EncodeE2eMessage(original));

    E2eMessage response;
    response.kind = E2eMessageKind::ClockResponse;
    response.session_id = "session-1";
    response.sender_peer_id = "peer-b";
    response.destination_peer_id = "peer-a";
    response.sequence = 9;
    response.protocol_version = 1;
    response.nonce = "nonce-9";
    response.t0_local_send_us = 100'000;
    response.t1_remote_receive_us = 151'000;
    response.t2_remote_send_us = 152'000;
    const auto response_bytes = EncodeE2eMessage(response);
    TEST_CHECK(response_bytes.has_value());
    const auto decoded_response = DecodeE2eMessage(
        kE2eMeasurementTopic, *response_bytes);
    TEST_CHECK(decoded_response.has_value());
    TEST_CHECK(decoded_response->kind == E2eMessageKind::ClockResponse);
    TEST_CHECK(decoded_response->t0_local_send_us == 100'000);
    TEST_CHECK(decoded_response->t1_remote_receive_us == 151'000);
    TEST_CHECK(decoded_response->t2_remote_send_us == 152'000);

    auto reversed_time = nlohmann::json::parse(*response_bytes);
    reversed_time["t2Us"] = "150000";
    TEST_CHECK(!DecodeE2eMessage(
        kE2eMeasurementTopic, reversed_time.dump()));
}

void CapabilityNegotiationDegradesHonestly() {
    const auto full = NegotiateE2eCapabilities(
        FullCapabilities(), FullCapabilities());
    TEST_CHECK(full.mode == E2eMeasurementMode::MediaCorrelated);
    TEST_CHECK(full.protocol_version == 1);
    TEST_CHECK(full.clock_sync);
    TEST_CHECK(full.media_correlation);

    auto clock_only = FullCapabilities();
    clock_only.frame_identity = false;
    const auto partial = NegotiateE2eCapabilities(
        FullCapabilities(), clock_only);
    TEST_CHECK(partial.mode == E2eMeasurementMode::ClockRoundTripOnly);
    TEST_CHECK(partial.clock_sync);
    TEST_CHECK(!partial.media_correlation);
    TEST_CHECK(partial.reason == "media_frame_identity_unavailable");

    const auto third_party = NegotiateE2eCapabilities(
        FullCapabilities(), std::nullopt);
    TEST_CHECK(third_party.mode == E2eMeasurementMode::Unsupported);
    TEST_CHECK(third_party.reason == "peer_no_measurement_protocol");

    auto incompatible = FullCapabilities();
    incompatible.protocol_versions = {2};
    const auto mismatch = NegotiateE2eCapabilities(
        FullCapabilities(), incompatible);
    TEST_CHECK(mismatch.mode == E2eMeasurementMode::Unsupported);
    TEST_CHECK(mismatch.reason == "protocol_version_mismatch");
}

void ClockCalibrationTracksOffsetDriftAndBounds() {
    ClockCalibrationPolicy policy;
    policy.maximum_samples = 8;
    policy.minimum_samples = 3;
    policy.maximum_age_us = 5'000'000;
    policy.maximum_uncertainty_us = 10'000;
    ClockCalibrator calibrator(policy);

    constexpr std::int64_t kOffsetUs = 50'000;
    constexpr double kDriftPpm = 100.0;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        const std::int64_t t0 = static_cast<std::int64_t>(sequence) * 10'000'000;
        const std::int64_t one_way = 2'000;
        ClockExchange exchange;
        exchange.sequence = sequence;
        exchange.t0_local_send_us = t0;
        exchange.t1_remote_receive_us = RemoteClock(
            t0 + one_way, kOffsetUs, kDriftPpm);
        exchange.t2_remote_send_us = exchange.t1_remote_receive_us;
        exchange.t3_local_receive_us = t0 + 2 * one_way;
        TEST_CHECK(calibrator.AddExchange(exchange));
    }

    const auto mapping = calibrator.Evaluate(40'003'000);
    TEST_CHECK(mapping.state == ClockCalibrationState::Valid);
    TEST_CHECK(mapping.accepted_samples == 4);
    TEST_CHECK(std::abs(mapping.drift_ppm - kDriftPpm) < 0.01);
    TEST_CHECK(mapping.uncertainty_us <= 2'010);

    const std::int64_t source_local = 40'500'000;
    const auto remote = RemoteClock(source_local, kOffsetUs, kDriftPpm);
    const auto converted = mapping.RemoteToLocal(remote);
    TEST_CHECK(converted.has_value());
    TEST_CHECK(std::abs(converted->local_time_us - source_local) <= 1);
    TEST_CHECK(converted->lower_bound_us <= source_local);
    TEST_CHECK(converted->upper_bound_us >= source_local);

    ClockExchange duplicate;
    duplicate.sequence = 4;
    duplicate.t0_local_send_us = 40'000'000;
    duplicate.t1_remote_receive_us = 40'050'000;
    duplicate.t2_remote_send_us = 40'050'000;
    duplicate.t3_local_receive_us = 40'004'000;
    TEST_CHECK(!calibrator.AddExchange(duplicate));
    TEST_CHECK(calibrator.Evaluate(40'003'000).rejected_samples == 1);

    const auto stale = calibrator.Evaluate(46'000'000);
    TEST_CHECK(stale.state == ClockCalibrationState::Stale);
    TEST_CHECK(!stale.RemoteToLocal(remote));
}

void AsymmetricOrUncertainClockNeverBecomesFalsePrecision() {
    ClockCalibrationPolicy policy;
    policy.maximum_uncertainty_us = 10'000;
    ClockCalibrator bounded(policy);
    constexpr std::int64_t kOffsetUs = 20'000;
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        const std::int64_t t0 = static_cast<std::int64_t>(sequence) * 1'000'000;
        ClockExchange exchange;
        exchange.sequence = sequence;
        exchange.t0_local_send_us = t0;
        exchange.t1_remote_receive_us = t0 + 1'000 + kOffsetUs;
        exchange.t2_remote_send_us = exchange.t1_remote_receive_us;
        exchange.t3_local_receive_us = t0 + 10'000;
        TEST_CHECK(bounded.AddExchange(exchange));
    }
    const auto mapping = bounded.Evaluate(3'006'000);
    TEST_CHECK(mapping.state == ClockCalibrationState::Valid);
    const auto actual_remote = 3'500'000 + kOffsetUs;
    const auto converted = mapping.RemoteToLocal(actual_remote);
    TEST_CHECK(converted.has_value());
    TEST_CHECK(converted->lower_bound_us <= 3'500'000);
    TEST_CHECK(converted->upper_bound_us >= 3'500'000);

    ClockCalibrator high_latency(policy);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        const std::int64_t t0 = static_cast<std::int64_t>(sequence) * 1'000'000;
        TEST_CHECK(high_latency.AddExchange({
            sequence,
            t0,
            t0 + 30'000 + kOffsetUs,
            t0 + 30'000 + kOffsetUs,
            t0 + 60'000,
        }));
    }
    const auto uncertain = high_latency.Evaluate(3'040'000);
    TEST_CHECK(uncertain.state == ClockCalibrationState::Uncertain);
    TEST_CHECK(uncertain.reason == "clock_uncertainty_exceeds_limit");
    TEST_CHECK(!uncertain.RemoteToLocal(actual_remote));
}

void SessionRejectsWrongContextReplayAndCapacity() {
    ClockCalibrationPolicy policy;
    policy.minimum_samples = 2;
    auto initiator = MakeSession(policy, 2);
    auto peer = PeerCapabilities(FullCapabilities());
    TEST_CHECK(initiator.AcceptCapabilities(peer).accepted());

    auto first = initiator.BeginClockProbe("nonce-1", 10, 1'000'000);
    auto second = initiator.BeginClockProbe("nonce-2", 11, 2'000'000);
    TEST_CHECK(first.accepted() && first.message);
    TEST_CHECK(second.accepted() && second.message);
    TEST_CHECK(initiator.pending_probe_count() == 2);
    TEST_CHECK(initiator.BeginClockProbe("nonce-3", 12, 3'000'000).status ==
        E2eActionStatus::CapacityExceeded);
    TEST_CHECK(initiator.BeginClockProbe("nonce-1", 13, 3'000'000).status ==
        E2eActionStatus::DuplicateOrUnknown);

    E2eMessage response;
    response.kind = E2eMessageKind::ClockResponse;
    response.session_id = "session-1";
    response.sender_peer_id = "peer-b";
    response.destination_peer_id = "peer-a";
    response.sequence = 11;
    response.protocol_version = 1;
    response.nonce = "nonce-2";
    response.t0_local_send_us = 2'000'000;
    response.t1_remote_receive_us = 2'052'000;
    response.t2_remote_send_us = 2'052'000;
    TEST_CHECK(initiator.AcceptClockResponse(response, 2'004'000).accepted());

    response.sequence = 10;
    response.nonce = "nonce-1";
    response.t0_local_send_us = 1'000'000;
    response.t1_remote_receive_us = 1'052'000;
    response.t2_remote_send_us = 1'052'000;
    TEST_CHECK(initiator.AcceptClockResponse(response, 1'004'000).accepted());
    TEST_CHECK(initiator.clock_mapping(2'004'000).state ==
        ClockCalibrationState::Valid);

    TEST_CHECK(initiator.AcceptClockResponse(response, 1'004'000).status ==
        E2eActionStatus::DuplicateOrUnknown);
    auto forged = response;
    forged.session_id = "session-old";
    TEST_CHECK(initiator.AcceptClockResponse(forged, 1'004'000).status ==
        E2eActionStatus::InvalidContext);

    initiator.Retire();
    TEST_CHECK(initiator.pending_probe_count() == 0);
    TEST_CHECK(initiator.BeginClockProbe("nonce-late", 20, 4'000'000).status ==
        E2eActionStatus::Retired);
    TEST_CHECK(initiator.AcceptClockResponse(response, 1'004'000).status ==
        E2eActionStatus::Retired);
}

void ResponderEchoesOnlyNegotiatedValidRequests() {
    auto responder = MakeSession();
    TEST_CHECK(responder.AcceptCapabilities(PeerCapabilities(FullCapabilities())).accepted());

    E2eMessage request;
    request.kind = E2eMessageKind::ClockRequest;
    request.session_id = "session-1";
    request.sender_peer_id = "peer-b";
    request.destination_peer_id = "peer-a";
    request.sequence = 7;
    request.protocol_version = 1;
    request.nonce = "nonce-7";
    request.t0_local_send_us = 100'000;
    const auto answered = responder.AnswerClockProbe(request, 150'000, 151'000);
    TEST_CHECK(answered.accepted() && answered.message);
    TEST_CHECK(answered.message->sender_peer_id == "peer-a");
    TEST_CHECK(answered.message->destination_peer_id == "peer-b");
    TEST_CHECK(answered.message->t0_local_send_us == 100'000);
    TEST_CHECK(answered.message->t1_remote_receive_us == 150'000);
    TEST_CHECK(answered.message->t2_remote_send_us == 151'000);

    request.protocol_version = 2;
    TEST_CHECK(responder.AnswerClockProbe(request, 150'000, 151'000).status ==
        E2eActionStatus::InvalidMessage);

    responder.MarkPeerUnavailable();
    TEST_CHECK(responder.negotiation().mode == E2eMeasurementMode::Unsupported);
    TEST_CHECK(responder.negotiation().reason == "peer_no_measurement_protocol");
    TEST_CHECK(responder.AnswerClockProbe(request, 150'000, 151'000).status ==
        E2eActionStatus::NotReady);
}

} // namespace

int main() {
    WireCodecIsBoundedAndStrict();
    CapabilityNegotiationDegradesHonestly();
    ClockCalibrationTracksOffsetDriftAndBounds();
    AsymmetricOrUncertainClockNeverBecomesFalsePrecision();
    SessionRejectsWrongContextReplayAndCapacity();
    ResponderEchoesOnlyNegotiatedValidRequests();
    return 0;
}
