#include "tests/support/test_check.h"

#include "src/render/canvas_render_timing.h"
#include "src/telemetry/process_resource_sampler.h"
#include "src/telemetry/session_telemetry.h"

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
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
using livekit::telemetry::ProductChainStatus;
using livekit::telemetry::SessionTelemetry;

// Exercise canvas attribution with real session probes, without requiring a
// WebRTC track or a GPU. Binding retirement still uses the real activity probe.
class CanvasTestBindingObserver final : public livekit::render::RenderSubmitObserver {
public:
    CanvasTestBindingObserver(
        const livekit::render::RenderFrameMetadata& metadata,
        std::shared_ptr<livekit::telemetry::RenderActivityProbe> binding,
        std::shared_ptr<livekit::telemetry::CanvasRenderProbe> canvas)
        : series_key_(metadata.series_key)
        , room_generation_(metadata.room_generation)
        , binding_epoch_(metadata.binding_epoch)
        , binding_(std::move(binding))
        , canvas_(std::move(canvas)) {}

    void SetExpected(bool, livekit::render::RenderExpectationReason,
                     Clock::time_point) override {}
    void OnSubmitted(const livekit::render::RenderFrameMetadata&, const char*,
                     Clock::time_point) override {}

    std::shared_ptr<livekit::render::CanvasRenderTimingObserver> CanvasTimingObserver(
        const livekit::render::RenderFrameMetadata& metadata) override {
        if (!binding_->active.load(std::memory_order_acquire) ||
            metadata.series_key != series_key_ ||
            metadata.room_generation != room_generation_ ||
            metadata.binding_epoch != binding_epoch_) {
            return {};
        }
        return canvas_;
    }

private:
    const std::string series_key_;
    const std::uint64_t room_generation_;
    const std::uint64_t binding_epoch_;
    const std::shared_ptr<livekit::telemetry::RenderActivityProbe> binding_;
    const std::shared_ptr<livekit::telemetry::CanvasRenderProbe> canvas_;
};

struct CanvasTestBinding {
    livekit::render::RenderFrameMetadata metadata;
    std::shared_ptr<livekit::telemetry::RenderActivityProbe> probe;
};

CanvasTestBinding RegisterCanvasTestBinding(
    const std::shared_ptr<SessionTelemetry>& telemetry,
    const std::string& series_key,
    std::uint64_t binding_epoch = 1) {
    CanvasTestBinding binding;
    binding.probe = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    binding.metadata.series_key = series_key;
    binding.metadata.room_generation = 8;
    binding.metadata.binding_epoch = binding_epoch;
    binding.metadata.frame_token = 1;
    binding.metadata.decoded_at = Event::Clock::now();
    binding.metadata.observer = std::make_shared<CanvasTestBindingObserver>(
        binding.metadata, binding.probe, telemetry->canvasRenderProbe());
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        series_key, binding.metadata.room_generation, binding_epoch,
        binding.metadata.decoded_at, true, true, 33ms, binding.probe,
        binding.metadata.decoded_at));
    return binding;
}

SessionTelemetry::SnapshotPtr ReadCanvasTestSnapshot(
    asio::io_context& context,
    const SessionTelemetry::Strand& strand,
    const std::shared_ptr<SessionTelemetry>& telemetry) {
    SessionTelemetry::SnapshotPtr snapshot;
    context.restart();
    asio::post(strand, [&snapshot, telemetry] {
        snapshot = telemetry->SnapshotOnStrand();
    });
    context.run();
    TEST_CHECK(snapshot);
    return snapshot;
}

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

void NetworkPathRecoveryAndQualityUseWindowedNativeCounters() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 113, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    std::deque<livekit::RoomStatsReport> reports;
    const auto make_report = [](std::uint64_t inbound_packets,
                                std::uint64_t inbound_retransmitted,
                                std::int64_t inbound_packets_lost,
                                std::uint64_t outbound_packets,
                                std::uint64_t outbound_retransmitted,
                                double total_round_trip_time,
                                std::uint64_t round_trip_time_measurements,
                                double bandwidth_duration,
                                std::uint32_t path_changes) {
        livekit::RoomStatsReport room;
        room.actual_peer_connection_count = 1;
        room.successful_peer_connection_count = 1;
        livekit::StatsReport pc;
        livekit::InboundRtpStreamStats inbound;
        inbound.id = "video-in";
        inbound.kind = "video";
        inbound.kind_available = true;
        inbound.packets_received = inbound_packets;
        inbound.packets_received_available = true;
        inbound.bytes_received = inbound_packets * 100;
        inbound.bytes_received_available = true;
        inbound.packets_lost = inbound_packets_lost;
        inbound.packets_lost_available = true;
        inbound.jitter = static_cast<double>(path_changes) / 1000.0;
        inbound.jitter_available = true;
        inbound.retransmitted_packets_received = inbound_retransmitted;
        inbound.retransmitted_packets_received_available = true;
        inbound.fec_packets_received = inbound_retransmitted / 2;
        inbound.fec_packets_received_available = true;
        inbound.nack_count = static_cast<std::uint32_t>(inbound_retransmitted);
        inbound.nack_count_available = true;
        inbound.pli_count = 2;
        inbound.pli_count_available = true;
        inbound.fir_count = 1;
        inbound.fir_count_available = true;
        inbound.codec_id = "codec-vp8";
        inbound.codec_id_available = true;
        inbound.frames_received = static_cast<std::uint32_t>(inbound_packets);
        inbound.frames_received_available = true;
        inbound.frames_decoded = static_cast<std::uint32_t>(inbound_packets - 20);
        inbound.frames_decoded_available = true;
        inbound.frames_dropped = path_changes;
        inbound.frames_dropped_available = true;
        inbound.total_decode_time = static_cast<double>(inbound_packets) * 0.001;
        inbound.total_decode_time_available = true;
        inbound.frame_width = 1280;
        inbound.frame_height = 720;
        inbound.frames_per_second = 30.0;
        inbound.frame_width_available = true;
        inbound.frame_height_available = true;
        inbound.frames_per_second_available = true;
        inbound.decoder_implementation = "libvpx";
        inbound.decoder_implementation_available = true;
        inbound.power_efficient_decoder = false;
        inbound.power_efficient_decoder_available = true;
        pc.inbound_rtp.push_back(inbound);

        livekit::OutboundRtpStreamStats outbound;
        outbound.id = "video-out";
        outbound.kind = "video";
        outbound.kind_available = true;
        outbound.packets_sent = outbound_packets;
        outbound.packets_sent_available = true;
        outbound.bytes_sent = outbound_packets * 100;
        outbound.bytes_sent_available = true;
        outbound.retransmitted_packets_sent = outbound_retransmitted;
        outbound.retransmitted_packets_sent_available = true;
        outbound.nack_count = static_cast<std::uint32_t>(outbound_retransmitted);
        outbound.nack_count_available = true;
        outbound.pli_count = 3;
        outbound.pli_count_available = true;
        outbound.fir_count = 2;
        outbound.fir_count_available = true;
        outbound.frame_width = 1280;
        outbound.frame_height = 720;
        outbound.frames_per_second = 30.0;
        outbound.frame_width_available = true;
        outbound.frame_height_available = true;
        outbound.frames_per_second_available = true;
        outbound.quality_limitation_reason = "bandwidth";
        outbound.quality_limitation_reason_available = true;
        outbound.quality_limitation_durations = {
            {"none", 10.0}, {"bandwidth", bandwidth_duration}};
        outbound.quality_limitation_durations_available = true;
        outbound.quality_limitation_resolution_changes = path_changes;
        outbound.quality_limitation_resolution_changes_available = true;
        outbound.codec_id = "codec-vp8";
        outbound.codec_id_available = true;
        outbound.frames_encoded = static_cast<std::uint32_t>(outbound_packets);
        outbound.frames_encoded_available = true;
        outbound.frames_sent = static_cast<std::uint32_t>(outbound_packets - 5);
        outbound.frames_sent_available = true;
        outbound.total_encode_time = static_cast<double>(outbound_packets) * 0.002;
        outbound.total_encode_time_available = true;
        outbound.encoder_implementation = "libvpx";
        outbound.encoder_implementation_available = true;
        outbound.power_efficient_encoder = false;
        outbound.power_efficient_encoder_available = true;
        outbound.scalability_mode = "L1T3";
        outbound.scalability_mode_available = true;
        pc.outbound_rtp.push_back(outbound);

        livekit::CodecStats codec;
        codec.id = "codec-vp8";
        codec.mime_type = "video/VP8";
        codec.mime_type_available = true;
        codec.clock_rate = 90000;
        codec.clock_rate_available = true;
        pc.codecs.push_back(codec);

        livekit::RemoteInboundRtpStreamStats remote_inbound;
        remote_inbound.id = "remote-video-in";
        remote_inbound.local_id = "video-out";
        remote_inbound.local_id_available = true;
        remote_inbound.round_trip_time =
            static_cast<double>(path_changes) / 100.0;
        remote_inbound.round_trip_time_available = true;
        remote_inbound.fraction_lost =
            static_cast<double>(inbound_packets_lost) / 100.0;
        remote_inbound.fraction_lost_available = true;
        remote_inbound.total_round_trip_time = total_round_trip_time;
        remote_inbound.total_round_trip_time_available = true;
        remote_inbound.round_trip_time_measurements =
            round_trip_time_measurements;
        remote_inbound.round_trip_time_measurements_available = true;
        pc.remote_inbound_rtp.push_back(remote_inbound);

        livekit::TransportStats transport;
        transport.id = "transport";
        transport.selected_candidate_pair_id = "selected";
        transport.selected_candidate_pair_id_available = true;
        transport.selected_candidate_pair_changes = path_changes;
        transport.selected_candidate_pair_changes_available = true;
        transport.bytes_sent = outbound_packets * 200;
        transport.bytes_sent_available = true;
        transport.bytes_received = inbound_packets * 150;
        transport.bytes_received_available = true;
        transport.packets_sent = outbound_packets;
        transport.packets_sent_available = true;
        transport.packets_received = inbound_packets;
        transport.packets_received_available = true;
        transport.dtls_state = "connected";
        transport.dtls_state_available = true;
        transport.ice_state = "connected";
        transport.ice_state_available = true;
        transport.ice_role = "controlling";
        transport.ice_role_available = true;
        pc.transports.push_back(transport);
        livekit::CandidatePairStats pair;
        pair.id = "selected";
        pair.transport_id = "transport";
        pair.local_candidate_id = "local";
        pair.remote_candidate_id = "remote";
        pair.current_pair = true;
        pair.selected_relationship_available = true;
        pair.current_round_trip_time =
            static_cast<double>(path_changes) / 100.0;
        pair.current_round_trip_time_available = true;
        pair.available_outgoing_bitrate =
            1000000.0 + static_cast<double>(outbound_packets) * 1000.0;
        pair.available_outgoing_bitrate_available = true;
        pair.available_incoming_bitrate =
            2000000.0 + static_cast<double>(inbound_packets) * 1000.0;
        pair.available_incoming_bitrate_available = true;
        pc.candidate_pairs.push_back(pair);
        livekit::IceCandidateStats local;
        local.id = "local";
        local.candidate_type = "relay";
        local.candidate_type_available = true;
        local.network_type = "wifi";
        local.network_type_available = true;
        local.protocol = "udp";
        local.protocol_available = true;
        local.relay_protocol = "tls";
        local.relay_protocol_available = true;
        pc.ice_candidates.push_back(local);
        livekit::IceCandidateStats remote;
        remote.id = "remote";
        remote.remote = true;
        remote.candidate_type = "host";
        remote.candidate_type_available = true;
        remote.protocol = "udp";
        remote.protocol_available = true;
        pc.ice_candidates.push_back(remote);
        room.reports.push_back(std::move(pc));
        return room;
    };

    reports.push_back(make_report(1000, 10, 5, 800, 8, 0.4, 2, 2.0, 3));
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
    TEST_CHECK(snapshot->network_recovery_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->inbound_rtp_traffic_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->outbound_rtp_traffic_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->inbound_packet_loss_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->inbound_jitter_availability == Availability::Valid);
    TEST_CHECK(snapshot->remote_rtcp_availability == Availability::Valid);
    TEST_CHECK(snapshot->transport_traffic_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->transport_state_availability == Availability::Valid);
    TEST_CHECK(snapshot->media_path_availability == Availability::Valid);
    TEST_CHECK(snapshot->media_path_switches == 0);
    TEST_CHECK(snapshot->local_candidate_types == "relay");
    TEST_CHECK(snapshot->local_network_types == "wifi");
    TEST_CHECK(snapshot->video_quality_limitation_current == "bandwidth");
    TEST_CHECK(snapshot->window_video_quality_bandwidth_duration_ms == -1);
    TEST_CHECK(snapshot->video_pipeline_availability == Availability::WarmingUp);
    TEST_CHECK(snapshot->video_codec_availability == Availability::Valid);
    TEST_CHECK(snapshot->inbound_video_codecs == "video/vp8");
    TEST_CHECK(snapshot->outbound_video_layers == "l1t3");
    TEST_CHECK(snapshot->video_processing_availability == Availability::WarmingUp);

    reports.push_back(make_report(1200, 14, 7, 900, 11, 0.9, 4, 2.75, 4));
    context.restart();
    asio::post(strand, [telemetry] { telemetry->RequestStatsSampleOnStrand(); });
    context.run_for(100ms);
    TEST_CHECK(snapshot->network_recovery_availability == Availability::Valid);
    TEST_CHECK(snapshot->inbound_rtp_traffic_availability == Availability::Valid);
    TEST_CHECK(snapshot->outbound_rtp_traffic_availability == Availability::Valid);
    TEST_CHECK(snapshot->window_inbound_rtp_bytes == 20000);
    TEST_CHECK(snapshot->window_outbound_rtp_bytes == 10000);
    TEST_CHECK(snapshot->inbound_rtp_bitrate_bps > 0.0);
    TEST_CHECK(snapshot->outbound_rtp_bitrate_bps > 0.0);
    TEST_CHECK(snapshot->inbound_packet_loss_availability == Availability::Valid);
    TEST_CHECK(snapshot->window_inbound_packets_lost == 2);
    TEST_CHECK(snapshot->window_inbound_packets_received == 200);
    TEST_CHECK(std::abs(snapshot->inbound_packet_loss_ratio -
                        (2.0 / 202.0)) < 1e-9);
    TEST_CHECK(snapshot->inbound_jitter_max_ms == 4.0);
    TEST_CHECK(snapshot->remote_rtcp_availability == Availability::Valid);
    TEST_CHECK(snapshot->remote_rtcp_current_rtt_max_ms == 40.0);
    TEST_CHECK(std::abs(snapshot->remote_rtcp_window_average_rtt_ms - 250.0) <
               1e-9);
    TEST_CHECK(snapshot->remote_rtcp_fraction_lost_max == 0.07);
    TEST_CHECK(snapshot->window_inbound_packets == 200);
    TEST_CHECK(snapshot->window_inbound_retransmitted_packets == 4);
    TEST_CHECK(std::abs(snapshot->inbound_retransmitted_packet_ratio - 0.02) < 1e-9);
    TEST_CHECK(snapshot->window_outbound_packets == 100);
    TEST_CHECK(snapshot->window_outbound_retransmitted_packets == 3);
    TEST_CHECK(std::abs(snapshot->outbound_retransmitted_packet_ratio - 0.03) < 1e-9);
    TEST_CHECK(snapshot->media_path_switches == 1);
    TEST_CHECK(snapshot->media_path_rtt_availability == Availability::Valid);
    TEST_CHECK(snapshot->media_path_rtt_max_ms == 40.0);
    TEST_CHECK(snapshot->media_bandwidth_availability == Availability::Valid);
    TEST_CHECK(snapshot->media_available_outgoing_bitrate_bps == 1900000.0);
    TEST_CHECK(snapshot->media_available_incoming_bitrate_bps == 3200000.0);
    TEST_CHECK(snapshot->transport_traffic_availability == Availability::Valid);
    TEST_CHECK(snapshot->window_transport_bytes_sent == 20000);
    TEST_CHECK(snapshot->window_transport_bytes_received == 30000);
    TEST_CHECK(snapshot->window_transport_packets_sent == 100);
    TEST_CHECK(snapshot->window_transport_packets_received == 200);
    TEST_CHECK(snapshot->transport_dtls_states == "connected");
    TEST_CHECK(snapshot->transport_connectivity_states == "connected");
    TEST_CHECK(snapshot->transport_roles == "controlling");
    TEST_CHECK(snapshot->video_quality_limitation_availability == Availability::Valid);
    TEST_CHECK(snapshot->window_video_quality_bandwidth_duration_ms == 750);
    TEST_CHECK(snapshot->window_video_quality_resolution_changes == 1);
    TEST_CHECK(snapshot->outbound_video_width == 1280);
    TEST_CHECK(snapshot->outbound_video_height == 720);
    TEST_CHECK(snapshot->video_pipeline_availability == Availability::Valid);
    TEST_CHECK(snapshot->window_inbound_video_frames_received == 200);
    TEST_CHECK(snapshot->window_inbound_video_frames_decoded == 200);
    TEST_CHECK(snapshot->window_inbound_video_frames_dropped == 1);
    TEST_CHECK(snapshot->window_outbound_video_frames_encoded == 100);
    TEST_CHECK(snapshot->window_outbound_video_frames_sent == 100);
    TEST_CHECK(std::abs(snapshot->inbound_video_frame_drop_ratio - 0.005) < 1e-9);
    TEST_CHECK(snapshot->video_processing_availability == Availability::Valid);
    TEST_CHECK(std::abs(snapshot->video_decode_ms_per_frame - 1.0) < 1e-9);
    TEST_CHECK(std::abs(snapshot->video_encode_ms_per_frame - 2.0) < 1e-9);

    reports.push_back(make_report(1300, 16, 6, 950, 12, 1.15, 5, 2.9, 4));
    context.restart();
    asio::post(strand, [telemetry] { telemetry->RequestStatsSampleOnStrand(); });
    context.run_for(100ms);
    TEST_CHECK(snapshot->inbound_packet_loss_availability == Availability::Invalid);
    TEST_CHECK(snapshot->inbound_packet_loss_reason ==
               "inbound_loss_late_packet_correction");
    TEST_CHECK(snapshot->window_inbound_packets_lost == -1);
    TEST_CHECK(snapshot->window_inbound_packets_received == 100);
    TEST_CHECK(snapshot->inbound_packet_loss_ratio < 0.0);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run();
}

void MissingRecoveryAndQualityCountersRemainUnavailable() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 115, 64);
    SessionTelemetry::SnapshotPtr snapshot;

    livekit::RoomStatsReport report;
    report.actual_peer_connection_count = 1;
    report.successful_peer_connection_count = 1;
    livekit::StatsReport pc;
    livekit::InboundRtpStreamStats inbound;
    inbound.id = "video-in-missing-recovery";
    inbound.kind = "video";
    inbound.kind_available = true;
    inbound.packets_received = 100;
    inbound.packets_received_available = true;
    pc.inbound_rtp.push_back(inbound);
    livekit::OutboundRtpStreamStats outbound;
    outbound.id = "video-out-reason-only";
    outbound.kind = "video";
    outbound.kind_available = true;
    outbound.packets_sent = 80;
    outbound.packets_sent_available = true;
    outbound.quality_limitation_reason = "cpu";
    outbound.quality_limitation_reason_available = true;
    pc.outbound_rtp.push_back(outbound);
    livekit::RemoteInboundRtpStreamStats remote_inbound;
    remote_inbound.id = "remote-inbound-missing-feedback";
    pc.remote_inbound_rtp.push_back(remote_inbound);
    livekit::TransportStats transport;
    transport.id = "transport-missing-fields";
    pc.transports.push_back(transport);
    report.reports.push_back(std::move(pc));

    asio::post(strand, [telemetry, &snapshot, report = std::move(report)]() mutable {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartStatsSamplingOnStrand(
            [report = std::move(report)](SessionTelemetry::LateCompletion) mutable
                -> asio::awaitable<livekit::RoomStatsReport> {
                co_return std::move(report);
            }, 1h);
    });
    context.run_for(100ms);

    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->network_recovery_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->inbound_rtp_traffic_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->outbound_rtp_traffic_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->inbound_packet_loss_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->inbound_jitter_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->remote_rtcp_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->transport_traffic_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->transport_state_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->inbound_rtp_bitrate_bps < 0.0);
    TEST_CHECK(snapshot->outbound_rtp_bitrate_bps < 0.0);
    TEST_CHECK(snapshot->inbound_packet_loss_ratio < 0.0);
    TEST_CHECK(snapshot->inbound_retransmission_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->outbound_retransmission_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->inbound_retransmitted_packet_ratio == -1.0);
    TEST_CHECK(snapshot->outbound_retransmitted_packet_ratio == -1.0);
    TEST_CHECK(snapshot->video_quality_limitation_availability ==
               Availability::Valid);
    TEST_CHECK(snapshot->video_quality_limitation_current == "cpu");
    TEST_CHECK(snapshot->video_quality_cpu_duration_ms == -1);
    TEST_CHECK(snapshot->window_video_quality_cpu_duration_ms == -1);
    TEST_CHECK(snapshot->video_quality_resolution_changes == -1);
    TEST_CHECK(snapshot->window_video_quality_resolution_changes == -1);

    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run();
}

void LocalDeviceContinuityUsesPublicationEpochAndRunningIntent() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 114, 64);
    auto video = std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    const auto base = SessionTelemetry::Clock::now();
    TEST_CHECK(telemetry->RegisterLocalPublication(
        "local_publish/device-video", 12, 5,
        livekit::telemetry::LocalMediaKind::Video,
        "device-video", base, true, video, base));
    context.run();

    video->frame_count.store(10, std::memory_order_relaxed);
    video->last_frame_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (base + 100ms).time_since_epoch()).count(),
        std::memory_order_release);
    video->format_changes.store(2, std::memory_order_relaxed);
    video->clock_resets.store(1, std::memory_order_relaxed);
    video->width.store(1280, std::memory_order_relaxed);
    video->height.store(720, std::memory_order_relaxed);
    SessionTelemetry::SnapshotPtr snapshot;
    context.restart();
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 200ms);
    });
    context.run();
    TEST_CHECK(snapshot->local_device_continuity_availability == Availability::Valid);
    TEST_CHECK(snapshot->active_local_device_streams == 1);
    TEST_CHECK(snapshot->local_device_format_changes == 2);
    TEST_CHECK(snapshot->local_device_clock_resets == 1);
    TEST_CHECK(snapshot->device_open_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->device_hotplug_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->device_state_availability == Availability::Valid);
    TEST_CHECK(snapshot->camera_requested && snapshot->camera_effective);
    TEST_CHECK(snapshot->actual_capture_width == 1280);
    TEST_CHECK(snapshot->actual_capture_height == 720);

    context.restart();
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 1600ms);
    });
    context.run();
    TEST_CHECK(snapshot->local_device_unexpected_stops == 1);
    TEST_CHECK(snapshot->active_local_device_streams == 0);
    TEST_CHECK(snapshot->local_device_interruption_duration_ms == 500);

    video->last_frame_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (base + 1700ms).time_since_epoch()).count(),
        std::memory_order_release);
    video->frame_count.store(11, std::memory_order_relaxed);
    context.restart();
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 1800ms);
    });
    context.run();
    TEST_CHECK(snapshot->active_local_device_streams == 1);
    TEST_CHECK(snapshot->local_device_unexpected_stops == 1);
    TEST_CHECK(snapshot->local_device_interruption_duration_ms == 600);

    auto replacement =
        std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    replacement->frame_count.store(1, std::memory_order_relaxed);
    replacement->last_frame_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (base + 1950ms).time_since_epoch()).count(),
        std::memory_order_release);
    replacement->format_changes.store(3, std::memory_order_relaxed);
    replacement->clock_resets.store(2, std::memory_order_relaxed);
    TEST_CHECK(telemetry->RegisterLocalPublication(
        "local_publish/device-video", 12, 6,
        livekit::telemetry::LocalMediaKind::Video,
        "device-video-replacement", base + 1900ms, true, replacement,
        base + 1900ms));
    context.restart();
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 2000ms);
    });
    context.run();
    TEST_CHECK(!video->active.load(std::memory_order_acquire));
    TEST_CHECK(replacement->active.load(std::memory_order_acquire));
    TEST_CHECK(snapshot->active_local_device_streams == 1);
    TEST_CHECK(snapshot->local_device_unexpected_stops == 1);
    TEST_CHECK(snapshot->local_device_interruption_duration_ms == 600);
    TEST_CHECK(snapshot->local_device_format_changes == 5);
    TEST_CHECK(snapshot->local_device_clock_resets == 3);
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
    livekit::telemetry::RenderPipelineSample pipeline;
    pipeline.router_submitted = 10;
    pipeline.router_replaced_before_render = 3;
    pipeline.router_dropped_capacity = 1;
    pipeline.delivered_to_qt_cpu = 6;
    pipeline.qt_cpu_conversion_failures = 1;
    pipeline.attached_track_count = 1;
    pipeline.requested_backend = "dx11";
    pipeline.actual_backend = "qt-cpu";
    pipeline.gpu_failure = "device-lost";
    pipeline.fallback_reason = "gpu-device-lost";
    TEST_CHECK(telemetry->RecordRenderPipelineSample(pipeline, base + 810ms));
    probe->cpu_convert.samples.store(2, std::memory_order_relaxed);
    probe->cpu_convert.total_us.store(90, std::memory_order_relaxed);
    probe->cpu_convert.maximum_us.store(50, std::memory_order_relaxed);
    probe->draw_submit.samples.store(2, std::memory_order_relaxed);
    probe->draw_submit.total_us.store(150, std::memory_order_relaxed);
    probe->draw_submit.maximum_us.store(90, std::memory_order_relaxed);
    telemetry->canvasRenderProbe()->OnCanvasStageTiming(
        livekit::render::CanvasRenderStage::PresentBlock, 120us);
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
    TEST_CHECK(snapshot->render_stage_availability == Availability::Valid);
    TEST_CHECK(snapshot->render_convert_samples == 2);
    TEST_CHECK(snapshot->render_convert_total_us == 90);
    TEST_CHECK(snapshot->render_convert_max_us == 50);
    TEST_CHECK(snapshot->render_draw_samples == 2);
    TEST_CHECK(snapshot->render_draw_total_us == 150);
    TEST_CHECK(snapshot->render_draw_max_us == 90);
    TEST_CHECK(snapshot->render_present_block_samples == 1);
    TEST_CHECK(snapshot->render_present_block_total_us == 120);
    TEST_CHECK(snapshot->render_present_block_max_us == 120);
    TEST_CHECK(snapshot->render_gpu_execution_availability ==
               Availability::Unsupported);
    TEST_CHECK(snapshot->render_interval_p50_ms == 1000.0);
    TEST_CHECK(snapshot->render_interval_p95_ms == 1000.0);
    TEST_CHECK(snapshot->render_interval_p99_ms == 1000.0);
    TEST_CHECK(snapshot->render_frame_age_availability == Availability::Valid);
    TEST_CHECK(snapshot->render_average_frame_age_ms == 10.0);
    TEST_CHECK(snapshot->render_maximum_frame_age_ms == 10);
    TEST_CHECK(snapshot->render_target_interval_ms == 33);
    TEST_CHECK(snapshot->render_pipeline_availability == Availability::Valid);
    TEST_CHECK(snapshot->render_router_replaced == 3);
    TEST_CHECK(snapshot->render_router_dropped_capacity == 1);
    TEST_CHECK(snapshot->render_actual_backend == "qt-cpu");
    TEST_CHECK(snapshot->render_backend_failures == 1);
    TEST_CHECK(snapshot->render_backend_fallbacks == 1);
    TEST_CHECK(snapshot->router_queue_availability == Availability::Valid);
    TEST_CHECK(snapshot->active_router_slots == 1);

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

void CanvasTimingCountsOperationsInsteadOfVideoResources() {
    using livekit::render::CanvasRenderStage;
    using livekit::render::CanvasRenderTimingBatch;
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 142, 128);
    std::vector<CanvasTestBinding> videos;
    CanvasRenderTimingBatch grid;
    for (int i = 0; i != 9; ++i) {
        videos.push_back(RegisterCanvasTestBinding(
            telemetry, "remote_render/grid/" + std::to_string(i)));
        const auto& video = videos.back();
        grid.Add(video.metadata);
        TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
            video.metadata.series_key, video.metadata.room_generation,
            video.metadata.binding_epoch, video.metadata.frame_token,
            video.metadata.decoded_at, "opengl_swap_buffers", video.probe,
            video.metadata.decoded_at + 10ms));
    }
    // One stream may occur in several scene items. Neither stream count nor
    // duplicate metadata changes the one measured draw/Present operation.
    grid.Add(videos.front().metadata);
    grid.Add(videos.front().metadata);
    grid.Notify(CanvasRenderStage::DrawSubmit, 80us);
    grid.Notify(CanvasRenderStage::PresentBlock, 120us);
    auto snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->unique_render_submits == 9);
    TEST_CHECK(snapshot->render_draw_samples == 1);
    TEST_CHECK(snapshot->render_draw_total_us == 80);
    TEST_CHECK(snapshot->render_draw_max_us == 80);
    TEST_CHECK(snapshot->render_present_block_samples == 1);
    TEST_CHECK(snapshot->render_present_block_total_us == 120);
    TEST_CHECK(snapshot->render_present_block_max_us == 120);

    // A static scene can really be presented again without new frame tokens.
    CanvasRenderTimingBatch redraw;
    for (const auto& video : videos) redraw.Add(video.metadata);
    redraw.Notify(CanvasRenderStage::DrawSubmit, 40us);
    redraw.Notify(CanvasRenderStage::PresentBlock, 30us);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->unique_render_submits == 9);
    TEST_CHECK(snapshot->render_draw_samples == 2);
    TEST_CHECK(snapshot->render_draw_total_us == 120);
    TEST_CHECK(snapshot->render_present_block_samples == 2);
    TEST_CHECK(snapshot->render_present_block_total_us == 150);

    // The same frame appearing in another canvas incurs another operation.
    CanvasRenderTimingBatch picture_in_picture;
    picture_in_picture.Add(videos.front().metadata);
    picture_in_picture.Notify(CanvasRenderStage::DrawSubmit, 10us);
    picture_in_picture.Notify(CanvasRenderStage::PresentBlock, 25us);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->render_draw_samples == 3);
    TEST_CHECK(snapshot->render_draw_total_us == 130);
    TEST_CHECK(snapshot->render_draw_max_us == 80);
    TEST_CHECK(snapshot->render_present_block_samples == 3);
    TEST_CHECK(snapshot->render_present_block_total_us == 175);
    TEST_CHECK(snapshot->render_present_block_max_us == 120);
}

void CanvasTimingAttributesMixedSessionsIndependently() {
    using livekit::render::CanvasRenderStage;
    using livekit::render::CanvasRenderTimingBatch;
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto first = std::make_shared<SessionTelemetry>(strand, 143, 32);
    auto second = std::make_shared<SessionTelemetry>(strand, 144, 32);
    const auto first_video = RegisterCanvasTestBinding(first, "remote_render/mixed/a");
    const auto first_other = RegisterCanvasTestBinding(first, "remote_render/mixed/b");
    // Equal room/binding generations across sessions must not merge sinks.
    const auto second_video = RegisterCanvasTestBinding(second, "remote_render/mixed/a");
    CanvasRenderTimingBatch mixed;
    mixed.Add(first_video.metadata);
    mixed.Add(second_video.metadata);
    mixed.Add(first_other.metadata);
    mixed.Add(second_video.metadata);
    mixed.Notify(CanvasRenderStage::DrawSubmit, 55us);
    mixed.Notify(CanvasRenderStage::PresentBlock, 90us);
    for (const auto& session : {first, second}) {
        const auto snapshot = ReadCanvasTestSnapshot(context, strand, session);
        TEST_CHECK(snapshot->render_draw_samples == 1);
        TEST_CHECK(snapshot->render_draw_total_us == 55);
        TEST_CHECK(snapshot->render_present_block_samples == 1);
        TEST_CHECK(snapshot->render_present_block_total_us == 90);
    }
}

void CanvasTimingSurvivesBindingReplacementAndStopsWithSession() {
    using livekit::render::CanvasRenderStage;
    using livekit::render::CanvasRenderTimingBatch;
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 145, 64);
    const auto original = RegisterCanvasTestBinding(telemetry, "remote_render/lifetime/a");
    CanvasRenderTimingBatch retained;
    retained.Add(original.metadata);
    retained.Notify(CanvasRenderStage::DrawSubmit, 10us);
    retained.Notify(CanvasRenderStage::PresentBlock, 100us);
    auto snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->render_present_block_total_us == 100);

    const auto replacement = RegisterCanvasTestBinding(
        telemetry, original.metadata.series_key, 2);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(!original.probe->active.load(std::memory_order_acquire));
    TEST_CHECK(snapshot->render_draw_samples == 1);
    TEST_CHECK(snapshot->render_draw_total_us == 10);
    TEST_CHECK(snapshot->render_present_block_samples == 1);
    TEST_CHECK(snapshot->render_present_block_total_us == 100);

    CanvasRenderTimingBatch invalid;
    invalid.Add({});
    invalid.Add(original.metadata); // Retired observer has the same session sink.
    auto wrong_binding = replacement.metadata;
    ++wrong_binding.binding_epoch;
    invalid.Add(wrong_binding);
    auto wrong_generation = replacement.metadata;
    ++wrong_generation.room_generation;
    invalid.Add(wrong_generation);
    auto missing_token = replacement.metadata;
    missing_token.frame_token = 0;
    invalid.Add(missing_token);
    invalid.Notify(CanvasRenderStage::DrawSubmit, 999us);
    invalid.Notify(CanvasRenderStage::PresentBlock, 999us);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->render_draw_total_us == 10);
    TEST_CHECK(snapshot->render_present_block_total_us == 100);

    // Invalid/retired resources appearing first cannot suppress the later
    // valid binding, even though they belong to the same session.
    invalid.Add(replacement.metadata);
    invalid.Add(replacement.metadata);
    invalid.Notify(CanvasRenderStage::DrawSubmit, 20us);
    invalid.Notify(CanvasRenderStage::PresentBlock, 60us);
    invalid.Notify(CanvasRenderStage::DrawSubmit, -1us);
    invalid.Notify(CanvasRenderStage::PresentBlock, -1us);
    telemetry->canvasRenderProbe()->OnCanvasStageTiming(
        CanvasRenderStage::DrawSubmit, -1us);
    telemetry->canvasRenderProbe()->OnCanvasStageTiming(
        CanvasRenderStage::PresentBlock, -1us);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->render_draw_samples == 2);
    TEST_CHECK(snapshot->render_draw_total_us == 30);
    TEST_CHECK(snapshot->render_draw_max_us == 20);
    TEST_CHECK(snapshot->render_present_block_samples == 2);
    TEST_CHECK(snapshot->render_present_block_total_us == 160);
    TEST_CHECK(snapshot->render_present_block_max_us == 100);

    // Room invalidates the activity probe when a binding exits. Canvas totals
    // belong to the session and must survive losing every active binding.
    replacement.probe->active.store(false, std::memory_order_release);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->render_bindings == 0);
    TEST_CHECK(snapshot->render_draw_total_us == 30);
    TEST_CHECK(snapshot->render_present_block_total_us == 160);
    context.restart();
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run();
    TEST_CHECK(!telemetry->canvasRenderProbe()->active.load(std::memory_order_acquire));

    // Already collected work can outlive the old session. It must be rejected,
    // and a new session with the same metadata must start from its own totals.
    retained.Notify(CanvasRenderStage::DrawSubmit, 999us);
    retained.Notify(CanvasRenderStage::PresentBlock, 999us);
    auto next_session = std::make_shared<SessionTelemetry>(strand, 146, 32);
    const auto next = RegisterCanvasTestBinding(
        next_session, original.metadata.series_key);
    CanvasRenderTimingBatch next_canvas;
    next_canvas.Add(original.metadata);
    next_canvas.Add(next.metadata);
    next_canvas.Notify(CanvasRenderStage::DrawSubmit, 30us);
    next_canvas.Notify(CanvasRenderStage::PresentBlock, 40us);
    snapshot = ReadCanvasTestSnapshot(context, strand, telemetry);
    TEST_CHECK(snapshot->session_complete);
    TEST_CHECK(snapshot->render_draw_samples == 2);
    TEST_CHECK(snapshot->render_draw_total_us == 30);
    TEST_CHECK(snapshot->render_present_block_samples == 2);
    TEST_CHECK(snapshot->render_present_block_total_us == 160);
    const auto next_snapshot = ReadCanvasTestSnapshot(context, strand, next_session);
    TEST_CHECK(!next_snapshot->session_complete);
    TEST_CHECK(next_snapshot->render_draw_samples == 1);
    TEST_CHECK(next_snapshot->render_draw_total_us == 30);
    TEST_CHECK(next_snapshot->render_present_block_samples == 1);
    TEST_CHECK(next_snapshot->render_present_block_total_us == 40);
}

void VideoPolicyRevisionAndRetirementAreSessionScoped() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 147, 32);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [&] {
        livekit::telemetry::VideoPolicySample sample;
        sample.coordinator_session = 147;
        sample.native_room_generation = 8;
        sample.catalog_revision = 12;
        sample.policy_revision = 4;
        sample.stage_content = "video";
        sample.policy_reason = "visible";
        sample.requested = 4;
        sample.selected = 3;
        sample.actual = 2;
        sample.bound = 1;
        sample.selected_not_actual = 1;
        sample.selected_not_bound = 2;
        TEST_CHECK(telemetry->RecordVideoPolicySampleOnStrand(sample));

        auto stale = sample;
        stale.policy_revision = 3;
        stale.requested = 16;
        TEST_CHECK(!telemetry->RecordVideoPolicySampleOnStrand(stale));
        snapshot = telemetry->SnapshotOnStrand();
    });
    context.run();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->video_policy_availability == Availability::Valid);
    TEST_CHECK(snapshot->video_policy_revision == 4);
    TEST_CHECK(snapshot->video_policy_requested == 4);
    TEST_CHECK(snapshot->video_policy_selected == 3);
    TEST_CHECK(snapshot->video_policy_actual == 2);
    TEST_CHECK(snapshot->video_policy_bound == 1);
    TEST_CHECK(snapshot->video_policy_selected_not_actual == 1);
    TEST_CHECK(snapshot->video_policy_selected_not_bound == 2);
    TEST_CHECK(snapshot->video_policy_stale_updates == 1);

    context.restart();
    asio::post(strand, [&] {
        livekit::telemetry::VideoPolicySample successor;
        successor.coordinator_session = 147;
        successor.native_room_generation = 9;
        successor.catalog_revision = 1;
        successor.policy_revision = 1;
        successor.stage_content = "whiteboard";
        successor.policy_reason = "whiteboard";
        TEST_CHECK(telemetry->RecordVideoPolicySampleOnStrand(successor));
        successor.retired = true;
        TEST_CHECK(telemetry->RecordVideoPolicySampleOnStrand(successor));
        telemetry->StopOnStrand();
        snapshot = telemetry->SnapshotOnStrand();
        TEST_CHECK(!telemetry->RecordVideoPolicySampleOnStrand(successor));
    });
    context.run();
    TEST_CHECK(snapshot->session_complete);
    TEST_CHECK(snapshot->video_policy_availability == Availability::NotExpected);
    TEST_CHECK(snapshot->video_policy_reason == "video_policy_retired");
    TEST_CHECK(snapshot->video_policy_native_room_generation == 9);
    TEST_CHECK(snapshot->video_policy_retired);
    TEST_CHECK(snapshot->video_policy_requested == 0);
    TEST_CHECK(snapshot->video_policy_selected == 0);
    TEST_CHECK(snapshot->video_policy_actual == 0);
    TEST_CHECK(snapshot->video_policy_bound == 0);
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
    TEST_CHECK(snapshot->resource_return_availability == Availability::Unsupported);
    TEST_CHECK(snapshot->resource_return_reason ==
               "post_stop_sampler_not_owned_after_session_teardown");
}

void SessionDurationsAndDisconnectHaveOneTerminal() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    const auto base = Event::Clock::now();
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 201, 64, base);
    SessionTelemetry::SnapshotPtr snapshot;
    asio::post(strand, [telemetry, &snapshot] {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
    });

    const auto connect = telemetry->StartOperation(
        OperationKind::Connect, {}, base + 100ms);
    TEST_CHECK(telemetry->FinishOperation(
        connect, OperationKind::Connect, OperationOutcome::Success,
        base + 200ms));
    const auto reconnect = telemetry->StartOperation(
        OperationKind::ReconnectEpisode, {}, base + 1200ms);
    TEST_CHECK(telemetry->FinishOperation(
        reconnect, OperationKind::ReconnectEpisode, OperationOutcome::Success,
        base + 1500ms));
    const auto disconnect = telemetry->StartOperation(
        OperationKind::Disconnect, {}, base + 2000ms);
    TEST_CHECK(telemetry->FinishOperation(
        disconnect, OperationKind::Disconnect, OperationOutcome::Success,
        base + 2050ms));
    TEST_CHECK(telemetry->FinishOperation(
        disconnect, OperationKind::Disconnect, OperationOutcome::Failure,
        base + 2100ms));
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 2200ms);
    });
    context.run();

    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->session_duration_availability == Availability::Valid);
    TEST_CHECK(snapshot->session_duration_ms == 2200);
    TEST_CHECK(snapshot->usable_duration_availability == Availability::Valid);
    TEST_CHECK(snapshot->usable_duration_ms == 1500);
    const auto* summary = FindOperation(*snapshot, OperationKind::Disconnect);
    TEST_CHECK(summary);
    TEST_CHECK(summary->started == 1);
    TEST_CHECK(summary->terminal == 1);
    TEST_CHECK(summary->success == 1);
    TEST_CHECK(summary->last_duration_ms == 50);
    TEST_CHECK(snapshot->operations_duplicate_terminal == 1);
}

void LocalPublishPipelinePreservesWarmingTimeoutAndMapping() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 202, 64);
    SessionTelemetry::SnapshotPtr snapshot;
    const auto now = Event::Clock::now();
    auto probe = std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    livekit::telemetry::LocalVideoPublishDescriptor publish;
    publish.requested_codec = "auto";
    publish.effective_codec = "vp9";
    publish.source = "camera";
    publish.mode = "svc";
    publish.resolved_profile = "negotiated";
    publish.resolved_scalability = "L3T3_KEY";
    publish.sender_track_ids = {"rtc-video", "rtc-video-backup"};
    TEST_CHECK(!probe->active.load(std::memory_order_acquire));
    probe->first_injected_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (now - 150ms).time_since_epoch()).count(),
        std::memory_order_release);
    TEST_CHECK(telemetry->RegisterLocalPublication(
        "local_publish/rtc-video", 7, 1,
        livekit::telemetry::LocalMediaKind::Video,
        "rtc-video", now - 200ms, true, probe, now - 100ms, {}, publish));
    TEST_CHECK(probe->active.load(std::memory_order_acquire));

    livekit::RoomStatsReport report;
    report.actual_peer_connection_count = 1;
    report.successful_peer_connection_count = 1;
    livekit::StatsReport publisher;
    publisher.senders_available = true;
    livekit::RtpSenderDiagnostic sender;
    sender.track_id = "rtc-video";
    sender.kind = "video";
    sender.mid = "0";
    sender.mid_available = true;
    sender.track_enabled = true;
    publisher.senders.push_back(sender);
    livekit::OutboundRtpStreamStats outbound;
    outbound.kind = "video";
    outbound.kind_available = true;
    outbound.mid = "0";
    outbound.mid_available = true;
    outbound.codec_id = "video-codec";
    outbound.codec_id_available = true;
    outbound.encoder_implementation = "libvpx";
    outbound.encoder_implementation_available = true;
    outbound.scalability_mode = "L3T3_KEY";
    outbound.scalability_mode_available = true;
    outbound.frames_encoded = 1;
    outbound.frames_encoded_available = true;
    outbound.packets_sent = 1;
    outbound.packets_sent_available = true;
    publisher.outbound_rtp.push_back(outbound);
    livekit::CodecStats codec;
    codec.id = "video-codec";
    codec.mime_type = "video/VP9";
    codec.mime_type_available = true;
    publisher.codecs.push_back(codec);
    report.reports.push_back(publisher);

    asio::post(strand, [telemetry, &snapshot, report = std::move(report)]() mutable {
        telemetry->SetSnapshotCallbackOnStrand(
            [&snapshot](SessionTelemetry::SnapshotPtr value) {
                snapshot = std::move(value);
            });
        telemetry->StartStatsSamplingOnStrand(
            [report = std::move(report)](SessionTelemetry::LateCompletion) mutable
                -> asio::awaitable<livekit::RoomStatsReport> {
                co_return std::move(report);
            }, 1s);
    });
    context.run_for(100ms);

    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->local_publish_media_availability == Availability::Valid);
    TEST_CHECK(snapshot->local_video_injection_availability == Availability::Valid);
    TEST_CHECK(snapshot->local_video_encode_availability == Availability::Valid);
    TEST_CHECK(snapshot->local_rtp_send_availability == Availability::Valid);
    TEST_CHECK(snapshot->local_video_first_injections == 1);
    TEST_CHECK(snapshot->local_video_first_encodes == 1);
    TEST_CHECK(snapshot->local_first_rtp_sends == 1);
    TEST_CHECK(snapshot->local_publish_no_media == 0);
    TEST_CHECK(snapshot->local_publish_stats_uncertainty_ms == 1000);
    TEST_CHECK(snapshot->video_publish_plan_availability == Availability::Valid);
    TEST_CHECK(snapshot->video_publish_requested_codecs == "auto");
    TEST_CHECK(snapshot->video_publish_effective_codecs == "vp9");
    TEST_CHECK(snapshot->video_publish_observed_codecs == "vp9");
    TEST_CHECK(snapshot->video_publish_sources == "camera");
    TEST_CHECK(snapshot->video_publish_direction == "send");
    TEST_CHECK(snapshot->video_publish_generations == "7");
    TEST_CHECK(snapshot->video_publish_modes == "svc");
    TEST_CHECK(snapshot->video_publish_encoder_implementations == "libvpx");
    TEST_CHECK(snapshot->video_publish_resolved_scalability == "L3T3_KEY");
    TEST_CHECK(snapshot->video_publish_observed_scalability == "l3t3_key");

    context.restart();
    TEST_CHECK(telemetry->RecordLocalVideoFrameInjected(
        "local_publish/rtc-video", 7, 99, Event::Clock::now()));
    context.run_for(100ms);
    TEST_CHECK(snapshot->stale_local_publication_drops == 1);
    asio::post(strand, [telemetry] { telemetry->StopOnStrand(); });
    context.run_for(100ms);

    asio::io_context bounded_context;
    auto bounded_strand = asio::make_strand(bounded_context);
    auto bounded_telemetry = std::make_shared<SessionTelemetry>(
        bounded_strand, 204, 1);
    Event queued;
    queued.kind = EventKind::GaugeSample;
    queued.session_generation = 204;
    TEST_CHECK(bounded_telemetry->Submit(std::move(queued)));
    auto rejected_probe =
        std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    TEST_CHECK(!bounded_telemetry->RegisterLocalPublication(
        "local_publish/rejected", 9, 1,
        livekit::telemetry::LocalMediaKind::Video,
        "rejected", now, true, rejected_probe, now));
    TEST_CHECK(!rejected_probe->active.load(std::memory_order_acquire));

    asio::io_context timeout_context;
    auto timeout_strand = asio::make_strand(timeout_context);
    auto timeout_telemetry = std::make_shared<SessionTelemetry>(
        timeout_strand, 203, 64);
    SessionTelemetry::SnapshotPtr timed_out;
    auto missing_probe =
        std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    const auto expired = Event::Clock::now() - 6s;
    TEST_CHECK(timeout_telemetry->RegisterLocalPublication(
        "local_publish/missing", 8, 1,
        livekit::telemetry::LocalMediaKind::Video,
        "missing", expired, true, missing_probe, expired + 10ms));
    livekit::RoomStatsReport timeout_report;
    timeout_report.actual_peer_connection_count = 1;
    timeout_report.successful_peer_connection_count = 1;
    livekit::StatsReport timeout_pc;
    timeout_pc.senders_available = true;
    livekit::RtpSenderDiagnostic timeout_sender;
    timeout_sender.track_id = "missing";
    timeout_sender.kind = "video";
    timeout_sender.mid = "timeout-mid";
    timeout_sender.mid_available = true;
    timeout_sender.track_enabled = true;
    timeout_pc.senders.push_back(timeout_sender);
    livekit::OutboundRtpStreamStats timeout_outbound;
    timeout_outbound.kind = "video";
    timeout_outbound.kind_available = true;
    timeout_outbound.mid = "timeout-mid";
    timeout_outbound.mid_available = true;
    timeout_outbound.frames_encoded_available = true;
    timeout_outbound.packets_sent_available = true;
    timeout_pc.outbound_rtp.push_back(timeout_outbound);
    timeout_report.reports.push_back(timeout_pc);
    asio::post(timeout_strand,
        [timeout_telemetry, &timed_out,
         report = std::move(timeout_report)]() mutable {
        timeout_telemetry->SetSnapshotCallbackOnStrand(
            [&timed_out](SessionTelemetry::SnapshotPtr value) {
                timed_out = std::move(value);
            });
        timeout_telemetry->StartStatsSamplingOnStrand(
            [report = std::move(report)](SessionTelemetry::LateCompletion) mutable
                -> asio::awaitable<livekit::RoomStatsReport> {
                co_return std::move(report);
            }, 1s);
    });
    timeout_context.run_for(100ms);
    TEST_CHECK(timed_out);
    TEST_CHECK(timed_out->local_publish_media_availability == Availability::Timeout);
    TEST_CHECK(timed_out->local_video_injection_availability == Availability::Timeout);
    TEST_CHECK(timed_out->local_video_encode_availability == Availability::Timeout);
    TEST_CHECK(timed_out->local_rtp_send_availability == Availability::Timeout);
    TEST_CHECK(timed_out->local_publish_no_media == 1);
    asio::post(timeout_strand,
        [timeout_telemetry] { timeout_telemetry->StopOnStrand(); });
    timeout_context.restart();
    timeout_context.run();

    asio::io_context collision_context;
    auto collision_strand = asio::make_strand(collision_context);
    auto collision_telemetry = std::make_shared<SessionTelemetry>(
        collision_strand, 205, 64);
    SessionTelemetry::SnapshotPtr collision_snapshot;
    auto collision_probe =
        std::make_shared<livekit::telemetry::LocalVideoActivityProbe>();
    TEST_CHECK(collision_telemetry->RegisterLocalPublication(
        "local_publish/collision", 10, 1,
        livekit::telemetry::LocalMediaKind::Video,
        "collision", expired, true, collision_probe, expired + 10ms));
    livekit::RoomStatsReport collision_report;
    collision_report.actual_peer_connection_count = 2;
    collision_report.successful_peer_connection_count = 2;
    livekit::StatsReport sender_pc;
    sender_pc.senders_available = true;
    livekit::RtpSenderDiagnostic collision_sender;
    collision_sender.track_id = "collision";
    collision_sender.kind = "video";
    collision_sender.mid = "shared-mid";
    collision_sender.mid_available = true;
    collision_sender.track_enabled = true;
    sender_pc.senders.push_back(collision_sender);
    livekit::StatsReport unrelated_pc;
    livekit::OutboundRtpStreamStats unrelated_outbound;
    unrelated_outbound.kind = "video";
    unrelated_outbound.kind_available = true;
    unrelated_outbound.mid = "shared-mid";
    unrelated_outbound.mid_available = true;
    unrelated_outbound.frames_encoded = 1;
    unrelated_outbound.frames_encoded_available = true;
    unrelated_outbound.packets_sent = 1;
    unrelated_outbound.packets_sent_available = true;
    unrelated_pc.outbound_rtp.push_back(unrelated_outbound);
    collision_report.reports.push_back(std::move(sender_pc));
    collision_report.reports.push_back(std::move(unrelated_pc));
    asio::post(collision_strand,
        [collision_telemetry, &collision_snapshot,
         report = std::move(collision_report)]() mutable {
        collision_telemetry->SetSnapshotCallbackOnStrand(
            [&collision_snapshot](SessionTelemetry::SnapshotPtr value) {
                collision_snapshot = std::move(value);
            });
        collision_telemetry->StartStatsSamplingOnStrand(
            [report = std::move(report)](SessionTelemetry::LateCompletion) mutable
                -> asio::awaitable<livekit::RoomStatsReport> {
                co_return std::move(report);
            }, 1s);
    });
    collision_context.run_for(100ms);
    TEST_CHECK(collision_snapshot);
    TEST_CHECK(collision_snapshot->local_video_encode_availability ==
               Availability::Unknown);
    TEST_CHECK(collision_snapshot->local_rtp_send_availability ==
               Availability::Unknown);
    TEST_CHECK(collision_snapshot->local_publish_media_availability ==
               Availability::Unknown);
    TEST_CHECK(collision_snapshot->local_publish_no_media == 0);
    asio::post(collision_strand,
        [collision_telemetry] { collision_telemetry->StopOnStrand(); });
    collision_context.restart();
    collision_context.run();
}

void FifthBatchProductChainsAndDensitiesAreDeterministic() {
    asio::io_context context;
    auto strand = asio::make_strand(context);
    const auto base = Event::Clock::now();
    auto telemetry = std::make_shared<SessionTelemetry>(strand, 301, 128, base);
    SessionTelemetry::SnapshotPtr snapshot;

    const auto admission = telemetry->StartOperation(
        OperationKind::Admission, "admission", base + 100ms);
    const auto startup = telemetry->StartOperation(
        OperationKind::Startup, "startup", base + 200ms);
    auto video = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    auto audio = std::make_shared<livekit::telemetry::AudioActivityProbe>();
    auto render = std::make_shared<livekit::telemetry::RenderActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/fifth/video", 1, 1, base + 300ms,
        true, true, video, base + 300ms));
    TEST_CHECK(telemetry->RegisterRemoteAudioBinding(
        "remote_audio/fifth/audio", 1, 1, base + 350ms,
        true, audio, base + 350ms));
    TEST_CHECK(telemetry->RegisterRemoteVideoRenderBinding(
        "remote_render/fifth/video", 1, 1, base + 300ms,
        true, true, 33ms, render, base + 300ms));
    TEST_CHECK(telemetry->RecordRemoteVideoFrame(
        "remote_video/fifth/video", 1, 1, 0, false,
        1280, 720, base + 450ms));
    TEST_CHECK(telemetry->RecordRemoteVideoRenderSubmit(
        "remote_render/fifth/video", 1, 1, 1, base + 490ms,
        "qt_cpu_paint", render, base + 500ms));
    context.run();

    context.restart();
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 600ms);
    });
    context.run();
    TEST_CHECK(snapshot);
    TEST_CHECK(snapshot->admission_to_usable_availability ==
               Availability::WarmingUp);
    TEST_CHECK(snapshot->subscription_media_availability ==
               Availability::WarmingUp);
    TEST_CHECK(snapshot->expected_remote_subscriptions == 2);
    TEST_CHECK(snapshot->delivered_remote_subscriptions == 1);
    TEST_CHECK(snapshot->longest_subscription_media_wait_ms == 250);
    TEST_CHECK(snapshot->last_admission_to_first_render_ms == 400);

    context.restart();
    TEST_CHECK(telemetry->FinishOperation(
        startup, OperationKind::Startup, OperationOutcome::Success,
        base + 700ms));
    TEST_CHECK(telemetry->FinishOperation(
        admission, OperationKind::Admission, OperationOutcome::Success,
        base + 710ms));
    TEST_CHECK(telemetry->RecordRemoteAudioFrame(
        "remote_audio/fifth/audio", 1, 1, 0, false,
        48000, 2, base + 750ms));
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 800ms);
    });
    context.run();
    TEST_CHECK(snapshot->admission_to_usable_availability == Availability::Valid);
    TEST_CHECK(snapshot->admission_to_usable_ms == 600);
    TEST_CHECK(snapshot->subscription_media_availability == Availability::Valid);
    TEST_CHECK(snapshot->delivered_remote_subscriptions == 2);
    TEST_CHECK(snapshot->remote_subscription_no_media == 0);
    TEST_CHECK(snapshot->longest_subscription_media_wait_ms == 400);

    context.restart();
    auto missing = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    TEST_CHECK(telemetry->RegisterRemoteVideoBinding(
        "remote_video/fifth/missing", 1, 2, base + 900ms,
        true, true, missing, base + 900ms));
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 5901ms);
    });
    context.run();
    TEST_CHECK(snapshot->subscription_media_availability == Availability::Timeout);
    TEST_CHECK(snapshot->expected_remote_subscriptions == 3);
    TEST_CHECK(snapshot->delivered_remote_subscriptions == 2);
    TEST_CHECK(snapshot->remote_subscription_no_media == 1);
    TEST_CHECK(snapshot->longest_subscription_media_wait_ms == 5001);

    std::set<std::string> metric_ids;
    std::size_t implemented = 0;
    std::size_t partial = 0;
    std::size_t unsupported = 0;
    std::size_t harness_only = 0;
    std::size_t deferred = 0;
    for (const auto& product_chain : snapshot->metric_product_chains) {
        TEST_CHECK(metric_ids.insert(product_chain.metric_id).second);
        switch (product_chain.status) {
        case ProductChainStatus::Implemented: ++implemented; break;
        case ProductChainStatus::Partial: ++partial; break;
        case ProductChainStatus::Unsupported: ++unsupported; break;
        case ProductChainStatus::ControlledHarnessOnly: ++harness_only; break;
        case ProductChainStatus::DeferredExternal: ++deferred; break;
        }
    }
    TEST_CHECK(metric_ids.size() == 33);
    TEST_CHECK(implemented == 5);
    TEST_CHECK(partial == 15);
    TEST_CHECK(unsupported == 6);
    TEST_CHECK(harness_only == 4);
    TEST_CHECK(deferred == 3);
    TEST_CHECK(metric_ids.contains("SES-03"));
    TEST_CHECK(metric_ids.contains("E2E-06"));

    context.restart();
    const auto failed_admission = telemetry->StartOperation(
        OperationKind::Admission, "admission-failed", base + 6000ms);
    const auto failed_startup = telemetry->StartOperation(
        OperationKind::Startup, "startup-failed", base + 6100ms);
    TEST_CHECK(!failed_admission.empty());
    TEST_CHECK(telemetry->FinishOperation(
        failed_startup, OperationKind::Startup, OperationOutcome::Failure,
        base + 6300ms));
    asio::post(strand, [telemetry, &snapshot, base] {
        snapshot = telemetry->SnapshotOnStrand(base + 6400ms);
    });
    context.run();
    TEST_CHECK(snapshot->admission_to_usable_availability == Availability::Invalid);
    TEST_CHECK(snapshot->admission_to_usable_reason == "startup_terminal_failure");
    TEST_CHECK(snapshot->admission_to_usable_ms == -1);
    TEST_CHECK(snapshot->last_admission_to_first_render_ms == -1);

    asio::io_context density_context;
    auto density_strand = asio::make_strand(density_context);
    const auto density_base = base + 1h;
    auto density = std::make_shared<SessionTelemetry>(
        density_strand, 302, 64, density_base);
    SessionTelemetry::SnapshotPtr density_snapshot;
    const auto connect = density->StartOperation(
        OperationKind::Connect, "connect", density_base);
    TEST_CHECK(density->FinishOperation(
        connect, OperationKind::Connect, OperationOutcome::Success,
        density_base + 100ms));
    const auto publish = density->StartOperation(
        OperationKind::PublishTrack, "publish", density_base + 200ms);
    TEST_CHECK(density->FinishOperation(
        publish, OperationKind::PublishTrack, OperationOutcome::Failure,
        density_base + 250ms));
    Event stats_timeout;
    stats_timeout.kind = EventKind::StatsRequestTimeout;
    stats_timeout.session_generation = 302;
    stats_timeout.source_time = density_base + 300ms;
    TEST_CHECK(density->Submit(std::move(stats_timeout)));
    Event stats_rejected;
    stats_rejected.kind = EventKind::StatsRequestRejected;
    stats_rejected.session_generation = 302;
    stats_rejected.source_time = density_base + 400ms;
    TEST_CHECK(density->Submit(std::move(stats_rejected)));
    auto no_media = std::make_shared<livekit::telemetry::VideoActivityProbe>();
    TEST_CHECK(density->RegisterRemoteVideoBinding(
        "remote_video/density/missing", 2, 1, density_base + 500ms,
        true, true, no_media, density_base + 500ms));
    const auto reconnect = density->StartOperation(
        OperationKind::ReconnectEpisode, "reconnect",
        density_base + 3600100ms);
    TEST_CHECK(!reconnect.empty());
    asio::post(density_strand, [density, &density_snapshot, density_base] {
        density_snapshot = density->SnapshotOnStrand(
            density_base + 3600100ms);
    });
    density_context.run();

    TEST_CHECK(density_snapshot);
    TEST_CHECK(density_snapshot->usable_duration_ms == 3600000);
    TEST_CHECK(density_snapshot->reconnect_density_availability ==
               Availability::Valid);
    TEST_CHECK(density_snapshot->reconnect_episodes == 1);
    TEST_CHECK(std::abs(density_snapshot->reconnect_episodes_per_hour - 1.0) <
               1e-9);
    TEST_CHECK(density_snapshot->stability_operation_failures == 1);
    TEST_CHECK(density_snapshot->stability_sampler_interruptions == 2);
    TEST_CHECK(density_snapshot->stability_device_stops == 0);
    TEST_CHECK(density_snapshot->stability_media_failures == 1);
    TEST_CHECK(density_snapshot->stability_anomalies == 4);
    TEST_CHECK(std::abs(density_snapshot->stability_anomalies_per_hour - 4.0) <
               1e-9);
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
    NetworkPathRecoveryAndQualityUseWindowedNativeCounters();
    MissingRecoveryAndQualityCountersRemainUnavailable();
    LocalDeviceContinuityUsesPublicationEpochAndRunningIntent();
    AudioQoeFollowsExpectedMediaState();
    AudioFirstFrameRejectsLateBindingCallbacks();
    ReconnectWaitsForAudioAndVisibleRender();
    ReconnectTimeoutPreservesRecoveredMedia();
    RenderSubmitDedupesAndExcludesHiddenIntervals();
    CanvasTimingCountsOperationsInsteadOfVideoResources();
    CanvasTimingAttributesMixedSessionsIndependently();
    CanvasTimingSurvivesBindingReplacementAndStopsWithSession();
    VideoPolicyRevisionAndRetirementAreSessionScoped();
    ProcessResourceSamplerUsesNormalizedCpuAndTypedAvailability();
    RuntimeSamplingMeasuresLagAndRejectsLateUiProbes();
    ResourceTrendIsBoundedAndExitReturnStaysHonest();
    SessionDurationsAndDisconnectHaveOneTerminal();
    LocalPublishPipelinePreservesWarmingTimeoutAndMapping();
    FifthBatchProductChainsAndDensitiesAreDeterministic();
    return 0;
}
