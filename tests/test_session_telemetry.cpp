#include "tests/support/test_check.h"

#include "src/telemetry/process_resource_sampler.h"
#include "src/telemetry/session_telemetry.h"

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using livekit::telemetry::Availability;
using livekit::telemetry::Event;
using livekit::telemetry::EventKind;
using livekit::telemetry::OperationKind;
using livekit::telemetry::OperationOutcome;
using livekit::telemetry::ProcessResourceSample;
using livekit::telemetry::ProcessResourceSampler;
using livekit::telemetry::SessionTelemetry;

Event Counter(std::uint64_t generation,
              std::uint64_t sequence,
              std::uint64_t epoch,
              std::int64_t value) {
    Event event;
    event.kind = EventKind::CounterSample;
    event.session_generation = generation;
    event.sequence = sequence;
    event.epoch = epoch;
    event.series_key = "pc/inbound/video/bytes";
    event.counter_value = value;
    return event;
}

Event Operation(EventKind kind,
                std::uint64_t generation,
                std::uint64_t sequence,
                const char *id,
                OperationKind operation_kind = OperationKind::PublishTrack,
                OperationOutcome outcome = OperationOutcome::Success,
                Event::Clock::time_point source_time = Event::Clock::time_point{}) {
    Event event;
    event.kind = kind;
    event.session_generation = generation;
    event.sequence = sequence;
    event.operation_id = id;
    event.operation_kind = operation_kind;
    event.operation_outcome = kind == EventKind::OperationTerminal
        ? outcome : OperationOutcome::None;
    event.source_time = source_time;
    return event;
}

livekit::RoomStatsReport AudioReport(
        livekit::InboundRtpStreamStats stream) {
    livekit::RoomStatsReport room;
    room.actual_peer_connection_count = 1;
    room.successful_peer_connection_count = 1;
    livekit::StatsReport report;
    report.inbound_rtp.push_back(std::move(stream));
    room.reports.push_back(std::move(report));
    return room;
}

livekit::InboundRtpStreamStats AudioStream(
        std::uint64_t total_samples,
        std::uint64_t concealed,
        std::uint64_t silent_concealed,
        std::uint64_t concealment_events,
        std::uint64_t inserted,
        std::uint64_t removed) {
    livekit::InboundRtpStreamStats stream;
    stream.id = "audio-inbound";
    stream.kind = "audio";
    stream.kind_available = true;
    stream.total_samples_received = total_samples;
    stream.concealed_samples = concealed;
    stream.silent_concealed_samples = silent_concealed;
    stream.concealment_events = concealment_events;
    stream.inserted_samples_for_deceleration = inserted;
    stream.removed_samples_for_acceleration = removed;
    stream.total_samples_received_available = true;
    stream.concealed_samples_available = true;
    stream.silent_concealed_samples_available = true;
    stream.concealment_events_available = true;
    stream.inserted_samples_for_deceleration_available = true;
    stream.removed_samples_for_acceleration_available = true;
    return stream;
}

void SetJitter(livekit::InboundRtpStreamStats& stream,
               double actual,
               double target,
               double minimum,
               std::uint64_t emitted) {
    stream.jitter_buffer_delay = actual;
    stream.jitter_buffer_target_delay = target;
    stream.jitter_buffer_minimum_delay = minimum;
    stream.jitter_buffer_emitted_count = emitted;
    stream.jitter_buffer_delay_available = true;
    stream.jitter_buffer_target_delay_available = true;
    stream.jitter_buffer_minimum_delay_available = true;
    stream.jitter_buffer_emitted_count_available = true;
}

void BoundedQueueAndStopBarrier() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 11, 2);
    std::vector<SessionTelemetry::SnapshotPtr> snapshots;
    asio::post(strand, [telemetry, &snapshots] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshots](SessionTelemetry::SnapshotPtr snapshot) {
                snapshots.push_back(std::move(snapshot));
            });
    });

    Event valid;
    valid.kind = EventKind::GaugeSample;
    valid.session_generation = 11;
    valid.availability = Availability::Valid;
    TEST_CHECK(telemetry->Submit(valid));
    TEST_CHECK(telemetry->Submit(valid));
    TEST_CHECK(!telemetry->Submit(valid));
    context.run();

    TEST_CHECK(!snapshots.empty());
    const auto drained = snapshots.back();
    TEST_CHECK(drained->queue_capacity == 2);
    TEST_CHECK(drained->queue_depth == 0);
    TEST_CHECK(drained->queue_high_water == 2);
    TEST_CHECK(drained->capacity_drops == 1);
    TEST_CHECK(drained->valid_samples == 2);
    TEST_CHECK(!drained->session_complete);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run();
    TEST_CHECK(!telemetry->Submit(valid));
    TEST_CHECK(snapshots.back()->session_complete);

    SessionTelemetry::SnapshotPtr stopped;
    context.restart();
    asio::post(strand, [telemetry, &stopped] {
        stopped = telemetry->SnapshotOnStrand();
    });
    context.run();
    TEST_CHECK(stopped->stopped_drops == 1);
    TEST_CHECK(stopped->session_complete);
}

void GenerationEpochAndOperationLedger() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 41, 32);
    std::vector<SessionTelemetry::SnapshotPtr> snapshots;
    asio::post(strand, [telemetry, &snapshots] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshots](SessionTelemetry::SnapshotPtr snapshot) {
                snapshots.push_back(std::move(snapshot));
            });
    });

    Event stale;
    stale.kind = EventKind::GaugeSample;
    stale.session_generation = 40;
    stale.availability = Availability::Valid;
    TEST_CHECK(telemetry->Submit(stale));
    TEST_CHECK(telemetry->Submit(Counter(41, 100, 7, 10)));
    TEST_CHECK(telemetry->Submit(Counter(41, 101, 7, 20)));
    TEST_CHECK(telemetry->Submit(Counter(41, 102, 8, 5)));
    TEST_CHECK(telemetry->Submit(Counter(41, 103, 8, 4)));
    const auto operation_start = Event::Clock::now();
    TEST_CHECK(telemetry->Submit(Operation(
        EventKind::OperationStarted, 41, 200, "publish-1",
        OperationKind::PublishTrack, OperationOutcome::None,
        operation_start)));
    TEST_CHECK(telemetry->Submit(Operation(
        EventKind::OperationTerminal, 41, 201, "publish-1",
        OperationKind::PublishTrack, OperationOutcome::Timeout,
        operation_start + 37ms)));
    TEST_CHECK(telemetry->Submit(Operation(
        EventKind::OperationTerminal, 41, 202, "publish-1")));
    TEST_CHECK(telemetry->Submit(Operation(
        EventKind::OperationTerminal, 41, 203, "missing")));
    context.run();

    TEST_CHECK(snapshots.size() >= 2);
    const auto initial = snapshots.front();
    const auto current = snapshots.back();
    TEST_CHECK(initial->revision < current->revision);
    TEST_CHECK(initial->stale_generation_drops == 0);
    TEST_CHECK(current->session_generation == 41);
    TEST_CHECK(current->stale_generation_drops == 1);
    TEST_CHECK(current->counter_resets == 2);
    TEST_CHECK(current->availability == Availability::Invalid);
    TEST_CHECK(current->operations_started == 1);
    TEST_CHECK(current->operations_terminal == 1);
    TEST_CHECK(current->operations_inflight == 0);
    TEST_CHECK(current->operations_missing_start == 1);
    TEST_CHECK(current->operations_duplicate_terminal == 1);
    TEST_CHECK(current->operations_kind_mismatch == 0);
    TEST_CHECK(current->operation_summaries.size() == 1);
    const auto &publish = current->operation_summaries.front();
    TEST_CHECK(publish.kind == OperationKind::PublishTrack);
    TEST_CHECK(publish.started == 1);
    TEST_CHECK(publish.terminal == 1);
    TEST_CHECK(publish.timeout == 1);
    TEST_CHECK(publish.success == 0);
    TEST_CHECK(publish.inflight == 0);
    TEST_CHECK(publish.last_duration_ms == 37);
    TEST_CHECK(initial->revision == 1); // Published snapshots remain immutable.
}

void TypedOperationRaceAndConvenienceApi() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 51, 32);
    SessionTelemetry::SnapshotPtr snapshot;
    const auto started_at = Event::Clock::now();
    const auto id = telemetry->StartOperation(
        OperationKind::ReconnectAttempt, "attempt", started_at);
    TEST_CHECK(!id.empty());
    TEST_CHECK(telemetry->FinishOperation(
        id, OperationKind::ReconnectAttempt, OperationOutcome::Success,
        started_at + 12ms));
    TEST_CHECK(telemetry->FinishOperation(
        id, OperationKind::ReconnectAttempt, OperationOutcome::Timeout,
        started_at + 20ms));
    TEST_CHECK(telemetry->FinishOperation(
        id, OperationKind::Connect, OperationOutcome::Failure,
        started_at + 25ms));
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    context.run();

    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->operations_started == 1);
    TEST_CHECK(snapshot->operations_terminal == 1);
    TEST_CHECK(snapshot->operations_duplicate_terminal == 2);
    TEST_CHECK(snapshot->operation_summaries.size() == 1);
    TEST_CHECK(snapshot->operation_summaries.front().success == 1);
    TEST_CHECK(snapshot->operation_summaries.front().timeout == 0);
    TEST_CHECK(snapshot->operation_summaries.front().last_duration_ms == 12);
}

void BatchAndMembersUseSeparateDenominators() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 52, 32);
    SessionTelemetry::SnapshotPtr snapshot;
    const auto batch = telemetry->StartOperation(OperationKind::PublishBatch);
    const auto audio = telemetry->StartOperation(OperationKind::PublishTrack);
    const auto video = telemetry->StartOperation(OperationKind::PublishTrack);
    TEST_CHECK(telemetry->FinishOperation(
        audio, OperationKind::PublishTrack, OperationOutcome::Success));
    TEST_CHECK(telemetry->FinishOperation(
        video, OperationKind::PublishTrack, OperationOutcome::Success));
    TEST_CHECK(telemetry->FinishOperation(
        batch, OperationKind::PublishBatch, OperationOutcome::Success));
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    context.run();

    const auto find = [&](OperationKind kind) {
        return std::find_if(
            snapshot->operation_summaries.begin(),
            snapshot->operation_summaries.end(),
            [kind](const auto& summary) { return summary.kind == kind; });
    };
    const auto batch_summary = find(OperationKind::PublishBatch);
    const auto member_summary = find(OperationKind::PublishTrack);
    TEST_CHECK(batch_summary != snapshot->operation_summaries.end());
    TEST_CHECK(member_summary != snapshot->operation_summaries.end());
    TEST_CHECK(batch_summary->started == 1 && batch_summary->success == 1);
    TEST_CHECK(member_summary->started == 2 && member_summary->success == 2);
    TEST_CHECK(snapshot->operations_started == 3);
    TEST_CHECK(snapshot->operations_terminal == 3);
}

void StatsSingleFlightAndCoverage() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 72, 32);
    std::vector<SessionTelemetry::SnapshotPtr> snapshots;
    int provider_calls = 0;
    asio::post(strand, [telemetry, &snapshots, &provider_calls] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshots](SessionTelemetry::SnapshotPtr snapshot) {
                snapshots.push_back(std::move(snapshot));
            });
        telemetry->StartStatsSamplingOnStrand(
            [&provider_calls](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                ++provider_calls;
                auto executor = co_await asio::this_coro::executor;
                asio::steady_timer timer(executor, 25ms);
                co_await timer.async_wait(asio::use_awaitable);
                livekit::RoomStatsReport report;
                report.actual_peer_connection_count = 1;
                report.successful_peer_connection_count = 1;
                co_return report;
            },
            1h);
        telemetry->RequestStatsSampleOnStrand();
    });

    context.run_for(100ms);
    TEST_CHECK(provider_calls == 1);
    TEST_CHECK(!snapshots.empty());
    const auto completed = snapshots.back();
    TEST_CHECK(completed->stats_requests_started == 1);
    TEST_CHECK(completed->stats_requests_completed == 1);
    TEST_CHECK(completed->stats_requests_skipped == 1);
    TEST_CHECK(!completed->stats_in_flight);
    TEST_CHECK(completed->availability == Availability::Valid);
    TEST_CHECK(completed->actual_pc_count == 1);
    TEST_CHECK(completed->successful_pc_count == 1);
    TEST_CHECK(completed->coverage == 1.0);

    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(100ms);
}

void StatsCompletionReturnsToOwningStrand() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 74, 16);
    bool provider_completed_off_strand = false;
    bool completion_published_on_strand = false;

    asio::post(strand, [telemetry, &context, strand,
                        &provider_completed_off_strand,
                        &completion_published_on_strand] {
        telemetry->SetSnapshotCallbackOnStrand(
            [strand, &completion_published_on_strand](
                SessionTelemetry::SnapshotPtr snapshot) {
                if (snapshot->stats_requests_completed == 1) {
                    completion_published_on_strand =
                        strand.running_in_this_thread();
                }
            });
        telemetry->StartStatsSamplingOnStrand(
            [executor = context.get_executor(), strand,
             &provider_completed_off_strand](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                co_return co_await asio::async_initiate<
                    decltype(asio::use_awaitable),
                    void(livekit::RoomStatsReport)>(
                    [executor, strand, &provider_completed_off_strand](
                        auto handler) mutable {
                        using Handler = decltype(handler);
                        auto handler_ptr =
                            std::make_shared<Handler>(std::move(handler));
                        asio::post(executor,
                            [handler_ptr, strand,
                             &provider_completed_off_strand]() mutable {
                                provider_completed_off_strand =
                                    !strand.running_in_this_thread();
                                livekit::RoomStatsReport report;
                                report.actual_peer_connection_count = 1;
                                report.successful_peer_connection_count = 1;
                                (*handler_ptr)(std::move(report));
                            });
                    },
                    asio::use_awaitable);
            },
            1h);
    });

    context.run_for(100ms);
    TEST_CHECK(provider_completed_off_strand);
    TEST_CHECK(completion_published_on_strand);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(100ms);
}

void StopWaitsForTheOnlyInFlightSample() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 73, 16);
    bool stopped = false;
    asio::post(strand, [telemetry, &stopped] {
        telemetry->StartStatsSamplingOnStrand(
            [](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                auto executor = co_await asio::this_coro::executor;
                asio::steady_timer timer(executor, 25ms);
                co_await timer.async_wait(asio::use_awaitable);
                co_return livekit::RoomStatsReport{};
            },
            1h);
        telemetry->StopOnStrand([&stopped] { stopped = true; });
        TEST_CHECK(!stopped);
    });
    context.run_for(100ms);
    TEST_CHECK(stopped);
}

const livekit::telemetry::OperationSummary *FindOperation(
    const livekit::telemetry::Snapshot &snapshot,
    OperationKind kind) {
    const auto found = std::find_if(
        snapshot.operation_summaries.begin(), snapshot.operation_summaries.end(),
        [kind](const auto &summary) { return summary.kind == kind; });
    return found == snapshot.operation_summaries.end() ? nullptr : &*found;
}

void FirstDecodedFrameUsesGenerationAndBindingEpoch() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 81, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto first_probe = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    TEST_CHECK(telemetry->RecordRoomConnectAccepted(7, base));
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/participant/track", 7, 11, base + 10ms,
        true, true, first_probe, base + 20ms));
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/participant/track", 7, 11, 0, false,
        1280, 720, base + 40ms));

    auto second_probe = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/participant/track", 7, 12, base + 100ms,
        true, true, second_probe, base + 110ms));
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/participant/track", 7, 11, 0, false,
        640, 360, base + 120ms));
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/participant/track", 7, 12, 0, false,
        1920, 1080, base + 150ms));
    context.run();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->remote_video_first_frames == 2);
    TEST_CHECK(snapshot->stale_binding_frame_drops == 1);
    TEST_CHECK(snapshot->room_connect_to_first_decoded_ms == 40);
    TEST_CHECK(snapshot->last_connect_to_first_decoded_ms == 150);
    TEST_CHECK(snapshot->last_subscribe_to_first_decoded_ms == 50);
    TEST_CHECK(snapshot->last_decoded_width == 1920);
    TEST_CHECK(snapshot->last_decoded_height == 1080);
    TEST_CHECK(snapshot->remote_video_first_frame_availability ==
               Availability::Valid);
}

void ReconnectWaitsForStableExpectedVideo() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 91, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto probe = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    probe->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        (base + 900ms).time_since_epoch()).count());
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/p/t", 5, 3, base, true, true, probe, base));
    const auto episode = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect", base + 1000ms);
    const auto recovery_epoch = telemetry->ActiveRecoveryEpoch();
    TEST_CHECK(recovery_epoch != 0);
    probe->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        (base + 1200ms).time_since_epoch()).count());
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/p/t", 5, 3, recovery_epoch, false,
        1280, 720, base + 1200ms, base + 900ms));
    TEST_CHECK(telemetry->FinishOperation(
        episode, OperationKind::ReconnectEpisode,
        OperationOutcome::Success, base + 1300ms));
    context.run();
    TEST_CHECK(snapshot);
    const auto *pending = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(pending && pending->terminal == 0 && pending->inflight == 1);
    TEST_CHECK(snapshot->reconnect_video_recovered == 0);
    TEST_CHECK(snapshot->reconnect_video_reason ==
               "signaling_restored_waiting_for_video");

    context.restart();
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/p/t", 5, 3, recovery_epoch, true,
        1280, 720, base + 1450ms));
    context.run();
    const auto *recovered = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(recovered && recovered->terminal == 1 && recovered->success == 1);
    TEST_CHECK(snapshot->reconnect_video_availability == Availability::Valid);
    TEST_CHECK(snapshot->reconnect_video_expected == 1);
    TEST_CHECK(snapshot->reconnect_video_recovered == 1);
    TEST_CHECK(snapshot->last_reconnect_signaling_ms == 300);
    TEST_CHECK(snapshot->last_reconnect_first_video_ms == 200);
    TEST_CHECK(snapshot->last_reconnect_stable_video_ms == 450);
    TEST_CHECK(snapshot->last_reconnect_media_interruption_ms == 550);
    TEST_CHECK(telemetry->ActiveRecoveryEpoch() == 0);
}

void ReconnectTimeoutAndExpectationChangeAreNotMediaSuccess() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 92, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto probe = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/p/t", 6, 9, base, true, true, probe, base));
    const auto timed_out = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect", base + 100ms);
    TEST_CHECK(telemetry->FinishOperation(
        timed_out, OperationKind::ReconnectEpisode,
        OperationOutcome::Success, base + 200ms));
    asio::post(strand, [telemetry, base] {
        telemetry->RefreshOnStrand(
            base + 200ms + SessionTelemetry::kRecoveryObservationWindow + 1ms);
    });
    context.run();
    TEST_CHECK(snapshot);
    auto *summary = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(summary && summary->terminal == 1 && summary->timeout == 1);
    TEST_CHECK(summary->success == 0);
    TEST_CHECK(snapshot->reconnect_video_availability == Availability::Timeout);
    TEST_CHECK(snapshot->reconnect_media_timeouts == 1);

    context.restart();
    const auto changed = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect", base + 20s);
    TEST_CHECK(telemetry->FinishOperation(
        changed, OperationKind::ReconnectEpisode,
        OperationOutcome::Success, base + 20100ms));
    TEST_CHECK(telemetry->SetRemoteVideoExpected(
        "remote_video/p/t", false,
        livekit::telemetry::MediaExpectationReason::PublicationEnded,
        base + 20200ms));
    context.run();
    summary = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(summary && summary->terminal == 2);
    TEST_CHECK(summary->success == 0 && summary->degraded_success == 1);
    TEST_CHECK(summary->timeout == 1 && summary->inflight == 0);
    TEST_CHECK(snapshot->reconnect_video_recovered == 0);
    TEST_CHECK(snapshot->reconnect_expectation_changes == 1);
    TEST_CHECK(snapshot->reconnect_video_reason ==
               "expectation_changed_during_recovery");
}

void NativeFreezeStatsPreserveMissingAndMeasuredZero() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 101, 32);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartStatsSamplingOnStrand(
            [](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                livekit::RoomStatsReport room;
                room.actual_peer_connection_count = 1;
                room.successful_peer_connection_count = 1;
                livekit::StatsReport report;
                livekit::InboundRtpStreamStats missing;
                missing.kind = "video";
                missing.kind_available = true;
                report.inbound_rtp.push_back(missing);
                livekit::InboundRtpStreamStats measured;
                measured.kind = "video";
                measured.kind_available = true;
                measured.freeze_count_available = true;
                measured.total_freezes_duration_available = true;
                measured.freeze_count = 0;
                measured.total_freezes_duration = 0.0;
                report.inbound_rtp.push_back(measured);
                room.reports.push_back(std::move(report));
                co_return room;
            }, 1h);
    });
    context.run_for(100ms);
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->native_video_freeze_availability == Availability::Valid);
    TEST_CHECK(snapshot->native_video_freeze_reason ==
               "native_freeze_partial_coverage");
    TEST_CHECK(snapshot->native_video_streams == 2);
    TEST_CHECK(snapshot->native_video_freeze_count == 0);
    TEST_CHECK(snapshot->native_video_freeze_duration_ms == 0);
}

void AudioStatsPreserveAvailabilityDeltasResetsAndStaleness() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 111, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    std::deque<livekit::RoomStatsReport> reports;

    reports.push_back(AudioReport(AudioStream(1000, 0, 0, 0, 0, 0)));
    asio::post(strand, [telemetry, &snapshot, &reports] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartStatsSamplingOnStrand(
            [&reports](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                TEST_CHECK(!reports.empty());
                auto report = std::move(reports.front());
                reports.pop_front();
                co_return report;
            }, 1h);
    });
    context.run_for(100ms);
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::WarmingUp);

    const auto request = [&](livekit::RoomStatsReport report) {
        reports.push_back(std::move(report));
        context.restart();
        asio::post(strand, [telemetry] { telemetry->RequestStatsSampleOnStrand(); });
        context.run_for(100ms);
        TEST_CHECK(snapshot);
    };

    request(AudioReport(AudioStream(2000, 0, 0, 0, 0, 0)));
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_concealed_ratio == 0.0);
    TEST_CHECK(snapshot->audio_non_silent_concealed_ratio == 0.0);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->audio_jitter_buffer_delay_ms < 0.0);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_inserted_ratio == 0.0);
    TEST_CHECK(snapshot->audio_removed_ratio == 0.0);

    auto third = AudioStream(3000, 100, 40, 2, 10, 5);
    SetJitter(third, 1.0, 1.2, 0.5, 100);
    request(AudioReport(std::move(third)));
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::WarmingUp);

    auto fourth = AudioStream(4000, 200, 80, 4, 20, 10);
    SetJitter(fourth, 1.05, 1.26, 0.52, 102);
    request(AudioReport(std::move(fourth)));
    TEST_CHECK(snapshot->audio_quality_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_quality_reason == "audio_quality_window_valid");
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_window_samples == 1000);
    TEST_CHECK(snapshot->audio_window_concealed_samples == 100);
    TEST_CHECK(snapshot->audio_window_silent_concealed_samples == 40);
    TEST_CHECK(snapshot->audio_window_concealment_events == 2);
    TEST_CHECK(std::abs(snapshot->audio_concealed_ratio - 0.1) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_non_silent_concealed_ratio - 0.06) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_jitter_buffer_delay_ms - 25.0) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_jitter_buffer_target_delay_ms - 30.0) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_jitter_buffer_minimum_delay_ms - 10.0) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_inserted_ratio - 0.01) < 1e-9);
    TEST_CHECK(std::abs(snapshot->audio_removed_ratio - 0.005) < 1e-9);

    SessionTelemetry::SnapshotPtr stale;
    context.restart();
    asio::post(strand, [telemetry, &stale] {
        stale = telemetry->SnapshotOnStrand(SessionTelemetry::Clock::now() + 4h);
    });
    context.run_for(100ms);
    TEST_CHECK(stale);
    TEST_CHECK(stale->audio_quality_availability == Availability::Stale);
    TEST_CHECK(stale->audio_concealment_availability == Availability::Stale);
    TEST_CHECK(stale->audio_jitter_buffer_availability == Availability::Stale);
    TEST_CHECK(stale->audio_time_stretch_availability == Availability::Stale);

    auto reset = AudioStream(50, 1, 0, 1, 1, 1);
    SetJitter(reset, 0.01, 0.02, 0.005, 1);
    request(AudioReport(std::move(reset)));
    TEST_CHECK(snapshot->counter_resets == 1);
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->audio_concealed_ratio < 0.0);
    TEST_CHECK(snapshot->audio_jitter_buffer_delay_ms < 0.0);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(100ms);
}

void AudioQoeFollowsExpectedMediaState() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 112, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    std::deque<livekit::RoomStatsReport> reports;
    const auto make_report = [](std::uint64_t samples,
                                std::uint64_t concealed,
                                std::uint64_t emitted) {
        auto stream = AudioStream(samples, concealed, 0, concealed > 0 ? 1 : 0,
            samples / 100, samples / 200);
        SetJitter(stream, emitted * 0.02, emitted * 0.03,
            emitted * 0.01, emitted);
        return AudioReport(std::move(stream));
    };
    reports.push_back(make_report(1000, 10, 100));
    auto probe = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    const auto base = Event::Clock::now();
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/t", 1, 1, base, true, probe, base));
    asio::post(strand, [telemetry, &snapshot, &reports] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartStatsSamplingOnStrand(
            [&reports](SessionTelemetry::LateCompletion)
                -> asio::awaitable<livekit::RoomStatsReport> {
                TEST_CHECK(!reports.empty());
                auto report = std::move(reports.front());
                reports.pop_front();
                co_return report;
            }, 1h);
    });
    context.run_for(100ms);
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->audio_quality_availability == Availability::WarmingUp);

    const auto request = [&](livekit::RoomStatsReport report) {
        reports.push_back(std::move(report));
        context.restart();
        asio::post(strand, [telemetry] { telemetry->RequestStatsSampleOnStrand(); });
        context.run_for(100ms);
        TEST_CHECK(snapshot);
    };
    request(make_report(2000, 20, 200));
    TEST_CHECK(snapshot->audio_quality_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::Valid);

    context.restart();
    TEST_CHECK(telemetry->SetRemoteAudioExpected(
        "remote_audio/p/t", false,
        livekit::telemetry::MediaExpectationReason::Muted, base + 1s));
    context.run_for(100ms);
    TEST_CHECK(snapshot->remote_audio_first_frame_availability ==
        Availability::NotExpected);
    TEST_CHECK(snapshot->audio_quality_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->audio_jitter_buffer_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->audio_time_stretch_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->audio_concealed_ratio < 0.0);
    TEST_CHECK(snapshot->audio_jitter_buffer_delay_ms < 0.0);
    TEST_CHECK(snapshot->audio_inserted_ratio < 0.0);
    TEST_CHECK(snapshot->audio_window_concealment_events == 0);

    request(make_report(3000, 30, 300));
    TEST_CHECK(snapshot->audio_quality_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->audio_concealed_ratio < 0.0);

    context.restart();
    TEST_CHECK(telemetry->SetRemoteAudioExpected(
        "remote_audio/p/t", true,
        livekit::telemetry::MediaExpectationReason::Unmuted, base + 2s));
    context.run_for(100ms);
    TEST_CHECK(snapshot->audio_quality_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->audio_concealed_ratio < 0.0);

    request(make_report(4000, 40, 400));
    TEST_CHECK(snapshot->audio_quality_availability == Availability::WarmingUp);
    request(make_report(5000, 50, 500));
    TEST_CHECK(snapshot->audio_quality_availability == Availability::Valid);
    TEST_CHECK(snapshot->audio_concealment_availability == Availability::Valid);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(100ms);
}

void AudioFirstFrameRejectsLateBindingCallbacks() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 121, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto first = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    auto second = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/t", 7, 1, base, true, first, base));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 7, 1, 0, false, 48000, 2, base + 10ms));
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/t", 7, 2, base + 20ms, true, second, base + 20ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 7, 1, 0, false, 16000, 1, base + 30ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 8, 2, 0, false, 32000, 1, base + 35ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 7, 2, 0, false, 48000, 2, base + 40ms));
    context.run();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->remote_audio_first_frames == 2);
    TEST_CHECK(snapshot->stale_audio_binding_drops == 2);
    TEST_CHECK(snapshot->last_subscribe_to_first_pcm_ms == 20);
    TEST_CHECK(snapshot->last_audio_sample_rate == 48000);
    TEST_CHECK(snapshot->last_audio_channels == 2);
}

void ReconnectWaitsForAudioAndVisibleRender() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 131, 128);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto audio = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    auto render = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    audio->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        (base + 900ms).time_since_epoch()).count());
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/t", 5, 3, base, true, audio, base));
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        "remote_render/p/t", 5, 3, base, true, true, 33ms, render, base));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 5, 3, 1, base + 890ms,
        "qt_cpu_paint", render, base + 900ms));
    context.run();

    context.restart();
    const auto episode = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect", base + 1000ms);
    const auto epoch = telemetry->ActiveRecoveryEpoch();
    TEST_CHECK(epoch != 0);
    TEST_CHECK(telemetry->FinishOperation(
        episode, OperationKind::ReconnectEpisode,
        OperationOutcome::Success, base + 1100ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 5, 3, epoch, false, 48000, 2,
        base + 1200ms, base + 900ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 5, 3, 2, base + 1240ms,
        "qt_cpu_paint", render, base + 1250ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/t", 5, 3, epoch, true, 48000, 2,
        base + 1500ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 5, 3, 3, base + 1590ms,
        "qt_cpu_paint", render, base + 1600ms));
    context.run();

    TEST_CHECK(snapshot);
    const auto *summary = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(summary && summary->success == 1 && summary->inflight == 0);
    TEST_CHECK(snapshot->reconnect_video_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->last_reconnect_stable_video_ms == -1);
    TEST_CHECK(snapshot->reconnect_audio_availability == Availability::Valid);
    TEST_CHECK(snapshot->last_reconnect_first_audio_ms == 200);
    TEST_CHECK(snapshot->last_reconnect_stable_audio_ms == 500);
    TEST_CHECK(snapshot->last_reconnect_audio_interruption_ms == 600);
    TEST_CHECK(snapshot->reconnect_render_availability == Availability::Valid);
    TEST_CHECK(snapshot->last_reconnect_first_render_ms == 250);
    TEST_CHECK(snapshot->last_reconnect_stable_render_ms == 600);
    TEST_CHECK(snapshot->last_reconnect_render_interruption_ms == 700);
}

void ReconnectTimeoutPreservesRecoveredMedia() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 132, 128);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto audio_a = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    auto audio_b = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    auto render = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    for (const auto& audio : {audio_a, audio_b}) {
        audio->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            (base + 900ms).time_since_epoch()).count());
    }
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/a", 6, 1, base, true, audio_a, base));
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/p/b", 6, 2, base, true, audio_b, base));
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        "remote_render/p/t", 6, 3, base, true, true, 33ms, render, base));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 6, 3, 1, base + 890ms,
        "qt_cpu_paint", render, base + 900ms));
    context.run();

    context.restart();
    const auto episode = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect", base + 1000ms);
    const auto epoch = telemetry->ActiveRecoveryEpoch();
    TEST_CHECK(telemetry->FinishOperation(
        episode, OperationKind::ReconnectEpisode,
        OperationOutcome::Success, base + 1100ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/a", 6, 1, epoch, false, 48000, 2,
        base + 1200ms, base + 900ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/p/a", 6, 1, epoch, true, 48000, 2,
        base + 1500ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 6, 3, 2, base + 1240ms,
        "qt_cpu_paint", render, base + 1250ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 6, 3, 3, base + 1590ms,
        "qt_cpu_paint", render, base + 1600ms));
    context.run();

    context.restart();
    asio::post(strand, [telemetry, base] {
        telemetry->RefreshOnStrand(
            base + 1100ms + SessionTelemetry::kRecoveryObservationWindow + 1ms);
    });
    context.run();
    TEST_CHECK(snapshot);
    const auto *summary = FindOperation(*snapshot, OperationKind::ReconnectEpisode);
    TEST_CHECK(summary && summary->timeout == 1 && summary->inflight == 0);
    TEST_CHECK(snapshot->reconnect_audio_availability == Availability::Timeout);
    TEST_CHECK(snapshot->reconnect_audio_recovered == 1);
    TEST_CHECK(snapshot->reconnect_audio_expected == 2);
    TEST_CHECK(snapshot->reconnect_render_availability == Availability::Valid);
    TEST_CHECK(snapshot->last_reconnect_stable_render_ms == 600);
    TEST_CHECK(snapshot->last_reconnect_render_interruption_ms == 700);
}

void RenderSubmitDedupesAndExcludesHiddenIntervals() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 141, 128);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });
    const auto base = Event::Clock::now();
    auto probe = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        "remote_render/p/t", 7, 1, base, true, true, 33ms, probe, base));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 7, 1, 1, base + 90ms,
        "qt_cpu_paint", probe, base + 100ms));
    TEST_CHECK(!telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 7, 1, 1, base + 190ms,
        "qt_cpu_paint", probe, base + 200ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 7, 1, 2, base + 790ms,
        "qt_cpu_paint", probe, base + 800ms));
    context.run();
    context.restart();
    asio::post(strand, [telemetry, base] {
        telemetry->RefreshOnStrand(base + 1400ms);
    });
    context.run();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->unique_render_submits == 2);
    TEST_CHECK(snapshot->render_first_frame_availability == Availability::Valid);
    TEST_CHECK(snapshot->render_first_frame_measurement_point == "qt_cpu_paint");
    TEST_CHECK(snapshot->render_stall_count == 2);
    TEST_CHECK(snapshot->render_stall_duration_ms == 300);
    TEST_CHECK(snapshot->render_longest_stall_ms == 200);
    TEST_CHECK(snapshot->render_stall_active);

    context.restart();
    TEST_CHECK(telemetry->SetRemoteVideoRenderExpected(
        "remote_render/p/t", 7, 1, probe, false,
        livekit::telemetry::MediaExpectationReason::SurfaceHidden,
        base + 1450ms));
    context.run();
    TEST_CHECK(snapshot->render_stall_count == 1);
    TEST_CHECK(snapshot->render_stall_duration_ms == 200);
    TEST_CHECK(!snapshot->render_stall_active);
    TEST_CHECK(snapshot->render_expected_duration_ms == 1450);

    context.restart();
    TEST_CHECK(telemetry->SetRemoteVideoRenderExpected(
        "remote_render/p/t", 7, 1, probe, true,
        livekit::telemetry::MediaExpectationReason::SurfaceVisible,
        base + 3000ms));
    context.run();
    context.restart();
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 7, 1, 3, base + 3090ms,
        "qt_cpu_paint", probe, base + 3100ms));
    context.run();
    context.restart();
    asio::post(strand, [telemetry, base] {
        telemetry->RefreshOnStrand(base + 3200ms);
    });
    context.run();
    TEST_CHECK(snapshot->render_stall_count == 1);
    TEST_CHECK(snapshot->render_stall_duration_ms == 200);
    TEST_CHECK(snapshot->render_expected_duration_ms == 1650);

    auto replacement = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    context.restart();
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        "remote_render/p/t", 7, 2, base + 3300ms, true, true,
        33ms, replacement, base + 3300ms));
    context.run();
    context.restart();
    TEST_CHECK(telemetry->SetRemoteVideoRenderExpected(
        "remote_render/p/t", 7, 1, probe, false,
        livekit::telemetry::MediaExpectationReason::RenderBindingEnded,
        base + 3310ms));
    TEST_CHECK(!telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/p/t", 7, 1, 4, base + 3310ms,
        "qt_cpu_paint", probe, base + 3320ms));
    context.run();
    TEST_CHECK(snapshot->stale_render_binding_drops == 1);
}

void ProcessResourceSamplerUsesNormalizedCpuAndTypedAvailability() {
    const auto cpu = ProcessResourceSampler::ComputeNormalizedCpuPercent(
        2'000'000, 1s, 4);
    TEST_CHECK(cpu.has_value());
    TEST_CHECK(std::abs(*cpu - 5.0) < 1e-9);
    TEST_CHECK(!ProcessResourceSampler::ComputeNormalizedCpuPercent(
        1, 0ns, 4).has_value());
    TEST_CHECK(!ProcessResourceSampler::ComputeNormalizedCpuPercent(
        1, 1s, 0).has_value());

    ProcessResourceSampler sampler;
    const auto first = sampler.Sample();
#if defined(_WIN32)
    TEST_CHECK(first.availability == Availability::Valid);
    TEST_CHECK(first.cpu_availability == Availability::WarmingUp);
    TEST_CHECK(first.memory_availability == Availability::Valid);
    TEST_CHECK(first.working_set_bytes > 0);
    TEST_CHECK(first.private_bytes > 0);
    TEST_CHECK(first.thread_count_availability == Availability::Valid);
    TEST_CHECK(first.thread_count > 0);
    TEST_CHECK(first.handle_count_availability == Availability::Valid);
    TEST_CHECK(first.handle_count > 0);
    TEST_CHECK(first.gpu_availability == Availability::Unsupported);
    std::this_thread::sleep_for(2ms);
    const auto second = sampler.Sample();
    TEST_CHECK(second.cpu_availability == Availability::Valid);
    TEST_CHECK(second.cpu_percent >= 0.0 && second.cpu_percent <= 100.0);
#else
    TEST_CHECK(first.availability == Availability::Unsupported);
#endif
}

void RuntimeSamplingMeasuresLagAndRejectsLateUiProbes() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 151, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    std::uint64_t dispatched_probe = 0;
    SessionTelemetry::Clock::time_point dispatched_at{};
    const auto resource_provider = [] {
        ProcessResourceSample sample;
        sample.captured_at = SessionTelemetry::Clock::now();
        sample.availability = Availability::Valid;
        sample.reason = "fixture_valid";
        sample.cpu_availability = Availability::Valid;
        sample.cpu_reason = "fixture_valid";
        sample.cpu_percent = 12.5;
        sample.logical_processor_count = 8;
        sample.memory_availability = Availability::Valid;
        sample.memory_reason = "fixture_valid";
        sample.working_set_bytes = 64 * 1024 * 1024;
        sample.peak_working_set_bytes = 96 * 1024 * 1024;
        sample.private_bytes = 48 * 1024 * 1024;
        sample.thread_count_availability = Availability::Valid;
        sample.thread_count_reason = "fixture_valid";
        sample.thread_count = 17;
        sample.thread_count_age_ms = 0;
        sample.handle_count_availability = Availability::Valid;
        sample.handle_count_reason = "fixture_valid";
        sample.handle_count = 81;
        sample.gpu_availability = Availability::Unsupported;
        sample.gpu_reason = "fixture_gpu_unsupported";
        sample.capture_duration_us = 125;
        return sample;
    };
    asio::post(strand, [telemetry, &snapshot, &dispatched_probe,
                        &dispatched_at, resource_provider] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartRuntimeSamplingOnStrand(
            resource_provider,
            [&dispatched_probe, &dispatched_at](
                std::uint64_t generation,
                std::uint64_t probe_id,
                SessionTelemetry::Clock::time_point source_time) {
                TEST_CHECK(generation == 151);
                dispatched_probe = probe_id;
                dispatched_at = source_time;
            },
            1h);
    });
    context.run_for(50ms);
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->resource_availability == Availability::Valid);
    TEST_CHECK(snapshot->resource_samples == 1);
    TEST_CHECK(snapshot->process_cpu_percent == 12.5);
    TEST_CHECK(snapshot->working_set_bytes == 64 * 1024 * 1024);
    TEST_CHECK(snapshot->process_thread_count == 17);
    TEST_CHECK(snapshot->process_handle_count == 81);
    TEST_CHECK(snapshot->gpu_resource_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->strand_lag_availability == Availability::Valid);
    TEST_CHECK(snapshot->ui_lag_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->ui_probe_in_flight);
    TEST_CHECK(dispatched_probe != 0);

    const auto first_probe = dispatched_probe;
    TEST_CHECK(telemetry->CompleteUiLagProbe(
        151, first_probe, dispatched_at + 25ms));
    context.restart();
    context.run_for(50ms);
    TEST_CHECK(snapshot->ui_lag_availability == Availability::Valid);
    TEST_CHECK(snapshot->last_ui_lag_ms == 25);
    TEST_CHECK(snapshot->maximum_ui_lag_ms == 25);
    TEST_CHECK(!snapshot->ui_probe_in_flight);

    Event queued;
    queued.kind = EventKind::GaugeSample;
    queued.session_generation = 151;
    queued.availability = Availability::Valid;
    queued.enqueued_at = Event::Clock::now() - 40ms;
    TEST_CHECK(telemetry->Submit(std::move(queued)));
    context.restart();
    context.run_for(50ms);
    TEST_CHECK(snapshot->event_queue_lag_availability == Availability::Valid);
    TEST_CHECK(snapshot->maximum_event_queue_lag_ms >= 40);

    context.restart();
    asio::post(strand, [telemetry] {
        const auto observed = SessionTelemetry::Clock::now();
        telemetry->RequestRuntimeSampleOnStrand(observed - 55ms, observed);
    });
    context.run_for(50ms);
    TEST_CHECK(snapshot->last_strand_lag_ms == 55);
    TEST_CHECK(snapshot->maximum_strand_lag_ms >= 55);
    const auto second_probe = dispatched_probe;
    const auto second_dispatched_at = dispatched_at;
    TEST_CHECK(second_probe != first_probe);

    context.restart();
    asio::post(strand, [telemetry] {
        const auto now = SessionTelemetry::Clock::now();
        telemetry->RequestRuntimeSampleOnStrand(now, now);
    });
    context.run_for(50ms);
    TEST_CHECK(snapshot->ui_probe_skipped == 1);
    TEST_CHECK(dispatched_probe == second_probe);

    TEST_CHECK(telemetry->CompleteUiLagProbe(
        151, first_probe, second_dispatched_at + 10ms));
    TEST_CHECK(telemetry->CompleteUiLagProbe(
        151, second_probe, second_dispatched_at + 30ms));
    context.restart();
    context.run_for(50ms);
    TEST_CHECK(snapshot->ui_probe_late_callbacks == 1);
    TEST_CHECK(snapshot->ui_lag_samples == 2);
    TEST_CHECK(snapshot->last_ui_lag_ms == 30);

    context.restart();
    asio::post(strand, [telemetry] {
        const auto first = SessionTelemetry::Clock::now();
        telemetry->RequestRuntimeSampleOnStrand(first, first);
        telemetry->RequestRuntimeSampleOnStrand(
            first + 4h, first + 4h);
    });
    context.run_for(50ms);
    TEST_CHECK(snapshot->ui_probe_timeouts == 1);
    TEST_CHECK(snapshot->ui_lag_availability == Availability::Timeout);
    TEST_CHECK(snapshot->ui_probe_in_flight);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(50ms);
}

void ResourceTrendIsBoundedAndExitReturnStaysHonest() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 161, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    const auto base = SessionTelemetry::Clock::now();
    std::uint64_t sample_index = 0;
    const auto mib = std::uint64_t{1024} * 1024;
    asio::post(strand, [telemetry, &snapshot, &sample_index, base, mib] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartRuntimeSamplingOnStrand(
            [&sample_index, base, mib] {
                ProcessResourceSample sample;
                const auto index = sample_index++;
                sample.captured_at = base + std::chrono::seconds(index);
                sample.availability = Availability::Valid;
                sample.reason = "fixture_valid";
                sample.cpu_availability = Availability::Valid;
                sample.cpu_reason = "fixture_valid";
                sample.cpu_percent = 5.0;
                sample.memory_availability = Availability::Valid;
                sample.memory_reason = "fixture_valid";
                sample.working_set_bytes = 100 * mib + index * 2 * mib;
                sample.peak_working_set_bytes = sample.working_set_bytes;
                sample.private_bytes = 80 * mib + index * mib;
                sample.thread_count_availability = Availability::Valid;
                sample.thread_count_reason = "fixture_valid";
                sample.thread_count = static_cast<std::uint32_t>(20 + index);
                sample.handle_count_availability = Availability::Valid;
                sample.handle_count_reason = "fixture_valid";
                sample.handle_count = static_cast<std::uint32_t>(100 + index);
                sample.gpu_availability = Availability::Unsupported;
                sample.gpu_reason = "fixture_gpu_unsupported";
                sample.capture_duration_us = 100 +
                    static_cast<std::int64_t>(index % 7);
                return sample;
            }, {}, 1s);
    });
    context.poll();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->resource_trend_availability == Availability::WarmingUp);

    context.restart();
    asio::post(strand, [telemetry, base] {
        for (std::uint64_t index = 1; index <= 300; ++index) {
            telemetry->RequestRuntimeSampleOnStrand(
                base + std::chrono::seconds(index),
                base + std::chrono::seconds(index));
        }
    });
    context.poll();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->resource_trend_availability == Availability::Valid);
    TEST_CHECK(snapshot->resource_trend_samples ==
               SessionTelemetry::kResourceTrendCapacity);
    TEST_CHECK(snapshot->resource_trend_span_ms == 299000);
    TEST_CHECK(std::abs(snapshot->resource_trend_coverage - 1.0) < 1e-9);
    TEST_CHECK(snapshot->minimum_private_bytes == 81 * mib);
    TEST_CHECK(snapshot->maximum_private_bytes == 380 * mib);
    TEST_CHECK(snapshot->minimum_working_set_bytes == 102 * mib);
    TEST_CHECK(snapshot->maximum_working_set_bytes == 700 * mib);
    TEST_CHECK(std::abs(snapshot->private_bytes_growth_mib_per_minute - 60.0) < 1e-6);
    TEST_CHECK(std::abs(snapshot->thread_growth_per_hour - 3600.0) < 1e-6);
    TEST_CHECK(std::abs(snapshot->handle_growth_per_hour - 3600.0) < 1e-6);
    TEST_CHECK(snapshot->resource_session_delta_availability == Availability::Valid);
    TEST_CHECK(snapshot->private_bytes_delta == 300 * static_cast<std::int64_t>(mib));
    TEST_CHECK(snapshot->working_set_delta_bytes == 600 * static_cast<std::int64_t>(mib));
    TEST_CHECK(snapshot->telemetry_cost_availability == Availability::Valid);
    TEST_CHECK(snapshot->telemetry_observed_cost_ratio >= 0.0);
    TEST_CHECK(snapshot->telemetry_ab_availability == Availability::NotExpected);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.poll();
    TEST_CHECK(snapshot->resource_final_delta_availability == Availability::Valid);
    TEST_CHECK(snapshot->resource_return_availability == Availability::Unknown);
    TEST_CHECK(snapshot->resource_return_reason ==
               "post_stop_stable_window_not_observed");
}

} // namespace

int main() {
    BoundedQueueAndStopBarrier();
    GenerationEpochAndOperationLedger();
    TypedOperationRaceAndConvenienceApi();
    BatchAndMembersUseSeparateDenominators();
    StatsSingleFlightAndCoverage();
    StatsCompletionReturnsToOwningStrand();
    StopWaitsForTheOnlyInFlightSample();
    FirstDecodedFrameUsesGenerationAndBindingEpoch();
    ReconnectWaitsForStableExpectedVideo();
    ReconnectTimeoutAndExpectationChangeAreNotMediaSuccess();
    NativeFreezeStatsPreserveMissingAndMeasuredZero();
    AudioStatsPreserveAvailabilityDeltasResetsAndStaleness();
    AudioQoeFollowsExpectedMediaState();
    AudioFirstFrameRejectsLateBindingCallbacks();
    ReconnectWaitsForAudioAndVisibleRender();
    ReconnectTimeoutPreservesRecoveredMedia();
    RenderSubmitDedupesAndExcludesHiddenIntervals();
    ProcessResourceSamplerUsesNormalizedCpuAndTypedAvailability();
    RuntimeSamplingMeasuresLagAndRejectsLateUiProbes();
    ResourceTrendIsBoundedAndExitReturnStaysHonest();
    return 0;
}
