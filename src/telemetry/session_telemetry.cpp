#include "session_telemetry.h"

#include <algorithm>
#include <cctype>
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

std::string JoinValues(const std::set<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ',';
        result += value;
    }
    return result;
}

std::string ControlledValue(
        const std::string& value,
        std::initializer_list<std::string_view> allowed) {
    for (const auto candidate : allowed) {
        if (value == candidate) return value;
    }
    return value.empty() ? std::string{} : "other";
}

std::string QualityReason(const std::string& value) {
    return ControlledValue(value, {"none", "cpu", "bandwidth", "other"});
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string VideoCodecName(const std::string& value) {
    return ControlledValue(LowerAscii(value),
        {"video/vp8", "video/vp9", "video/h264", "video/av1"});
}

std::string VideoImplementationName(const std::string& value) {
    const auto lower = LowerAscii(value);
    if (lower.empty()) return {};
    if (lower.find("libvpx") != std::string::npos) return "libvpx";
    if (lower.find("openh264") != std::string::npos) return "openh264";
    if (lower.find("ffmpeg") != std::string::npos) return "ffmpeg";
    if (lower.find("intel") != std::string::npos) return "intel";
    if (lower.find("nvidia") != std::string::npos) return "nvidia";
    if (lower.find("amd") != std::string::npos) return "amd";
    if (lower.find("external") != std::string::npos) return "external";
    return "other";
}

std::size_t RenderIntervalBucket(std::int64_t interval_ns) {
    constexpr std::array<std::int64_t, 8> bounds_ms{
        16, 25, 34, 50, 100, 250, 500, 1000};
    const auto interval_ms = interval_ns / 1'000'000.0;
    for (std::size_t i = 0; i < bounds_ms.size(); ++i) {
        if (interval_ms <= static_cast<double>(bounds_ms[i])) return i;
    }
    return bounds_ms.size();
}

double RenderIntervalPercentile(
        const std::array<std::uint64_t,
            RenderActivityProbe::kIntervalHistogramBuckets>& histogram,
        double percentile,
        std::int64_t maximum_interval_ns) {
    constexpr std::array<double, 8> bounds_ms{
        16.0, 25.0, 34.0, 50.0, 100.0, 250.0, 500.0, 1000.0};
    std::uint64_t count = 0;
    for (const auto value : histogram) count += value;
    if (count == 0) return -1.0;
    const auto rank = static_cast<std::uint64_t>(
        std::ceil(percentile * static_cast<double>(count)));
    std::uint64_t accumulated = 0;
    for (std::size_t i = 0; i < histogram.size(); ++i) {
        accumulated += histogram[i];
        if (accumulated < (std::max)(std::uint64_t{1}, rank)) continue;
        if (i < bounds_ms.size()) return bounds_ms[i];
        return maximum_interval_ns > 0
            ? static_cast<double>(maximum_interval_ns) / 1'000'000.0
            : bounds_ms.back();
    }
    return -1.0;
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

template <typename T>
void SaturatingAddUnsigned(T& target, T value) {
    static_assert(std::is_unsigned_v<T>);
    const auto limit = (std::numeric_limits<T>::max)();
    target = value > limit - target ? limit : target + value;
}

const std::vector<MetricProductChainStatus>& FifthBatchProductChains() {
    static const std::vector<MetricProductChainStatus> entries{
        {"SES-03", ProductChainStatus::Implemented,
         "admission_to_startup_terminal_chain_implemented"},
        {"PUB-04", ProductChainStatus::Implemented,
         "subscription_first_media_observation_window_implemented"},
        {"FF-07", ProductChainStatus::Implemented,
         "admission_connect_subscription_to_render_chain_implemented"},
        {"REC-01", ProductChainStatus::Implemented,
         "reconnect_episode_density_implemented"},
        {"STB-05", ProductChainStatus::Implemented,
         "typed_anomaly_density_implemented"},

        {"SES-02", ProductChainStatus::Partial,
         "fine_grained_session_stages_not_instrumented"},
        {"SES-06", ProductChainStatus::Partial,
         "typed_operation_failure_reason_not_exposed"},
        {"PUB-06", ProductChainStatus::Partial,
         "remote_control_ack_not_exposed"},
        {"REC-03", ProductChainStatus::Partial,
         "recovery_transport_subscription_share_stages_not_exposed"},
        {"REC-06", ProductChainStatus::Partial,
         "typed_reconnect_strategy_not_exposed"},
        {"AUD-04", ProductChainStatus::Partial,
         "audio_cadence_provider_partial"},
        {"AUD-05", ProductChainStatus::Partial,
         "audio_device_underrun_provider_not_exposed"},
        {"AUD-06", ProductChainStatus::Partial,
         "audio_level_clipping_provider_not_exposed"},
        {"AUD-07", ProductChainStatus::Partial,
         "apm_effective_state_provider_not_exposed"},
        {"AUD-08", ProductChainStatus::Partial,
         "playout_delay_provider_not_exposed"},
        {"DEV-04", ProductChainStatus::Partial,
         "native_device_failure_category_not_exposed"},
        {"DEV-05", ProductChainStatus::Partial,
         "device_selection_category_not_projected"},
        {"RES-05", ProductChainStatus::Partial,
         "history_export_queue_depth_not_exposed"},
        {"RES-06", ProductChainStatus::Partial,
         "post_stop_sampler_not_owned_after_session_teardown"},
        {"MET-05", ProductChainStatus::Partial,
         "controlled_enabled_disabled_run_not_executed"},

        {"FF-06", ProductChainStatus::Unsupported,
         "native_device_open_milestone_not_exposed"},
        {"DEV-01", ProductChainStatus::Unsupported,
         "native_device_open_milestone_not_exposed"},
        {"DEV-03", ProductChainStatus::Unsupported,
         "os_device_change_provider_not_installed"},
        {"RES-02", ProductChainStatus::Unsupported,
         "process_gpu_provider_not_configured"},
        {"STB-01", ProductChainStatus::Unsupported,
         "crash_evidence_provider_not_configured"},
        {"STB-03", ProductChainStatus::Unsupported,
         "independent_process_watchdog_not_installed"},

        {"E2E-01", ProductChainStatus::ControlledHarnessOnly,
         "controlled_peer_harness_only"},
        {"E2E-02", ProductChainStatus::ControlledHarnessOnly,
         "controlled_peer_harness_only"},
        {"E2E-03", ProductChainStatus::ControlledHarnessOnly,
         "controlled_peer_harness_only"},
        {"E2E-07", ProductChainStatus::ControlledHarnessOnly,
         "controlled_peer_harness_only"},
        {"E2E-04", ProductChainStatus::DeferredExternal,
         "external_optical_environment_required"},
        {"E2E-05", ProductChainStatus::DeferredExternal,
         "external_acoustic_environment_required"},
        {"E2E-06", ProductChainStatus::DeferredExternal,
         "external_av_sync_environment_required"},
    };
    return entries;
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
    case OperationKind::Disconnect: return "disconnect";
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

const char* ProductChainStatusName(ProductChainStatus status) noexcept {
    switch (status) {
    case ProductChainStatus::Implemented: return "IMPLEMENTED_DETERMINISTIC";
    case ProductChainStatus::Partial: return "PARTIAL_PRODUCT_CHAIN";
    case ProductChainStatus::Unsupported: return "UNSUPPORTED_CURRENT_PROVIDER";
    case ProductChainStatus::ControlledHarnessOnly: return "CONTROLLED_HARNESS_ONLY";
    case ProductChainStatus::DeferredExternal: return "DEFERRED_EXTERNAL";
    }
    return "PARTIAL_PRODUCT_CHAIN";
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
    std::size_t queue_capacity,
    Clock::time_point session_started_at)
    : strand_(std::move(strand))
    , session_generation_(session_generation)
    , queue_capacity_((std::max)(std::size_t{1}, queue_capacity))
    , stats_timer_(strand_)
    , runtime_timer_(strand_)
    , session_started_at_(session_started_at == Clock::time_point{}
          ? Clock::now() : session_started_at) {
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
    probe->expectation_reason.store(
        expected_render ? MediaExpectationReason::SurfaceVisible
                        : MediaExpectationReason::SurfaceHidden,
        std::memory_order_release);
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
    probe->expectation_reason.store(reason, std::memory_order_release);
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
        probe->interval_histogram[RenderIntervalBucket(interval)].fetch_add(
            1, std::memory_order_relaxed);
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
    if (decoded_at != Clock::time_point{} && source_time >= decoded_at) {
        const auto age_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            source_time - decoded_at).count();
        probe->frame_age_sum_ns.fetch_add(age_ns, std::memory_order_relaxed);
        probe->frame_age_count.fetch_add(1, std::memory_order_relaxed);
        AtomicMaximum(probe->maximum_frame_age_ns, age_ns);
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

bool SessionTelemetry::RegisterLocalPublication(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t publication_epoch,
    LocalMediaKind media_kind,
    std::string rtc_track_id,
    Clock::time_point publish_accepted_at,
    bool expected_send,
    std::shared_ptr<LocalVideoActivityProbe> video_probe,
    Clock::time_point committed_at,
    std::shared_ptr<LocalAudioActivityProbe> audio_probe) {
    if (series_key.empty() || room_generation == 0 || publication_epoch == 0 ||
        media_kind == LocalMediaKind::Unknown || rtc_track_id.empty() ||
        publish_accepted_at == Clock::time_point{} ||
        (media_kind == LocalMediaKind::Video && !video_probe)) {
        return false;
    }
    if (video_probe) {
        if (!video_probe->active.load(std::memory_order_acquire)) {
            video_probe->telemetry = weak_from_this();
            video_probe->series_key = series_key;
            video_probe->room_generation = room_generation;
            video_probe->publication_epoch = publication_epoch;
            video_probe->active.store(true, std::memory_order_release);
        } else {
            const auto owner = video_probe->telemetry.lock();
            if (owner.get() != this || video_probe->series_key != series_key ||
                video_probe->room_generation != room_generation ||
                video_probe->publication_epoch != publication_epoch) {
                return false;
            }
        }
    }
    if (audio_probe) {
        audio_probe->active.store(true, std::memory_order_release);
    }
    Event event;
    event.kind = EventKind::LocalPublicationBinding;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.publication_epoch = publication_epoch;
    event.local_media_kind = media_kind;
    event.rtc_track_id = std::move(rtc_track_id);
    event.related_time = publish_accepted_at;
    event.expected = expected_send;
    event.local_video_probe = video_probe;
    event.local_audio_probe = audio_probe;
    event.source_time = committed_at;
    const bool submitted = Submit(std::move(event));
    if (!submitted && video_probe) {
        video_probe->active.store(false, std::memory_order_release);
    }
    if (!submitted && audio_probe) {
        audio_probe->active.store(false, std::memory_order_release);
    }
    return submitted;
}

bool SessionTelemetry::EndLocalPublication(
    std::string series_key,
    Clock::time_point source_time) {
    if (series_key.empty()) return false;
    Event event;
    event.kind = EventKind::LocalPublicationEnded;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordLocalVideoFrameInjected(
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t publication_epoch,
    Clock::time_point source_time) {
    if (series_key.empty() || room_generation == 0 || publication_epoch == 0) {
        return false;
    }
    Event event;
    event.kind = EventKind::LocalVideoFrameInjected;
    event.session_generation = session_generation_;
    event.series_key = std::move(series_key);
    event.room_generation = room_generation;
    event.publication_epoch = publication_epoch;
    event.source_time = source_time;
    return Submit(std::move(event));
}

bool SessionTelemetry::RecordRenderPipelineSample(
    RenderPipelineSample sample,
    Clock::time_point source_time) {
    sample.requested_backend = ControlledValue(sample.requested_backend,
        {"none", "qt-cpu", "dx11", "opengl"});
    sample.actual_backend = ControlledValue(sample.actual_backend,
        {"none", "qt-cpu", "dx11", "opengl"});
    sample.gpu_failure = ControlledValue(sample.gpu_failure, {
        "none", "invalid-backend-selection", "remote-session-policy",
        "invalid-module-path", "module-open-failed", "module-entry-missing",
        "module-identity-mismatch", "module-abi-mismatch",
        "module-capabilities-mismatch", "module-selection-locked",
        "module-load-exception", "threaded-gl-unavailable",
        "context-create-failed", "device-create-failed", "graphics-reset",
        "make-current-failed", "surface-lost", "device-lost", "out-of-memory",
        "resource-limit", "presentation-timeout", "renderer-startup-timeout",
        "module-device-failed", "render-owner-exception",
        "unknown-renderer-failure"});
    sample.fallback_reason = ControlledValue(sample.fallback_reason, {
        "none", "user-selected-cpu", "remote-session-policy",
        "invalid-configuration", "module-load-failed",
        "gpu-initialization-failed", "gpu-device-lost", "gpu-runtime-failed"});
    Event event;
    event.kind = EventKind::RenderPipelineSample;
    event.session_generation = session_generation_;
    event.source_time = source_time;
    event.render_pipeline = std::move(sample);
    return Submit(std::move(event));
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
    if (session_stopped_at_ == Clock::time_point{}) {
        session_stopped_at_ = Clock::now();
        CloseUsableIntervalOnStrand(session_stopped_at_);
    }
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
    UpdateLocalPublishAvailabilityOnStrand(now);
    UpdateLocalDeviceStatsOnStrand(now);
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
        if (event.operation_kind == OperationKind::Admission) {
            latest_admission_accepted_at_ = event.source_time;
            state_.admission_to_usable_availability = Availability::WarmingUp;
            state_.admission_to_usable_reason = "waiting_for_startup_terminal";
            state_.admission_to_usable_ms = -1;
            state_.last_admission_to_first_render_ms = -1;
        }
        if (event.operation_kind == OperationKind::ReconnectEpisode ||
            event.operation_kind == OperationKind::Disconnect) {
            CloseUsableIntervalOnStrand(event.source_time);
        }
        if (event.operation_kind == OperationKind::Disconnect) {
            for (auto& [_, publication] : local_publications_) {
                publication.active = false;
                publication.expected_send = false;
                if (publication.video_probe) {
                    publication.video_probe->active.store(
                        false, std::memory_order_release);
                }
                if (publication.audio_probe) {
                    publication.audio_probe->active.store(
                        false, std::memory_order_release);
                }
            }
        }
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
            media->second.first_frame_at = event.source_time;
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
            if (latest_admission_accepted_at_ != Clock::time_point{} &&
                event.source_time >= latest_admission_accepted_at_) {
                state_.last_admission_to_first_render_ms = MillisecondsBetween(
                    latest_admission_accepted_at_, event.source_time);
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
    case EventKind::LocalPublicationBinding: {
        if (event.series_key.empty() || event.room_generation == 0 ||
            event.publication_epoch == 0 ||
            event.local_media_kind == LocalMediaKind::Unknown ||
            event.rtc_track_id.empty() ||
            event.related_time == Clock::time_point{} ||
            event.source_time < event.related_time ||
            (event.local_media_kind == LocalMediaKind::Video &&
             !event.local_video_probe)) {
            ++state_.mapping_failures;
            break;
        }
        auto found = local_publications_.find(event.series_key);
        if (found == local_publications_.end() &&
            local_publications_.size() >= kMaxSeries) {
            ++state_.capacity_drops;
            break;
        }
        if (found != local_publications_.end() && found->second.video_probe) {
            retired_device_format_changes_ +=
                found->second.video_probe->format_changes.load(
                    std::memory_order_relaxed);
            retired_device_clock_resets_ +=
                found->second.video_probe->clock_resets.load(
                    std::memory_order_relaxed);
            found->second.video_probe->active.store(false, std::memory_order_release);
        }
        if (found != local_publications_.end() && found->second.audio_probe) {
            retired_device_format_changes_ +=
                found->second.audio_probe->format_changes.load(
                    std::memory_order_relaxed);
            found->second.audio_probe->active.store(false, std::memory_order_release);
        }
        if (found != local_publications_.end()) {
            auto& previous = found->second;
            retired_device_interruption_ += previous.device_stall_accumulated;
            if (previous.device_stall_active &&
                event.source_time >= previous.device_stall_started_at) {
                retired_device_interruption_ +=
                    event.source_time - previous.device_stall_started_at;
            }
        }
        LocalPublicationState publication;
        publication.room_generation = event.room_generation;
        publication.publication_epoch = event.publication_epoch;
        publication.media_kind = event.local_media_kind;
        publication.rtc_track_id = event.rtc_track_id;
        publication.expected_send = event.expected;
        publication.was_expected = event.expected;
        publication.sender_enabled = event.expected;
        publication.publish_accepted_at = event.related_time;
        publication.committed_at = event.source_time;
        publication.video_probe = event.local_video_probe;
        publication.audio_probe = event.local_audio_probe;
        local_publications_[event.series_key] = std::move(publication);
        ++state_.local_publications;
        UpdateLocalPublishAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::LocalPublicationEnded: {
        const auto found = local_publications_.find(event.series_key);
        if (found == local_publications_.end()) {
            ++state_.stale_local_publication_drops;
            break;
        }
        found->second.active = false;
        found->second.expected_send = false;
        if (found->second.video_probe) {
            found->second.video_probe->active.store(false, std::memory_order_release);
        }
        if (found->second.audio_probe) {
            found->second.audio_probe->active.store(false, std::memory_order_release);
        }
        UpdateLocalPublishAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::LocalVideoFrameInjected: {
        const auto found = local_publications_.find(event.series_key);
        if (found == local_publications_.end() ||
            found->second.room_generation != event.room_generation ||
            found->second.publication_epoch != event.publication_epoch ||
            found->second.media_kind != LocalMediaKind::Video ||
            !found->second.video_probe ||
            !found->second.video_probe->active.load(std::memory_order_acquire)) {
            ++state_.stale_local_publication_drops;
            break;
        }
        if (found->second.first_injected_at == Clock::time_point{}) {
            found->second.first_injected_at = event.source_time;
            ++state_.local_video_first_injections;
            state_.last_publish_to_video_injection_ms = MillisecondsBetween(
                found->second.publish_accepted_at, event.source_time);
        }
        UpdateLocalPublishAvailabilityOnStrand(event.source_time);
        break;
    }
    case EventKind::RenderPipelineSample: {
        const auto& sample = event.render_pipeline;
        if (sample.gpu_failure != "none" &&
            sample.gpu_failure != state_.render_gpu_failure) {
            ++state_.render_backend_failures;
        }
        if (sample.fallback_reason != "none" &&
            sample.fallback_reason != state_.render_fallback_reason) {
            ++state_.render_backend_fallbacks;
        }
        state_.render_router_submitted = sample.router_submitted;
        state_.render_router_replaced = sample.router_replaced_before_render;
        state_.render_router_rejected_generation =
            sample.router_rejected_generation;
        state_.render_router_rejected_binding = sample.router_rejected_binding;
        state_.render_router_dropped_invalid = sample.router_dropped_invalid;
        state_.render_router_dropped_capacity = sample.router_dropped_capacity;
        state_.render_delivered_to_gpu = sample.delivered_to_gpu;
        state_.render_delivered_to_qt_cpu = sample.delivered_to_qt_cpu;
        state_.render_qt_conversion_failures =
            sample.qt_cpu_conversion_failures;
        state_.render_rejected_track_attachments =
            sample.rejected_track_attachments;
        state_.render_attached_track_count = sample.attached_track_count;
        state_.render_requested_backend = sample.requested_backend;
        state_.render_actual_backend = sample.actual_backend;
        state_.render_gpu_failure = sample.gpu_failure;
        state_.render_fallback_reason = sample.fallback_reason;
        state_.render_pipeline_availability = Availability::Valid;
        state_.render_pipeline_reason = sample.gpu_failure == "none"
            ? "render_pipeline_statistics_valid"
            : "render_pipeline_statistics_with_typed_backend_failure";
        state_.render_policy_skipped_frames = sample.router_replaced_before_render;

        state_.router_queue_availability = Availability::Valid;
        state_.router_queue_reason = "bounded_latest_frame_router_observed";
        state_.router_frames_submitted = sample.router_submitted;
        state_.router_frames_replaced = sample.router_replaced_before_render;
        state_.router_capacity_drops = sample.router_dropped_capacity;
        state_.active_router_slots = sample.attached_track_count;
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
    if (operation.kind == OperationKind::Startup) {
        if ((operation.outcome == OperationOutcome::Success ||
             operation.outcome == OperationOutcome::DegradedSuccess) &&
            latest_admission_accepted_at_ != Clock::time_point{} &&
            finished_at >= latest_admission_accepted_at_) {
            state_.admission_to_usable_availability = Availability::Valid;
            state_.admission_to_usable_reason = operation.outcome ==
                    OperationOutcome::DegradedSuccess
                ? "admission_to_degraded_startup_terminal_valid"
                : "admission_to_startup_terminal_valid";
            state_.admission_to_usable_ms = MillisecondsBetween(
                latest_admission_accepted_at_, finished_at);
        } else if (operation.outcome == OperationOutcome::Timeout) {
            state_.admission_to_usable_availability = Availability::Timeout;
            state_.admission_to_usable_reason = "startup_terminal_timeout";
            state_.admission_to_usable_ms = -1;
        } else if (operation.outcome == OperationOutcome::Failure) {
            state_.admission_to_usable_availability = Availability::Invalid;
            state_.admission_to_usable_reason = "startup_terminal_failure";
            state_.admission_to_usable_ms = -1;
        } else if (operation.outcome == OperationOutcome::Cancelled) {
            state_.admission_to_usable_availability = Availability::NotExpected;
            state_.admission_to_usable_reason = "startup_cancelled";
            state_.admission_to_usable_ms = -1;
        }
    }
    if ((operation.kind == OperationKind::Connect ||
         operation.kind == OperationKind::ReconnectEpisode) &&
        (operation.outcome == OperationOutcome::Success ||
         operation.outcome == OperationOutcome::DegradedSuccess)) {
        if (usable_since_ == Clock::time_point{}) usable_since_ = finished_at;
        room_was_usable_ = true;
    }
}

void SessionTelemetry::CloseUsableIntervalOnStrand(Clock::time_point now) {
    AssertOnStrand();
    if (usable_since_ == Clock::time_point{}) return;
    if (now >= usable_since_) {
        usable_accumulated_ += std::chrono::duration_cast<std::chrono::milliseconds>(
            now - usable_since_);
    }
    usable_since_ = {};
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
    std::array<std::uint64_t,
        RenderActivityProbe::kIntervalHistogramBuckets> interval_histogram{};
    std::int64_t frame_age_sum_ns = 0;
    std::int64_t maximum_frame_age_ns = 0;
    std::uint64_t frame_age_count = 0;
    std::int64_t target_interval_ns = 0;
    std::uint64_t expected_bindings = 0;
    std::uint64_t hidden_bindings = 0;
    std::uint64_t minimized_bindings = 0;
    std::chrono::nanoseconds expected_duration{0};
    bool active_stall = false;
    std::uint64_t convert_samples = 0;
    std::int64_t convert_total_us = 0;
    std::int64_t convert_max_us = 0;
    std::uint64_t upload_samples = 0;
    std::int64_t upload_total_us = 0;
    std::int64_t upload_max_us = 0;
    std::uint64_t draw_samples = 0;
    std::int64_t draw_total_us = 0;
    std::int64_t draw_max_us = 0;
    std::uint64_t present_samples = 0;
    std::int64_t present_total_us = 0;
    std::int64_t present_max_us = 0;

    for (const auto& [_, render] : render_media_) {
        if (!render.probe || !render.probe->active.load(std::memory_order_acquire)) {
            continue;
        }
        const auto& probe = *render.probe;
        const auto collect_stage = [](const RenderActivityProbe::StageAccumulator& stage,
                                      std::uint64_t& samples,
                                      std::int64_t& total,
                                      std::int64_t& maximum) {
            samples += stage.samples.load(std::memory_order_relaxed);
            total += stage.total_us.load(std::memory_order_relaxed);
            maximum = (std::max)(maximum,
                stage.maximum_us.load(std::memory_order_relaxed));
        };
        collect_stage(probe.cpu_convert, convert_samples,
                      convert_total_us, convert_max_us);
        collect_stage(probe.upload_submit, upload_samples,
                      upload_total_us, upload_max_us);
        collect_stage(probe.draw_submit, draw_samples,
                      draw_total_us, draw_max_us);
        collect_stage(probe.present_block, present_samples,
                      present_total_us, present_max_us);
        unique_submits += probe.unique_submits.load(std::memory_order_relaxed);
        interval_sum_ns += probe.interval_sum_ns.load(std::memory_order_relaxed);
        interval_count += probe.interval_count.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < interval_histogram.size(); ++i) {
            interval_histogram[i] += probe.interval_histogram[i].load(
                std::memory_order_relaxed);
        }
        frame_age_sum_ns += probe.frame_age_sum_ns.load(std::memory_order_relaxed);
        frame_age_count += probe.frame_age_count.load(std::memory_order_relaxed);
        maximum_frame_age_ns = (std::max)(maximum_frame_age_ns,
            probe.maximum_frame_age_ns.load(std::memory_order_relaxed));
        maximum_interval_ns = (std::max)(maximum_interval_ns,
            probe.maximum_interval_ns.load(std::memory_order_relaxed));
        stalls += probe.closed_stalls.load(std::memory_order_relaxed);
        stall_ns += probe.closed_stall_duration_ns.load(std::memory_order_relaxed);
        longest_stall_ns = (std::max)(longest_stall_ns,
            probe.longest_stall_duration_ns.load(std::memory_order_relaxed));
        expected_duration += render.expected_accumulated;
        if (!render.expected_render) {
            const auto reason = probe.expectation_reason.load(
                std::memory_order_acquire);
            hidden_bindings += reason == MediaExpectationReason::SurfaceHidden;
            minimized_bindings += reason == MediaExpectationReason::WindowMinimized;
            continue;
        }
        ++expected_bindings;
        if (probe.target_interval_ns > 0 &&
            (target_interval_ns == 0 || probe.target_interval_ns < target_interval_ns)) {
            target_interval_ns = probe.target_interval_ns;
        }
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
    state_.render_interval_p50_ms = RenderIntervalPercentile(
        interval_histogram, 0.50, maximum_interval_ns);
    state_.render_interval_p95_ms = RenderIntervalPercentile(
        interval_histogram, 0.95, maximum_interval_ns);
    state_.render_interval_p99_ms = RenderIntervalPercentile(
        interval_histogram, 0.99, maximum_interval_ns);
    state_.render_submit_fps = state_.render_average_interval_ms > 0.0
        ? 1000.0 / state_.render_average_interval_ms : -1.0;
    state_.render_average_frame_age_ms = frame_age_count == 0 ? -1.0
        : static_cast<double>(frame_age_sum_ns) /
            static_cast<double>(frame_age_count) / 1'000'000.0;
    state_.render_maximum_frame_age_ms = frame_age_count == 0 ? -1
        : maximum_frame_age_ns / 1'000'000;
    state_.render_target_interval_ms = target_interval_ns == 0 ? -1
        : target_interval_ns / 1'000'000;
    state_.render_expected_bindings = expected_bindings;
    state_.render_hidden_bindings = hidden_bindings;
    state_.render_minimized_bindings = minimized_bindings;
    if (frame_age_count > 0) {
        state_.render_frame_age_availability = Availability::Valid;
        state_.render_frame_age_reason = "same_clock_decode_to_submit_samples_valid";
    } else if (has_expected) {
        state_.render_frame_age_availability = Availability::WarmingUp;
        state_.render_frame_age_reason = "waiting_for_decode_to_submit_sample";
    } else {
        state_.render_frame_age_availability = render_media_.empty()
            ? Availability::Unknown : Availability::NotExpected;
        state_.render_frame_age_reason = render_media_.empty()
            ? "no_render_binding" : "no_visible_render_expected";
    }
    state_.render_stall_count = stalls;
    state_.render_stall_duration_ms = stall_ns / 1'000'000;
    state_.render_longest_stall_ms = longest_stall_ns / 1'000'000;
    state_.render_expected_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(expected_duration).count();
    state_.render_stall_ratio = expected_duration.count() > 0
        ? static_cast<double>(stall_ns) / static_cast<double>(expected_duration.count())
        : -1.0;
    state_.render_stall_active = active_stall;
    state_.render_convert_samples = convert_samples;
    state_.render_convert_total_us = convert_total_us;
    state_.render_convert_max_us = convert_samples == 0 ? -1 : convert_max_us;
    state_.render_upload_samples = upload_samples;
    state_.render_upload_total_us = upload_total_us;
    state_.render_upload_max_us = upload_samples == 0 ? -1 : upload_max_us;
    state_.render_draw_samples = draw_samples;
    state_.render_draw_total_us = draw_total_us;
    state_.render_draw_max_us = draw_samples == 0 ? -1 : draw_max_us;
    state_.render_present_block_samples = present_samples;
    state_.render_present_block_total_us = present_total_us;
    state_.render_present_block_max_us = present_samples == 0 ? -1 : present_max_us;
    const auto render_stage_samples = convert_samples + upload_samples +
        draw_samples + present_samples;
    if (render_stage_samples > 0) {
        state_.render_stage_availability = Availability::Valid;
        state_.render_stage_reason = "render_cpu_stage_spans_valid";
    } else if (has_expected) {
        state_.render_stage_availability = Availability::WarmingUp;
        state_.render_stage_reason = "waiting_for_render_stage_sample";
    } else {
        state_.render_stage_availability = render_media_.empty()
            ? Availability::Unknown : Availability::NotExpected;
        state_.render_stage_reason = render_media_.empty()
            ? "no_render_stage_samples" : "no_visible_render_expected";
    }

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

void SessionTelemetry::UpdateLocalPublishAvailabilityOnStrand(
    Clock::time_point now) {
    AssertOnStrand();
    const auto timed_out = [&](const LocalPublicationState& publication) {
        return publication.publish_accepted_at != Clock::time_point{} &&
            now >= publication.publish_accepted_at &&
            now - publication.publish_accepted_at >= kLocalPublishObservationWindow;
    };

    state_.active_local_publications = 0;
    state_.expected_local_publications = 0;
    bool active_expected = false;
    bool media_warming = false;
    bool media_unknown = false;
    bool media_timeout = false;
    bool video_expected = false;
    bool injection_warming = false;
    bool injection_timeout = false;
    bool encode_warming = false;
    bool encode_unknown = false;
    bool encode_timeout = false;
    bool send_warming = false;
    bool send_unknown = false;
    bool send_timeout = false;
    bool historical_expected = false;
    bool historical_video = false;

    for (auto& [_, publication] : local_publications_) {
        historical_expected |= publication.was_expected;
        historical_video |= publication.was_expected &&
            publication.media_kind == LocalMediaKind::Video;
        if (publication.media_kind == LocalMediaKind::Video &&
            publication.first_injected_at == Clock::time_point{} &&
            publication.video_probe) {
            const auto first_ns = publication.video_probe->first_injected_ns.load(
                std::memory_order_acquire);
            if (first_ns > 0) {
                publication.first_injected_at = FromNanoseconds(first_ns);
                ++state_.local_video_first_injections;
                state_.last_publish_to_video_injection_ms = MillisecondsBetween(
                    publication.publish_accepted_at, publication.first_injected_at);
            }
        }

        if (!publication.active) continue;
        ++state_.active_local_publications;
        if (!publication.expected_send) continue;
        publication.was_expected = true;
        ++state_.expected_local_publications;
        active_expected = true;
        const bool expired = timed_out(publication);

        if (publication.media_kind == LocalMediaKind::Video) {
            video_expected = true;
            if (publication.first_injected_at == Clock::time_point{}) {
                if (expired) {
                    publication.injection_timeout = true;
                    injection_timeout = true;
                } else {
                    injection_warming = true;
                    media_warming = true;
                }
            }
            if (publication.first_encoded_at == Clock::time_point{}) {
                if (!expired) {
                    encode_warming = true;
                    media_warming = true;
                } else if (publication.outbound_mapping_observed &&
                           publication.encode_counter_observed) {
                    publication.encode_timeout = true;
                    encode_timeout = true;
                    media_timeout = true;
                } else {
                    encode_unknown = true;
                    media_unknown = true;
                }
            }
        }

        if (publication.first_sent_at == Clock::time_point{}) {
            if (!expired) {
                send_warming = true;
                media_warming = true;
            } else if (publication.outbound_mapping_observed &&
                       publication.send_counter_observed) {
                publication.send_timeout = true;
                send_timeout = true;
                media_timeout = true;
            } else {
                send_unknown = true;
                media_unknown = true;
            }
        }

        if (expired && !publication.no_media_reported &&
            (publication.encode_timeout || publication.send_timeout)) {
            publication.no_media_reported = true;
            ++state_.local_publish_no_media;
        }
    }

    for (const auto& [_, publication] : local_publications_) {
        injection_timeout |= publication.injection_timeout;
        encode_timeout |= publication.encode_timeout;
        send_timeout |= publication.send_timeout;
        media_timeout |= publication.no_media_reported;
    }

    if (video_expected || historical_video) {
        if (injection_timeout) {
            state_.local_video_injection_availability = Availability::Timeout;
            state_.local_video_injection_reason = "local_video_injection_timeout";
        } else if (injection_warming) {
            state_.local_video_injection_availability = Availability::WarmingUp;
            state_.local_video_injection_reason = "waiting_for_local_video_injection";
        } else if (state_.local_video_first_injections > 0) {
            state_.local_video_injection_availability = Availability::Valid;
            state_.local_video_injection_reason = "local_video_injection_observed";
        } else {
            state_.local_video_injection_availability = Availability::Unknown;
            state_.local_video_injection_reason = "local_video_ended_before_observation";
        }

        if (encode_timeout) {
            state_.local_video_encode_availability = Availability::Timeout;
            state_.local_video_encode_reason = "local_video_encode_timeout";
        } else if (encode_unknown) {
            state_.local_video_encode_availability = Availability::Unknown;
            state_.local_video_encode_reason = "outbound_video_mapping_unavailable";
        } else if (encode_warming) {
            state_.local_video_encode_availability = Availability::WarmingUp;
            state_.local_video_encode_reason = "waiting_for_local_video_encode";
        } else if (state_.local_video_first_encodes > 0) {
            state_.local_video_encode_availability = Availability::Valid;
            state_.local_video_encode_reason = "local_video_encode_observed";
        } else {
            state_.local_video_encode_availability = Availability::Unknown;
            state_.local_video_encode_reason = "local_video_ended_before_encode_observation";
        }
    } else {
        state_.local_video_injection_availability = Availability::NotExpected;
        state_.local_video_injection_reason = "no_local_video_expected";
        state_.local_video_encode_availability = Availability::NotExpected;
        state_.local_video_encode_reason = "no_local_video_expected";
    }

    if (active_expected || historical_expected) {
        if (send_timeout) {
            state_.local_rtp_send_availability = Availability::Timeout;
            state_.local_rtp_send_reason = "local_rtp_send_timeout";
        } else if (send_unknown) {
            state_.local_rtp_send_availability = Availability::Unknown;
            state_.local_rtp_send_reason = "outbound_rtp_mapping_unavailable";
        } else if (send_warming) {
            state_.local_rtp_send_availability = Availability::WarmingUp;
            state_.local_rtp_send_reason = "waiting_for_local_rtp_send";
        } else if (state_.local_first_rtp_sends > 0) {
            state_.local_rtp_send_availability = Availability::Valid;
            state_.local_rtp_send_reason = "local_rtp_send_observed";
        } else {
            state_.local_rtp_send_availability = Availability::Unknown;
            state_.local_rtp_send_reason = "local_publication_ended_before_send_observation";
        }
    } else {
        state_.local_rtp_send_availability = Availability::NotExpected;
        state_.local_rtp_send_reason = "no_local_publication_expected";
    }

    if (media_timeout) {
        state_.local_publish_media_availability = Availability::Timeout;
        state_.local_publish_media_reason = "local_publication_no_media_timeout";
    } else if (media_unknown) {
        state_.local_publish_media_availability = Availability::Unknown;
        state_.local_publish_media_reason = "local_publication_mapping_unavailable";
    } else if (media_warming) {
        state_.local_publish_media_availability = Availability::WarmingUp;
        state_.local_publish_media_reason = "local_publication_media_warming_up";
    } else if (historical_expected && state_.local_first_rtp_sends > 0) {
        state_.local_publish_media_availability = Availability::Valid;
        state_.local_publish_media_reason = active_expected
            ? "all_expected_local_publications_sending"
            : "local_publications_ended_after_media_observed";
    } else if (historical_expected) {
        state_.local_publish_media_availability = Availability::Unknown;
        state_.local_publish_media_reason =
            "local_publication_ended_before_media_observation";
    } else if (!local_publications_.empty()) {
        state_.local_publish_media_availability = Availability::NotExpected;
        state_.local_publish_media_reason = "local_publications_not_expected_to_send";
    } else {
        state_.local_publish_media_availability = Availability::Unknown;
        state_.local_publish_media_reason = "no_local_publication";
    }
}

void SessionTelemetry::UpdateLocalDeviceStatsOnStrand(Clock::time_point now) {
    AssertOnStrand();
    std::uint64_t expected = 0;
    std::uint64_t active = 0;
    std::uint64_t format_changes = retired_device_format_changes_;
    std::uint64_t clock_resets = retired_device_clock_resets_;
    bool waiting_for_first_frame = false;
    bool missing_probe = false;
    std::chrono::nanoseconds interruption_duration = retired_device_interruption_;
    state_.microphone_requested = false;
    state_.microphone_effective = false;
    state_.camera_requested = false;
    state_.camera_effective = false;
    state_.actual_capture_width = 0;
    state_.actual_capture_height = 0;
    state_.actual_capture_sample_rate = 0;
    state_.actual_capture_channels = 0;

    for (auto& [_, publication] : local_publications_) {
        std::int64_t last_frame_ns = 0;
        std::uint64_t frames = 0;
        if (publication.video_probe) {
            last_frame_ns = publication.video_probe->last_frame_ns.load(
                std::memory_order_acquire);
            frames = publication.video_probe->frame_count.load(
                std::memory_order_relaxed);
            format_changes += publication.video_probe->format_changes.load(
                std::memory_order_relaxed);
            clock_resets += publication.video_probe->clock_resets.load(
                std::memory_order_relaxed);
            if (publication.active) {
                state_.camera_requested |= publication.expected_send;
                state_.camera_effective |= frames > 0;
                const auto width = publication.video_probe->width.load(
                    std::memory_order_relaxed);
                const auto height = publication.video_probe->height.load(
                    std::memory_order_relaxed);
                if (static_cast<std::uint64_t>(width) * height >
                    static_cast<std::uint64_t>(state_.actual_capture_width) *
                        state_.actual_capture_height) {
                    state_.actual_capture_width = width;
                    state_.actual_capture_height = height;
                }
            }
        } else if (publication.audio_probe) {
            last_frame_ns = publication.audio_probe->last_frame_ns.load(
                std::memory_order_acquire);
            frames = publication.audio_probe->frame_count.load(
                std::memory_order_relaxed);
            format_changes += publication.audio_probe->format_changes.load(
                std::memory_order_relaxed);
            if (publication.active) {
                state_.microphone_requested |= publication.expected_send;
                state_.microphone_effective |= frames > 0;
                state_.actual_capture_sample_rate = (std::max)(
                    state_.actual_capture_sample_rate,
                    publication.audio_probe->sample_rate.load(
                        std::memory_order_relaxed));
                state_.actual_capture_channels = (std::max)(
                    state_.actual_capture_channels,
                    publication.audio_probe->channels.load(
                        std::memory_order_relaxed));
            }
        }
        const auto last_frame = last_frame_ns > 0
            ? FromNanoseconds(last_frame_ns) : Clock::time_point{};
        const bool running_intent = publication.active &&
            publication.expected_send &&
            ((publication.video_probe && publication.video_probe->active.load(
                std::memory_order_acquire)) ||
             (publication.audio_probe && publication.audio_probe->active.load(
                std::memory_order_acquire)));
        if (publication.active && publication.expected_send &&
            !publication.video_probe && !publication.audio_probe) {
            missing_probe = true;
        }
        if (!running_intent) {
            if (publication.device_stall_active) {
                publication.device_stall_accumulated +=
                    now - publication.device_stall_started_at;
                publication.device_stall_active = false;
                publication.device_stall_started_at = {};
            }
            interruption_duration += publication.device_stall_accumulated;
            continue;
        }
        ++expected;
        if (frames == 0 || last_frame == Clock::time_point{}) {
            waiting_for_first_frame = true;
            interruption_duration += publication.device_stall_accumulated;
            continue;
        }
        if (publication.device_stall_active &&
            last_frame > publication.device_stall_started_at) {
            publication.device_stall_accumulated +=
                last_frame - publication.device_stall_started_at;
            publication.device_stall_active = false;
            publication.device_stall_started_at = {};
        }
        if (!publication.device_stall_active && now >= last_frame &&
            now - last_frame >= kLocalDeviceStallThreshold) {
            publication.device_stall_active = true;
            publication.device_stall_started_at =
                last_frame + kLocalDeviceStallThreshold;
            ++state_.local_device_unexpected_stops;
        }
        if (!publication.device_stall_active) ++active;
        interruption_duration += publication.device_stall_accumulated;
        if (publication.device_stall_active &&
            now >= publication.device_stall_started_at) {
            interruption_duration += now - publication.device_stall_started_at;
        }
    }

    state_.expected_local_device_streams = expected;
    state_.active_local_device_streams = active;
    state_.local_device_interruption_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            interruption_duration).count();
    state_.local_device_format_changes = format_changes;
    state_.local_device_clock_resets = clock_resets;
    state_.device_switch_attempts = 0;
    state_.device_switch_successes = 0;
    state_.device_switch_failures = 0;
    state_.device_switch_timeouts = 0;
    for (const auto& summary : state_.operation_summaries) {
        if (summary.kind != OperationKind::CameraDeviceSwitch &&
            summary.kind != OperationKind::MicrophoneDeviceSwitch &&
            summary.kind != OperationKind::SpeakerDeviceSwitch) {
            continue;
        }
        state_.device_switch_attempts += summary.started;
        state_.device_switch_successes += summary.success + summary.degraded_success;
        state_.device_switch_failures += summary.failure;
        state_.device_switch_timeouts += summary.timeout;
    }
    if (state_.device_switch_attempts == 0) {
        state_.device_failure_availability = Availability::NotExpected;
        state_.device_failure_reason = "no_device_switch_operation";
    } else if (state_.device_switch_failures > 0 ||
               state_.device_switch_timeouts > 0) {
        state_.device_failure_availability = Availability::Unsupported;
        state_.device_failure_reason =
            "operation_outcome_valid_native_failure_category_not_exposed";
    } else if (state_.device_switch_successes < state_.device_switch_attempts) {
        state_.device_failure_availability = Availability::WarmingUp;
        state_.device_failure_reason = "device_switch_operation_inflight";
    } else {
        state_.device_failure_availability = Availability::Valid;
        state_.device_failure_reason = "device_switch_terminal_outcomes_valid";
    }

    if (local_publications_.empty()) {
        state_.device_state_availability = Availability::Unknown;
        state_.device_state_reason = "no_local_publication";
    } else if ((state_.camera_requested && !state_.camera_effective) ||
               (state_.microphone_requested && !state_.microphone_effective)) {
        state_.device_state_availability = Availability::WarmingUp;
        state_.device_state_reason = "requested_source_waiting_for_samples";
    } else {
        state_.device_state_availability = Availability::Valid;
        state_.device_state_reason =
            "requested_effective_state_and_actual_format_valid";
    }
    if (missing_probe) {
        state_.local_device_continuity_availability = Availability::Unsupported;
        state_.local_device_continuity_reason = "local_device_probe_missing";
    } else if (expected == 0) {
        state_.local_device_continuity_availability =
            local_publications_.empty() ? Availability::Unknown
                                        : Availability::NotExpected;
        state_.local_device_continuity_reason = local_publications_.empty()
            ? "no_local_publication" : "no_local_device_running_intent";
    } else if (waiting_for_first_frame && active == 0) {
        state_.local_device_continuity_availability = Availability::WarmingUp;
        state_.local_device_continuity_reason =
            "local_device_first_frame_warming_up";
    } else {
        state_.local_device_continuity_availability = Availability::Valid;
        state_.local_device_continuity_reason = active < expected
            ? "local_device_unexpected_stop_active"
            : "local_device_continuity_valid";
    }
}

void SessionTelemetry::UpdateLocalPublishStatsOnStrand(
    const RoomStatsReport& report,
    Clock::time_point received_at) {
    AssertOnStrand();
    for (auto& [_, publication] : local_publications_) {
        if (!publication.active || publication.rtc_track_id.empty()) continue;
        bool sender_observed = false;
        bool sender_enabled = false;
        for (const auto& pc_report : report.reports) {
            if (!pc_report.senders_available) continue;
            std::set<std::string> mids;
            for (const auto& sender : pc_report.senders) {
                if (sender.track_id != publication.rtc_track_id) continue;
                sender_observed = true;
                sender_enabled |= sender.track_enabled;
                if (sender.mid_available && !sender.mid.empty()) mids.insert(sender.mid);
            }
            if (mids.empty()) continue;
            for (const auto& stream : pc_report.outbound_rtp) {
                if (!stream.mid_available || !mids.contains(stream.mid)) continue;
                publication.outbound_mapping_observed = true;
                if (publication.media_kind == LocalMediaKind::Video &&
                    stream.frames_encoded_available) {
                    publication.encode_counter_observed = true;
                    if (stream.frames_encoded > 0 &&
                        publication.first_encoded_at == Clock::time_point{}) {
                        publication.first_encoded_at = received_at;
                        ++state_.local_video_first_encodes;
                        state_.last_publish_to_video_encode_ms = MillisecondsBetween(
                            publication.publish_accepted_at, received_at);
                    }
                }
                if (stream.packets_sent_available || stream.bytes_sent_available) {
                    publication.send_counter_observed = true;
                    const bool sent =
                        (stream.packets_sent_available && stream.packets_sent > 0) ||
                        (stream.bytes_sent_available && stream.bytes_sent > 0);
                    if (sent && publication.first_sent_at == Clock::time_point{}) {
                        publication.first_sent_at = received_at;
                        ++state_.local_first_rtp_sends;
                        state_.last_publish_to_rtp_send_ms = MillisecondsBetween(
                            publication.publish_accepted_at, received_at);
                    }
                }
            }
        }
        if (sender_observed) {
            publication.sender_observed = true;
            publication.sender_enabled = sender_enabled;
            publication.expected_send = sender_enabled;
            publication.was_expected |= sender_enabled;
        }
    }
    state_.local_publish_stats_uncertainty_ms = stats_interval_.count();
    UpdateLocalPublishAvailabilityOnStrand(received_at);
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

    std::uint64_t outbound_streams = 0;
    std::uint64_t quality_supported = 0;
    std::uint64_t quality_warmed = 0;
    std::uint64_t duration_supported = 0;
    std::uint64_t duration_warmed = 0;
    std::uint64_t resolution_supported = 0;
    std::uint64_t resolution_warmed = 0;
    std::set<std::string> current_reasons;
    std::set<std::string> quality_seen;
    double cumulative_none = 0.0;
    double cumulative_cpu = 0.0;
    double cumulative_bandwidth = 0.0;
    double cumulative_other = 0.0;
    double window_none = 0.0;
    double window_cpu = 0.0;
    double window_bandwidth = 0.0;
    double window_other = 0.0;
    std::uint64_t resolution_changes = 0;
    std::uint64_t window_resolution_changes = 0;
    std::uint32_t representative_width = 0;
    std::uint32_t representative_height = 0;
    double representative_fps = -1.0;
    std::size_t pc_index = 0;
    for (const auto& pc_report : report.reports) {
        for (const auto& stream : pc_report.outbound_rtp) {
            if (!stream.kind_available || stream.kind != "video") continue;
            ++outbound_streams;
            if (stream.frame_width_available && stream.frame_height_available &&
                static_cast<std::uint64_t>(stream.frame_width) * stream.frame_height >
                    static_cast<std::uint64_t>(representative_width) *
                        representative_height) {
                representative_width = stream.frame_width;
                representative_height = stream.frame_height;
            }
            if (stream.frames_per_second_available &&
                std::isfinite(stream.frames_per_second) &&
                stream.frames_per_second >= 0.0) {
                representative_fps = (std::max)(
                    representative_fps, stream.frames_per_second);
            }
            if (stream.quality_limitation_reason_available) {
                current_reasons.insert(QualityReason(
                    stream.quality_limitation_reason));
            }
            const bool durations_valid =
                stream.quality_limitation_durations_available &&
                !stream.quality_limitation_durations.empty() &&
                std::all_of(
                    stream.quality_limitation_durations.begin(),
                    stream.quality_limitation_durations.end(),
                    [](const auto& entry) {
                        return std::isfinite(entry.second) && entry.second >= 0.0;
                    });
            const bool supported = stream.quality_limitation_reason_available ||
                durations_valid ||
                stream.quality_limitation_resolution_changes_available;
            if (!supported) continue;
            ++quality_supported;
            if (durations_valid) ++duration_supported;
            if (stream.quality_limitation_resolution_changes_available) {
                ++resolution_supported;
            }
            const auto key = std::to_string(pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            quality_seen.insert(key);
            auto& baseline = video_quality_baselines_[key];
            std::unordered_map<std::string, double> current_durations;
            if (durations_valid) {
                for (const auto& [native_reason, duration] :
                     stream.quality_limitation_durations) {
                    current_durations[QualityReason(native_reason)] += duration;
                }
            }
            const auto add_duration = [&](const std::string& reason, double value) {
                if (reason == "none") cumulative_none += value;
                else if (reason == "cpu") cumulative_cpu += value;
                else if (reason == "bandwidth") cumulative_bandwidth += value;
                else cumulative_other += value;
            };
            for (const auto& [reason, duration] : current_durations) {
                add_duration(reason, duration);
            }
            if (stream.quality_limitation_resolution_changes_available) {
                resolution_changes += stream.quality_limitation_resolution_changes;
            }
            bool reset = false;
            if (baseline.initialized) {
                for (const auto& [reason, duration] : current_durations) {
                    const auto previous = baseline.durations.find(reason);
                    if (previous != baseline.durations.end() &&
                        duration < previous->second) {
                        reset = true;
                        break;
                    }
                }
                if (baseline.resolution_changes_available &&
                    stream.quality_limitation_resolution_changes_available &&
                    stream.quality_limitation_resolution_changes <
                        baseline.resolution_changes) {
                    reset = true;
                }
            }
            if (reset) {
                baseline.initialized = false;
                ++state_.counter_resets;
            }
            if (baseline.initialized) {
                ++quality_warmed;
                bool has_duration_delta = false;
                for (const auto& [reason, duration] : current_durations) {
                    const auto previous = baseline.durations.find(reason);
                    if (previous == baseline.durations.end()) continue;
                    has_duration_delta = true;
                    const auto delta = duration - previous->second;
                    if (reason == "none") window_none += delta;
                    else if (reason == "cpu") window_cpu += delta;
                    else if (reason == "bandwidth") window_bandwidth += delta;
                    else window_other += delta;
                }
                if (has_duration_delta) ++duration_warmed;
                if (baseline.resolution_changes_available &&
                    stream.quality_limitation_resolution_changes_available) {
                    ++resolution_warmed;
                    window_resolution_changes +=
                        stream.quality_limitation_resolution_changes -
                        baseline.resolution_changes;
                }
            }
            baseline.durations = std::move(current_durations);
            baseline.resolution_changes =
                stream.quality_limitation_resolution_changes;
            baseline.resolution_changes_available =
                stream.quality_limitation_resolution_changes_available;
            baseline.initialized = true;
        }
        ++pc_index;
    }
    for (auto it = video_quality_baselines_.begin();
         it != video_quality_baselines_.end();) {
        if (!quality_seen.contains(it->first)) it = video_quality_baselines_.erase(it);
        else ++it;
    }
    state_.outbound_video_streams = outbound_streams;
    state_.video_quality_limitation_current = JoinValues(current_reasons);
    state_.video_quality_none_duration_ms = duration_supported == 0 ? -1
        : static_cast<std::int64_t>(std::llround(cumulative_none * 1000.0));
    state_.video_quality_cpu_duration_ms = duration_supported == 0 ? -1
        : static_cast<std::int64_t>(std::llround(cumulative_cpu * 1000.0));
    state_.video_quality_bandwidth_duration_ms = duration_supported == 0 ? -1
        : static_cast<std::int64_t>(std::llround(cumulative_bandwidth * 1000.0));
    state_.video_quality_other_duration_ms = duration_supported == 0 ? -1
        : static_cast<std::int64_t>(std::llround(cumulative_other * 1000.0));
    state_.window_video_quality_none_duration_ms = duration_warmed == 0 ? -1
        : static_cast<std::int64_t>(std::llround(window_none * 1000.0));
    state_.window_video_quality_cpu_duration_ms = duration_warmed == 0 ? -1
        : static_cast<std::int64_t>(std::llround(window_cpu * 1000.0));
    state_.window_video_quality_bandwidth_duration_ms = duration_warmed == 0 ? -1
        : static_cast<std::int64_t>(std::llround(window_bandwidth * 1000.0));
    state_.window_video_quality_other_duration_ms = duration_warmed == 0 ? -1
        : static_cast<std::int64_t>(std::llround(window_other * 1000.0));
    state_.video_quality_resolution_changes = resolution_supported == 0 ? -1
        : static_cast<std::int64_t>(resolution_changes);
    state_.window_video_quality_resolution_changes = resolution_warmed == 0 ? -1
        : static_cast<std::int64_t>(window_resolution_changes);
    state_.outbound_video_width = representative_width;
    state_.outbound_video_height = representative_height;
    state_.outbound_video_fps = representative_fps;
    if (outbound_streams == 0) {
        state_.video_quality_limitation_availability = Availability::Unknown;
        state_.video_quality_limitation_reason = "no_outbound_video_stats";
    } else if (quality_supported == 0) {
        state_.video_quality_limitation_availability = Availability::Unsupported;
        state_.video_quality_limitation_reason =
            "quality_limitation_fields_missing";
    } else {
        state_.video_quality_limitation_availability = Availability::Valid;
        state_.video_quality_limitation_reason =
            quality_supported == outbound_streams && quality_warmed > 0
            ? "quality_limitation_native_window_valid"
            : "quality_limitation_native_partial_coverage";
    }

    enum : std::uint32_t {
        kVideoPrimaryFrames = 1u << 0,
        kVideoSecondaryFrames = 1u << 1,
        kVideoDroppedFrames = 1u << 2,
        kVideoProcessingTime = 1u << 3,
    };
    state_.inbound_video_frames_received = 0;
    state_.inbound_video_frames_decoded = 0;
    state_.inbound_video_frames_dropped = 0;
    state_.outbound_video_frames_encoded = 0;
    state_.outbound_video_frames_sent = 0;
    state_.window_inbound_video_frames_received = 0;
    state_.window_inbound_video_frames_decoded = 0;
    state_.window_inbound_video_frames_dropped = 0;
    state_.window_outbound_video_frames_encoded = 0;
    state_.window_outbound_video_frames_sent = 0;
    state_.inbound_video_frame_drop_ratio = -1.0;
    state_.inbound_video_width = 0;
    state_.inbound_video_height = 0;
    state_.inbound_video_fps = -1.0;
    state_.video_decode_ms_per_frame = -1.0;
    state_.video_encode_ms_per_frame = -1.0;

    std::set<std::string> inbound_codecs;
    std::set<std::string> outbound_codecs;
    std::set<std::string> decoder_implementations;
    std::set<std::string> encoder_implementations;
    std::set<std::string> decoder_efficiency;
    std::set<std::string> encoder_efficiency;
    std::set<std::string> outbound_layers;
    std::set<std::string> video_seen;
    std::uint64_t pipeline_streams = 0;
    std::uint64_t pipeline_supported = 0;
    std::uint64_t pipeline_warmed = 0;
    std::uint64_t codec_supported = 0;
    std::uint64_t processing_supported = 0;
    std::uint64_t processing_warmed = 0;
    double decode_seconds_delta = 0.0;
    double encode_seconds_delta = 0.0;
    std::uint64_t decoded_delta_for_time = 0;
    std::uint64_t encoded_delta_for_time = 0;

    std::size_t video_pc_index = 0;
    for (const auto& pc_report : report.reports) {
        const auto codec_name = [&](const std::string& id) {
            const auto codec = std::find_if(
                pc_report.codecs.begin(), pc_report.codecs.end(),
                [&](const CodecStats& candidate) { return candidate.id == id; });
            return codec != pc_report.codecs.end() && codec->mime_type_available
                ? VideoCodecName(codec->mime_type) : std::string{};
        };
        for (const auto& stream : pc_report.inbound_rtp) {
            if (!stream.kind_available || stream.kind != "video") continue;
            ++pipeline_streams;
            std::uint32_t mask = 0;
            if (stream.frames_received_available) mask |= kVideoPrimaryFrames;
            if (stream.frames_decoded_available) mask |= kVideoSecondaryFrames;
            if (stream.frames_dropped_available) mask |= kVideoDroppedFrames;
            if (stream.total_decode_time_available &&
                std::isfinite(stream.total_decode_time) &&
                stream.total_decode_time >= 0.0) {
                mask |= kVideoProcessingTime;
            }
            if ((mask & (kVideoPrimaryFrames | kVideoSecondaryFrames)) != 0) {
                ++pipeline_supported;
            }
            if (stream.frames_received_available) {
                state_.inbound_video_frames_received += stream.frames_received;
            }
            if (stream.frames_decoded_available) {
                state_.inbound_video_frames_decoded += stream.frames_decoded;
            }
            if (stream.frames_dropped_available) {
                state_.inbound_video_frames_dropped += stream.frames_dropped;
            }
            if (stream.frame_width_available && stream.frame_height_available &&
                static_cast<std::uint64_t>(stream.frame_width) * stream.frame_height >
                    static_cast<std::uint64_t>(state_.inbound_video_width) *
                        state_.inbound_video_height) {
                state_.inbound_video_width = stream.frame_width;
                state_.inbound_video_height = stream.frame_height;
            }
            if (stream.frames_per_second_available &&
                std::isfinite(stream.frames_per_second) &&
                stream.frames_per_second >= 0.0) {
                state_.inbound_video_fps = (std::max)(
                    state_.inbound_video_fps, stream.frames_per_second);
            }
            bool has_codec_detail = false;
            if (stream.codec_id_available) {
                const auto name = codec_name(stream.codec_id);
                if (!name.empty()) {
                    inbound_codecs.insert(name);
                    has_codec_detail = true;
                }
            }
            if (stream.decoder_implementation_available) {
                const auto implementation = VideoImplementationName(
                    stream.decoder_implementation);
                if (!implementation.empty()) {
                    decoder_implementations.insert(implementation);
                    has_codec_detail = true;
                }
            }
            if (stream.power_efficient_decoder_available) {
                decoder_efficiency.insert(
                    stream.power_efficient_decoder ? "true" : "false");
                has_codec_detail = true;
            }
            codec_supported += has_codec_detail;

            const auto key = "in/" + std::to_string(video_pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            video_seen.insert(key);
            auto& baseline = video_stats_baselines_[key];
            VideoStatsBaseline current;
            current.primary_frames = stream.frames_received;
            current.secondary_frames = stream.frames_decoded;
            current.dropped_frames = stream.frames_dropped;
            current.processing_seconds = stream.total_decode_time;
            current.observed_at = received_at;
            current.availability_mask = mask;
            const bool availability_changed = baseline.initialized &&
                baseline.availability_mask != current.availability_mask;
            const bool regressed = baseline.initialized && !availability_changed &&
                (((mask & kVideoPrimaryFrames) != 0 &&
                  current.primary_frames < baseline.primary_frames) ||
                 ((mask & kVideoSecondaryFrames) != 0 &&
                  current.secondary_frames < baseline.secondary_frames) ||
                 ((mask & kVideoDroppedFrames) != 0 &&
                  current.dropped_frames < baseline.dropped_frames) ||
                 ((mask & kVideoProcessingTime) != 0 &&
                  current.processing_seconds < baseline.processing_seconds));
            if (availability_changed || regressed) {
                baseline.initialized = false;
                if (regressed) ++state_.counter_resets;
            }
            if (baseline.initialized && baseline.observed_at != Clock::time_point{} &&
                received_at > baseline.observed_at) {
                ++pipeline_warmed;
                if ((mask & kVideoPrimaryFrames) != 0) {
                    state_.window_inbound_video_frames_received +=
                        current.primary_frames - baseline.primary_frames;
                }
                if ((mask & kVideoSecondaryFrames) != 0) {
                    const auto delta = current.secondary_frames -
                        baseline.secondary_frames;
                    state_.window_inbound_video_frames_decoded += delta;
                    if ((mask & kVideoProcessingTime) != 0) {
                        decode_seconds_delta += current.processing_seconds -
                            baseline.processing_seconds;
                        decoded_delta_for_time += delta;
                        if (delta > 0) ++processing_warmed;
                    }
                }
                if ((mask & kVideoDroppedFrames) != 0) {
                    state_.window_inbound_video_frames_dropped +=
                        current.dropped_frames - baseline.dropped_frames;
                }
            }
            if ((mask & (kVideoSecondaryFrames | kVideoProcessingTime)) ==
                (kVideoSecondaryFrames | kVideoProcessingTime)) {
                ++processing_supported;
            }
            baseline = current;
            baseline.initialized = true;
        }
        for (const auto& stream : pc_report.outbound_rtp) {
            if (!stream.kind_available || stream.kind != "video") continue;
            ++pipeline_streams;
            std::uint32_t mask = 0;
            if (stream.frames_encoded_available) mask |= kVideoPrimaryFrames;
            if (stream.frames_sent_available) mask |= kVideoSecondaryFrames;
            if (stream.total_encode_time_available &&
                std::isfinite(stream.total_encode_time) &&
                stream.total_encode_time >= 0.0) {
                mask |= kVideoProcessingTime;
            }
            if ((mask & (kVideoPrimaryFrames | kVideoSecondaryFrames)) != 0) {
                ++pipeline_supported;
            }
            if (stream.frames_encoded_available) {
                state_.outbound_video_frames_encoded += stream.frames_encoded;
            }
            if (stream.frames_sent_available) {
                state_.outbound_video_frames_sent += stream.frames_sent;
            }
            bool has_codec_detail = false;
            if (stream.codec_id_available) {
                const auto name = codec_name(stream.codec_id);
                if (!name.empty()) {
                    outbound_codecs.insert(name);
                    has_codec_detail = true;
                }
            }
            if (stream.encoder_implementation_available) {
                const auto implementation = VideoImplementationName(
                    stream.encoder_implementation);
                if (!implementation.empty()) {
                    encoder_implementations.insert(implementation);
                    has_codec_detail = true;
                }
            }
            if (stream.power_efficient_encoder_available) {
                encoder_efficiency.insert(
                    stream.power_efficient_encoder ? "true" : "false");
                has_codec_detail = true;
            }
            if (stream.scalability_mode_available) {
                const auto layer = ControlledValue(
                    LowerAscii(stream.scalability_mode),
                    {"l1t1", "l1t2", "l1t3", "l2t1", "l2t2", "l2t3",
                     "l3t1", "l3t2", "l3t3", "l2t2_key", "l3t3_key"});
                if (!layer.empty()) {
                    outbound_layers.insert(layer);
                    has_codec_detail = true;
                }
            } else if (stream.rid_available) {
                outbound_layers.insert("rid-present");
                has_codec_detail = true;
            }
            codec_supported += has_codec_detail;

            const auto key = "out/" + std::to_string(video_pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            video_seen.insert(key);
            auto& baseline = video_stats_baselines_[key];
            VideoStatsBaseline current;
            current.primary_frames = stream.frames_encoded;
            current.secondary_frames = stream.frames_sent;
            current.processing_seconds = stream.total_encode_time;
            current.observed_at = received_at;
            current.availability_mask = mask;
            const bool availability_changed = baseline.initialized &&
                baseline.availability_mask != current.availability_mask;
            const bool regressed = baseline.initialized && !availability_changed &&
                (((mask & kVideoPrimaryFrames) != 0 &&
                  current.primary_frames < baseline.primary_frames) ||
                 ((mask & kVideoSecondaryFrames) != 0 &&
                  current.secondary_frames < baseline.secondary_frames) ||
                 ((mask & kVideoProcessingTime) != 0 &&
                  current.processing_seconds < baseline.processing_seconds));
            if (availability_changed || regressed) {
                baseline.initialized = false;
                if (regressed) ++state_.counter_resets;
            }
            if (baseline.initialized && baseline.observed_at != Clock::time_point{} &&
                received_at > baseline.observed_at) {
                ++pipeline_warmed;
                if ((mask & kVideoPrimaryFrames) != 0) {
                    const auto delta = current.primary_frames -
                        baseline.primary_frames;
                    state_.window_outbound_video_frames_encoded += delta;
                    if ((mask & kVideoProcessingTime) != 0) {
                        encode_seconds_delta += current.processing_seconds -
                            baseline.processing_seconds;
                        encoded_delta_for_time += delta;
                        if (delta > 0) ++processing_warmed;
                    }
                }
                if ((mask & kVideoSecondaryFrames) != 0) {
                    state_.window_outbound_video_frames_sent +=
                        current.secondary_frames - baseline.secondary_frames;
                }
            }
            if ((mask & (kVideoPrimaryFrames | kVideoProcessingTime)) ==
                (kVideoPrimaryFrames | kVideoProcessingTime)) {
                ++processing_supported;
            }
            baseline = current;
            baseline.initialized = true;
        }
        ++video_pc_index;
    }
    for (auto it = video_stats_baselines_.begin();
         it != video_stats_baselines_.end();) {
        if (!video_seen.contains(it->first)) it = video_stats_baselines_.erase(it);
        else ++it;
    }
    const auto inbound_drop_denominator =
        state_.window_inbound_video_frames_received;
    if (inbound_drop_denominator > 0) {
        state_.inbound_video_frame_drop_ratio =
            static_cast<double>(state_.window_inbound_video_frames_dropped) /
            static_cast<double>(inbound_drop_denominator);
    }
    if (decoded_delta_for_time > 0) {
        state_.video_decode_ms_per_frame = decode_seconds_delta * 1000.0 /
            static_cast<double>(decoded_delta_for_time);
    }
    if (encoded_delta_for_time > 0) {
        state_.video_encode_ms_per_frame = encode_seconds_delta * 1000.0 /
            static_cast<double>(encoded_delta_for_time);
    }
    state_.inbound_video_codecs = JoinValues(inbound_codecs);
    state_.outbound_video_codecs = JoinValues(outbound_codecs);
    state_.decoder_implementations = JoinValues(decoder_implementations);
    state_.encoder_implementations = JoinValues(encoder_implementations);
    state_.decoder_power_efficiency = JoinValues(decoder_efficiency);
    state_.encoder_power_efficiency = JoinValues(encoder_efficiency);
    state_.outbound_video_layers = JoinValues(outbound_layers);
    if (pipeline_streams == 0) {
        state_.video_pipeline_availability = Availability::Unknown;
        state_.video_pipeline_reason = "no_video_rtp_stats";
    } else if (pipeline_supported == 0) {
        state_.video_pipeline_availability = Availability::Unsupported;
        state_.video_pipeline_reason = "video_frame_counters_missing";
    } else if (pipeline_warmed == 0) {
        state_.video_pipeline_availability = Availability::WarmingUp;
        state_.video_pipeline_reason = "video_counter_baseline_warming_up";
    } else {
        state_.video_pipeline_availability = Availability::Valid;
        state_.video_pipeline_reason = pipeline_supported == pipeline_streams
            ? "video_pipeline_window_valid"
            : "video_pipeline_partial_coverage";
    }
    if (pipeline_streams == 0) {
        state_.video_codec_availability = Availability::Unknown;
        state_.video_codec_reason = "no_video_rtp_stats";
    } else if (codec_supported == 0) {
        state_.video_codec_availability = Availability::Unsupported;
        state_.video_codec_reason = "codec_implementation_fields_missing";
    } else {
        state_.video_codec_availability = Availability::Valid;
        state_.video_codec_reason = codec_supported == pipeline_streams
            ? "codec_id_implementation_and_layer_join_valid"
            : "codec_details_partial_coverage";
    }
    if (pipeline_streams == 0) {
        state_.video_processing_availability = Availability::Unknown;
        state_.video_processing_reason = "no_video_rtp_stats";
    } else if (processing_supported == 0) {
        state_.video_processing_availability = Availability::Unsupported;
        state_.video_processing_reason = "total_encode_decode_time_missing";
    } else if (processing_warmed == 0) {
        state_.video_processing_availability = Availability::WarmingUp;
        state_.video_processing_reason = "processing_counter_baseline_warming_up";
    } else {
        state_.video_processing_availability = Availability::Valid;
        state_.video_processing_reason = "counter_delta_ms_per_frame_valid";
    }
}

void SessionTelemetry::UpdateNetworkStatsOnStrand(
    const RoomStatsReport& report,
    Clock::time_point received_at) {
    AssertOnStrand();
    enum : std::uint32_t {
        kPackets = 1u << 0, kRetransmittedPackets = 1u << 1,
        kRetransmittedBytes = 1u << 2, kFecPackets = 1u << 3,
        kFecBytes = 1u << 4, kFecDiscarded = 1u << 5,
        kNack = 1u << 6, kPli = 1u << 7, kFir = 1u << 8,
        kBytes = 1u << 9, kPacketsLost = 1u << 10,
    };
    state_.inbound_rtp_streams = 0;
    state_.outbound_rtp_streams = 0;
    state_.inbound_rtp_bytes = 0;
    state_.outbound_rtp_bytes = 0;
    state_.window_inbound_rtp_bytes = 0;
    state_.window_outbound_rtp_bytes = 0;
    state_.inbound_rtp_bitrate_bps = -1.0;
    state_.outbound_rtp_bitrate_bps = -1.0;
    state_.inbound_packets_lost = 0;
    state_.window_inbound_packets_lost = 0;
    state_.window_inbound_packets_received = 0;
    state_.inbound_packet_loss_ratio = -1.0;
    state_.inbound_jitter_max_ms = -1.0;
    state_.remote_rtcp_streams = 0;
    state_.remote_rtcp_current_rtt_max_ms = -1.0;
    state_.remote_rtcp_window_average_rtt_ms = -1.0;
    state_.remote_rtcp_fraction_lost_max = -1.0;
    state_.network_recovery_streams = 0;
    state_.inbound_retransmitted_packets = 0;
    state_.inbound_retransmitted_bytes = 0;
    state_.inbound_fec_packets = 0;
    state_.inbound_fec_bytes = 0;
    state_.inbound_fec_discarded_packets = 0;
    state_.inbound_nack_count = 0;
    state_.inbound_pli_count = 0;
    state_.inbound_fir_count = 0;
    state_.outbound_retransmitted_packets = 0;
    state_.outbound_retransmitted_bytes = 0;
    state_.outbound_nack_count = 0;
    state_.outbound_pli_count = 0;
    state_.outbound_fir_count = 0;
    state_.window_inbound_packets = 0;
    state_.window_inbound_retransmitted_packets = 0;
    state_.window_inbound_fec_packets = 0;
    state_.window_inbound_nack_count = 0;
    state_.window_inbound_pli_count = 0;
    state_.window_inbound_fir_count = 0;
    state_.window_outbound_packets = 0;
    state_.window_outbound_retransmitted_packets = 0;
    state_.window_outbound_nack_count = 0;
    state_.window_outbound_pli_count = 0;
    state_.window_outbound_fir_count = 0;
    std::uint64_t supported_streams = 0;
    std::uint64_t warmed_streams = 0;
    std::uint64_t inbound_streams = 0;
    std::uint64_t outbound_streams = 0;
    std::uint64_t inbound_retransmission_supported = 0;
    std::uint64_t inbound_retransmission_warmed = 0;
    std::uint64_t inbound_fec_supported = 0;
    std::uint64_t inbound_fec_warmed = 0;
    std::uint64_t inbound_feedback_supported = 0;
    std::uint64_t inbound_feedback_warmed = 0;
    std::uint64_t outbound_retransmission_supported = 0;
    std::uint64_t outbound_retransmission_warmed = 0;
    std::uint64_t outbound_feedback_supported = 0;
    std::uint64_t outbound_feedback_warmed = 0;
    std::uint64_t inbound_traffic_supported = 0;
    std::uint64_t inbound_traffic_warmed = 0;
    std::uint64_t outbound_traffic_supported = 0;
    std::uint64_t outbound_traffic_warmed = 0;
    std::uint64_t inbound_loss_supported = 0;
    std::uint64_t inbound_loss_warmed = 0;
    std::uint64_t inbound_jitter_supported = 0;
    bool inbound_loss_late_correction = false;
    std::set<std::string> inbound_seen;
    std::set<std::string> outbound_seen;

    const auto update_baseline = [&](NetworkStatsBaseline& baseline,
            const NetworkStatsBaseline& current,
            bool inbound) {
        bool reset = false;
        const auto common = baseline.availability_mask & current.availability_mask;
        const auto regressed = [&](std::uint32_t bit, std::uint64_t now,
                                   std::uint64_t before) {
            return (common & bit) != 0 && now < before;
        };
        if (baseline.initialized) {
            reset = regressed(kBytes, current.bytes, baseline.bytes) ||
                regressed(kPackets, current.packets, baseline.packets) ||
                regressed(kRetransmittedPackets, current.retransmitted_packets,
                          baseline.retransmitted_packets) ||
                regressed(kRetransmittedBytes, current.retransmitted_bytes,
                          baseline.retransmitted_bytes) ||
                regressed(kFecPackets, current.fec_packets, baseline.fec_packets) ||
                regressed(kFecBytes, current.fec_bytes, baseline.fec_bytes) ||
                regressed(kFecDiscarded, current.fec_discarded,
                          baseline.fec_discarded) ||
                regressed(kNack, current.nack, baseline.nack) ||
                regressed(kPli, current.pli, baseline.pli) ||
                regressed(kFir, current.fir, baseline.fir);
        }
        if (reset) {
            baseline.initialized = false;
            ++state_.counter_resets;
        }
        if (baseline.initialized && baseline.observed_at != Clock::time_point{} &&
            received_at > baseline.observed_at) {
            const auto elapsed = std::chrono::duration<double>(
                received_at - baseline.observed_at).count();
            if ((common & kBytes) != 0) {
                const auto bytes_delta = current.bytes - baseline.bytes;
                if (inbound) {
                    ++inbound_traffic_warmed;
                    SaturatingAddUnsigned(
                        state_.window_inbound_rtp_bytes, bytes_delta);
                    state_.inbound_rtp_bitrate_bps =
                        (std::max)(0.0, state_.inbound_rtp_bitrate_bps) +
                        static_cast<double>(bytes_delta) * 8.0 / elapsed;
                } else {
                    ++outbound_traffic_warmed;
                    SaturatingAddUnsigned(
                        state_.window_outbound_rtp_bytes, bytes_delta);
                    state_.outbound_rtp_bitrate_bps =
                        (std::max)(0.0, state_.outbound_rtp_bitrate_bps) +
                        static_cast<double>(bytes_delta) * 8.0 / elapsed;
                }
            }
            if (inbound && (common & (kPackets | kPacketsLost)) ==
                               (kPackets | kPacketsLost)) {
                ++inbound_loss_warmed;
                const auto received_delta = current.packets - baseline.packets;
                // Native packetsLost is int32; widening before subtraction
                // preserves legitimate negative late-packet corrections.
                const auto lost_delta =
                    current.packets_lost - baseline.packets_lost;
                SaturatingAddUnsigned(
                    state_.window_inbound_packets_received, received_delta);
                if ((lost_delta > 0 && state_.window_inbound_packets_lost >
                        (std::numeric_limits<std::int64_t>::max)() - lost_delta) ||
                    (lost_delta < 0 && state_.window_inbound_packets_lost <
                        (std::numeric_limits<std::int64_t>::min)() - lost_delta)) {
                    state_.window_inbound_packets_lost = lost_delta > 0
                        ? (std::numeric_limits<std::int64_t>::max)()
                        : (std::numeric_limits<std::int64_t>::min)();
                } else {
                    state_.window_inbound_packets_lost += lost_delta;
                }
                inbound_loss_late_correction |= lost_delta < 0;
            }
        }
        const auto recovery_common = common &
            (kRetransmittedPackets | kRetransmittedBytes | kFecPackets |
             kFecBytes | kFecDiscarded | kNack | kPli | kFir);
        if (baseline.initialized && recovery_common != 0) {
            ++warmed_streams;
            if (inbound) {
                if ((common & (kRetransmittedPackets | kRetransmittedBytes)) != 0) {
                    ++inbound_retransmission_warmed;
                }
                if ((common & (kFecPackets | kFecBytes | kFecDiscarded)) != 0) {
                    ++inbound_fec_warmed;
                }
                if ((common & (kNack | kPli | kFir)) != 0) {
                    ++inbound_feedback_warmed;
                }
            } else {
                if ((common & (kRetransmittedPackets | kRetransmittedBytes)) != 0) {
                    ++outbound_retransmission_warmed;
                }
                if ((common & (kNack | kPli | kFir)) != 0) {
                    ++outbound_feedback_warmed;
                }
            }
            const auto delta = [&](std::uint32_t bit, std::uint64_t now,
                                   std::uint64_t before) {
                return (common & bit) != 0 ? now - before : std::uint64_t{0};
            };
            if (inbound) {
                state_.window_inbound_packets +=
                    delta(kPackets, current.packets, baseline.packets);
                state_.window_inbound_retransmitted_packets += delta(
                    kRetransmittedPackets, current.retransmitted_packets,
                    baseline.retransmitted_packets);
                state_.window_inbound_fec_packets +=
                    delta(kFecPackets, current.fec_packets, baseline.fec_packets);
                state_.window_inbound_nack_count +=
                    delta(kNack, current.nack, baseline.nack);
                state_.window_inbound_pli_count +=
                    delta(kPli, current.pli, baseline.pli);
                state_.window_inbound_fir_count +=
                    delta(kFir, current.fir, baseline.fir);
            } else {
                state_.window_outbound_packets +=
                    delta(kPackets, current.packets, baseline.packets);
                state_.window_outbound_retransmitted_packets += delta(
                    kRetransmittedPackets, current.retransmitted_packets,
                    baseline.retransmitted_packets);
                state_.window_outbound_nack_count +=
                    delta(kNack, current.nack, baseline.nack);
                state_.window_outbound_pli_count +=
                    delta(kPli, current.pli, baseline.pli);
                state_.window_outbound_fir_count +=
                    delta(kFir, current.fir, baseline.fir);
            }
        }
        baseline = current;
        baseline.observed_at = received_at;
        baseline.initialized = true;
    };

    std::size_t pc_index = 0;
    for (const auto& pc_report : report.reports) {
        for (const auto& stream : pc_report.inbound_rtp) {
            ++inbound_streams;
            NetworkStatsBaseline current;
            if (stream.bytes_received_available) current.availability_mask |= kBytes;
            if (stream.packets_received_available) current.availability_mask |= kPackets;
            if (stream.packets_lost_available) current.availability_mask |= kPacketsLost;
            if (stream.retransmitted_packets_received_available) current.availability_mask |= kRetransmittedPackets;
            if (stream.retransmitted_bytes_received_available) current.availability_mask |= kRetransmittedBytes;
            if (stream.fec_packets_received_available) current.availability_mask |= kFecPackets;
            if (stream.fec_bytes_received_available) current.availability_mask |= kFecBytes;
            if (stream.fec_packets_discarded_available) current.availability_mask |= kFecDiscarded;
            if (stream.nack_count_available) current.availability_mask |= kNack;
            if (stream.pli_count_available) current.availability_mask |= kPli;
            if (stream.fir_count_available) current.availability_mask |= kFir;
            current.bytes = stream.bytes_received;
            current.packets = stream.packets_received;
            current.packets_lost = stream.packets_lost;
            current.retransmitted_packets = stream.retransmitted_packets_received;
            current.retransmitted_bytes = stream.retransmitted_bytes_received;
            current.fec_packets = stream.fec_packets_received;
            current.fec_bytes = stream.fec_bytes_received;
            current.fec_discarded = stream.fec_packets_discarded;
            current.nack = stream.nack_count;
            current.pli = stream.pli_count;
            current.fir = stream.fir_count;
            ++state_.network_recovery_streams;
            if (stream.bytes_received_available) {
                ++inbound_traffic_supported;
                SaturatingAddUnsigned(
                    state_.inbound_rtp_bytes, stream.bytes_received);
            }
            if (stream.packets_received_available && stream.packets_lost_available) {
                ++inbound_loss_supported;
                if ((stream.packets_lost > 0 && state_.inbound_packets_lost >
                        (std::numeric_limits<std::int64_t>::max)() -
                            stream.packets_lost) ||
                    (stream.packets_lost < 0 && state_.inbound_packets_lost <
                        (std::numeric_limits<std::int64_t>::min)() -
                            stream.packets_lost)) {
                    state_.inbound_packets_lost = stream.packets_lost > 0
                        ? (std::numeric_limits<std::int64_t>::max)()
                        : (std::numeric_limits<std::int64_t>::min)();
                } else {
                    state_.inbound_packets_lost += stream.packets_lost;
                }
            }
            if (stream.jitter_available && std::isfinite(stream.jitter) &&
                stream.jitter >= 0.0) {
                ++inbound_jitter_supported;
                state_.inbound_jitter_max_ms = (std::max)(
                    state_.inbound_jitter_max_ms, stream.jitter * 1000.0);
            }
            const auto recovery_mask = current.availability_mask & ~kPackets;
            const auto recovery_fields = recovery_mask &
                (kRetransmittedPackets | kRetransmittedBytes | kFecPackets |
                 kFecBytes | kFecDiscarded | kNack | kPli | kFir);
            if (recovery_fields != 0) {
                ++supported_streams;
                if ((recovery_fields &
                     (kRetransmittedPackets | kRetransmittedBytes)) != 0) {
                    ++inbound_retransmission_supported;
                }
                if ((recovery_fields &
                     (kFecPackets | kFecBytes | kFecDiscarded)) != 0) {
                    ++inbound_fec_supported;
                }
                if ((recovery_fields & (kNack | kPli | kFir)) != 0) {
                    ++inbound_feedback_supported;
                }
                state_.inbound_retransmitted_packets += current.retransmitted_packets;
                state_.inbound_retransmitted_bytes += current.retransmitted_bytes;
                state_.inbound_fec_packets += current.fec_packets;
                state_.inbound_fec_bytes += current.fec_bytes;
                state_.inbound_fec_discarded_packets += current.fec_discarded;
                state_.inbound_nack_count += current.nack;
                state_.inbound_pli_count += current.pli;
                state_.inbound_fir_count += current.fir;
            }
            const auto key = std::to_string(pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            inbound_seen.insert(key);
            update_baseline(inbound_network_baselines_[key], current, true);
        }
        for (const auto& stream : pc_report.outbound_rtp) {
            ++outbound_streams;
            NetworkStatsBaseline current;
            if (stream.bytes_sent_available) current.availability_mask |= kBytes;
            if (stream.packets_sent_available) current.availability_mask |= kPackets;
            if (stream.retransmitted_packets_sent_available) current.availability_mask |= kRetransmittedPackets;
            if (stream.retransmitted_bytes_sent_available) current.availability_mask |= kRetransmittedBytes;
            if (stream.nack_count_available) current.availability_mask |= kNack;
            if (stream.pli_count_available) current.availability_mask |= kPli;
            if (stream.fir_count_available) current.availability_mask |= kFir;
            current.bytes = stream.bytes_sent;
            current.packets = stream.packets_sent;
            current.retransmitted_packets = stream.retransmitted_packets_sent;
            current.retransmitted_bytes = stream.retransmitted_bytes_sent;
            current.nack = stream.nack_count;
            current.pli = stream.pli_count;
            current.fir = stream.fir_count;
            ++state_.network_recovery_streams;
            if (stream.bytes_sent_available) {
                ++outbound_traffic_supported;
                SaturatingAddUnsigned(state_.outbound_rtp_bytes, stream.bytes_sent);
            }
            const auto recovery_mask = current.availability_mask & ~kPackets;
            const auto recovery_fields = recovery_mask &
                (kRetransmittedPackets | kRetransmittedBytes | kNack | kPli | kFir);
            if (recovery_fields != 0) {
                ++supported_streams;
                if ((recovery_fields &
                     (kRetransmittedPackets | kRetransmittedBytes)) != 0) {
                    ++outbound_retransmission_supported;
                }
                if ((recovery_fields & (kNack | kPli | kFir)) != 0) {
                    ++outbound_feedback_supported;
                }
                state_.outbound_retransmitted_packets += current.retransmitted_packets;
                state_.outbound_retransmitted_bytes += current.retransmitted_bytes;
                state_.outbound_nack_count += current.nack;
                state_.outbound_pli_count += current.pli;
                state_.outbound_fir_count += current.fir;
            }
            const auto key = std::to_string(pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            outbound_seen.insert(key);
            update_baseline(outbound_network_baselines_[key], current, false);
        }
        ++pc_index;
    }
    for (auto it = inbound_network_baselines_.begin();
         it != inbound_network_baselines_.end();) {
        if (!inbound_seen.contains(it->first)) it = inbound_network_baselines_.erase(it);
        else ++it;
    }
    for (auto it = outbound_network_baselines_.begin();
         it != outbound_network_baselines_.end();) {
        if (!outbound_seen.contains(it->first)) it = outbound_network_baselines_.erase(it);
        else ++it;
    }
    state_.inbound_rtp_streams = inbound_streams;
    state_.outbound_rtp_streams = outbound_streams;
    const auto set_window_category = [](
            std::uint64_t streams,
            std::uint64_t supported,
            std::uint64_t warmed,
            Availability& availability,
            std::string& reason,
            const char* no_streams,
            const char* fields_missing,
            const char* warming,
            const char* valid,
            const char* partial) {
        if (streams == 0) {
            availability = Availability::Unknown;
            reason = no_streams;
        } else if (supported == 0) {
            availability = Availability::Unsupported;
            reason = fields_missing;
        } else if (warmed == 0) {
            availability = Availability::WarmingUp;
            reason = warming;
        } else {
            availability = Availability::Valid;
            reason = supported == streams && warmed == supported ? valid : partial;
        }
    };
    set_window_category(inbound_streams, inbound_traffic_supported,
        inbound_traffic_warmed, state_.inbound_rtp_traffic_availability,
        state_.inbound_rtp_traffic_reason, "no_inbound_rtp_stats",
        "inbound_rtp_bytes_missing", "inbound_rtp_bitrate_baseline_warming_up",
        "inbound_rtp_bitrate_window_valid", "inbound_rtp_bitrate_partial_coverage");
    set_window_category(outbound_streams, outbound_traffic_supported,
        outbound_traffic_warmed, state_.outbound_rtp_traffic_availability,
        state_.outbound_rtp_traffic_reason, "no_outbound_rtp_stats",
        "outbound_rtp_bytes_missing", "outbound_rtp_bitrate_baseline_warming_up",
        "outbound_rtp_bitrate_window_valid", "outbound_rtp_bitrate_partial_coverage");
    set_window_category(inbound_streams, inbound_loss_supported,
        inbound_loss_warmed, state_.inbound_packet_loss_availability,
        state_.inbound_packet_loss_reason, "no_inbound_rtp_stats",
        "inbound_loss_fields_missing", "inbound_loss_baseline_warming_up",
        "inbound_loss_window_valid", "inbound_loss_partial_coverage");
    if (inbound_loss_warmed > 0 && inbound_loss_late_correction) {
        state_.inbound_packet_loss_availability = Availability::Invalid;
        state_.inbound_packet_loss_reason = "inbound_loss_late_packet_correction";
    } else if (inbound_loss_warmed > 0 &&
               state_.window_inbound_packets_lost >= 0) {
        const auto denominator =
            static_cast<double>(state_.window_inbound_packets_received) +
            static_cast<double>(state_.window_inbound_packets_lost);
        if (denominator > 0.0) {
            state_.inbound_packet_loss_ratio =
                static_cast<double>(state_.window_inbound_packets_lost) /
                denominator;
        }
    }
    if (inbound_streams == 0) {
        state_.inbound_jitter_availability = Availability::Unknown;
        state_.inbound_jitter_reason = "no_inbound_rtp_stats";
    } else if (inbound_jitter_supported == 0) {
        state_.inbound_jitter_availability = Availability::Unsupported;
        state_.inbound_jitter_reason = "inbound_jitter_field_missing";
    } else {
        state_.inbound_jitter_availability = Availability::Valid;
        state_.inbound_jitter_reason = inbound_jitter_supported == inbound_streams
            ? "inbound_jitter_current_valid"
            : "inbound_jitter_partial_coverage";
    }
    state_.inbound_retransmitted_packet_ratio =
        inbound_retransmission_warmed == 0 ||
        state_.window_inbound_packets == 0 ? -1.0
        : static_cast<double>(state_.window_inbound_retransmitted_packets) /
            static_cast<double>(state_.window_inbound_packets);
    state_.outbound_retransmitted_packet_ratio =
        outbound_retransmission_warmed == 0 ||
        state_.window_outbound_packets == 0 ? -1.0
        : static_cast<double>(state_.window_outbound_retransmitted_packets) /
            static_cast<double>(state_.window_outbound_packets);
    if (state_.network_recovery_streams == 0) {
        state_.network_recovery_availability = Availability::Unknown;
        state_.network_recovery_reason = "no_rtp_stats";
    } else if (supported_streams == 0) {
        state_.network_recovery_availability = Availability::Unsupported;
        state_.network_recovery_reason = "recovery_counters_missing";
    } else if (warmed_streams == 0) {
        state_.network_recovery_availability = Availability::WarmingUp;
        state_.network_recovery_reason = "recovery_counter_baseline_warming_up";
    } else {
        state_.network_recovery_availability = Availability::Valid;
        state_.network_recovery_reason = supported_streams == state_.network_recovery_streams
            ? "recovery_counter_window_valid"
            : "recovery_counter_partial_coverage";
    }
    const auto set_category = [](
            std::uint64_t streams,
            std::uint64_t supported,
            std::uint64_t warmed,
            Availability& availability,
            std::string& reason) {
        if (streams == 0) {
            availability = Availability::Unknown;
            reason = "network_category_no_streams";
        } else if (supported == 0) {
            availability = Availability::Unsupported;
            reason = "network_category_fields_missing";
        } else if (warmed == 0) {
            availability = Availability::WarmingUp;
            reason = "network_category_baseline_warming_up";
        } else {
            availability = Availability::Valid;
            reason = supported == streams && warmed == supported
                ? "network_category_window_valid"
                : "network_category_partial_coverage";
        }
    };
    set_category(inbound_streams, inbound_retransmission_supported,
        inbound_retransmission_warmed,
        state_.inbound_retransmission_availability,
        state_.inbound_retransmission_reason);
    set_category(inbound_streams, inbound_fec_supported,
        inbound_fec_warmed, state_.inbound_fec_availability,
        state_.inbound_fec_reason);
    set_category(inbound_streams, inbound_feedback_supported,
        inbound_feedback_warmed, state_.inbound_feedback_availability,
        state_.inbound_feedback_reason);
    set_category(outbound_streams, outbound_retransmission_supported,
        outbound_retransmission_warmed,
        state_.outbound_retransmission_availability,
        state_.outbound_retransmission_reason);
    set_category(outbound_streams, outbound_feedback_supported,
        outbound_feedback_warmed, state_.outbound_feedback_availability,
        state_.outbound_feedback_reason);

    std::uint64_t remote_rtcp_supported = 0;
    std::uint64_t remote_rtcp_immediate_supported = 0;
    std::uint64_t remote_rtcp_window_supported = 0;
    std::uint64_t remote_rtcp_window_warmed = 0;
    double remote_rtcp_window_total_seconds = 0.0;
    std::uint64_t remote_rtcp_window_measurements = 0;
    std::set<std::string> remote_rtcp_seen;
    pc_index = 0;
    for (const auto& pc_report : report.reports) {
        for (const auto& stream : pc_report.remote_inbound_rtp) {
            ++state_.remote_rtcp_streams;
            const bool current_rtt_valid = stream.round_trip_time_available &&
                std::isfinite(stream.round_trip_time) &&
                stream.round_trip_time >= 0.0;
            const bool fraction_lost_valid = stream.fraction_lost_available &&
                std::isfinite(stream.fraction_lost) &&
                stream.fraction_lost >= 0.0 && stream.fraction_lost <= 1.0;
            const bool window_fields_valid =
                stream.total_round_trip_time_available &&
                stream.round_trip_time_measurements_available &&
                std::isfinite(stream.total_round_trip_time) &&
                stream.total_round_trip_time >= 0.0;
            if (current_rtt_valid || fraction_lost_valid || window_fields_valid) {
                ++remote_rtcp_supported;
            }
            if (current_rtt_valid || fraction_lost_valid) {
                ++remote_rtcp_immediate_supported;
            }
            if (current_rtt_valid) {
                state_.remote_rtcp_current_rtt_max_ms = (std::max)(
                    state_.remote_rtcp_current_rtt_max_ms,
                    stream.round_trip_time * 1000.0);
            }
            if (fraction_lost_valid) {
                state_.remote_rtcp_fraction_lost_max = (std::max)(
                    state_.remote_rtcp_fraction_lost_max,
                    stream.fraction_lost);
            }
            const auto key = std::to_string(pc_index) + "/" +
                (stream.id.empty() ? stream.ssrc : stream.id);
            if (!window_fields_valid) continue;
            ++remote_rtcp_window_supported;
            remote_rtcp_seen.insert(key);
            auto& baseline = remote_rtcp_baselines_[key];
            if (baseline.initialized &&
                (stream.total_round_trip_time < baseline.total_round_trip_time ||
                 stream.round_trip_time_measurements <
                     baseline.round_trip_time_measurements)) {
                baseline.initialized = false;
                ++state_.counter_resets;
            }
            if (baseline.initialized) {
                const auto measurements = stream.round_trip_time_measurements -
                    baseline.round_trip_time_measurements;
                const auto total = stream.total_round_trip_time -
                    baseline.total_round_trip_time;
                if (measurements > 0 && total >= 0.0) {
                    ++remote_rtcp_window_warmed;
                    SaturatingAddUnsigned(
                        remote_rtcp_window_measurements, measurements);
                    remote_rtcp_window_total_seconds += total;
                }
            }
            baseline.total_round_trip_time = stream.total_round_trip_time;
            baseline.round_trip_time_measurements =
                stream.round_trip_time_measurements;
            baseline.initialized = true;
        }
        ++pc_index;
    }
    for (auto it = remote_rtcp_baselines_.begin();
         it != remote_rtcp_baselines_.end();) {
        if (!remote_rtcp_seen.contains(it->first)) {
            it = remote_rtcp_baselines_.erase(it);
        } else {
            ++it;
        }
    }
    if (remote_rtcp_window_measurements > 0) {
        state_.remote_rtcp_window_average_rtt_ms =
            remote_rtcp_window_total_seconds * 1000.0 /
            static_cast<double>(remote_rtcp_window_measurements);
    }
    if (state_.remote_rtcp_streams == 0) {
        state_.remote_rtcp_availability = Availability::Unknown;
        state_.remote_rtcp_reason = "no_remote_inbound_rtcp_stats";
    } else if (remote_rtcp_supported == 0) {
        state_.remote_rtcp_availability = Availability::Unsupported;
        state_.remote_rtcp_reason = "remote_rtcp_fields_missing";
    } else if (remote_rtcp_immediate_supported == 0 &&
               remote_rtcp_window_supported > 0 &&
               remote_rtcp_window_warmed == 0) {
        state_.remote_rtcp_availability = Availability::WarmingUp;
        state_.remote_rtcp_reason = "remote_rtcp_baseline_warming_up";
    } else {
        state_.remote_rtcp_availability = Availability::Valid;
        state_.remote_rtcp_reason =
            remote_rtcp_supported == state_.remote_rtcp_streams &&
                    (remote_rtcp_window_supported == 0 ||
                     remote_rtcp_window_warmed == remote_rtcp_window_supported)
                ? "remote_rtcp_feedback_valid"
                : "remote_rtcp_feedback_partial_coverage";
    }

    std::set<std::string> local_types;
    std::set<std::string> remote_types;
    std::set<std::string> networks;
    std::set<std::string> protocols;
    std::set<std::string> relay_protocols;
    std::set<std::string> tcp_types;
    std::set<std::string> dtls_states;
    std::set<std::string> connectivity_states;
    std::set<std::string> transport_roles;
    std::set<std::string> transports_seen;
    enum : std::uint32_t {
        kTransportBytesSent = 1u << 0,
        kTransportBytesReceived = 1u << 1,
        kTransportPacketsSent = 1u << 2,
        kTransportPacketsReceived = 1u << 3,
    };
    constexpr std::uint32_t kCompleteTransportTraffic =
        kTransportBytesSent | kTransportBytesReceived |
        kTransportPacketsSent | kTransportPacketsReceived;
    state_.transport_stats_count = 0;
    state_.transport_bytes_sent = 0;
    state_.transport_bytes_received = 0;
    state_.transport_packets_sent = 0;
    state_.transport_packets_received = 0;
    state_.window_transport_bytes_sent = 0;
    state_.window_transport_bytes_received = 0;
    state_.window_transport_packets_sent = 0;
    state_.window_transport_packets_received = 0;
    state_.media_path_rtt_max_ms = -1.0;
    state_.media_available_outgoing_bitrate_bps = -1.0;
    state_.media_available_incoming_bitrate_bps = -1.0;
    std::uint64_t transport_count = 0;
    std::uint64_t transport_traffic_supported = 0;
    std::uint64_t transport_traffic_warmed = 0;
    std::uint64_t transport_state_supported = 0;
    std::uint64_t selected_available = 0;
    std::uint64_t resolved = 0;
    std::uint64_t detailed = 0;
    std::uint64_t path_rtt_supported = 0;
    std::uint64_t bandwidth_supported = 0;
    pc_index = 0;
    for (const auto& pc_report : report.reports) {
        for (const auto& transport : pc_report.transports) {
            ++transport_count;
            const auto key = std::to_string(pc_index) + "/" + transport.id;
            transports_seen.insert(key);
            auto& baseline = transport_baselines_[key];
            std::uint32_t traffic_mask = 0;
            if (transport.bytes_sent_available) {
                traffic_mask |= kTransportBytesSent;
            }
            if (transport.bytes_received_available) {
                traffic_mask |= kTransportBytesReceived;
            }
            if (transport.packets_sent_available) {
                traffic_mask |= kTransportPacketsSent;
            }
            if (transport.packets_received_available) {
                traffic_mask |= kTransportPacketsReceived;
            }
            if (traffic_mask == kCompleteTransportTraffic) {
                ++transport_traffic_supported;
                SaturatingAddUnsigned(
                    state_.transport_bytes_sent, transport.bytes_sent);
                SaturatingAddUnsigned(
                    state_.transport_bytes_received, transport.bytes_received);
                SaturatingAddUnsigned(
                    state_.transport_packets_sent, transport.packets_sent);
                SaturatingAddUnsigned(
                    state_.transport_packets_received, transport.packets_received);
                if (baseline.traffic_initialized &&
                    (transport.bytes_sent < baseline.bytes_sent ||
                     transport.bytes_received < baseline.bytes_received ||
                     transport.packets_sent < baseline.packets_sent ||
                     transport.packets_received < baseline.packets_received)) {
                    baseline.traffic_initialized = false;
                    ++state_.counter_resets;
                }
                if (baseline.traffic_initialized &&
                    baseline.observed_at != Clock::time_point{} &&
                    received_at > baseline.observed_at) {
                    ++transport_traffic_warmed;
                    SaturatingAddUnsigned(state_.window_transport_bytes_sent,
                        transport.bytes_sent - baseline.bytes_sent);
                    SaturatingAddUnsigned(state_.window_transport_bytes_received,
                        transport.bytes_received - baseline.bytes_received);
                    SaturatingAddUnsigned(state_.window_transport_packets_sent,
                        transport.packets_sent - baseline.packets_sent);
                    SaturatingAddUnsigned(state_.window_transport_packets_received,
                        transport.packets_received - baseline.packets_received);
                }
                baseline.bytes_sent = transport.bytes_sent;
                baseline.bytes_received = transport.bytes_received;
                baseline.packets_sent = transport.packets_sent;
                baseline.packets_received = transport.packets_received;
                baseline.traffic_availability_mask = traffic_mask;
                baseline.observed_at = received_at;
                baseline.traffic_initialized = true;
            } else {
                baseline.traffic_availability_mask = traffic_mask;
                baseline.traffic_initialized = false;
            }
            const bool has_transport_state = transport.dtls_state_available ||
                transport.ice_state_available || transport.ice_role_available;
            if (has_transport_state) ++transport_state_supported;
            if (transport.dtls_state_available) {
                dtls_states.insert(ControlledValue(transport.dtls_state,
                    {"new", "connecting", "connected", "closed", "failed"}));
            }
            if (transport.ice_state_available) {
                connectivity_states.insert(ControlledValue(transport.ice_state,
                    {"new", "checking", "connected", "completed",
                     "disconnected", "failed", "closed"}));
            }
            if (transport.ice_role_available) {
                transport_roles.insert(ControlledValue(transport.ice_role,
                    {"controlling", "controlled"}));
            }
            if (!transport.selected_candidate_pair_id_available ||
                transport.selected_candidate_pair_id.empty()) {
                baseline.initialized = false;
                continue;
            }
            ++selected_available;
            if (baseline.initialized) {
                if (transport.selected_candidate_pair_changes_available &&
                    baseline.changes_available) {
                    if (transport.selected_candidate_pair_changes >=
                        baseline.selected_pair_changes) {
                        state_.media_path_switches +=
                            transport.selected_candidate_pair_changes -
                            baseline.selected_pair_changes;
                    } else {
                        ++state_.counter_resets;
                    }
                } else if (transport.selected_candidate_pair_id !=
                           baseline.selected_pair_id) {
                    ++state_.media_path_switches;
                }
            }
            baseline.selected_pair_id = transport.selected_candidate_pair_id;
            baseline.selected_pair_changes = transport.selected_candidate_pair_changes;
            baseline.changes_available =
                transport.selected_candidate_pair_changes_available;
            baseline.initialized = true;
            const auto pair = std::find_if(
                pc_report.candidate_pairs.begin(), pc_report.candidate_pairs.end(),
                [&](const auto& item) {
                    return item.id == transport.selected_candidate_pair_id &&
                        item.current_pair && item.selected_relationship_available;
                });
            if (pair == pc_report.candidate_pairs.end()) continue;
            ++resolved;
            if (pair->current_round_trip_time_available &&
                std::isfinite(pair->current_round_trip_time) &&
                pair->current_round_trip_time >= 0.0) {
                ++path_rtt_supported;
                state_.media_path_rtt_max_ms = (std::max)(
                    state_.media_path_rtt_max_ms,
                    pair->current_round_trip_time * 1000.0);
            }
            bool has_bandwidth = false;
            if (pair->available_outgoing_bitrate_available &&
                std::isfinite(pair->available_outgoing_bitrate) &&
                pair->available_outgoing_bitrate >= 0.0) {
                has_bandwidth = true;
                state_.media_available_outgoing_bitrate_bps = (std::max)(
                    state_.media_available_outgoing_bitrate_bps,
                    pair->available_outgoing_bitrate);
            }
            if (pair->available_incoming_bitrate_available &&
                std::isfinite(pair->available_incoming_bitrate) &&
                pair->available_incoming_bitrate >= 0.0) {
                has_bandwidth = true;
                state_.media_available_incoming_bitrate_bps = (std::max)(
                    state_.media_available_incoming_bitrate_bps,
                    pair->available_incoming_bitrate);
            }
            if (has_bandwidth) ++bandwidth_supported;
            const auto find_candidate = [&](const std::string& id) {
                return std::find_if(
                    pc_report.ice_candidates.begin(), pc_report.ice_candidates.end(),
                    [&](const auto& item) { return item.id == id; });
            };
            const auto local = find_candidate(pair->local_candidate_id);
            const auto remote = find_candidate(pair->remote_candidate_id);
            if (local != pc_report.ice_candidates.end() &&
                remote != pc_report.ice_candidates.end()) {
                ++detailed;
            }
            if (local != pc_report.ice_candidates.end()) {
                if (local->candidate_type_available) local_types.insert(
                    ControlledValue(local->candidate_type,
                        {"host", "srflx", "prflx", "relay"}));
                if (local->network_type_available) networks.insert(
                    ControlledValue(local->network_type,
                        {"ethernet", "wifi", "cellular", "vpn", "unknown"}));
                if (local->protocol_available) protocols.insert(
                    ControlledValue(local->protocol, {"udp", "tcp"}));
                if (local->relay_protocol_available) relay_protocols.insert(
                    ControlledValue(local->relay_protocol, {"udp", "tcp", "tls"}));
                if (local->tcp_type_available) tcp_types.insert(
                    ControlledValue(local->tcp_type,
                        {"active", "passive", "so"}));
            }
            if (remote != pc_report.ice_candidates.end()) {
                if (remote->candidate_type_available) remote_types.insert(
                    ControlledValue(remote->candidate_type,
                        {"host", "srflx", "prflx", "relay"}));
                if (remote->protocol_available) protocols.insert(
                    ControlledValue(remote->protocol, {"udp", "tcp"}));
            }
        }
        ++pc_index;
    }
    for (auto it = transport_baselines_.begin(); it != transport_baselines_.end();) {
        if (!transports_seen.contains(it->first)) it = transport_baselines_.erase(it);
        else ++it;
    }
    state_.selected_media_transports = resolved;
    state_.local_candidate_types = JoinValues(local_types);
    state_.remote_candidate_types = JoinValues(remote_types);
    state_.local_network_types = JoinValues(networks);
    state_.media_protocols = JoinValues(protocols);
    state_.relay_protocols = JoinValues(relay_protocols);
    state_.tcp_types = JoinValues(tcp_types);
    state_.transport_stats_count = transport_count;
    state_.transport_dtls_states = JoinValues(dtls_states);
    state_.transport_connectivity_states = JoinValues(connectivity_states);
    state_.transport_roles = JoinValues(transport_roles);
    set_window_category(transport_count, transport_traffic_supported,
        transport_traffic_warmed, state_.transport_traffic_availability,
        state_.transport_traffic_reason, "no_transport_stats",
        "transport_traffic_fields_missing",
        "transport_traffic_baseline_warming_up",
        "transport_traffic_window_valid", "transport_traffic_partial_coverage");
    if (transport_count == 0) {
        state_.transport_state_availability = Availability::Unknown;
        state_.transport_state_reason = "no_transport_stats";
    } else if (transport_state_supported == 0) {
        state_.transport_state_availability = Availability::Unsupported;
        state_.transport_state_reason = "transport_state_fields_missing";
    } else {
        state_.transport_state_availability = Availability::Valid;
        state_.transport_state_reason = transport_state_supported == transport_count
            ? "transport_state_valid" : "transport_state_partial_coverage";
    }
    if (transport_count == 0) {
        state_.media_path_availability = Availability::Unknown;
        state_.media_path_reason = "no_transport_stats";
    } else if (selected_available == 0) {
        state_.media_path_availability = Availability::Unknown;
        state_.media_path_reason = "selected_candidate_pair_id_missing";
    } else if (resolved == 0) {
        state_.media_path_availability = Availability::Unknown;
        state_.media_path_reason = "selected_candidate_pair_not_resolved";
    } else {
        state_.media_path_availability = Availability::Valid;
        state_.media_path_reason = resolved == transport_count && detailed == resolved
            ? "selected_media_path_valid" : "selected_media_path_partial_coverage";
    }
    if (resolved == 0) {
        state_.media_path_rtt_availability = Availability::Unknown;
        state_.media_path_rtt_reason = "selected_candidate_pair_not_resolved";
        state_.media_bandwidth_availability = Availability::Unknown;
        state_.media_bandwidth_reason = "selected_candidate_pair_not_resolved";
    } else {
        state_.media_path_rtt_availability = path_rtt_supported == 0
            ? Availability::Unsupported : Availability::Valid;
        state_.media_path_rtt_reason = path_rtt_supported == 0
            ? "selected_path_rtt_field_missing"
            : path_rtt_supported == resolved
                ? "selected_path_rtt_valid" : "selected_path_rtt_partial_coverage";
        state_.media_bandwidth_availability = bandwidth_supported == 0
            ? Availability::Unsupported : Availability::Valid;
        state_.media_bandwidth_reason = bandwidth_supported == 0
            ? "selected_path_bandwidth_fields_missing"
            : bandwidth_supported == resolved
                ? "selected_path_bandwidth_valid"
                : "selected_path_bandwidth_partial_coverage";
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
        UpdateLocalPublishStatsOnStrand(report, now);
        UpdateVideoStatsOnStrand(report, now);
        UpdateNetworkStatsOnStrand(report, now);
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
    state_.resource_return_availability = Availability::Unsupported;
    state_.resource_return_reason =
        "post_stop_sampler_not_owned_after_session_teardown";
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
    UpdateLocalPublishAvailabilityOnStrand(now);
    UpdateLocalDeviceStatsOnStrand(now);
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
    const auto duration_end = session_stopped_at_ == Clock::time_point{}
        ? now : session_stopped_at_;
    snapshot.session_duration_availability = Availability::Valid;
    snapshot.session_duration_reason = session_stopped_at_ == Clock::time_point{}
        ? "session_duration_in_progress" : "session_duration_complete";
    snapshot.session_duration_ms = MillisecondsBetween(
        session_started_at_, duration_end);
    const auto usable_tail = usable_since_ != Clock::time_point{} &&
        duration_end >= usable_since_
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              duration_end - usable_since_)
        : std::chrono::milliseconds::zero();
    if (room_was_usable_) {
        snapshot.usable_duration_availability = Availability::Valid;
        snapshot.usable_duration_reason = session_stopped_at_ == Clock::time_point{}
            ? "room_usable_duration_in_progress" : "room_usable_duration_complete";
        snapshot.usable_duration_ms =
            (usable_accumulated_ + usable_tail).count();
    } else {
        snapshot.usable_duration_availability = stop_finalized_
            ? Availability::NotExpected : Availability::WarmingUp;
        snapshot.usable_duration_reason = stop_finalized_
            ? "room_never_became_usable" : "waiting_for_connect_success";
        snapshot.usable_duration_ms = -1;
    }
    snapshot.metric_product_chains = FifthBatchProductChains();
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
    bool subscription_origin_missing = false;
    const auto observe_subscription = [&](bool expected, bool delivered,
                                          Clock::time_point accepted_at,
                                          Clock::time_point delivered_at) {
        if (!expected) return;
        ++snapshot.expected_remote_subscriptions;
        if (accepted_at == Clock::time_point{} || now < accepted_at) {
            subscription_origin_missing = true;
            return;
        }
        if (delivered && delivered_at != Clock::time_point{} &&
            delivered_at >= accepted_at) {
            ++snapshot.delivered_remote_subscriptions;
        } else if (delivered) {
            subscription_origin_missing = true;
            return;
        }
        const auto wait_end = delivered ? delivered_at : now;
        const auto wait_ms = MillisecondsBetween(accepted_at, wait_end);
        snapshot.longest_subscription_media_wait_ms = (std::max)(
            snapshot.longest_subscription_media_wait_ms, wait_ms);
        if (!delivered && now - accepted_at >= kSubscriptionMediaObservationWindow) {
            ++snapshot.remote_subscription_no_media;
        }
    };
    for (const auto& [_, media] : media_) {
        observe_subscription(media.expected_receive, media.first_frame_seen,
                             media.subscription_accepted, media.first_frame_at);
    }
    for (const auto& [_, media] : audio_media_) {
        observe_subscription(media.expected_receive, media.first_frame_seen,
                             media.subscription_accepted, media.first_frame_at);
    }
    if (snapshot.expected_remote_subscriptions == 0) {
        snapshot.subscription_media_availability =
            media_.empty() && audio_media_.empty()
            ? Availability::Unknown : Availability::NotExpected;
        snapshot.subscription_media_reason = media_.empty() && audio_media_.empty()
            ? "no_remote_media_binding" : "no_remote_media_expected";
    } else if (snapshot.delivered_remote_subscriptions ==
               snapshot.expected_remote_subscriptions) {
        snapshot.subscription_media_availability = Availability::Valid;
        snapshot.subscription_media_reason =
            "all_expected_subscriptions_delivered_media";
    } else if (snapshot.remote_subscription_no_media > 0) {
        snapshot.subscription_media_availability = Availability::Timeout;
        snapshot.subscription_media_reason =
            "expected_subscription_media_timeout";
    } else if (subscription_origin_missing) {
        snapshot.subscription_media_availability = Availability::Unknown;
        snapshot.subscription_media_reason =
            "subscription_observation_origin_missing";
    } else {
        snapshot.subscription_media_availability = Availability::WarmingUp;
        snapshot.subscription_media_reason =
            "waiting_for_expected_subscription_media";
    }

    for (const auto& summary : snapshot.operation_summaries) {
        if (summary.kind == OperationKind::ReconnectEpisode) {
            snapshot.reconnect_episodes = summary.started;
            SaturatingAddUnsigned(snapshot.stability_operation_failures,
                summary.failure);
            continue;
        }
        SaturatingAddUnsigned(snapshot.stability_operation_failures,
            summary.failure);
        SaturatingAddUnsigned(snapshot.stability_operation_failures,
            summary.timeout);
    }
    SaturatingAddUnsigned(snapshot.stability_sampler_interruptions,
        snapshot.stats_request_timeouts);
    SaturatingAddUnsigned(snapshot.stability_sampler_interruptions,
        snapshot.stats_request_rejections);
    SaturatingAddUnsigned(snapshot.stability_sampler_interruptions,
        snapshot.resource_sample_failures);
    snapshot.stability_device_stops = snapshot.local_device_unexpected_stops;
    SaturatingAddUnsigned(snapshot.stability_media_failures,
        snapshot.reconnect_media_timeouts);
    SaturatingAddUnsigned(snapshot.stability_media_failures,
        snapshot.local_publish_no_media);
    SaturatingAddUnsigned(snapshot.stability_media_failures,
        snapshot.remote_subscription_no_media);
    snapshot.stability_anomalies = snapshot.stability_operation_failures;
    SaturatingAddUnsigned(snapshot.stability_anomalies,
        snapshot.stability_sampler_interruptions);
    SaturatingAddUnsigned(snapshot.stability_anomalies,
        snapshot.stability_device_stops);
    SaturatingAddUnsigned(snapshot.stability_anomalies,
        snapshot.stability_media_failures);
    if (snapshot.usable_duration_availability == Availability::Valid &&
        snapshot.usable_duration_ms > 0) {
        const auto hours = static_cast<double>(snapshot.usable_duration_ms) /
            3'600'000.0;
        snapshot.reconnect_density_availability = Availability::Valid;
        snapshot.reconnect_density_reason =
            "reconnect_episode_density_valid";
        snapshot.reconnect_episodes_per_hour =
            static_cast<double>(snapshot.reconnect_episodes) / hours;
        snapshot.stability_anomaly_density_availability = Availability::Valid;
        snapshot.stability_anomaly_density_reason =
            "typed_anomaly_density_valid";
        snapshot.stability_anomalies_per_hour =
            static_cast<double>(snapshot.stability_anomalies) / hours;
    } else {
        const auto unavailable = stop_finalized_
            ? Availability::NotExpected : Availability::WarmingUp;
        snapshot.reconnect_density_availability = unavailable;
        snapshot.reconnect_density_reason = stop_finalized_
            ? "session_never_became_usable" : "usable_duration_warming_up";
        snapshot.stability_anomaly_density_availability = unavailable;
        snapshot.stability_anomaly_density_reason =
            snapshot.reconnect_density_reason;
    }
    snapshot.internal_resource_availability = Availability::Valid;
    snapshot.internal_resource_reason =
        "session_owned_binding_and_publication_counts_valid";
    snapshot.active_native_bindings = snapshot.remote_video_bindings +
        snapshot.remote_audio_bindings + snapshot.render_bindings;
    snapshot.active_local_media_streams = snapshot.active_local_publications;
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
            if (snapshot.network_recovery_availability == Availability::Valid) {
                snapshot.network_recovery_availability = Availability::Stale;
                snapshot.network_recovery_reason = "stats_sample_stale";
            }
            const auto stale_network = [](Availability& availability,
                                           std::string& reason) {
                if (availability == Availability::Valid) {
                    availability = Availability::Stale;
                    reason = "stats_sample_stale";
                }
            };
            stale_network(snapshot.inbound_rtp_traffic_availability,
                          snapshot.inbound_rtp_traffic_reason);
            stale_network(snapshot.outbound_rtp_traffic_availability,
                          snapshot.outbound_rtp_traffic_reason);
            stale_network(snapshot.inbound_packet_loss_availability,
                          snapshot.inbound_packet_loss_reason);
            stale_network(snapshot.inbound_jitter_availability,
                          snapshot.inbound_jitter_reason);
            stale_network(snapshot.remote_rtcp_availability,
                          snapshot.remote_rtcp_reason);
            stale_network(snapshot.inbound_retransmission_availability,
                          snapshot.inbound_retransmission_reason);
            stale_network(snapshot.inbound_fec_availability,
                          snapshot.inbound_fec_reason);
            stale_network(snapshot.inbound_feedback_availability,
                          snapshot.inbound_feedback_reason);
            stale_network(snapshot.outbound_retransmission_availability,
                          snapshot.outbound_retransmission_reason);
            stale_network(snapshot.outbound_feedback_availability,
                          snapshot.outbound_feedback_reason);
            if (snapshot.media_path_availability == Availability::Valid) {
                snapshot.media_path_availability = Availability::Stale;
                snapshot.media_path_reason = "stats_sample_stale";
            }
            stale_network(snapshot.media_path_rtt_availability,
                          snapshot.media_path_rtt_reason);
            stale_network(snapshot.media_bandwidth_availability,
                          snapshot.media_bandwidth_reason);
            stale_network(snapshot.transport_traffic_availability,
                          snapshot.transport_traffic_reason);
            stale_network(snapshot.transport_state_availability,
                          snapshot.transport_state_reason);
            if (snapshot.video_quality_limitation_availability ==
                Availability::Valid) {
                snapshot.video_quality_limitation_availability = Availability::Stale;
                snapshot.video_quality_limitation_reason = "stats_sample_stale";
            }
            stale_network(snapshot.video_pipeline_availability,
                          snapshot.video_pipeline_reason);
            stale_network(snapshot.video_codec_availability,
                          snapshot.video_codec_reason);
            stale_network(snapshot.video_processing_availability,
                          snapshot.video_processing_reason);
            if (snapshot.local_video_encode_availability == Availability::Valid &&
                snapshot.expected_local_publications > 0) {
                snapshot.local_video_encode_availability = Availability::Stale;
                snapshot.local_video_encode_reason = "stats_sample_stale";
            }
            if (snapshot.local_rtp_send_availability == Availability::Valid &&
                snapshot.expected_local_publications > 0) {
                snapshot.local_rtp_send_availability = Availability::Stale;
                snapshot.local_rtp_send_reason = "stats_sample_stale";
            }
        }
    }
    return snapshot;
}

} // namespace livekit::telemetry
