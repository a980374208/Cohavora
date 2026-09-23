#include "session_telemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace livekit::telemetry {

namespace {

std::int64_t ToNanoseconds(SessionTelemetry::Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count();
}

SessionTelemetry::Clock::time_point FromNanoseconds(std::int64_t value) {
    return SessionTelemetry::Clock::time_point(std::chrono::nanoseconds(value));
}

std::int64_t MillisecondsBetween(
    SessionTelemetry::Clock::time_point begin,
    SessionTelemetry::Clock::time_point end) {
    return (std::max)(std::int64_t{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
}

template <typename T>
void AtomicMaximum(std::atomic<T>& target, T value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed)) {
    }
}

template <typename T>
std::int64_t SaturatingSignedDelta(T current, T baseline) {
    const auto positive_limit = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
    if (current >= baseline) {
        const auto delta = static_cast<std::uint64_t>(current - baseline);
        return delta > positive_limit
            ? (std::numeric_limits<std::int64_t>::max)()
            : static_cast<std::int64_t>(delta);
    }
    const auto delta = static_cast<std::uint64_t>(baseline - current);
    return delta > positive_limit
        ? (std::numeric_limits<std::int64_t>::min)()
        : -static_cast<std::int64_t>(delta);
}

} // namespace

const char* AvailabilityName(Availability availability) noexcept {
    switch (availability) {
    case Availability::Valid: return "VALID";
    case Availability::WarmingUp: return "WARMING_UP";
    case Availability::NotExpected: return "NOT_EXPECTED";
    case Availability::Unsupported: return "UNSUPPORTED";
    case Availability::Timeout: return "TIMEOUT";
    case Availability::Stale: return "STALE";
    case Availability::Invalid: return "INVALID";
    case Availability::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* OperationKindName(OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::Admission: return "admission";
    case OperationKind::Connect: return "connect";
    case OperationKind::Startup: return "startup";
    case OperationKind::PublishBatch: return "publish_batch";
    case OperationKind::PublishTrack: return "publish_track";
    case OperationKind::Subscribe: return "subscribe";
    case OperationKind::Unsubscribe: return "unsubscribe";
    case OperationKind::Unpublish: return "unpublish";
    case OperationKind::ReconnectEpisode: return "reconnect_episode";
    case OperationKind::ReconnectAttempt: return "reconnect_attempt";
    case OperationKind::CameraDeviceSwitch: return "camera_device_switch";
    case OperationKind::MicrophoneDeviceSwitch: return "microphone_device_switch";
    case OperationKind::SpeakerDeviceSwitch: return "speaker_device_switch";
    case OperationKind::Unknown: return "unknown";
    }
    return "unknown";
}

const char* OperationOutcomeName(OperationOutcome outcome) noexcept {
    switch (outcome) {
    case OperationOutcome::Success: return "success";
    case OperationOutcome::DegradedSuccess: return "degraded_success";
    case OperationOutcome::Failure: return "failure";
    case OperationOutcome::Timeout: return "timeout";
    case OperationOutcome::Cancelled: return "cancelled";
    case OperationOutcome::None: return "none";
    }
    return "none";
}

const char* MediaExpectationReasonName(MediaExpectationReason reason) noexcept {
    switch (reason) {
    case MediaExpectationReason::BindingActive: return "binding_active";
    case MediaExpectationReason::Subscribed: return "subscribed";
    case MediaExpectationReason::Unsubscribed: return "unsubscribed";
    case MediaExpectationReason::Muted: return "muted";
    case MediaExpectationReason::Unmuted: return "unmuted";
    case MediaExpectationReason::StreamPaused: return "stream_paused";
    case MediaExpectationReason::StreamActive: return "stream_active";
    case MediaExpectationReason::PermissionDenied: return "subscription_permission_denied";
    case MediaExpectationReason::PermissionAllowed: return "subscription_permission_allowed";
    case MediaExpectationReason::PublicationEnded: return "publication_ended";
    case MediaExpectationReason::SurfaceVisible: return "surface_visible";
    case MediaExpectationReason::SurfaceHidden: return "surface_hidden";
    case MediaExpectationReason::WindowMinimized: return "window_minimized";
    case MediaExpectationReason::RenderBindingEnded: return "render_binding_ended";
    case MediaExpectationReason::VolumeZero: return "volume_zero";
    case MediaExpectationReason::VolumeAudible: return "volume_audible";
    case MediaExpectationReason::OutputMuted: return "output_muted";
    case MediaExpectationReason::OutputUnmuted: return "output_unmuted";
    }
    return "binding_active";
}

SessionTelemetry::SessionTelemetry(
    Strand strand,
    std::uint64_t session_generation,
    std::size_t queue_capacity)
    : strand_(std::move(strand))
    , session_generation_(session_generation)
    , queue_capacity_((std::max)(std::size_t{1}, queue_capacity))
    , stats_timer_(strand_)
    , runtime_timer_(strand_) {
    state_.session_generation = session_generation_;
    state_.queue_capacity = queue_capacity_;
    latest_snapshot_ = std::make_shared<const Snapshot>(state_);
}

bool SessionTelemetry::Submit(Event event) {
    if (!accepting_.load(std::memory_order_acquire)) {
        producer_stopped_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (event.session_generation == 0) {
        event.session_generation = session_generation_;
    }
    if (event.sequence == 0) {
        event.sequence = next_event_sequence_.fetch_add(1, std::memory_order_relaxed);
    }
    if (event.source_time == Clock::time_point{}) {
        event.source_time = Clock::now();
    }
    if (event.enqueued_at == Clock::time_point{}) {
        event.enqueued_at = Clock::now();
    }

    bool should_post = false;
    {
        std::lock_guard lock(queue_mutex_);
        if (!accepting_.load(std::memory_order_relaxed)) {
            producer_stopped_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (queue_.size() >= queue_capacity_) {
            producer_capacity_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(event));
        const auto depth = queue_.size();
        auto high_water = producer_high_water_.load(std::memory_order_relaxed);
        while (depth > high_water &&
               !producer_high_water_.compare_exchange_weak(
                   high_water, depth, std::memory_order_relaxed)) {
        }
        if (!drain_posted_) {
            drain_posted_ = true;
            should_post = true;
        }
    }
    if (should_post) {
        ScheduleDrain();
    }
    return true;
}

std::string SessionTelemetry::StartOperation(
    OperationKind kind,
    std::string id_prefix,
    Clock::time_point source_time) {
    if (kind == OperationKind::Unknown) return {};
    if (id_prefix.empty()) id_prefix = OperationKindName(kind);
    const auto id = id_prefix + ":" + std::to_string(session_generation_) + ":" +
        std::to_string(next_operation_id_.fetch_add(1, std::memory_order_relaxed));
    Event event;
    event.kind = EventKind::OperationStarted;
    event.session_generation = session_generation_;
    event.source_time = source_time;
    event.operation_id = id;
    event.operation_kind = kind;
    if (kind == OperationKind::ReconnectEpisode) {
        event.recovery_epoch = next_recovery_epoch_.fetch_add(
            1, std::memory_order_relaxed);
        active_recovery_epoch_.store(event.recovery_epoch, std::memory_order_release);
    }
    if (Submit(std::move(event))) return id;
    if (kind == OperationKind::ReconnectEpisode) {
        active_recovery_epoch_.store(0, std::memory_order_release);
    }
    return {};
}

bool SessionTelemetry::FinishOperation(
    const std::string& operation_id,
    OperationKind kind,
    OperationOutcome outcome,
    Clock::time_point source_time) {
    if (operation_id.empty() || outcome == OperationOutcome::None) return false;
    Event event;
    event.kind = EventKind::OperationTerminal;
    event.session_generation = session_generation_;
    event.source_time = source_time;
    event.operation_id = operation_id;
    event.operation_kind = kind;
    event.operation_outcome = outcome;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordRoomConnectAccepted(
    std::uint64_t room_generation,
    Clock::time_point source_time) {
    if (room_generation == 0) return false;
    Event event;
    event.kind = EventKind::RoomConnectAccepted;
    event.session_generation = session_generation_;
    event.room_generation = room_generation;
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RegisterRemoteVideoBinding(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    Clock::time_point subscription_accepted,
    bool expected_receive,
    bool continuous_video,
    std::shared_ptr<VideoActivityProbe> probe,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0 || !probe) {
        return false;
    }
    Event event;
    event.kind = EventKind::RemoteVideoBinding;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.related_time = subscription_accepted;
    event.expected = expected_receive;
    event.continuous_video = continuous_video;
    event.video_probe = std::move(probe);
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::SetRemoteVideoExpected(
    std::string series_key,
    bool expected_receive,
    MediaExpectationReason reason,
    Clock::time_point source_time) {
    if (series_key.empty()) return false;
    Event event;
    event.kind = EventKind::RemoteVideoExpectation;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.expected = expected_receive;
    event.expectation_reason = reason;
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordRemoteVideoFrame(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    std::uint64_t recovery_epoch,
    bool recovery_stable,
    std::uint32_t width,
    std::uint32_t height,
    Clock::time_point source_time,
    Clock::time_point last_good_at) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0) {
        return false;
    }
    Event event;
    event.kind = EventKind::RemoteVideoFrame;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.recovery_epoch = recovery_epoch;
    event.recovery_stable = recovery_stable;
    event.frame_width = width;
    event.frame_height = height;
    event.source_time = source_time;
    event.related_time = last_good_at;
    return Submit(std::move(event));
}

bool SessionTelemetry::RegisterRemoteAudioBinding(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    Clock::time_point subscription_accepted,
    bool expected_receive,
    std::shared_ptr<AudioActivityProbe> probe,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0 || !probe) {
        return false;
    }
    Event event;
    event.kind = EventKind::RemoteAudioBinding;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.related_time = subscription_accepted;
    event.expected = expected_receive;
    event.audio_probe = std::move(probe);
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::SetRemoteAudioExpected(
    std::string series_key,
    bool expected_receive,
    MediaExpectationReason reason,
    Clock::time_point source_time) {
    if (series_key.empty()) return false;
    Event event;
    event.kind = EventKind::RemoteAudioExpectation;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.expected = expected_receive;
    event.expectation_reason = reason;
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordRemoteAudioFrame(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    std::uint64_t recovery_epoch,
    bool recovery_stable,
    std::uint32_t sample_rate,
    std::uint32_t channels,
    Clock::time_point source_time,
    Clock::time_point last_good_at) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0) {
        return false;
    }
    Event event;
    event.kind = EventKind::RemoteAudioFrame;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.recovery_epoch = recovery_epoch;
    event.recovery_stable = recovery_stable;
    event.sample_rate = sample_rate;
    event.channels = channels;
    event.source_time = source_time;
    event.related_time = last_good_at;
    return Submit(std::move(event));
}

bool SessionTelemetry::RegisterRemoteVideoRenderBinding(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    Clock::time_point subscription_accepted,
    bool expected_render,
    bool continuous_video,
    std::chrono::milliseconds target_interval,
    std::shared_ptr<RenderActivityProbe> probe,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0 || !probe) {
        return false;
    }
    probe->expected.store(expected_render, std::memory_order_release);
    probe->continuous_video = continuous_video;
    probe->target_interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        (std::max)(std::chrono::milliseconds(1), target_interval)).count();
    Event event;
    event.kind = EventKind::RemoteVideoRenderBinding;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.related_time = subscription_accepted;
    event.expected = expected_render;
    event.continuous_video = continuous_video;
    event.target_interval_ms = target_interval.count();
    event.render_probe = std::move(probe);
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::SetRemoteVideoRenderExpected(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    std::shared_ptr<RenderActivityProbe> probe,
    bool expected_render,
    MediaExpectationReason reason,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0 || !probe) {
        return false;
    }
    const bool previous = probe->expected.exchange(
        expected_render, std::memory_order_acq_rel);
    if (previous != expected_render) {
        // The interval while a surface is hidden or minimized is outside the
        // render-stall denominator and cannot bridge two visible intervals.
        probe->last_submit_ns.store(0, std::memory_order_release);
    }
    Event event;
    event.kind = EventKind::RemoteVideoRenderExpectation;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.render_probe = std::move(probe);
    event.expected = expected_render;
    event.expectation_reason = reason;
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordRemoteVideoRenderSubmit(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t binding_epoch,
    std::uint64_t frame_token,
    Clock::time_point decoded_at,
    std::string measurement_point,
    std::shared_ptr<RenderActivityProbe> probe,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || binding_epoch == 0 ||
        frame_token == 0 || !probe ||
        !probe->active.load(std::memory_order_acquire) ||
        !probe->expected.load(std::memory_order_acquire)) {
        return false;
    }

    auto previous_token = probe->last_token.load(std::memory_order_acquire);
    while (frame_token > previous_token &&
           !probe->last_token.compare_exchange_weak(
               previous_token, frame_token, std::memory_order_acq_rel)) {
    }
    if (frame_token <= previous_token) return false;

    const auto submit_ns = ToNanoseconds(source_time);
    const auto previous_ns = probe->last_submit_ns.exchange(
        submit_ns, std::memory_order_acq_rel);
    probe->unique_submits.fetch_add(1, std::memory_order_relaxed);
    auto first_ns = std::int64_t{0};
    probe->first_submit_ns.compare_exchange_strong(
        first_ns, submit_ns, std::memory_order_release, std::memory_order_relaxed);
    if (previous_ns > 0 && submit_ns >= previous_ns) {
        const auto interval = submit_ns - previous_ns;
        probe->interval_sum_ns.fetch_add(interval, std::memory_order_relaxed);
        probe->interval_count.fetch_add(1, std::memory_order_relaxed);
        AtomicMaximum(probe->maximum_interval_ns, interval);
        if (probe->continuous_video) {
            const auto threshold = (std::max)(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::milliseconds(500)).count(),
                probe->target_interval_ns * 3);
            if (interval > threshold) {
                const auto stall = interval - threshold;
                probe->closed_stalls.fetch_add(1, std::memory_order_relaxed);
                probe->closed_stall_duration_ns.fetch_add(
                    stall, std::memory_order_relaxed);
                AtomicMaximum(probe->longest_stall_duration_ns, stall);
            }
        }
    }

    const auto recovery_epoch = ActiveRecoveryEpoch();
    const bool first = !probe->first_event_submitted.exchange(
        true, std::memory_order_acq_rel);
    auto observed_epoch = probe->recovery_epoch.load(std::memory_order_acquire);
    if (observed_epoch != recovery_epoch) {
        probe->recovery_epoch.store(recovery_epoch, std::memory_order_release);
        probe->recovery_first_ns.store(0, std::memory_order_release);
        probe->recovery_last_good_ns.store(previous_ns, std::memory_order_release);
        probe->recovery_stable_submitted.store(false, std::memory_order_release);
        observed_epoch = recovery_epoch;
    }
    const auto recovery_first_ns = probe->recovery_first_ns.load(
        std::memory_order_acquire);
    const bool recovery_first = recovery_epoch != 0 && recovery_first_ns == 0;
    const bool recovery_stable = recovery_epoch != 0 && recovery_first_ns != 0 &&
        !probe->recovery_stable_submitted.load(std::memory_order_acquire) &&
        (!probe->continuous_video || submit_ns - recovery_first_ns >=
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kRecoveryStableWindow).count());
    if (!first && !recovery_first && !recovery_stable) return true;

    Event event;
    event.kind = EventKind::RemoteVideoRenderSubmit;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.binding_epoch = binding_epoch;
    event.frame_token = frame_token;
    event.recovery_epoch = recovery_epoch;
    event.recovery_stable = recovery_stable;
    event.source_time = source_time;
    event.related_time = decoded_at;
    event.measurement_point = std::move(measurement_point);
    event.render_probe = probe;
    if (!Submit(std::move(event))) return false;
    if (recovery_first) {
        probe->recovery_first_ns.store(submit_ns, std::memory_order_release);
    }
    if (recovery_stable) {
        probe->recovery_stable_submitted.store(true, std::memory_order_release);
    }
    return true;
}

void SessionTelemetry::SetSnapshotCallbackOnStrand(SnapshotCallback callback) {
    AssertOnStrand();
    snapshot_callback_ = std::move(callback);
    PublishSnapshotOnStrand(Clock::now());
}

void SessionTelemetry::StartStatsSamplingOnStrand(
    StatsProvider provider,
    std::chrono::milliseconds interval) {
    AssertOnStrand();
    stats_provider_ = std::move(provider);
    stats_interval_ = (std::max)(std::chrono::milliseconds(10), interval);
    stale_after_ = (std::max)(stats_interval_ * 3, std::chrono::milliseconds(100));
    stats_started_ = true;
    RequestStatsSampleOnStrand();
    ScheduleNextStatsTickOnStrand();
}

void SessionTelemetry::RequestStatsSampleOnStrand() {
    AssertOnStrand();
    if (!accepting_.load(std::memory_order_acquire) || !stats_provider_) {
        return;
    }
    if (stats_in_flight_) {
        Event skipped;
        skipped.kind = EventKind::StatsRequestSkipped;
        skipped.session_generation = session_generation_;
        skipped.source_time = Clock::now();
        ApplyOnStrand(skipped);
        PublishSnapshotOnStrand(skipped.source_time);
        return;
    }

    stats_in_flight_ = true;
    const auto request_started = Clock::now();
    Event started;
    started.kind = EventKind::StatsRequestStarted;
    started.session_generation = session_generation_;
    started.source_time = request_started;
    ApplyOnStrand(started);
    PublishSnapshotOnStrand(request_started);

    auto provider = stats_provider_;
    const auto completion_strand = strand_;
    const auto weak = weak_from_this();
    LateCompletion late = [weak, generation = session_generation_] {
        if (const auto owner = weak.lock()) {
            Event event;
            event.kind = EventKind::LateCallback;
            event.session_generation = generation;
            event.source_time = Clock::now();
            owner->Submit(std::move(event));
        }
    };
    asio::co_spawn(
        strand_,
        [weak, completion_strand, provider = std::move(provider),
         late = std::move(late), request_started]() mutable
            -> asio::awaitable<void> {
            RoomStatsReport report;
            bool failed = false;
            try {
                report = co_await provider(std::move(late));
            } catch (...) {
                failed = true;
            }
            // Providers may bridge WebRTC callbacks through a raw io_context
            // executor, so their completion is not guaranteed to resume this
            // coroutine on the session strand.
            co_await asio::post(completion_strand, asio::use_awaitable);
            if (const auto owner = weak.lock()) {
                owner->CompleteStatsOnStrand(
                    std::move(report), request_started, failed);
            }
            co_return;
        },
        asio::detached);
}

void SessionTelemetry::StartRuntimeSamplingOnStrand(
    ResourceProvider resource_provider,
    UiProbeDispatcher ui_probe_dispatcher,
    std::chrono::milliseconds interval) {
    AssertOnStrand();
    resource_provider_ = std::move(resource_provider);
    ui_probe_dispatcher_ = std::move(ui_probe_dispatcher);
    runtime_interval_ = (std::max)(std::chrono::milliseconds(10), interval);
    runtime_stale_after_ = (std::max)(
        runtime_interval_ * 3, std::chrono::milliseconds(100));
    runtime_started_ = true;
    const auto now = Clock::now();
    RequestRuntimeSampleOnStrand(now, now);
    ScheduleNextRuntimeTickOnStrand();
}

void SessionTelemetry::RequestRuntimeSampleOnStrand(
    Clock::time_point scheduled_at,
    Clock::time_point observed_at) {
    AssertOnStrand();
    if (!accepting_.load(std::memory_order_acquire) || !runtime_started_) return;

    const auto strand_lag = MillisecondsBetween(scheduled_at, observed_at);
    state_.strand_lag_availability = Availability::Valid;
    state_.strand_lag_reason = "session_strand_timer_lag_valid";
    ++state_.strand_lag_samples;
    state_.last_strand_lag_ms = strand_lag;
    state_.maximum_strand_lag_ms = (std::max)(
        state_.maximum_strand_lag_ms, strand_lag);

    ProcessResourceSample resource_sample;
    if (resource_provider_) {
        try {
            resource_sample = resource_provider_();
        } catch (...) {
            resource_sample.captured_at = observed_at;
            resource_sample.availability = Availability::Invalid;
            resource_sample.reason = "process_resource_provider_failed";
        }
    } else {
        resource_sample.captured_at = observed_at;
        resource_sample.availability = Availability::Unsupported;
        resource_sample.reason = "process_resource_provider_not_configured";
    }
    ApplyProcessResourceSampleOnStrand(std::move(resource_sample), observed_at);

    if (ui_probe_in_flight_id_ != 0) {
        const auto pending_for = observed_at >= ui_probe_dispatched_at_
            ? observed_at - ui_probe_dispatched_at_ : Clock::duration::zero();
        if (pending_for >= runtime_stale_after_) {
            ++state_.ui_probe_timeouts;
            state_.ui_lag_availability = Availability::Timeout;
            state_.ui_lag_reason = "ui_probe_timeout";
            ui_probe_in_flight_id_ = 0;
            state_.ui_probe_in_flight = false;
        } else {
            ++state_.ui_probe_skipped;
        }
    }

    if (ui_probe_in_flight_id_ == 0 && ui_probe_dispatcher_) {
        const auto probe_id = next_ui_probe_id_++;
        ui_probe_in_flight_id_ = probe_id;
        ui_probe_dispatched_at_ = observed_at;
        state_.ui_probe_in_flight = true;
        if (state_.ui_lag_availability == Availability::Unknown) {
            state_.ui_lag_availability = Availability::WarmingUp;
            state_.ui_lag_reason = "ui_probe_in_flight";
        }
        try {
            ui_probe_dispatcher_(session_generation_, probe_id, observed_at);
        } catch (...) {
            ui_probe_in_flight_id_ = 0;
            state_.ui_probe_in_flight = false;
            state_.ui_lag_availability = Availability::Invalid;
            state_.ui_lag_reason = "ui_probe_dispatch_failed";
        }
    } else if (!ui_probe_dispatcher_) {
        state_.ui_lag_availability = Availability::Unsupported;
        state_.ui_lag_reason = "ui_probe_dispatcher_not_configured";
    }

    PublishSnapshotOnStrand(observed_at);
}

bool SessionTelemetry::CompleteUiLagProbe(
    std::uint64_t session_generation,
    std::uint64_t probe_id,
    Clock::time_point executed_at) {
    Event event;
    event.kind = EventKind::UiLagProbeCompleted;
    event.session_generation = session_generation;
    event.epoch = probe_id;
    event.source_time = executed_at;
    return Submit(std::move(event));
}

void SessionTelemetry::RefreshOnStrand(Clock::time_point now) {
    AssertOnStrand();
    MaybeFinishReconnectOnStrand(now);
    UpdateRenderAvailabilityOnStrand(now);
    PublishSnapshotOnStrand(now);
}

void SessionTelemetry::StopOnStrand(std::function<void()> on_stopped) {
    AssertOnStrand();
    if (on_stopped) {
        stop_callbacks_.push_back(std::move(on_stopped));
    }
    if (stop_finalized_) {
        auto callbacks = std::move(stop_callbacks_);
        for (auto& callback : callbacks) callback();
        return;
    }
    stopping_ = true;
    accepting_.store(false, std::memory_order_release);
    active_recovery_epoch_.store(0, std::memory_order_release);
    stats_started_ = false;
    stats_provider_ = {};
    runtime_started_ = false;
    resource_provider_ = {};
    ui_probe_dispatcher_ = {};
    ui_probe_in_flight_id_ = 0;
    state_.ui_probe_in_flight = false;
    std::error_code ignored;
    stats_timer_.cancel(ignored);
    runtime_timer_.cancel(ignored);
    DrainOnStrand();
    if (!stats_in_flight_) {
        FinalizeStopOnStrand();
    }
}

SessionTelemetry::SnapshotPtr SessionTelemetry::SnapshotOnStrand(Clock::time_point now) {
    AssertOnStrand();
    UpdateRenderAvailabilityOnStrand(now);
    const auto build_started_at = Clock::now();
    auto snapshot = BuildSnapshotOnStrand(now);
    const auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - build_started_at).count();
    ++state_.telemetry_snapshot_publications;
    state_.last_snapshot_build_us = build_us;
    state_.maximum_snapshot_build_us = (std::max)(
        state_.maximum_snapshot_build_us, build_us);
    state_.total_snapshot_build_us += build_us;
    PopulateTelemetryCostOnSnapshot(snapshot, now);
    latest_snapshot_ = std::make_shared<const Snapshot>(std::move(snapshot));
    return latest_snapshot_;
}

void SessionTelemetry::AssertOnStrand() const {
    if (!strand_.running_in_this_thread()) {
        std::terminate();
    }
}

void SessionTelemetry::ScheduleDrain() {
    const auto weak = weak_from_this();
    asio::post(strand_, [weak] {
        if (const auto owner = weak.lock()) {
            owner->DrainOnStrand();
        }
    });
}

void SessionTelemetry::DrainOnStrand() {
    AssertOnStrand();
    std::deque<Event> pending;
    {
        std::lock_guard lock(queue_mutex_);
        pending.swap(queue_);
        drain_posted_ = false;
    }

    state_.capacity_drops +=
        producer_capacity_drops_.exchange(0, std::memory_order_relaxed);
    state_.stopped_drops +=
        producer_stopped_drops_.exchange(0, std::memory_order_relaxed);
    state_.queue_high_water = (std::max)(
        state_.queue_high_water,
        producer_high_water_.load(std::memory_order_relaxed));

    for (const auto& event : pending) {
        if (event.enqueued_at != Clock::time_point{}) {
            const auto queue_lag = MillisecondsBetween(
                event.enqueued_at, Clock::now());
            state_.event_queue_lag_availability = Availability::Valid;
            state_.event_queue_lag_reason = "bounded_event_queue_lag_valid";
            ++state_.event_queue_lag_samples;
            state_.last_event_queue_lag_ms = queue_lag;
            state_.maximum_event_queue_lag_ms = (std::max)(
                state_.maximum_event_queue_lag_ms, queue_lag);
        }
        ApplyOnStrand(event);
    }
    PublishSnapshotOnStrand(Clock::now());

    bool should_post = false;
    {
        std::lock_guard lock(queue_mutex_);
        if (!queue_.empty() && !drain_posted_) {
            drain_posted_ = true;
            should_post = true;
        }
    }
    if (should_post) {
        ScheduleDrain();
    }
}

void SessionTelemetry::ApplyOnStrand(const Event& event) {
    AssertOnStrand();
    if (event.session_generation != session_generation_) {
        ++state_.stale_generation_drops;
        return;
    }

    switch (event.kind) {
    case EventKind::GaugeSample:
        state_.last_sample_at = event.source_time;
        state_.availability = event.availability;
        state_.reason = event.availability == Availability::Valid
            ? "sample_valid" : "sample_unavailable";
        if (event.availability == Availability::Valid) ++state_.valid_samples;
        else ++state_.unavailable_samples;
        break;
    case EventKind::CounterSample: {
        if (event.series_key.empty() || !event.counter_value) {
            ++state_.mapping_failures;
            ++state_.unavailable_samples;
            state_.availability = Availability::Invalid;
            state_.reason = "counter_missing_identity_or_value";
            break;
        }
        auto found = series_.find(event.series_key);
        if (found == series_.end()) {
            if (series_.size() >= kMaxSeries) {
                ++state_.capacity_drops;
                break;
            }
            found = series_.emplace(event.series_key, SeriesState{}).first;
        }
        auto& series = found->second;
        if (series.last_sequence != 0 && event.sequence <= series.last_sequence) {
            ++state_.out_of_order_drops;
            break;
        }
        series.last_sequence = event.sequence;
        state_.last_sample_at = event.source_time;
        if (!series.has_counter || series.epoch != event.epoch) {
            if (series.has_counter) ++state_.counter_resets;
            series.epoch = event.epoch;
            series.counter_value = *event.counter_value;
            series.has_counter = true;
            state_.availability = Availability::WarmingUp;
            state_.reason = "counter_warmup";
            ++state_.unavailable_samples;
        } else if (*event.counter_value < series.counter_value) {
            series.counter_value = *event.counter_value;
            ++state_.counter_resets;
            ++state_.unavailable_samples;
            state_.availability = Availability::Invalid;
            state_.reason = "counter_reset_or_unproven_wrap";
        } else {
            series.counter_value = *event.counter_value;
            ++state_.valid_samples;
            state_.availability = Availability::Valid;
            state_.reason = "counter_window_valid";
        }
        break;
    }
    case EventKind::StatsRequestStarted:
        ++state_.stats_requests_started;
        state_.stats_in_flight = true;
        if (state_.availability == Availability::Unknown) {
            state_.availability = Availability::WarmingUp;
            state_.reason = "stats_request_started";
        }
        break;
    case EventKind::StatsRequestCompleted:
        ++state_.stats_requests_completed;
        state_.stats_in_flight = false;
        state_.actual_pc_count = event.expected_pc_count;
        state_.successful_pc_count = event.successful_pc_count;
        state_.stats_request_timeouts += event.timed_out_pc_count;
        state_.stats_request_rejections += event.rejected_pc_count;
        state_.last_stats_request_ms = event.duration_ms;
        state_.last_sample_at = event.source_time;
        if (event.expected_pc_count == 0) {
            state_.availability = Availability::NotExpected;
            state_.reason = "no_peer_connection";
            state_.coverage = 0.0;
        } else if (event.successful_pc_count == event.expected_pc_count) {
            state_.availability = Availability::Valid;
            state_.reason = "stats_complete";
            state_.coverage = 1.0;
            ++state_.valid_samples;
        } else if (event.successful_pc_count > 0) {
            state_.availability = Availability::Valid;
            state_.reason = "stats_partial_coverage";
            state_.coverage = static_cast<double>(event.successful_pc_count) /
                static_cast<double>(event.expected_pc_count);
            ++state_.valid_samples;
        } else if (event.timed_out_pc_count > 0) {
            state_.availability = Availability::Timeout;
            state_.reason = "stats_timeout";
            state_.coverage = 0.0;
            ++state_.unavailable_samples;
        } else {
            state_.availability = Availability::Unsupported;
            state_.reason = "stats_request_rejected";
            state_.coverage = 0.0;
            ++state_.unavailable_samples;
        }
        break;
    case EventKind::StatsRequestTimeout:
        ++state_.stats_request_timeouts;
        state_.stats_in_flight = false;
        state_.availability = Availability::Timeout;
        state_.reason = "stats_provider_timeout";
        ++state_.unavailable_samples;
        break;
    case EventKind::StatsRequestRejected:
        ++state_.stats_request_rejections;
        state_.stats_in_flight = false;
        state_.availability = Availability::Unsupported;
        state_.reason = "stats_provider_rejected";
        ++state_.unavailable_samples;
        break;
    case EventKind::StatsRequestSkipped:
        ++state_.stats_requests_skipped;
        break;
    case EventKind::LateCallback:
        ++state_.late_callbacks;
        break;
    case EventKind::MappingFailure:
        ++state_.mapping_failures;
        break;
    case EventKind::OperationStarted: {
        if (event.operation_id.empty() ||
            event.operation_kind == OperationKind::Unknown) {
            ++state_.operations_missing_start;
            break;
        }
        const auto found = operations_.find(event.operation_id);
        if (found != operations_.end()) {
            break;
        }
        if (found == operations_.end() && operations_.size() >= kMaxOperations) {
            const auto terminal = std::min_element(
                operations_.begin(), operations_.end(),
                [](const auto& lhs, const auto& rhs) {
                    if (lhs.second.terminal != rhs.second.terminal) {
                        return lhs.second.terminal;
                    }
                    return lhs.second.sequence < rhs.second.sequence;
                });
            if (terminal == operations_.end() || !terminal->second.terminal) {
                ++state_.capacity_drops;
                break;
            }
            operations_.erase(terminal);
        }
        operations_[event.operation_id] = OperationState{
            false, event.sequence, event.operation_kind,
            OperationOutcome::None, event.source_time, event.recovery_epoch};
        ++state_.operations_started;
        ++state_.operations_inflight;
        auto summary = std::find_if(
            state_.operation_summaries.begin(), state_.operation_summaries.end(),
            [&](const OperationSummary& candidate) {
                return candidate.kind == event.operation_kind;
            });
        if (summary == state_.operation_summaries.end()) {
            state_.operation_summaries.push_back(OperationSummary{});
            summary = std::prev(state_.operation_summaries.end());
            summary->kind = event.operation_kind;
        }
        ++summary->started;
        ++summary->inflight;
        if (event.operation_kind == OperationKind::ReconnectEpisode) {
            recovery_ = RecoveryState{};
            recovery_.active = true;
            recovery_.epoch = event.recovery_epoch;
            recovery_.operation_id = event.operation_id;
            recovery_.outage_started_at = event.source_time;
            for (const auto& [key, media] : media_) {
                if (!media.expected_receive || !media.probe ||
                    !media.probe->active.load(std::memory_order_acquire)) {
                    continue;
                }
                RecoveryTrackState track;
                const auto last_ns = media.probe->last_frame_ns.load(
                    std::memory_order_acquire);
                if (last_ns > 0) {
                    const auto last_frame = FromNanoseconds(last_ns);
                    if (last_frame <= event.source_time) {
                        track.last_good_at = last_frame;
                    }
                }
                recovery_.tracks.emplace(key, track);
            }
            for (const auto& [key, media] : audio_media_) {
                if (!media.expected_receive || !media.probe ||
                    !media.probe->active.load(std::memory_order_acquire)) {
                    continue;
                }
                RecoveryTrackState track;
                const auto last_ns = media.probe->last_frame_ns.load(
                    std::memory_order_acquire);
                if (last_ns > 0) {
                    const auto last_frame = FromNanoseconds(last_ns);
                    if (last_frame <= event.source_time) track.last_good_at = last_frame;
                }
                recovery_.audio_tracks.emplace(key, track);
            }
            for (const auto& [key, media] : render_media_) {
                if (!media.expected_render || !media.probe ||
                    !media.probe->active.load(std::memory_order_acquire)) {
                    continue;
                }
                RecoveryTrackState track;
                const auto last_ns = media.probe->last_submit_ns.load(
                    std::memory_order_acquire);
                if (last_ns > 0) {
                    const auto last_frame = FromNanoseconds(last_ns);
                    if (last_frame <= event.source_time) track.last_good_at = last_frame;
                }
                recovery_.render_tracks.emplace(key, track);
            }
            state_.reconnect_video_expected = recovery_.tracks.size();
            state_.reconnect_video_recovered = 0;
            state_.reconnect_audio_expected = recovery_.audio_tracks.size();
            state_.reconnect_audio_recovered = 0;
            state_.reconnect_render_expected = recovery_.render_tracks.size();
            state_.reconnect_render_recovered = 0;
            state_.reconnect_expectation_changes = 0;
            state_.last_reconnect_signaling_ms = -1;
            state_.last_reconnect_first_video_ms = -1;
            state_.last_reconnect_stable_video_ms = -1;
            state_.last_reconnect_media_interruption_ms = -1;
            state_.last_reconnect_first_audio_ms = -1;
            state_.last_reconnect_stable_audio_ms = -1;
            state_.last_reconnect_audio_interruption_ms = -1;
            state_.last_reconnect_first_render_ms = -1;
            state_.last_reconnect_stable_render_ms = -1;
            state_.last_reconnect_render_interruption_ms = -1;
            state_.reconnect_video_availability = recovery_.tracks.empty()
                ? Availability::NotExpected : Availability::WarmingUp;
            state_.reconnect_video_reason = recovery_.tracks.empty()
                ? "no_remote_video_expected_at_outage"
                : "outage_detected_waiting_for_video";
            state_.reconnect_audio_availability = recovery_.audio_tracks.empty()
                ? Availability::NotExpected : Availability::WarmingUp;
            state_.reconnect_audio_reason = recovery_.audio_tracks.empty()
                ? "no_remote_audio_expected_at_outage"
                : "outage_detected_waiting_for_audio";
            state_.reconnect_render_availability = recovery_.render_tracks.empty()
                ? Availability::NotExpected : Availability::WarmingUp;
            state_.reconnect_render_reason = recovery_.render_tracks.empty()
                ? "no_visible_render_expected_at_outage"
                : "outage_detected_waiting_for_render";
        }
        break;
    }
    case EventKind::OperationTerminal: {
        const auto found = operations_.find(event.operation_id);
        if (found == operations_.end()) {
            ++state_.operations_missing_start;
        } else if (found->second.terminal) {
            ++state_.operations_duplicate_terminal;
        } else if (event.operation_kind != OperationKind::Unknown &&
                   event.operation_kind != found->second.kind) {
            ++state_.operations_kind_mismatch;
        } else if (found->second.kind == OperationKind::ReconnectEpisode &&
                   event.operation_outcome == OperationOutcome::Success &&
                   recovery_.active &&
                   recovery_.operation_id == event.operation_id) {
            if (recovery_.signaling_restored) {
                ++state_.operations_duplicate_terminal;
                break;
            }
            recovery_.signaling_restored = true;
            recovery_.signaling_restored_at = event.source_time;
            state_.last_reconnect_signaling_ms = MillisecondsBetween(
                recovery_.outage_started_at, event.source_time);
            if (!recovery_.tracks.empty()) {
                state_.reconnect_video_availability = Availability::WarmingUp;
                state_.reconnect_video_reason = "signaling_restored_waiting_for_video";
            }
            if (!recovery_.audio_tracks.empty()) {
                state_.reconnect_audio_availability = Availability::WarmingUp;
                state_.reconnect_audio_reason = "signaling_restored_waiting_for_audio";
            }
            if (!recovery_.render_tracks.empty()) {
                state_.reconnect_render_availability = Availability::WarmingUp;
                state_.reconnect_render_reason = "signaling_restored_waiting_for_render";
            }
            MaybeFinishReconnectOnStrand(event.source_time);
        } else {
            const auto outcome = event.operation_outcome == OperationOutcome::None
                ? OperationOutcome::Failure : event.operation_outcome;
            if (found->second.kind == OperationKind::ReconnectEpisode &&
                recovery_.active && recovery_.operation_id == event.operation_id) {
                state_.reconnect_video_availability = outcome == OperationOutcome::Timeout
                    ? Availability::Timeout : Availability::Invalid;
                state_.reconnect_video_reason = outcome == OperationOutcome::Cancelled
                    ? "reconnect_cancelled" : "reconnect_failed_before_media_recovery";
                state_.reconnect_audio_availability = state_.reconnect_video_availability;
                state_.reconnect_audio_reason = state_.reconnect_video_reason;
                state_.reconnect_render_availability = state_.reconnect_video_availability;
                state_.reconnect_render_reason = state_.reconnect_video_reason;
                recovery_.active = false;
                active_recovery_epoch_.store(0, std::memory_order_release);
            }
            FinishOperationOnStrand(found->second, outcome, event.source_time);
        }
        break;
    }
    case EventKind::RoomConnectAccepted:
        room_connect_starts_[event.room_generation] = event.source_time;
        state_.room_connect_to_first_decoded_ms = -1;
        if (!media_.empty()) {
            state_.remote_video_first_frame_availability = Availability::WarmingUp;
            state_.remote_video_first_frame_reason = "waiting_for_decoded_frame";
        }
        break;
    case EventKind::RemoteVideoBinding: {
        if (event.series_key.empty() || event.room_generation == 0 ||
            event.binding_epoch == 0 || !event.video_probe) {
            ++state_.mapping_failures;
            break;
        }
        auto found = media_.find(event.series_key);
        if (found == media_.end() && media_.size() >= kMaxSeries) {
            ++state_.capacity_drops;
            break;
        }
        if (found != media_.end() && found->second.probe) {
            found->second.probe->active.store(false, std::memory_order_release);
        }
        MediaState media;
        media.room_generation = event.room_generation;
        media.binding_epoch = event.binding_epoch;
        media.expected_receive = event.expected;
        media.continuous_video = event.continuous_video;
        media.subscription_accepted = event.related_time;
        media.probe = event.video_probe;
        media_[event.series_key] = std::move(media);
        state_.remote_video_bindings = media_.size();
        UpdateFirstFrameAvailabilityOnStrand();
        break;
    }
    case EventKind::RemoteVideoExpectation: {
        auto media = media_.find(event.series_key);
        if (media == media_.end()) {
            if (media_.size() >= kMaxSeries) {
                ++state_.capacity_drops;
                break;
            }
            media = media_.emplace(event.series_key, MediaState{}).first;
        }
        media->second.expected_receive = event.expected;
        if (!event.expected && recovery_.active) {
            const auto pending = recovery_.tracks.find(event.series_key);
            if (pending != recovery_.tracks.end()) {
                recovery_.tracks.erase(pending);
                recovery_.expectation_changed = true;
                recovery_.video_expectation_changed = true;
                ++state_.reconnect_expectation_changes;
                state_.reconnect_video_reason = MediaExpectationReasonName(
                    event.expectation_reason);
                MaybeFinishReconnectOnStrand(event.source_time);
            }
        }
        UpdateFirstFrameAvailabilityOnStrand();
        break;
    }
    case EventKind::RemoteVideoFrame: {
        const auto media = media_.find(event.series_key);
        if (media == media_.end() ||
            media->second.room_generation != event.room_generation ||
            media->second.binding_epoch != event.binding_epoch ||
            !media->second.probe ||
            !media->second.probe->active.load(std::memory_order_acquire)) {
            ++state_.stale_binding_frame_drops;
            break;
        }
        if (!media->second.first_frame_seen) {
            media->second.first_frame_seen = true;
            media->second.first_frame_at = event.source_time;
            ++state_.remote_video_first_frames;
            state_.remote_video_first_frame_availability = Availability::Valid;
            state_.remote_video_first_frame_reason = "decoded_frame_received";
            state_.last_decoded_width = event.frame_width;
            state_.last_decoded_height = event.frame_height;
            const auto connect = room_connect_starts_.find(event.room_generation);
            if (connect != room_connect_starts_.end() &&
                event.source_time >= connect->second) {
                state_.last_connect_to_first_decoded_ms = MillisecondsBetween(
                    connect->second, event.source_time);
                if (state_.room_connect_to_first_decoded_ms < 0) {
                    state_.room_connect_to_first_decoded_ms =
                        state_.last_connect_to_first_decoded_ms;
                }
            }
            if (media->second.subscription_accepted != Clock::time_point{} &&
                event.source_time >= media->second.subscription_accepted) {
                state_.last_subscribe_to_first_decoded_ms = MillisecondsBetween(
                    media->second.subscription_accepted, event.source_time);
            }
            UpdateFirstFrameAvailabilityOnStrand();
        }
        if (!recovery_.active || event.recovery_epoch == 0 ||
            event.recovery_epoch != recovery_.epoch ||
            event.source_time < recovery_.outage_started_at) {
            break;
        }
        const auto recovering = recovery_.tracks.find(event.series_key);
        if (recovering == recovery_.tracks.end() || recovering->second.recovered) {
            break;
        }
        auto& track = recovering->second;
        if (track.last_good_at == Clock::time_point{} &&
            event.related_time != Clock::time_point{} &&
            event.related_time <= recovery_.outage_started_at) {
            track.last_good_at = event.related_time;
        }
        if (track.first_recovered_at == Clock::time_point{}) {
            track.first_recovered_at = event.source_time;
            const auto first_ms = MillisecondsBetween(
                recovery_.outage_started_at, event.source_time);
            if (state_.last_reconnect_first_video_ms < 0 ||
                first_ms > state_.last_reconnect_first_video_ms) {
                state_.last_reconnect_first_video_ms = first_ms;
            }
        }
        const bool stable = !media->second.continuous_video ||
            (event.recovery_stable &&
             event.source_time - track.first_recovered_at >= kRecoveryStableWindow);
        if (stable) {
            track.recovered = true;
            track.stable_recovered_at = event.source_time;
            ++state_.reconnect_video_recovered;
            MaybeFinishReconnectOnStrand(event.source_time);
        }
        break;
    }
    case EventKind::RemoteAudioBinding: {
        if (event.series_key.empty() || event.room_generation == 0 ||
            event.binding_epoch == 0 || !event.audio_probe) {
            ++state_.mapping_failures;
            break;
        }
        auto found = audio_media_.find(event.series_key);
        if (found == audio_media_.end() && audio_media_.size() >= kMaxSeries) {
            ++state_.capacity_drops;
            break;
        }
        if (found != audio_media_.end() && found->second.probe) {
            found->second.probe->active.store(false, std::memory_order_release);
        }
        AudioMediaState media;
        media.room_generation = event.room_generation;
        media.binding_epoch = event.binding_epoch;
        media.expected_receive = event.expected;
        media.subscription_accepted = event.related_time;
        media.probe = event.audio_probe;
        audio_media_[event.series_key] = std::move(media);
        state_.remote_audio_bindings = audio_media_.size();
        UpdateAudioFirstFrameAvailabilityOnStrand();
        ReconcileAudioQualityExpectationOnStrand(true);
        break;
    }
    case EventKind::RemoteAudioExpectation: {
        auto media = audio_media_.find(event.series_key);
        if (media == audio_media_.end()) {
            if (audio_media_.size() >= kMaxSeries) {
                ++state_.capacity_drops;
                break;
            }
            media = audio_media_.emplace(event.series_key, AudioMediaState{}).first;
        }
        const bool expectation_changed =
            media->second.expected_receive != event.expected;
        media->second.expected_receive = event.expected;
        if (!event.expected && recovery_.active) {
            const auto pending = recovery_.audio_tracks.find(event.series_key);
            if (pending != recovery_.audio_tracks.end()) {
                recovery_.audio_tracks.erase(pending);
                recovery_.expectation_changed = true;
                recovery_.audio_expectation_changed = true;
                ++state_.reconnect_expectation_changes;
                state_.reconnect_audio_reason = MediaExpectationReasonName(
                    event.expectation_reason);
                MaybeFinishReconnectOnStrand(event.source_time);
            }
        }
        UpdateAudioFirstFrameAvailabilityOnStrand();
        if (expectation_changed) {
            ReconcileAudioQualityExpectationOnStrand(true);
        }
        break;
    }
    case EventKind::RemoteAudioFrame: {
        const auto media = audio_media_.find(event.series_key);
        if (media == audio_media_.end() ||
            media->second.room_generation != event.room_generation ||
            media->second.binding_epoch != event.binding_epoch ||
            !media->second.probe ||
            !media->second.probe->active.load(std::memory_order_acquire)) {
            ++state_.stale_audio_binding_drops;
            break;
        }
        if (!media->second.first_frame_seen) {
            media->second.first_frame_seen = true;
            ++state_.remote_audio_first_frames;
            state_.last_audio_sample_rate = event.sample_rate;
            state_.last_audio_channels = event.channels;
            if (media->second.subscription_accepted != Clock::time_point{} &&
                event.source_time >= media->second.subscription_accepted) {
                state_.last_subscribe_to_first_pcm_ms = MillisecondsBetween(
                    media->second.subscription_accepted, event.source_time);
            }
            UpdateAudioFirstFrameAvailabilityOnStrand();
        }
        if (!recovery_.active || event.recovery_epoch == 0 ||
            event.recovery_epoch != recovery_.epoch ||
            event.source_time < recovery_.outage_started_at) {
            break;
        }
        const auto recovering = recovery_.audio_tracks.find(event.series_key);
        if (recovering == recovery_.audio_tracks.end() || recovering->second.recovered) {
            break;
        }
        auto& track = recovering->second;
        if (track.last_good_at == Clock::time_point{} &&
            event.related_time != Clock::time_point{} &&
            event.related_time <= recovery_.outage_started_at) {
            track.last_good_at = event.related_time;
        }
        if (track.first_recovered_at == Clock::time_point{}) {
            track.first_recovered_at = event.source_time;
            state_.last_reconnect_first_audio_ms = (std::max)(
                state_.last_reconnect_first_audio_ms,
                MillisecondsBetween(recovery_.outage_started_at, event.source_time));
        }
        if (event.recovery_stable &&
            event.source_time - track.first_recovered_at >= kRecoveryStableWindow) {
            track.recovered = true;
            track.stable_recovered_at = event.source_time;
            ++state_.reconnect_audio_recovered;
            MaybeFinishReconnectOnStrand(event.source_time);
        }
        break;
    }
    case EventKind::RemoteVideoRenderBinding: {
        if (event.series_key.empty() || event.room_generation == 0 ||
            event.binding_epoch == 0 || !event.render_probe) {
            ++state_.mapping_failures;
            break;
        }
        auto found = render_media_.find(event.series_key);
        if (found == render_media_.end() && render_media_.size() >= kMaxSeries) {
            ++state_.capacity_drops;
            break;
        }
        if (found != render_media_.end() && found->second.probe) {
            found->second.probe->active.store(false, std::memory_order_release);
        }
        RenderState render;
        render.room_generation = event.room_generation;
        render.binding_epoch = event.binding_epoch;
        render.expected_render = event.expected;
        render.continuous_video = event.continuous_video;
        render.subscription_accepted = event.related_time;
        render.expected_since = event.expected ? event.source_time : Clock::time_point{};
        render.probe = event.render_probe;
        render_media_[event.series_key] = std::move(render);
        state_.render_bindings = render_media_.size();
        UpdateRenderAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::RemoteVideoRenderExpectation: {
        const auto media = render_media_.find(event.series_key);
        if (media == render_media_.end() ||
            media->second.room_generation != event.room_generation ||
            media->second.binding_epoch != event.binding_epoch ||
            !media->second.probe || media->second.probe != event.render_probe ||
            (event.expected &&
             !media->second.probe->active.load(std::memory_order_acquire))) {
            ++state_.stale_render_binding_drops;
            break;
        }
        auto& render = media->second;
        if (render.expected_render != event.expected) {
            if (render.expected_render && render.expected_since != Clock::time_point{} &&
                event.source_time >= render.expected_since) {
                render.expected_accumulated += event.source_time - render.expected_since;
            }
            render.expected_render = event.expected;
            render.expected_since = event.expected ? event.source_time : Clock::time_point{};
        }
        if (render.probe) {
            render.probe->expected.store(event.expected, std::memory_order_release);
        }
        if (!event.expected && recovery_.active) {
            const auto pending = recovery_.render_tracks.find(event.series_key);
            if (pending != recovery_.render_tracks.end()) {
                recovery_.render_tracks.erase(pending);
                recovery_.expectation_changed = true;
                recovery_.render_expectation_changed = true;
                ++state_.reconnect_expectation_changes;
                state_.reconnect_render_reason = MediaExpectationReasonName(
                    event.expectation_reason);
                MaybeFinishReconnectOnStrand(event.source_time);
            }
        }
        UpdateRenderAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::RemoteVideoRenderSubmit: {
        const auto media = render_media_.find(event.series_key);
        if (media == render_media_.end() ||
            media->second.room_generation != event.room_generation ||
            media->second.binding_epoch != event.binding_epoch ||
            !media->second.probe || media->second.probe != event.render_probe ||
            !media->second.probe->active.load(std::memory_order_acquire)) {
            ++state_.stale_render_binding_drops;
            break;
        }
        auto& render = media->second;
        if (!render.first_submit_seen) {
            render.first_submit_seen = true;
            state_.render_first_frame_availability = Availability::Valid;
            state_.render_first_frame_reason = "first_unique_frame_submitted";
            state_.render_first_frame_measurement_point = event.measurement_point;
            if (event.related_time != Clock::time_point{} &&
                event.source_time >= event.related_time) {
                state_.last_decode_to_render_ms = MillisecondsBetween(
                    event.related_time, event.source_time);
            }
            if (render.subscription_accepted != Clock::time_point{} &&
                event.source_time >= render.subscription_accepted) {
                state_.last_subscribe_to_first_render_ms = MillisecondsBetween(
                    render.subscription_accepted, event.source_time);
            }
            const auto connect = room_connect_starts_.find(event.room_generation);
            if (connect != room_connect_starts_.end() &&
                event.source_time >= connect->second) {
                state_.last_connect_to_first_render_ms = MillisecondsBetween(
                    connect->second, event.source_time);
            }
        }
        if (!recovery_.active || event.recovery_epoch == 0 ||
            event.recovery_epoch != recovery_.epoch ||
            event.source_time < recovery_.outage_started_at) {
            UpdateRenderAvailabilityOnStrand(event.source_time);
            break;
        }
        const auto recovering = recovery_.render_tracks.find(event.series_key);
        if (recovering == recovery_.render_tracks.end() || recovering->second.recovered) {
            UpdateRenderAvailabilityOnStrand(event.source_time);
            break;
        }
        auto& track = recovering->second;
        const auto last_good_ns = render.probe->recovery_last_good_ns.load(
            std::memory_order_acquire);
        if (track.last_good_at == Clock::time_point{} && last_good_ns > 0) {
            const auto last_good = FromNanoseconds(last_good_ns);
            if (last_good <= recovery_.outage_started_at) track.last_good_at = last_good;
        }
        if (track.first_recovered_at == Clock::time_point{}) {
            track.first_recovered_at = event.source_time;
            state_.last_reconnect_first_render_ms = (std::max)(
                state_.last_reconnect_first_render_ms,
                MillisecondsBetween(recovery_.outage_started_at, event.source_time));
        }
        if (event.recovery_stable &&
            (!render.continuous_video ||
             event.source_time - track.first_recovered_at >= kRecoveryStableWindow)) {
            track.recovered = true;
            track.stable_recovered_at = event.source_time;
            ++state_.reconnect_render_recovered;
            MaybeFinishReconnectOnStrand(event.source_time);
        }
        UpdateRenderAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::UiLagProbeCompleted:
        if (ui_probe_in_flight_id_ == 0 ||
            event.epoch != ui_probe_in_flight_id_) {
            ++state_.ui_probe_late_callbacks;
            break;
        }
        if (event.source_time < ui_probe_dispatched_at_) {
            state_.ui_lag_availability = Availability::Invalid;
            state_.ui_lag_reason = "ui_probe_clock_regressed";
        } else {
            const auto ui_lag = MillisecondsBetween(
                ui_probe_dispatched_at_, event.source_time);
            state_.ui_lag_availability = Availability::Valid;
            state_.ui_lag_reason = "qt_event_loop_delivery_lag_valid";
            ++state_.ui_lag_samples;
            state_.last_ui_lag_ms = ui_lag;
            state_.maximum_ui_lag_ms = (std::max)(
                state_.maximum_ui_lag_ms, ui_lag);
        }
        ui_probe_in_flight_id_ = 0;
        state_.ui_probe_in_flight = false;
        break;
    }
}

void SessionTelemetry::FinishOperationOnStrand(
    OperationState& operation,
    OperationOutcome outcome,
    Clock::time_point finished_at) {
    if (operation.terminal) return;
    operation.terminal = true;
    operation.outcome = outcome == OperationOutcome::None
        ? OperationOutcome::Failure : outcome;
    ++state_.operations_terminal;
    if (state_.operations_inflight > 0) --state_.operations_inflight;
    auto summary = std::find_if(
        state_.operation_summaries.begin(), state_.operation_summaries.end(),
        [&](const OperationSummary& candidate) {
            return candidate.kind == operation.kind;
        });
    if (summary == state_.operation_summaries.end()) {
        state_.operation_summaries.push_back(OperationSummary{});
        summary = std::prev(state_.operation_summaries.end());
        summary->kind = operation.kind;
    }
    ++summary->terminal;
    if (summary->inflight > 0) --summary->inflight;
    switch (operation.outcome) {
    case OperationOutcome::Success: ++summary->success; break;
    case OperationOutcome::DegradedSuccess: ++summary->degraded_success; break;
    case OperationOutcome::Failure: ++summary->failure; break;
    case OperationOutcome::Timeout: ++summary->timeout; break;
    case OperationOutcome::Cancelled: ++summary->cancelled; break;
    case OperationOutcome::None: ++summary->failure; break;
    }
    summary->last_duration_ms = MillisecondsBetween(
        operation.started_at, finished_at);
}

void SessionTelemetry::UpdateFirstFrameAvailabilityOnStrand() {
    AssertOnStrand();
    bool has_expected = false;
    bool has_waiting = false;
    for (const auto& [_, media] : media_) {
        if (!media.expected_receive) continue;
        has_expected = true;
        has_waiting |= !media.first_frame_seen;
    }
    if (has_waiting) {
        state_.remote_video_first_frame_availability = Availability::WarmingUp;
        state_.remote_video_first_frame_reason = "waiting_for_decoded_frame";
    } else if (has_expected) {
        state_.remote_video_first_frame_availability = Availability::Valid;
        state_.remote_video_first_frame_reason = "all_expected_bindings_decoded";
    } else if (!media_.empty()) {
        state_.remote_video_first_frame_availability = Availability::NotExpected;
        state_.remote_video_first_frame_reason = "no_remote_video_expected";
    } else {
        state_.remote_video_first_frame_availability = Availability::Unknown;
        state_.remote_video_first_frame_reason = "no_remote_video_binding";
    }
}

void SessionTelemetry::UpdateAudioFirstFrameAvailabilityOnStrand() {
    AssertOnStrand();
    bool has_expected = false;
    bool has_waiting = false;
    for (const auto& [_, media] : audio_media_) {
        if (!media.expected_receive) continue;
        has_expected = true;
        has_waiting |= !media.first_frame_seen;
    }
    if (has_waiting) {
        state_.remote_audio_first_frame_availability = Availability::WarmingUp;
        state_.remote_audio_first_frame_reason = "waiting_for_pcm";
    } else if (has_expected) {
        state_.remote_audio_first_frame_availability = Availability::Valid;
        state_.remote_audio_first_frame_reason = "all_expected_bindings_delivered_pcm";
    } else if (!audio_media_.empty()) {
        state_.remote_audio_first_frame_availability = Availability::NotExpected;
        state_.remote_audio_first_frame_reason = "no_remote_audio_expected";
    } else {
        state_.remote_audio_first_frame_availability = Availability::Unknown;
        state_.remote_audio_first_frame_reason = "no_remote_audio_binding";
    }
}

void SessionTelemetry::ReconcileAudioQualityExpectationOnStrand(
    bool reset_window) {
    AssertOnStrand();
    if (audio_media_.empty()) return;

    bool has_active = false;
    bool has_expected = false;
    for (const auto& [_, media] : audio_media_) {
        if (!media.probe ||
            !media.probe->active.load(std::memory_order_acquire)) {
            continue;
        }
        has_active = true;
        has_expected |= media.expected_receive;
    }
    if (has_active && has_expected && !reset_window) return;

    if (reset_window) audio_stats_baselines_.clear();
    state_.audio_window_samples = 0;
    state_.audio_window_concealed_samples = 0;
    state_.audio_window_silent_concealed_samples = 0;
    state_.audio_window_concealment_events = 0;
    state_.audio_inserted_samples = 0;
    state_.audio_removed_samples = 0;
    state_.audio_concealed_ratio = -1.0;
    state_.audio_non_silent_concealed_ratio = -1.0;
    state_.audio_jitter_buffer_delay_ms = -1.0;
    state_.audio_jitter_buffer_target_delay_ms = -1.0;
    state_.audio_jitter_buffer_minimum_delay_ms = -1.0;
    state_.audio_inserted_ratio = -1.0;
    state_.audio_removed_ratio = -1.0;

    const auto availability = !has_active
        ? Availability::Unknown
        : has_expected ? Availability::WarmingUp : Availability::NotExpected;
    const char* reason = !has_active
        ? "no_active_remote_audio_binding"
        : has_expected
            ? "audio_expectation_changed_baseline_warming_up"
            : "no_remote_audio_expected";
    state_.audio_quality_availability = availability;
    state_.audio_quality_reason = reason;
    state_.audio_concealment_availability = availability;
    state_.audio_concealment_reason = reason;
    state_.audio_jitter_buffer_availability = availability;
    state_.audio_jitter_buffer_reason = reason;
    state_.audio_time_stretch_availability = availability;
    state_.audio_time_stretch_reason = reason;
}

void SessionTelemetry::UpdateRenderAvailabilityOnStrand(Clock::time_point now) {
    AssertOnStrand();
    bool has_expected = false;
    bool has_waiting = false;
    bool has_continuous = false;
    std::uint64_t unique_submits = 0;
    std::uint64_t stalls = 0;
    std::int64_t stall_ns = 0;
    std::int64_t longest_stall_ns = 0;
    std::int64_t interval_sum_ns = 0;
    std::uint64_t interval_count = 0;
    std::int64_t maximum_interval_ns = 0;
    std::chrono::nanoseconds expected_duration{0};
    bool active_stall = false;

    for (const auto& [_, render] : render_media_) {
        if (!render.probe || !render.probe->active.load(std::memory_order_acquire)) {
            continue;
        }
        const auto& probe = *render.probe;
        unique_submits += probe.unique_submits.load(std::memory_order_relaxed);
        interval_sum_ns += probe.interval_sum_ns.load(std::memory_order_relaxed);
        interval_count += probe.interval_count.load(std::memory_order_relaxed);
        maximum_interval_ns = (std::max)(maximum_interval_ns,
            probe.maximum_interval_ns.load(std::memory_order_relaxed));
        stalls += probe.closed_stalls.load(std::memory_order_relaxed);
        stall_ns += probe.closed_stall_duration_ns.load(std::memory_order_relaxed);
        longest_stall_ns = (std::max)(longest_stall_ns,
            probe.longest_stall_duration_ns.load(std::memory_order_relaxed));
        expected_duration += render.expected_accumulated;
        if (!render.expected_render) continue;
        has_expected = true;
        has_waiting |= !render.first_submit_seen;
        has_continuous |= render.continuous_video;
        if (render.expected_since != Clock::time_point{} && now >= render.expected_since) {
            expected_duration += now - render.expected_since;
        }
        if (!render.continuous_video || !render.first_submit_seen) continue;
        const auto last_ns = probe.last_submit_ns.load(std::memory_order_acquire);
        if (last_ns <= 0) continue;
        const auto threshold_ns = (std::max)(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::milliseconds(500)).count(),
            probe.target_interval_ns * 3);
        const auto now_ns = ToNanoseconds(now);
        if (now_ns > last_ns + threshold_ns) {
            const auto ongoing_ns = now_ns - last_ns - threshold_ns;
            ++stalls;
            stall_ns += ongoing_ns;
            longest_stall_ns = (std::max)(longest_stall_ns, ongoing_ns);
            active_stall = true;
        }
    }

    state_.unique_render_submits = unique_submits;
    state_.render_average_interval_ms = interval_count == 0 ? -1.0
        : static_cast<double>(interval_sum_ns) /
            static_cast<double>(interval_count) / 1'000'000.0;
    state_.render_maximum_interval_ms = maximum_interval_ns == 0 ? -1
        : maximum_interval_ns / 1'000'000;
    state_.render_stall_count = stalls;
    state_.render_stall_duration_ms = stall_ns / 1'000'000;
    state_.render_longest_stall_ms = longest_stall_ns / 1'000'000;
    state_.render_expected_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(expected_duration).count();
    state_.render_stall_ratio = expected_duration.count() > 0
        ? static_cast<double>(stall_ns) / static_cast<double>(expected_duration.count())
        : -1.0;
    state_.render_stall_active = active_stall;

    if (has_waiting) {
        state_.render_first_frame_availability = Availability::WarmingUp;
        state_.render_first_frame_reason = "waiting_for_visible_submit";
    } else if (has_expected) {
        state_.render_first_frame_availability = Availability::Valid;
        state_.render_first_frame_reason = "all_expected_surfaces_submitted";
    } else if (!render_media_.empty()) {
        state_.render_first_frame_availability = Availability::NotExpected;
        state_.render_first_frame_reason = "no_visible_render_expected";
    } else {
        state_.render_first_frame_availability = Availability::Unknown;
        state_.render_first_frame_reason = "no_expected_render_binding";
    }

    if (!has_expected) {
        state_.render_stall_availability = render_media_.empty()
            ? Availability::Unknown : Availability::NotExpected;
        state_.render_stall_reason = render_media_.empty()
            ? "no_expected_render_binding" : "no_visible_render_expected";
    } else if (has_waiting) {
        state_.render_stall_availability = Availability::WarmingUp;
        state_.render_stall_reason = "waiting_for_first_submit";
    } else if (!has_continuous) {
        state_.render_stall_availability = Availability::NotExpected;
        state_.render_stall_reason = "static_content_not_classified";
    } else {
        state_.render_stall_availability = Availability::Valid;
        state_.render_stall_reason = active_stall
            ? "visible_render_stall_active" : "render_stall_window_valid";
    }
}

void SessionTelemetry::MaybeFinishReconnectOnStrand(Clock::time_point now) {
    AssertOnStrand();
    if (!recovery_.active || !recovery_.signaling_restored) return;
    const auto operation = operations_.find(recovery_.operation_id);
    if (operation == operations_.end() || operation->second.terminal) {
        recovery_.active = false;
        active_recovery_epoch_.store(0, std::memory_order_release);
        return;
    }

    const bool all_video_recovered = std::all_of(
        recovery_.tracks.begin(), recovery_.tracks.end(),
        [](const auto& item) { return item.second.recovered; });
    const bool all_audio_recovered = std::all_of(
        recovery_.audio_tracks.begin(), recovery_.audio_tracks.end(),
        [](const auto& item) { return item.second.recovered; });
    const bool all_render_recovered = std::all_of(
        recovery_.render_tracks.begin(), recovery_.render_tracks.end(),
        [](const auto& item) { return item.second.recovered; });

    const auto latest_stable = [this](const auto& tracks) {
        auto result = recovery_.signaling_restored_at;
        for (const auto& [_, track] : tracks) {
            if (track.recovered) {
                result = (std::max)(result, track.stable_recovered_at);
            }
        }
        return result;
    };
    const auto maximum_interruption = [](const auto& tracks) {
        std::int64_t result = -1;
        for (const auto& [_, track] : tracks) {
            if (track.recovered && track.last_good_at != Clock::time_point{} &&
                track.stable_recovered_at >= track.last_good_at) {
                result = (std::max)(result, MillisecondsBetween(
                    track.last_good_at, track.stable_recovered_at));
            }
        }
        return result;
    };
    const auto video_finished_at = latest_stable(recovery_.tracks);
    const auto audio_finished_at = latest_stable(recovery_.audio_tracks);
    const auto render_finished_at = latest_stable(recovery_.render_tracks);

    const auto settle_video = [&](bool observation_timed_out) {
        if (state_.reconnect_video_expected == 0) {
            state_.reconnect_video_availability = Availability::NotExpected;
            state_.reconnect_video_reason = "no_remote_video_expected_at_outage";
        } else if (recovery_.video_expectation_changed) {
            state_.reconnect_video_availability = Availability::NotExpected;
            state_.reconnect_video_reason = "expectation_changed_during_recovery";
        } else if (all_video_recovered) {
            state_.reconnect_video_availability = Availability::Valid;
            state_.reconnect_video_reason = "decoded_video_stably_recovered";
            state_.last_reconnect_stable_video_ms = MillisecondsBetween(
                recovery_.outage_started_at, video_finished_at);
            state_.last_reconnect_media_interruption_ms =
                maximum_interruption(recovery_.tracks);
        } else if (observation_timed_out) {
            state_.reconnect_video_availability = Availability::Timeout;
            state_.reconnect_video_reason =
                "signaling_restored_video_not_recovered";
        }
    };
    const auto settle_audio = [&](bool observation_timed_out) {
        if (state_.reconnect_audio_expected == 0) {
            state_.reconnect_audio_availability = Availability::NotExpected;
            state_.reconnect_audio_reason = "no_remote_audio_expected_at_outage";
        } else if (recovery_.audio_expectation_changed) {
            state_.reconnect_audio_availability = Availability::NotExpected;
            state_.reconnect_audio_reason = "expectation_changed_during_recovery";
        } else if (all_audio_recovered) {
            state_.reconnect_audio_availability = Availability::Valid;
            state_.reconnect_audio_reason = "pcm_stably_recovered";
            state_.last_reconnect_stable_audio_ms = MillisecondsBetween(
                recovery_.outage_started_at, audio_finished_at);
            state_.last_reconnect_audio_interruption_ms =
                maximum_interruption(recovery_.audio_tracks);
        } else if (observation_timed_out) {
            state_.reconnect_audio_availability = Availability::Timeout;
            state_.reconnect_audio_reason =
                "signaling_restored_audio_not_recovered";
        }
    };
    const auto settle_render = [&](bool observation_timed_out) {
        if (state_.reconnect_render_expected == 0) {
            state_.reconnect_render_availability = Availability::NotExpected;
            state_.reconnect_render_reason = "no_visible_render_expected_at_outage";
        } else if (recovery_.render_expectation_changed) {
            state_.reconnect_render_availability = Availability::NotExpected;
            state_.reconnect_render_reason = "expectation_changed_during_recovery";
        } else if (all_render_recovered) {
            state_.reconnect_render_availability = Availability::Valid;
            state_.reconnect_render_reason = "visible_render_stably_recovered";
            state_.last_reconnect_stable_render_ms = MillisecondsBetween(
                recovery_.outage_started_at, render_finished_at);
            state_.last_reconnect_render_interruption_ms =
                maximum_interruption(recovery_.render_tracks);
        } else if (observation_timed_out) {
            state_.reconnect_render_availability = Availability::Timeout;
            state_.reconnect_render_reason =
                "signaling_restored_render_not_recovered";
        }
    };

    if (all_video_recovered && all_audio_recovered && all_render_recovered) {
        settle_video(false);
        settle_audio(false);
        settle_render(false);
        auto finished_at = (std::max)(
            video_finished_at, (std::max)(audio_finished_at, render_finished_at));
        if (recovery_.expectation_changed) {
            finished_at = (std::max)(finished_at, now);
        }
        FinishOperationOnStrand(
            operation->second,
            recovery_.expectation_changed
                ? OperationOutcome::DegradedSuccess
                : OperationOutcome::Success,
            finished_at);
        recovery_.active = false;
        active_recovery_epoch_.store(0, std::memory_order_release);
        return;
    }

    if (now - recovery_.signaling_restored_at >= kRecoveryObservationWindow) {
        settle_video(true);
        settle_audio(true);
        settle_render(true);
        ++state_.reconnect_media_timeouts;
        FinishOperationOnStrand(
            operation->second, OperationOutcome::Timeout, now);
        recovery_.active = false;
        active_recovery_epoch_.store(0, std::memory_order_release);
    }
}

void SessionTelemetry::UpdateVideoStatsOnStrand(
    const RoomStatsReport& report,
    Clock::time_point received_at) {
    AssertOnStrand();
    std::uint64_t video_streams = 0;
    std::uint64_t supported_streams = 0;
    std::uint64_t freeze_count = 0;
    double freeze_duration_seconds = 0.0;
    for (const auto& pc_report : report.reports) {
        for (const auto& stream : pc_report.inbound_rtp) {
            if (!stream.kind_available || stream.kind != "video") continue;
            ++video_streams;
            if (!stream.freeze_count_available ||
                !stream.total_freezes_duration_available ||
                !std::isfinite(stream.total_freezes_duration) ||
                stream.total_freezes_duration < 0.0) {
                continue;
            }
            ++supported_streams;
            freeze_count += stream.freeze_count;
            freeze_duration_seconds += stream.total_freezes_duration;
        }
    }

    state_.native_video_streams = video_streams;
    if (video_streams == 0) {
        state_.native_video_freeze_availability = Availability::Unknown;
        state_.native_video_freeze_reason = "no_inbound_video_stats";
        state_.native_video_freeze_count = 0;
        state_.native_video_freeze_duration_ms = -1;
    } else if (supported_streams == 0) {
        state_.native_video_freeze_availability = Availability::Unsupported;
        state_.native_video_freeze_reason = "native_freeze_fields_missing";
        state_.native_video_freeze_count = 0;
        state_.native_video_freeze_duration_ms = -1;
    } else {
        state_.native_video_freeze_availability = Availability::Valid;
        state_.native_video_freeze_reason = supported_streams == video_streams
            ? "native_freeze_fields_valid"
            : "native_freeze_partial_coverage";
        state_.native_video_freeze_count = freeze_count;
        state_.native_video_freeze_duration_ms = static_cast<std::int64_t>(
            std::llround(freeze_duration_seconds * 1000.0));
    }
    state_.last_sample_at = received_at;
}

void SessionTelemetry::UpdateAudioStatsOnStrand(
    const RoomStatsReport& report,
    Clock::time_point received_at) {
    AssertOnStrand();
    std::uint64_t audio_streams = 0;
    std::uint64_t concealment_supported = 0;
    std::uint64_t concealment_warmed = 0;
    std::uint64_t jitter_supported = 0;
    std::uint64_t jitter_warmed = 0;
    std::uint64_t stretch_supported = 0;
    std::uint64_t stretch_warmed = 0;
    std::uint64_t concealment_sample_delta = 0;
    std::uint64_t stretch_sample_delta = 0;
    std::uint64_t concealed_delta = 0;
    std::uint64_t silent_delta = 0;
    std::uint64_t event_delta = 0;
    std::uint64_t inserted_delta = 0;
    std::uint64_t removed_delta = 0;
    std::uint64_t emitted_delta = 0;
    double jitter_delay_delta = 0.0;
    double jitter_target_delta = 0.0;
    double jitter_minimum_delta = 0.0;
    std::set<std::string> seen;

    std::size_t pc_index = 0;
    for (const auto& pc_report : report.reports) {
        for (const auto& stream : pc_report.inbound_rtp) {
            if (!stream.kind_available || stream.kind != "audio") continue;
            ++audio_streams;
            const auto key = std::to_string(pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            seen.insert(key);
            const bool concealment_fields = stream.total_samples_received_available &&
                stream.concealed_samples_available &&
                stream.silent_concealed_samples_available &&
                stream.concealment_events_available;
            const bool stretch_fields = stream.total_samples_received_available &&
                stream.inserted_samples_for_deceleration_available &&
                stream.removed_samples_for_acceleration_available;
            const bool jitter_fields = stream.jitter_buffer_delay_available &&
                stream.jitter_buffer_target_delay_available &&
                stream.jitter_buffer_minimum_delay_available &&
                stream.jitter_buffer_emitted_count_available &&
                std::isfinite(stream.jitter_buffer_delay) &&
                std::isfinite(stream.jitter_buffer_target_delay) &&
                std::isfinite(stream.jitter_buffer_minimum_delay) &&
                stream.jitter_buffer_delay >= 0.0 &&
                stream.jitter_buffer_target_delay >= 0.0 &&
                stream.jitter_buffer_minimum_delay >= 0.0;
            auto& baseline = audio_stats_baselines_[key];
            bool reset = false;

            if (concealment_fields) {
                ++concealment_supported;
                if (baseline.concealment_initialized &&
                    (stream.total_samples_received < baseline.total_samples ||
                     stream.concealed_samples < baseline.concealed ||
                     stream.silent_concealed_samples < baseline.silent_concealed ||
                     stream.concealment_events < baseline.concealment_events)) {
                    baseline.concealment_initialized = false;
                    reset = true;
                }
                if (baseline.concealment_initialized) {
                    ++concealment_warmed;
                    concealment_sample_delta +=
                        stream.total_samples_received - baseline.total_samples;
                    concealed_delta += stream.concealed_samples - baseline.concealed;
                    silent_delta +=
                        stream.silent_concealed_samples - baseline.silent_concealed;
                    event_delta +=
                        stream.concealment_events - baseline.concealment_events;
                }
                baseline.concealed = stream.concealed_samples;
                baseline.silent_concealed = stream.silent_concealed_samples;
                baseline.concealment_events = stream.concealment_events;
                baseline.concealment_initialized = true;
            } else {
                baseline.concealment_initialized = false;
            }

            if (stretch_fields) {
                ++stretch_supported;
                if (baseline.time_stretch_initialized &&
                    (stream.total_samples_received < baseline.total_samples ||
                     stream.inserted_samples_for_deceleration < baseline.inserted ||
                     stream.removed_samples_for_acceleration < baseline.removed)) {
                    baseline.time_stretch_initialized = false;
                    reset = true;
                }
                if (baseline.time_stretch_initialized) {
                    ++stretch_warmed;
                    stretch_sample_delta +=
                        stream.total_samples_received - baseline.total_samples;
                    inserted_delta +=
                        stream.inserted_samples_for_deceleration - baseline.inserted;
                    removed_delta +=
                        stream.removed_samples_for_acceleration - baseline.removed;
                }
                baseline.inserted = stream.inserted_samples_for_deceleration;
                baseline.removed = stream.removed_samples_for_acceleration;
                baseline.time_stretch_initialized = true;
            } else {
                baseline.time_stretch_initialized = false;
            }

            if (jitter_fields) {
                ++jitter_supported;
                if (baseline.jitter_initialized &&
                    (stream.jitter_buffer_emitted_count < baseline.jitter_emitted ||
                     stream.jitter_buffer_delay < baseline.jitter_delay ||
                     stream.jitter_buffer_target_delay < baseline.jitter_target ||
                     stream.jitter_buffer_minimum_delay < baseline.jitter_minimum)) {
                    baseline.jitter_initialized = false;
                    reset = true;
                }
                if (baseline.jitter_initialized) {
                    ++jitter_warmed;
                    emitted_delta +=
                        stream.jitter_buffer_emitted_count - baseline.jitter_emitted;
                    jitter_delay_delta +=
                        stream.jitter_buffer_delay - baseline.jitter_delay;
                    jitter_target_delta +=
                        stream.jitter_buffer_target_delay - baseline.jitter_target;
                    jitter_minimum_delta +=
                        stream.jitter_buffer_minimum_delay - baseline.jitter_minimum;
                }
                baseline.jitter_emitted = stream.jitter_buffer_emitted_count;
                baseline.jitter_delay = stream.jitter_buffer_delay;
                baseline.jitter_target = stream.jitter_buffer_target_delay;
                baseline.jitter_minimum = stream.jitter_buffer_minimum_delay;
                baseline.jitter_initialized = true;
            } else {
                baseline.jitter_initialized = false;
            }

            if (stream.total_samples_received_available) {
                baseline.total_samples = stream.total_samples_received;
            }
            if (reset) ++state_.counter_resets;
        }
        ++pc_index;
    }
    for (auto it = audio_stats_baselines_.begin(); it != audio_stats_baselines_.end();) {
        if (!seen.count(it->first)) it = audio_stats_baselines_.erase(it);
        else ++it;
    }

    state_.audio_streams = audio_streams;
    state_.audio_window_samples = concealment_sample_delta;
    state_.audio_window_concealed_samples = concealed_delta;
    state_.audio_window_silent_concealed_samples = silent_delta;
    state_.audio_window_concealment_events = event_delta;
    state_.audio_inserted_samples = inserted_delta;
    state_.audio_removed_samples = removed_delta;
    state_.audio_concealed_ratio = concealment_sample_delta == 0 ? -1.0
        : static_cast<double>(concealed_delta) /
            static_cast<double>(concealment_sample_delta);
    const auto non_silent_concealed = concealed_delta >= silent_delta
        ? concealed_delta - silent_delta : 0;
    state_.audio_non_silent_concealed_ratio = concealment_sample_delta == 0 ? -1.0
        : static_cast<double>(non_silent_concealed) /
            static_cast<double>(concealment_sample_delta);
    state_.audio_inserted_ratio = stretch_sample_delta == 0 ? -1.0
        : static_cast<double>(inserted_delta) /
            static_cast<double>(stretch_sample_delta);
    state_.audio_removed_ratio = stretch_sample_delta == 0 ? -1.0
        : static_cast<double>(removed_delta) /
            static_cast<double>(stretch_sample_delta);
    state_.audio_jitter_buffer_delay_ms = emitted_delta == 0 ? -1.0
        : jitter_delay_delta * 1000.0 / static_cast<double>(emitted_delta);
    state_.audio_jitter_buffer_target_delay_ms = emitted_delta == 0 ? -1.0
        : jitter_target_delta * 1000.0 / static_cast<double>(emitted_delta);
    state_.audio_jitter_buffer_minimum_delay_ms = emitted_delta == 0 ? -1.0
        : jitter_minimum_delta * 1000.0 / static_cast<double>(emitted_delta);

    const auto set_group_availability = [audio_streams](
            std::uint64_t supported,
            std::uint64_t warmed,
            Availability& availability,
            std::string& reason,
            const char* missing,
            const char* warming,
            const char* valid,
            const char* partial) {
        if (audio_streams == 0) {
            availability = Availability::Unknown;
            reason = "no_inbound_audio_stats";
        } else if (supported == 0) {
            availability = Availability::Unsupported;
            reason = missing;
        } else if (warmed == 0) {
            availability = Availability::WarmingUp;
            reason = warming;
        } else {
            availability = Availability::Valid;
            reason = supported == audio_streams && warmed == audio_streams
                ? valid : partial;
        }
    };
    set_group_availability(
        concealment_supported, concealment_warmed,
        state_.audio_concealment_availability,
        state_.audio_concealment_reason,
        "concealment_fields_missing", "concealment_baseline_warming_up",
        "concealment_window_valid", "concealment_partial_coverage");
    set_group_availability(
        jitter_supported, jitter_warmed,
        state_.audio_jitter_buffer_availability,
        state_.audio_jitter_buffer_reason,
        "jitter_buffer_fields_missing", "jitter_buffer_baseline_warming_up",
        "jitter_buffer_window_valid", "jitter_buffer_partial_coverage");
    set_group_availability(
        stretch_supported, stretch_warmed,
        state_.audio_time_stretch_availability,
        state_.audio_time_stretch_reason,
        "time_stretch_fields_missing", "time_stretch_baseline_warming_up",
        "time_stretch_window_valid", "time_stretch_partial_coverage");

    const auto supported_groups = concealment_supported + jitter_supported +
        stretch_supported;
    const auto warmed_groups = concealment_warmed + jitter_warmed + stretch_warmed;
    if (audio_streams == 0) {
        state_.audio_quality_availability = Availability::Unknown;
        state_.audio_quality_reason = "no_inbound_audio_stats";
    } else if (supported_groups == 0) {
        state_.audio_quality_availability = Availability::Unsupported;
        state_.audio_quality_reason = "audio_quality_fields_missing";
    } else if (warmed_groups == 0) {
        state_.audio_quality_availability = Availability::WarmingUp;
        state_.audio_quality_reason = "audio_delta_baseline_warming_up";
    } else {
        state_.audio_quality_availability = Availability::Valid;
        state_.audio_quality_reason = concealment_supported == audio_streams &&
            concealment_warmed == audio_streams &&
            jitter_supported == audio_streams && jitter_warmed == audio_streams &&
            stretch_supported == audio_streams && stretch_warmed == audio_streams
            ? "audio_quality_window_valid" : "audio_quality_partial_coverage";
    }
    ReconcileAudioQualityExpectationOnStrand(false);
    state_.last_sample_at = received_at;
}

void SessionTelemetry::CompleteStatsOnStrand(
    RoomStatsReport report,
    Clock::time_point request_started,
    bool provider_failed) {
    AssertOnStrand();
    if (!stats_in_flight_) {
        return;
    }
    stats_in_flight_ = false;
    const auto now = Clock::now();
    Event completed;
    completed.kind = provider_failed
        ? EventKind::StatsRequestRejected
        : EventKind::StatsRequestCompleted;
    completed.session_generation = session_generation_;
    completed.source_time = now;
    completed.expected_pc_count = report.actual_peer_connection_count;
    completed.successful_pc_count = report.successful_peer_connection_count;
    completed.timed_out_pc_count = report.timed_out_peer_connection_count;
    completed.rejected_pc_count = report.rejected_peer_connection_count;
    completed.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - request_started).count();
    if (!provider_failed) {
        UpdateVideoStatsOnStrand(report, now);
        UpdateAudioStatsOnStrand(report, now);
    }
    ApplyOnStrand(completed);
    UpdateRenderAvailabilityOnStrand(now);
    PublishSnapshotOnStrand(now);
    if (stopping_) {
        FinalizeStopOnStrand();
    }
}

void SessionTelemetry::ApplyProcessResourceSampleOnStrand(
    ProcessResourceSample sample,
    Clock::time_point observed_at) {
    AssertOnStrand();
    if (sample.captured_at == Clock::time_point{}) {
        sample.captured_at = observed_at;
    }
    ++state_.resource_samples;
    if (sample.availability == Availability::Invalid ||
        sample.availability == Availability::Timeout) {
        ++state_.resource_sample_failures;
    }
    state_.last_resource_sample_at = sample.captured_at;
    state_.resource_availability = sample.availability;
    state_.resource_reason = std::move(sample.reason);
    state_.last_resource_sample_us = sample.capture_duration_us;
    state_.cpu_availability = sample.cpu_availability;
    state_.cpu_reason = std::move(sample.cpu_reason);
    state_.process_cpu_percent = sample.cpu_percent;
    state_.logical_processor_count = sample.logical_processor_count;
    state_.memory_availability = sample.memory_availability;
    state_.memory_reason = std::move(sample.memory_reason);
    state_.working_set_bytes = sample.working_set_bytes;
    state_.peak_working_set_bytes = sample.peak_working_set_bytes;
    state_.private_bytes = sample.private_bytes;
    state_.thread_count_availability = sample.thread_count_availability;
    state_.thread_count_reason = std::move(sample.thread_count_reason);
    state_.process_thread_count = sample.thread_count;
    state_.thread_count_sample_age_ms = sample.thread_count_age_ms;
    state_.handle_count_availability = sample.handle_count_availability;
    state_.handle_count_reason = std::move(sample.handle_count_reason);
    state_.process_handle_count = sample.handle_count;
    state_.gpu_resource_availability = sample.gpu_availability;
    state_.gpu_resource_reason = std::move(sample.gpu_reason);
    if (sample.capture_duration_us >= 0) {
        state_.total_resource_sample_us += sample.capture_duration_us;
        state_.maximum_resource_sample_us = (std::max)(
            state_.maximum_resource_sample_us, sample.capture_duration_us);
        state_.average_resource_sample_us =
            static_cast<double>(state_.total_resource_sample_us) /
            static_cast<double>(state_.resource_samples);
    }
    if (resource_observation_started_at_ == Clock::time_point{}) {
        resource_observation_started_at_ = sample.captured_at;
    }
    UpdateResourceTrendOnStrand(sample);
}

void SessionTelemetry::UpdateResourceTrendOnStrand(
    const ProcessResourceSample& sample) {
    AssertOnStrand();
    ResourceTrendSample point;
    point.captured_at = sample.captured_at;
    if (sample.memory_availability == Availability::Valid) {
        point.working_set_bytes = sample.working_set_bytes;
        point.private_bytes = sample.private_bytes;
    }
    if (sample.thread_count_availability == Availability::Valid) {
        point.thread_count = sample.thread_count;
    }
    if (sample.handle_count_availability == Availability::Valid) {
        point.handle_count = sample.handle_count;
    }
    if (!resource_baseline_ &&
        (point.working_set_bytes || point.private_bytes ||
         point.thread_count || point.handle_count)) {
        resource_baseline_ = point;
    }
    resource_trend_.push_back(point);
    while (resource_trend_.size() > kResourceTrendCapacity) {
        resource_trend_.pop_front();
    }

    state_.resource_trend_samples = resource_trend_.size();
    if (resource_trend_.empty()) return;
    state_.resource_trend_span_ms = MillisecondsBetween(
        resource_trend_.front().captured_at,
        resource_trend_.back().captured_at);
    const auto expected_interval_ms = (std::max)(
        std::int64_t{1}, runtime_interval_.count());
    const auto expected_samples = (std::max)(
        std::uint64_t{1},
        static_cast<std::uint64_t>(state_.resource_trend_span_ms /
            expected_interval_ms) + 1);
    state_.resource_trend_coverage = (std::min)(
        1.0,
        static_cast<double>(resource_trend_.size()) /
            static_cast<double>(expected_samples));

    const auto range = [this](auto member) {
        using Optional = std::decay_t<decltype(
            std::declval<ResourceTrendSample>().*member)>;
        using Value = typename Optional::value_type;
        std::optional<Value> minimum;
        std::optional<Value> maximum;
        for (const auto& item : resource_trend_) {
            const auto& value = item.*member;
            if (!value) continue;
            minimum = minimum ? (std::min)(*minimum, *value) : *value;
            maximum = maximum ? (std::max)(*maximum, *value) : *value;
        }
        return std::pair{minimum, maximum};
    };
    const auto working_set_range = range(
        &ResourceTrendSample::working_set_bytes);
    const auto private_range = range(&ResourceTrendSample::private_bytes);
    const auto thread_range = range(&ResourceTrendSample::thread_count);
    const auto handle_range = range(&ResourceTrendSample::handle_count);
    if (working_set_range.first) {
        state_.minimum_working_set_bytes = *working_set_range.first;
        state_.maximum_working_set_bytes = *working_set_range.second;
    }
    if (private_range.first) {
        state_.minimum_private_bytes = *private_range.first;
        state_.maximum_private_bytes = *private_range.second;
    }
    if (thread_range.first) {
        state_.minimum_thread_count = *thread_range.first;
        state_.maximum_thread_count = *thread_range.second;
    }
    if (handle_range.first) {
        state_.minimum_handle_count = *handle_range.first;
        state_.maximum_handle_count = *handle_range.second;
    }

    const auto slope_per_second = [this](auto member) -> std::optional<double> {
        const auto origin = resource_trend_.front().captured_at;
        long double sum_x = 0.0L;
        long double sum_y = 0.0L;
        long double sum_xx = 0.0L;
        long double sum_xy = 0.0L;
        std::size_t count = 0;
        for (const auto& item : resource_trend_) {
            const auto& value = item.*member;
            if (!value) continue;
            const auto x = std::chrono::duration<long double>(
                item.captured_at - origin).count();
            const auto y = static_cast<long double>(*value);
            sum_x += x;
            sum_y += y;
            sum_xx += x * x;
            sum_xy += x * y;
            ++count;
        }
        if (count < kResourceTrendMinimumSamples) return std::nullopt;
        const auto n = static_cast<long double>(count);
        const auto denominator = n * sum_xx - sum_x * sum_x;
        if (std::abs(denominator) <=
            (std::numeric_limits<long double>::epsilon)()) {
            return std::nullopt;
        }
        return static_cast<double>((n * sum_xy - sum_x * sum_y) /
            denominator);
    };
    const auto private_slope = slope_per_second(
        &ResourceTrendSample::private_bytes);
    const auto thread_slope = slope_per_second(
        &ResourceTrendSample::thread_count);
    const auto handle_slope = slope_per_second(
        &ResourceTrendSample::handle_count);
    if (private_slope && thread_slope && handle_slope) {
        state_.resource_trend_availability = Availability::Valid;
        state_.resource_trend_reason =
            "bounded_growth_signal_window_valid_not_leak_confirmation";
        state_.private_bytes_growth_mib_per_minute =
            *private_slope * 60.0 / (1024.0 * 1024.0);
        state_.thread_growth_per_hour = *thread_slope * 3600.0;
        state_.handle_growth_per_hour = *handle_slope * 3600.0;
    } else {
        state_.resource_trend_availability = Availability::WarmingUp;
        state_.resource_trend_reason = "resource_trend_minimum_window_not_met";
    }

    if (resource_baseline_) {
        const auto& baseline = *resource_baseline_;
        const bool complete = baseline.working_set_bytes && point.working_set_bytes &&
            baseline.private_bytes && point.private_bytes &&
            baseline.thread_count && point.thread_count &&
            baseline.handle_count && point.handle_count;
        if (complete) {
            state_.resource_session_delta_availability = Availability::Valid;
            state_.resource_session_delta_reason =
                "session_baseline_to_latest_sample_delta_valid";
            state_.working_set_delta_bytes = SaturatingSignedDelta(
                *point.working_set_bytes, *baseline.working_set_bytes);
            state_.private_bytes_delta = SaturatingSignedDelta(
                *point.private_bytes, *baseline.private_bytes);
            state_.thread_count_delta = SaturatingSignedDelta(
                *point.thread_count, *baseline.thread_count);
            state_.handle_count_delta = SaturatingSignedDelta(
                *point.handle_count, *baseline.handle_count);
        } else {
            state_.resource_session_delta_availability = Availability::WarmingUp;
            state_.resource_session_delta_reason =
                "resource_baseline_or_latest_sample_partial";
        }
    }
}

void SessionTelemetry::FinalizeResourceSessionOnStrand() {
    AssertOnStrand();
    if (state_.resource_session_delta_availability == Availability::Valid) {
        state_.resource_final_delta_availability = Availability::Valid;
        state_.resource_final_delta_reason =
            "session_baseline_to_pre_stop_sample_delta_valid";
    } else {
        state_.resource_final_delta_availability = Availability::Unknown;
        state_.resource_final_delta_reason =
            "pre_stop_resource_sample_unavailable";
    }
    state_.resource_return_availability = Availability::Unknown;
    state_.resource_return_reason = "post_stop_stable_window_not_observed";
}

void SessionTelemetry::PopulateTelemetryCostOnSnapshot(
    Snapshot& snapshot,
    Clock::time_point now) const {
    snapshot.telemetry_snapshot_publications =
        state_.telemetry_snapshot_publications;
    snapshot.total_resource_sample_us = state_.total_resource_sample_us;
    snapshot.maximum_resource_sample_us = state_.maximum_resource_sample_us;
    snapshot.average_resource_sample_us = state_.average_resource_sample_us;
    snapshot.last_snapshot_build_us = state_.last_snapshot_build_us;
    snapshot.maximum_snapshot_build_us = state_.maximum_snapshot_build_us;
    snapshot.total_snapshot_build_us = state_.total_snapshot_build_us;
    snapshot.last_snapshot_callback_us = state_.last_snapshot_callback_us;
    snapshot.maximum_snapshot_callback_us = state_.maximum_snapshot_callback_us;
    snapshot.total_snapshot_callback_us = state_.total_snapshot_callback_us;
    if (resource_observation_started_at_ == Clock::time_point{} ||
        now <= resource_observation_started_at_) {
        snapshot.telemetry_cost_availability = Availability::WarmingUp;
        snapshot.telemetry_cost_reason = "telemetry_cost_elapsed_window_warming_up";
        snapshot.telemetry_observed_cost_ratio = -1.0;
        return;
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - resource_observation_started_at_).count();
    if (elapsed_us <= 0) return;
    const auto observed_us = state_.total_resource_sample_us +
        state_.total_snapshot_build_us + state_.total_snapshot_callback_us;
    snapshot.telemetry_cost_availability = Availability::Valid;
    snapshot.telemetry_cost_reason = "observed_sampler_snapshot_cost_valid";
    snapshot.telemetry_observed_cost_ratio =
        static_cast<double>(observed_us) / static_cast<double>(elapsed_us);
}

void SessionTelemetry::ScheduleNextStatsTickOnStrand() {
    AssertOnStrand();
    if (!stats_started_ || !accepting_.load(std::memory_order_acquire)) {
        return;
    }
    stats_timer_.expires_after(stats_interval_);
    const auto weak = weak_from_this();
    stats_timer_.async_wait([weak](const std::error_code& error) {
        if (error) return;
        if (const auto owner = weak.lock()) {
            owner->RefreshOnStrand(Clock::now());
            owner->RequestStatsSampleOnStrand();
            owner->ScheduleNextStatsTickOnStrand();
        }
    });
}

void SessionTelemetry::ScheduleNextRuntimeTickOnStrand() {
    AssertOnStrand();
    if (!runtime_started_ || !accepting_.load(std::memory_order_acquire)) {
        return;
    }
    const auto scheduled_at = Clock::now() + runtime_interval_;
    runtime_timer_.expires_at(scheduled_at);
    const auto weak = weak_from_this();
    runtime_timer_.async_wait([weak, scheduled_at](const std::error_code& error) {
        if (error) return;
        if (const auto owner = weak.lock()) {
            owner->RequestRuntimeSampleOnStrand(scheduled_at, Clock::now());
            owner->ScheduleNextRuntimeTickOnStrand();
        }
    });
}

void SessionTelemetry::FinalizeStopOnStrand() {
    AssertOnStrand();
    if (stop_finalized_ || stats_in_flight_) return;
    stop_finalized_ = true;
    FinalizeResourceSessionOnStrand();
    PublishSnapshotOnStrand(Clock::now());
    snapshot_callback_ = {};
    auto callbacks = std::move(stop_callbacks_);
    for (auto& callback : callbacks) callback();
}

void SessionTelemetry::PublishSnapshotOnStrand(Clock::time_point now) {
    AssertOnStrand();
    UpdateRenderAvailabilityOnStrand(now);
    ++state_.revision;
    const auto build_started_at = Clock::now();
    auto snapshot = BuildSnapshotOnStrand(now);
    const auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - build_started_at).count();
    ++state_.telemetry_snapshot_publications;
    state_.last_snapshot_build_us = build_us;
    state_.maximum_snapshot_build_us = (std::max)(
        state_.maximum_snapshot_build_us, build_us);
    state_.total_snapshot_build_us += build_us;
    PopulateTelemetryCostOnSnapshot(snapshot, now);
    latest_snapshot_ = std::make_shared<const Snapshot>(std::move(snapshot));
    if (snapshot_callback_) {
        const auto callback_started_at = Clock::now();
        snapshot_callback_(latest_snapshot_);
        const auto callback_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - callback_started_at).count();
        state_.last_snapshot_callback_us = callback_us;
        state_.maximum_snapshot_callback_us = (std::max)(
            state_.maximum_snapshot_callback_us, callback_us);
        state_.total_snapshot_callback_us += callback_us;
    }
}

Snapshot SessionTelemetry::BuildSnapshotOnStrand(Clock::time_point now) const {
    AssertOnStrand();
    Snapshot snapshot = state_;
    snapshot.generated_at = now;
    snapshot.session_complete = stop_finalized_;
    snapshot.stats_in_flight = stats_in_flight_;
    snapshot.revision = state_.revision;
    {
        std::lock_guard lock(queue_mutex_);
        snapshot.queue_depth = queue_.size();
    }
    snapshot.capacity_drops +=
        producer_capacity_drops_.load(std::memory_order_relaxed);
    snapshot.stopped_drops +=
        producer_stopped_drops_.load(std::memory_order_relaxed);
    snapshot.queue_high_water = (std::max)(
        snapshot.queue_high_water,
        producer_high_water_.load(std::memory_order_relaxed));
    snapshot.remote_video_bindings = static_cast<std::uint64_t>(std::count_if(
        media_.begin(), media_.end(), [](const auto& item) {
            return item.second.probe &&
                item.second.probe->active.load(std::memory_order_acquire);
        }));
    snapshot.remote_audio_bindings = static_cast<std::uint64_t>(std::count_if(
        audio_media_.begin(), audio_media_.end(), [](const auto& item) {
            return item.second.probe &&
                item.second.probe->active.load(std::memory_order_acquire);
        }));
    snapshot.render_bindings = static_cast<std::uint64_t>(std::count_if(
        render_media_.begin(), render_media_.end(), [](const auto& item) {
            return item.second.probe &&
                item.second.probe->active.load(std::memory_order_acquire);
        }));
    if (snapshot.last_resource_sample_at != Clock::time_point{}) {
        snapshot.resource_sample_age_ms = (std::max)(std::int64_t{0},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - snapshot.last_resource_sample_at).count());
        if (snapshot.resource_sample_age_ms > runtime_stale_after_.count()) {
            snapshot.resource_availability = Availability::Stale;
            snapshot.resource_reason = "process_resource_sample_stale";
            const auto mark_stale = [](Availability& availability,
                                       std::string& reason) {
                if (availability == Availability::Valid ||
                    availability == Availability::WarmingUp) {
                    availability = Availability::Stale;
                    reason = "process_resource_sample_stale";
                }
            };
            mark_stale(snapshot.cpu_availability, snapshot.cpu_reason);
            mark_stale(snapshot.memory_availability, snapshot.memory_reason);
            mark_stale(snapshot.thread_count_availability,
                snapshot.thread_count_reason);
            mark_stale(snapshot.handle_count_availability,
                snapshot.handle_count_reason);
            mark_stale(snapshot.strand_lag_availability,
                snapshot.strand_lag_reason);
            mark_stale(snapshot.resource_trend_availability,
                snapshot.resource_trend_reason);
        }
    }
    if (snapshot.ui_probe_in_flight &&
        ui_probe_dispatched_at_ != Clock::time_point{} &&
        now >= ui_probe_dispatched_at_ &&
        now - ui_probe_dispatched_at_ >= runtime_stale_after_) {
        snapshot.ui_lag_availability = Availability::Timeout;
        snapshot.ui_lag_reason = "ui_probe_timeout";
    }
    if (snapshot.last_sample_at != Clock::time_point{}) {
        snapshot.sample_age_ms = (std::max)(std::int64_t{0},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - snapshot.last_sample_at).count());
        if (snapshot.sample_age_ms > stale_after_.count() &&
            snapshot.availability != Availability::NotExpected &&
            snapshot.availability != Availability::Unsupported) {
            snapshot.availability = Availability::Stale;
            snapshot.reason = "sample_stale";
            if (snapshot.native_video_freeze_availability == Availability::Valid) {
                snapshot.native_video_freeze_availability = Availability::Stale;
                snapshot.native_video_freeze_reason = "stats_sample_stale";
            }
            if (snapshot.audio_quality_availability == Availability::Valid) {
                snapshot.audio_quality_availability = Availability::Stale;
                snapshot.audio_quality_reason = "stats_sample_stale";
            }
            if (snapshot.audio_concealment_availability == Availability::Valid) {
                snapshot.audio_concealment_availability = Availability::Stale;
                snapshot.audio_concealment_reason = "stats_sample_stale";
            }
            if (snapshot.audio_jitter_buffer_availability == Availability::Valid) {
                snapshot.audio_jitter_buffer_availability = Availability::Stale;
                snapshot.audio_jitter_buffer_reason = "stats_sample_stale";
            }
            if (snapshot.audio_time_stretch_availability == Availability::Valid) {
                snapshot.audio_time_stretch_availability = Availability::Stale;
                snapshot.audio_time_stretch_reason = "stats_sample_stale";
            }
        }
    }
    return snapshot;
}

} // namespace livekit::telemetry
