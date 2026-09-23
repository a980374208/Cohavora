#include "src/render/video_render_frame.h"
#include "src/telemetry/e2e_measurement.h"
#include "src/telemetry/e2e_media_marker.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace livekit;
using namespace livekit::render;
using namespace livekit::telemetry;

constexpr std::string_view kTrackId = "TR_controlled_video";

E2eCapabilities FullCapabilities() {
    return {{1}, true, true, true};
}

E2eMeasurementSession MakeSession(
        std::string local_peer,
        std::string remote_peer,
        std::size_t maximum_pending_media = 16) {
    ClockCalibrationPolicy policy;
    policy.minimum_samples = 3;
    policy.maximum_uncertainty_us = 10'000;
    E2eMeasurementSessionConfig config;
    config.session_id = "session-1";
    config.local_peer_id = std::move(local_peer);
    config.remote_peer_id = std::move(remote_peer);
    config.local_capabilities = FullCapabilities();
    config.clock_policy = policy;
    config.maximum_pending_media_probes = maximum_pending_media;
    return E2eMeasurementSession(std::move(config));
}

void Negotiate(
        E2eMeasurementSession& initiator,
        E2eMeasurementSession& responder) {
    TEST_CHECK(responder.AcceptCapabilities(
        initiator.BuildCapabilities(1)).accepted());
    TEST_CHECK(initiator.AcceptCapabilities(
        responder.BuildCapabilities(1)).accepted());
}

void CalibrateInitiator(
        E2eMeasurementSession& initiator,
        const E2eMeasurementSession& responder) {
    constexpr std::int64_t kRemoteOffsetUs = 50'000;
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        const auto t0 = static_cast<std::int64_t>(sequence) * 1'000'000;
        auto request = initiator.BeginClockProbe(
            "clock-" + std::to_string(sequence), sequence, t0);
        TEST_CHECK(request.accepted() && request.message);
        auto response = responder.AnswerClockProbe(
            *request.message,
            t0 + 2'000 + kRemoteOffsetUs,
            t0 + 2'000 + kRemoteOffsetUs);
        TEST_CHECK(response.accepted() && response.message);
        TEST_CHECK(initiator.AcceptClockResponse(
            *response.message, t0 + 4'000).accepted());
    }
}

VideoFrame ControlledFrame(int width = 640, int height = 360) {
    auto frame = VideoFrame::create(width, height, VideoBufferType::I420);
    std::fill(frame.data(), frame.data() + frame.dataSize(), 128);
    return frame;
}

VideoFrame DownscaleI420Nearest(
        const VideoFrame& source,
        int destination_width,
        int destination_height) {
    TEST_CHECK(source.type() == VideoBufferType::I420);
    auto destination = ControlledFrame(destination_width, destination_height);
    for (int y = 0; y < destination_height; ++y) {
        const auto source_y = y * source.height() / destination_height;
        for (int x = 0; x < destination_width; ++x) {
            const auto source_x = x * source.width() / destination_width;
            destination.data()[y * destination_width + x] =
                source.data()[source_y * source.width() + source_x];
        }
    }
    return destination;
}

void FlipEmbeddedMarkerBit(VideoFrame& frame, std::size_t bit_index) {
    constexpr int kBitsPerRow = 20;
    constexpr int kGridColumns = 40;
    constexpr int kGridRows = 8;
    TEST_CHECK(frame.type() == VideoBufferType::I420 && bit_index < 160);
    const int left = frame.width() / 16;
    const int top = frame.height() / 16;
    const int region_width = frame.width() - 2 * left;
    const int region_height = frame.height() / 2;
    const int row = static_cast<int>(bit_index / kBitsPerRow);
    const int first_column = static_cast<int>(bit_index % kBitsPerRow) * 2;
    const auto fill = [&](int column, std::uint8_t value) {
        const int x0 = left + column * region_width / kGridColumns;
        const int x1 = left + (column + 1) * region_width / kGridColumns;
        const int y0 = top + row * region_height / kGridRows;
        const int y1 = top + (row + 1) * region_height / kGridRows;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                frame.data()[y * frame.width() + x] = value;
            }
        }
    };
    const auto first_x = left + first_column * region_width / kGridColumns;
    const auto first_y = top + row * region_height / kGridRows;
    const bool original_bit = frame.data()[first_y * frame.width() + first_x] > 128;
    fill(first_column, original_bit ? 16 : 235);
    fill(first_column + 1, original_bit ? 235 : 16);
}

OwnedI420Frame::Ptr CopyOwned(
        const VideoFrame& source,
        RenderFrameMetadata metadata = {}) {
    TEST_CHECK(source.type() == VideoBufferType::I420);
    const auto width = source.width();
    const auto height = source.height();
    const auto chroma_width = (width + 1) / 2;
    const auto chroma_height = (height + 1) / 2;
    const auto* y = source.data();
    const auto* u = y + static_cast<std::size_t>(width) * height;
    const auto* v = u + static_cast<std::size_t>(chroma_width) * chroma_height;
    return OwnedI420Frame::CopyFromPlanes(
        width, height, y, width, u, chroma_width, v, chroma_width,
        0, VideoRotation::VIDEO_ROTATION_0, {}, std::move(metadata));
}

RenderSubmitObserver::Clock::time_point AtUs(std::int64_t value) {
    return RenderSubmitObserver::Clock::time_point(
        std::chrono::microseconds(value));
}

class MediaAckObserver final : public RenderSubmitObserver {
public:
    MediaAckObserver(E2eMeasurementSession& session, std::string track_id)
        : session_(session), track_id_(std::move(track_id)) {}

    void SetExpected(bool,
                     RenderExpectationReason,
                     Clock::time_point) override {}

    void OnSubmitted(const RenderFrameMetadata& metadata,
                     const char* measurement_point,
                     Clock::time_point source_time) override {
        ++submit_calls;
        auto result = session_.BuildMediaAckAfterRenderSubmit(
            track_id_, metadata.frame_token,
            std::chrono::duration_cast<std::chrono::microseconds>(
                source_time.time_since_epoch()).count(),
            measurement_point ? measurement_point : "render_submit",
            640, 360);
        last_status = result.status;
        if (result.accepted() && result.message) {
            ++accepted_acks;
            acknowledgement = std::move(result.message);
        }
    }

    int submit_calls = 0;
    int accepted_acks = 0;
    E2eActionStatus last_status = E2eActionStatus::InvalidMessage;
    std::optional<E2eMessage> acknowledgement;

private:
    E2eMeasurementSession& session_;
    std::string track_id_;
};

void MarkerSurvivesControlledNoiseAndScaling() {
    constexpr E2eMediaMarker marker{0x7a51f00dcafebeefULL, 42};
    auto frame = ControlledFrame();
    TEST_CHECK(EmbedE2eMediaMarker(frame, marker));
    auto decoded = DecodeE2eMediaMarker(frame);
    TEST_CHECK(decoded && decoded->probe_id == marker.probe_id &&
        decoded->sequence == marker.sequence);

    auto owned = CopyOwned(frame);
    decoded = DecodeE2eMediaMarker(*owned);
    TEST_CHECK(decoded && decoded->probe_id == marker.probe_id &&
        decoded->sequence == marker.sequence);

    auto corrupted = frame;
    FlipEmbeddedMarkerBit(corrupted, 40);
    TEST_CHECK(!DecodeE2eMediaMarker(corrupted));

    const auto y_size = static_cast<std::size_t>(
        frame.width()) * frame.height();
    for (std::size_t i = 0; i < y_size; ++i) {
        const auto delta = static_cast<int>(i % 17) - 8;
        frame.data()[i] = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(frame.data()[i]) + delta, 0, 255));
    }
    decoded = DecodeE2eMediaMarker(frame);
    TEST_CHECK(decoded && decoded->probe_id == marker.probe_id &&
        decoded->sequence == marker.sequence);

    const auto scaled = DownscaleI420Nearest(frame, 320, 180);
    decoded = DecodeE2eMediaMarker(scaled);
    TEST_CHECK(decoded && decoded->probe_id == marker.probe_id &&
        decoded->sequence == marker.sequence);

    auto unrelated = ControlledFrame();
    TEST_CHECK(!DecodeE2eMediaMarker(unrelated));
    auto too_small = ControlledFrame(79, 31);
    TEST_CHECK(!EmbedE2eMediaMarker(too_small, marker));
    auto rgba = VideoFrame::create(640, 360, VideoBufferType::RGBA);
    TEST_CHECK(!EmbedE2eMediaMarker(rgba, marker));
    TEST_CHECK(!DecodeE2eMediaMarker(rgba));
}

void MediaWireCodecIsStrict() {
    E2eMessage probe;
    probe.kind = E2eMessageKind::MediaProbe;
    probe.session_id = "session-1";
    probe.sender_peer_id = "peer-a";
    probe.destination_peer_id = "peer-b";
    probe.sequence = 42;
    probe.protocol_version = 1;
    probe.nonce = "nonce-42";
    probe.track_id = std::string(kTrackId);
    probe.probe_id = 0x7a51f00dcafebeefULL;
    const auto encoded_probe = EncodeE2eMessage(probe);
    TEST_CHECK(encoded_probe);
    const auto decoded_probe = DecodeE2eMessage(
        kE2eMeasurementTopic, *encoded_probe);
    TEST_CHECK(decoded_probe && decoded_probe->kind == E2eMessageKind::MediaProbe);
    TEST_CHECK(decoded_probe->track_id == kTrackId &&
        decoded_probe->probe_id == probe.probe_id);

    auto extra = nlohmann::json::parse(*encoded_probe);
    extra["captureUs"] = "not-accepted";
    TEST_CHECK(!DecodeE2eMessage(kE2eMeasurementTopic, extra.dump()));

    auto acknowledgement = probe;
    acknowledgement.kind = E2eMessageKind::MediaAck;
    acknowledgement.sender_peer_id = "peer-b";
    acknowledgement.destination_peer_id = "peer-a";
    acknowledgement.remote_decode_us = 4'080'000;
    acknowledgement.remote_render_submit_us = 4'085'000;
    acknowledgement.measurement_point = "qt_cpu_paint";
    acknowledgement.remote_frame_width = 640;
    acknowledgement.remote_frame_height = 360;
    const auto encoded_ack = EncodeE2eMessage(acknowledgement);
    TEST_CHECK(encoded_ack);
    const auto decoded_ack = DecodeE2eMessage(
        kE2eMeasurementTopic, *encoded_ack);
    TEST_CHECK(decoded_ack && decoded_ack->kind == E2eMessageKind::MediaAck);
    TEST_CHECK(decoded_ack->remote_render_submit_us == 4'085'000);
    TEST_CHECK(decoded_ack->remote_frame_width == 640);
    TEST_CHECK(decoded_ack->remote_frame_height == 360);

    auto partial_dimensions = acknowledgement;
    partial_dimensions.remote_frame_height = 0;
    TEST_CHECK(!EncodeE2eMessage(partial_dimensions));
    auto partial_wire_dimensions = nlohmann::json::parse(*encoded_ack);
    partial_wire_dimensions.erase("frameHeight");
    TEST_CHECK(!DecodeE2eMessage(
        kE2eMeasurementTopic, partial_wire_dimensions.dump()));

    auto reversed = nlohmann::json::parse(*encoded_ack);
    reversed["renderSubmitUs"] = "4079999";
    TEST_CHECK(!DecodeE2eMessage(kE2eMeasurementTopic, reversed.dump()));
    acknowledgement.measurement_point = "not an identifier";
    TEST_CHECK(!EncodeE2eMessage(acknowledgement));
    probe.sequence = static_cast<std::uint64_t>(
        std::numeric_limits<std::uint32_t>::max()) + 1;
    TEST_CHECK(!EncodeE2eMessage(probe));
}

void AckRequiresDecodedMarkerAndActualRenderSubmit() {
    auto initiator = MakeSession("peer-a", "peer-b");
    auto responder = MakeSession("peer-b", "peer-a");
    Negotiate(initiator, responder);
    CalibrateInitiator(initiator, responder);

    constexpr E2eMediaMarker marker{0x7a51f00dcafebeefULL, 42};
    auto started = initiator.BeginMediaProbe(
        std::string(kTrackId), "nonce-42", marker.probe_id, marker.sequence,
        {4'000'000, 4'010'000, 4'011'000});
    TEST_CHECK(started.accepted() && started.message);
    const auto announcement_bytes = EncodeE2eMessage(*started.message);
    TEST_CHECK(announcement_bytes);
    const auto announcement = DecodeE2eMessage(
        kE2eMeasurementTopic, *announcement_bytes);
    TEST_CHECK(announcement && responder.AcceptMediaProbe(*announcement).accepted());

    // A data-channel announcement is not media evidence and cannot emit ACK.
    TEST_CHECK(responder.BuildMediaAckAfterRenderSubmit(
        kTrackId, 77, 4'085'000, "qt_cpu_paint").status ==
        E2eActionStatus::DuplicateOrUnknown);

    auto observer = std::make_shared<MediaAckObserver>(
        responder, std::string(kTrackId));
    RenderFrameMetadata metadata;
    metadata.series_key = "remote_render/peer-a/TR_controlled_video";
    metadata.room_generation = 1;
    metadata.binding_epoch = 9;
    metadata.frame_token = 77;
    metadata.decoded_at = AtUs(4'080'000);
    metadata.observer = observer;

    auto frame = ControlledFrame();
    TEST_CHECK(EmbedE2eMediaMarker(frame, marker));
    auto owned = CopyOwned(frame, metadata);
    const auto decoded_marker = DecodeE2eMediaMarker(*owned);
    TEST_CHECK(decoded_marker);
    TEST_CHECK(responder.ObserveDecodedMediaMarker(
        kTrackId, *decoded_marker, 4'080'000,
        owned->render_metadata().frame_token).accepted());
    TEST_CHECK(!observer->acknowledgement);

    auto render_frame = VideoRenderFrame::FromI420(std::move(owned));
    TEST_CHECK(render_frame);
    render_frame->NotifyRendered("qt_cpu_paint", AtUs(4'085'000));
    TEST_CHECK(observer->accepted_acks == 1 && observer->acknowledgement);
    TEST_CHECK(observer->acknowledgement->track_id == kTrackId);
    TEST_CHECK(observer->acknowledgement->probe_id == marker.probe_id);

    const auto acknowledgement_bytes = EncodeE2eMessage(
        *observer->acknowledgement);
    TEST_CHECK(acknowledgement_bytes);
    const auto acknowledgement = DecodeE2eMessage(
        kE2eMeasurementTopic, *acknowledgement_bytes);
    TEST_CHECK(acknowledgement);
    const auto completed = initiator.AcceptMediaAck(
        *acknowledgement, 4'045'000);
    TEST_CHECK(completed.accepted() && completed.media_measurement);
    const auto& measurement = *completed.media_measurement;
    TEST_CHECK(measurement.ack_round_trip_us == 34'000);
    TEST_CHECK(measurement.publication_to_decode_us == 30'000);
    TEST_CHECK(measurement.capture_to_render_submit_us == 25'000);
    TEST_CHECK(measurement.clock_state == ClockCalibrationState::Valid);
    TEST_CHECK(measurement.uncertainty_us <= 2'001);
    TEST_CHECK(measurement.measurement_point == "qt_cpu_paint");
    TEST_CHECK(measurement.remote_frame_width == 640);
    TEST_CHECK(measurement.remote_frame_height == 360);

    render_frame->NotifyRendered("qt_cpu_paint", AtUs(4'086'000));
    TEST_CHECK(observer->submit_calls == 2 && observer->accepted_acks == 1);
    TEST_CHECK(observer->last_status == E2eActionStatus::DuplicateOrUnknown);
    TEST_CHECK(initiator.AcceptMediaAck(
        *acknowledgement, 4'046'000).status ==
        E2eActionStatus::DuplicateOrUnknown);
}

E2eMessage CompleteInbound(
        E2eMeasurementSession& responder,
        const E2eMessage& announcement,
        std::uint64_t render_token,
        std::int64_t decode_us,
        std::int64_t submit_us) {
    TEST_CHECK(responder.AcceptMediaProbe(announcement).accepted());
    E2eMediaMarker marker{
        announcement.probe_id,
        static_cast<std::uint32_t>(announcement.sequence)};
    TEST_CHECK(responder.ObserveDecodedMediaMarker(
        announcement.track_id, marker, decode_us, render_token).accepted());
    auto ack = responder.BuildMediaAckAfterRenderSubmit(
        announcement.track_id, render_token, submit_us, "gpu_present");
    TEST_CHECK(ack.accepted() && ack.message);
    return *ack.message;
}

void RejectsForgeryReplaysReplacementAndLateCallbacks() {
    auto initiator = MakeSession("peer-a", "peer-b", 2);
    auto responder = MakeSession("peer-b", "peer-a", 2);
    Negotiate(initiator, responder);

    auto first = initiator.BeginMediaProbe(
        "TR_first", "nonce-first", 101, 10,
        {1'000, 1'100, 1'200});
    auto second = initiator.BeginMediaProbe(
        "TR_second", "nonce-second", 202, 11,
        {2'000, 2'100, 2'200});
    TEST_CHECK(first.accepted() && first.message);
    TEST_CHECK(second.accepted() && second.message);
    TEST_CHECK(initiator.pending_media_probe_count() == 2);
    TEST_CHECK(initiator.BeginMediaProbe(
        "TR_third", "nonce-third", 303, 12,
        {3'000, 3'100, 3'200}).status ==
        E2eActionStatus::CapacityExceeded);

    auto forged_announcement = *first.message;
    forged_announcement.session_id = "old-session";
    TEST_CHECK(responder.AcceptMediaProbe(forged_announcement).status ==
        E2eActionStatus::InvalidContext);

    // Complete the second probe first; pending state is keyed, not FIFO.
    auto second_ack = CompleteInbound(
        responder, *second.message, 22, 2'500, 2'600);
    auto forged_ack = second_ack;
    forged_ack.track_id = "TR_forged";
    TEST_CHECK(initiator.AcceptMediaAck(forged_ack, 2'900).status ==
        E2eActionStatus::InvalidMessage);
    const auto second_result = initiator.AcceptMediaAck(second_ack, 2'900);
    TEST_CHECK(second_result.accepted() && second_result.media_measurement);
    TEST_CHECK(second_result.media_measurement->ack_round_trip_us == 700);
    TEST_CHECK(!second_result.media_measurement->publication_to_decode_us);
    TEST_CHECK(!second_result.media_measurement->capture_to_render_submit_us);
    TEST_CHECK(second_result.media_measurement->one_way_reason ==
        "insufficient_clock_samples");

    auto first_ack = CompleteInbound(
        responder, *first.message, 11, 1'500, 1'600);
    TEST_CHECK(initiator.AcceptMediaAck(first_ack, 1'900).accepted());
    TEST_CHECK(initiator.pending_media_probe_count() == 0);

    auto replacement = initiator.BeginMediaProbe(
        "TR_replaced", "nonce-replaced", 404, 12,
        {3'000, 3'100, 3'200});
    TEST_CHECK(replacement.accepted() && replacement.message);
    TEST_CHECK(responder.AcceptMediaProbe(*replacement.message).accepted());
    TEST_CHECK(responder.ObserveDecodedMediaMarker(
        "TR_replaced", {404, 12}, 3'500, 33).accepted());
    responder.CancelMediaTrack("TR_replaced");
    TEST_CHECK(responder.BuildMediaAckAfterRenderSubmit(
        "TR_replaced", 33, 3'600, "gpu_present").status ==
        E2eActionStatus::DuplicateOrUnknown);
    initiator.CancelMediaTrack("TR_replaced");
    TEST_CHECK(initiator.pending_media_probe_count() == 0);

    TEST_CHECK(responder.AcceptMediaProbe(*replacement.message).status ==
        E2eActionStatus::DuplicateOrUnknown);
    responder.Retire();
    TEST_CHECK(responder.ObserveDecodedMediaMarker(
        "TR_replaced", {404, 12}, 3'700, 34).status ==
        E2eActionStatus::Retired);
}

} // namespace

int main() {
    MarkerSurvivesControlledNoiseAndScaling();
    MediaWireCodecIsStrict();
    AckRequiresDecodedMarkerAndActualRenderSubmit();
    RejectsForgeryReplaysReplacementAndLateCallbacks();
    return 0;
}
