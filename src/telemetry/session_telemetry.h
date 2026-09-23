#pragma once

#include "stats.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace livekit::telemetry {

enum class Availability {
    Valid,
    WarmingUp,
    NotExpected,
    Unsupported,
    Timeout,
    Stale,
    Invalid,
    Unknown,
};

const char* AvailabilityName(Availability availability) noexcept;

enum class EventKind {
    GaugeSample,
    CounterSample,
    StatsRequestStarted,
    StatsRequestCompleted,
    StatsRequestTimeout,
    StatsRequestRejected,
    StatsRequestSkipped,
    LateCallback,
    MappingFailure,
    OperationStarted,
    OperationTerminal,
    RoomConnectAccepted,
    RemoteVideoBinding,
    RemoteVideoExpectation,
    RemoteVideoFrame,
    RemoteAudioBinding,
    RemoteAudioExpectation,
    RemoteAudioFrame,
    RemoteVideoRenderBinding,
    RemoteVideoRenderExpectation,
    RemoteVideoRenderSubmit,
    UiLagProbeCompleted,
};

enum class OperationKind {
    Unknown,
    Admission,
    Connect,
    Startup,
    PublishBatch,
    PublishTrack,
    Subscribe,
    Unsubscribe,
    Unpublish,
    ReconnectEpisode,
    ReconnectAttempt,
    CameraDeviceSwitch,
    MicrophoneDeviceSwitch,
    SpeakerDeviceSwitch,
};

const char* OperationKindName(OperationKind kind) noexcept;

enum class OperationOutcome {
    None,
    Success,
    DegradedSuccess,
    Failure,
    Timeout,
    Cancelled,
};

const char* OperationOutcomeName(OperationOutcome outcome) noexcept;

enum class MediaExpectationReason {
    BindingActive,
    Subscribed,
    Unsubscribed,
    Muted,
    Unmuted,
    StreamPaused,
    StreamActive,
    PermissionDenied,
    PermissionAllowed,
    PublicationEnded,
    SurfaceVisible,
    SurfaceHidden,
    WindowMinimized,
    RenderBindingEnded,
    VolumeZero,
    VolumeAudible,
    OutputMuted,
    OutputUnmuted,
};

const char* MediaExpectationReasonName(MediaExpectationReason reason) noexcept;

struct VideoActivityProbe {
    std::atomic<bool> active{true};
    std::atomic<std::int64_t> last_frame_ns{0};
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};
};

struct AudioActivityProbe {
    std::atomic<bool> active{true};
    std::atomic<std::int64_t> last_frame_ns{0};
    std::atomic<std::uint32_t> sample_rate{0};
    std::atomic<std::uint32_t> channels{0};
};

// Updated at the real CPU-paint or GPU-present boundary. Only first/recovery
// milestones enter the bounded event queue; cumulative stall counters remain
// lock-free producer data sampled by the session strand.
struct RenderActivityProbe {
    std::atomic<bool> active{true};
    std::atomic<bool> expected{false};
    std::atomic<std::uint64_t> last_token{0};
    std::atomic<std::int64_t> first_submit_ns{0};
    std::atomic<std::int64_t> last_submit_ns{0};
    std::atomic<std::uint64_t> unique_submits{0};
    std::atomic<std::uint64_t> closed_stalls{0};
    std::atomic<std::int64_t> closed_stall_duration_ns{0};
    std::atomic<std::int64_t> longest_stall_duration_ns{0};
    std::atomic<std::int64_t> interval_sum_ns{0};
    std::atomic<std::int64_t> maximum_interval_ns{0};
    std::atomic<std::uint64_t> interval_count{0};
    std::atomic<bool> first_event_submitted{false};
    std::atomic<std::uint64_t> recovery_epoch{0};
    std::atomic<std::int64_t> recovery_first_ns{0};
    std::atomic<std::int64_t> recovery_last_good_ns{0};
    std::atomic<bool> recovery_stable_submitted{false};
    std::int64_t target_interval_ns = 0;
    bool continuous_video = true;
};

struct Event {
    using Clock = std::chrono::steady_clock;

    EventKind kind = EventKind::GaugeSample;
    std::uint64_t session_generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    Clock::time_point source_time{};
    Clock::time_point enqueued_at{};
    std::string series_key;
    std::string operation_id;
    OperationKind operation_kind = OperationKind::Unknown;
    OperationOutcome operation_outcome = OperationOutcome::None;
    Availability availability = Availability::Unknown;
    std::optional<std::int64_t> counter_value;
    std::uint32_t expected_pc_count = 0;
    std::uint32_t successful_pc_count = 0;
    std::uint32_t timed_out_pc_count = 0;
    std::uint32_t rejected_pc_count = 0;
    std::int64_t duration_ms = 0;
    std::uint64_t room_generation = 0;
    std::uint64_t binding_epoch = 0;
    std::uint64_t recovery_epoch = 0;
    Clock::time_point related_time{};
    bool expected = false;
    bool recovery_stable = false;
    bool continuous_video = true;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frame_token = 0;
    std::int64_t target_interval_ms = 0;
    std::string measurement_point;
    MediaExpectationReason expectation_reason = MediaExpectationReason::BindingActive;
    std::shared_ptr<VideoActivityProbe> video_probe;
    std::shared_ptr<AudioActivityProbe> audio_probe;
    std::shared_ptr<RenderActivityProbe> render_probe;
};

struct OperationSummary {
    OperationKind kind = OperationKind::Unknown;
    std::uint64_t started = 0;
    std::uint64_t terminal = 0;
    std::uint64_t success = 0;
    std::uint64_t degraded_success = 0;
    std::uint64_t failure = 0;
    std::uint64_t timeout = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t inflight = 0;
    std::int64_t last_duration_ms = -1;
};

struct ProcessResourceSample {
    using Clock = std::chrono::steady_clock;

    Clock::time_point captured_at{};
    Availability availability = Availability::Unknown;
    std::string reason = "not_sampled";
    Availability cpu_availability = Availability::Unknown;
    std::string cpu_reason = "not_sampled";
    double cpu_percent = -1.0;
    std::uint32_t logical_processor_count = 0;
    Availability memory_availability = Availability::Unknown;
    std::string memory_reason = "not_sampled";
    std::uint64_t working_set_bytes = 0;
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t private_bytes = 0;
    Availability thread_count_availability = Availability::Unknown;
    std::string thread_count_reason = "not_sampled";
    std::uint32_t thread_count = 0;
    std::int64_t thread_count_age_ms = -1;
    Availability handle_count_availability = Availability::Unknown;
    std::string handle_count_reason = "not_sampled";
    std::uint32_t handle_count = 0;
    Availability gpu_availability = Availability::Unknown;
    std::string gpu_reason = "not_sampled";
    std::int64_t capture_duration_us = -1;
};

struct Snapshot {
    using Clock = std::chrono::steady_clock;

    std::uint64_t session_generation = 0;
    std::uint64_t revision = 0;
    bool session_complete = false;
    Availability availability = Availability::Unknown;
    std::string reason = "not_sampled";
    Clock::time_point generated_at{};
    Clock::time_point last_sample_at{};
    std::int64_t sample_age_ms = -1;
    double coverage = 0.0;

    std::size_t queue_capacity = 0;
    std::size_t queue_depth = 0;
    std::size_t queue_high_water = 0;
    std::uint64_t capacity_drops = 0;
    std::uint64_t stopped_drops = 0;
    std::uint64_t stale_generation_drops = 0;
    std::uint64_t out_of_order_drops = 0;
    std::uint64_t late_callbacks = 0;
    std::uint64_t mapping_failures = 0;
    std::uint64_t counter_resets = 0;
    std::uint64_t valid_samples = 0;
    std::uint64_t unavailable_samples = 0;

    Availability event_queue_lag_availability = Availability::Unknown;
    std::string event_queue_lag_reason = "no_queued_event_sample";
    std::uint64_t event_queue_lag_samples = 0;
    std::int64_t last_event_queue_lag_ms = -1;
    std::int64_t maximum_event_queue_lag_ms = -1;

    Availability resource_availability = Availability::Unknown;
    std::string resource_reason = "not_sampled";
    Clock::time_point last_resource_sample_at{};
    std::int64_t resource_sample_age_ms = -1;
    std::uint64_t resource_samples = 0;
    std::uint64_t resource_sample_failures = 0;
    std::int64_t last_resource_sample_us = -1;
    Availability cpu_availability = Availability::Unknown;
    std::string cpu_reason = "not_sampled";
    double process_cpu_percent = -1.0;
    std::uint32_t logical_processor_count = 0;
    Availability memory_availability = Availability::Unknown;
    std::string memory_reason = "not_sampled";
    std::uint64_t working_set_bytes = 0;
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t private_bytes = 0;
    Availability thread_count_availability = Availability::Unknown;
    std::string thread_count_reason = "not_sampled";
    std::uint32_t process_thread_count = 0;
    std::int64_t thread_count_sample_age_ms = -1;
    Availability handle_count_availability = Availability::Unknown;
    std::string handle_count_reason = "not_sampled";
    std::uint32_t process_handle_count = 0;
    Availability gpu_resource_availability = Availability::Unknown;
    std::string gpu_resource_reason = "not_sampled";

    Availability resource_trend_availability = Availability::Unknown;
    std::string resource_trend_reason = "resource_trend_not_started";
    std::uint64_t resource_trend_samples = 0;
    std::int64_t resource_trend_span_ms = -1;
    double resource_trend_coverage = 0.0;
    std::uint64_t minimum_working_set_bytes = 0;
    std::uint64_t maximum_working_set_bytes = 0;
    std::uint64_t minimum_private_bytes = 0;
    std::uint64_t maximum_private_bytes = 0;
    std::uint32_t minimum_thread_count = 0;
    std::uint32_t maximum_thread_count = 0;
    std::uint32_t minimum_handle_count = 0;
    std::uint32_t maximum_handle_count = 0;
    double private_bytes_growth_mib_per_minute = 0.0;
    double thread_growth_per_hour = 0.0;
    double handle_growth_per_hour = 0.0;

    Availability resource_session_delta_availability = Availability::Unknown;
    std::string resource_session_delta_reason = "resource_baseline_not_sampled";
    std::int64_t working_set_delta_bytes = 0;
    std::int64_t private_bytes_delta = 0;
    std::int64_t thread_count_delta = 0;
    std::int64_t handle_count_delta = 0;
    Availability resource_final_delta_availability = Availability::Unknown;
    std::string resource_final_delta_reason = "session_not_stopped";
    Availability resource_return_availability = Availability::Unknown;
    std::string resource_return_reason = "session_not_stopped";

    Availability telemetry_cost_availability = Availability::Unknown;
    std::string telemetry_cost_reason = "telemetry_cost_not_sampled";
    std::uint64_t telemetry_snapshot_publications = 0;
    std::int64_t total_resource_sample_us = 0;
    std::int64_t maximum_resource_sample_us = -1;
    double average_resource_sample_us = -1.0;
    std::int64_t last_snapshot_build_us = -1;
    std::int64_t maximum_snapshot_build_us = -1;
    std::int64_t total_snapshot_build_us = 0;
    std::int64_t last_snapshot_callback_us = -1;
    std::int64_t maximum_snapshot_callback_us = -1;
    std::int64_t total_snapshot_callback_us = 0;
    double telemetry_observed_cost_ratio = -1.0;
    Availability telemetry_ab_availability = Availability::NotExpected;
    std::string telemetry_ab_reason = "controlled_enabled_disabled_run_not_executed";

    Availability strand_lag_availability = Availability::Unknown;
    std::string strand_lag_reason = "runtime_sampler_not_started";
    std::uint64_t strand_lag_samples = 0;
    std::int64_t last_strand_lag_ms = -1;
    std::int64_t maximum_strand_lag_ms = -1;
    Availability ui_lag_availability = Availability::Unknown;
    std::string ui_lag_reason = "ui_probe_not_started";
    std::uint64_t ui_lag_samples = 0;
    std::int64_t last_ui_lag_ms = -1;
    std::int64_t maximum_ui_lag_ms = -1;
    std::uint64_t ui_probe_timeouts = 0;
    std::uint64_t ui_probe_skipped = 0;
    std::uint64_t ui_probe_late_callbacks = 0;
    bool ui_probe_in_flight = false;

    bool stats_in_flight = false;
    std::uint32_t actual_pc_count = 0;
    std::uint32_t successful_pc_count = 0;
    std::uint64_t stats_requests_started = 0;
    std::uint64_t stats_requests_completed = 0;
    std::uint64_t stats_request_timeouts = 0;
    std::uint64_t stats_request_rejections = 0;
    std::uint64_t stats_requests_skipped = 0;
    std::int64_t last_stats_request_ms = -1;

    std::uint64_t operations_started = 0;
    std::uint64_t operations_terminal = 0;
    std::uint64_t operations_inflight = 0;
    std::uint64_t operations_missing_start = 0;
    std::uint64_t operations_duplicate_terminal = 0;
    std::uint64_t operations_kind_mismatch = 0;
    std::vector<OperationSummary> operation_summaries;

    Availability remote_video_first_frame_availability = Availability::Unknown;
    std::string remote_video_first_frame_reason = "no_remote_video_binding";
    std::string remote_video_first_frame_measurement_point =
        "native_video_sink_onframe_entry";
    std::uint64_t remote_video_bindings = 0;
    std::uint64_t remote_video_first_frames = 0;
    std::uint64_t stale_binding_frame_drops = 0;
    std::int64_t room_connect_to_first_decoded_ms = -1;
    std::int64_t last_connect_to_first_decoded_ms = -1;
    std::int64_t last_subscribe_to_first_decoded_ms = -1;
    std::uint32_t last_decoded_width = 0;
    std::uint32_t last_decoded_height = 0;

    // Native inbound stats describe a sink boundary, not a visible render stall.
    Availability native_video_freeze_availability = Availability::Unknown;
    std::string native_video_freeze_reason = "not_sampled";
    std::string native_video_freeze_measurement_point = "rtc_inbound_video_sink";
    std::uint64_t native_video_streams = 0;
    std::uint64_t native_video_freeze_count = 0;
    std::int64_t native_video_freeze_duration_ms = -1;

    Availability reconnect_video_availability = Availability::Unknown;
    std::string reconnect_video_reason = "no_reconnect_episode";
    std::string reconnect_video_measurement_point =
        "native_video_sink_stable_delivery";
    std::uint64_t reconnect_video_expected = 0;
    std::uint64_t reconnect_video_recovered = 0;
    std::uint64_t reconnect_expectation_changes = 0;
    std::uint64_t reconnect_media_timeouts = 0;
    std::int64_t last_reconnect_signaling_ms = -1;
    std::int64_t last_reconnect_first_video_ms = -1;
    std::int64_t last_reconnect_stable_video_ms = -1;
    std::int64_t last_reconnect_media_interruption_ms = -1;

    Availability remote_audio_first_frame_availability = Availability::Unknown;
    std::string remote_audio_first_frame_reason = "no_remote_audio_binding";
    std::string remote_audio_first_frame_measurement_point =
        "native_audio_sink_ondata_entry";
    std::uint64_t remote_audio_bindings = 0;
    std::uint64_t remote_audio_first_frames = 0;
    std::uint64_t stale_audio_binding_drops = 0;
    std::int64_t last_subscribe_to_first_pcm_ms = -1;
    std::uint32_t last_audio_sample_rate = 0;
    std::uint32_t last_audio_channels = 0;

    Availability audio_quality_availability = Availability::Unknown;
    std::string audio_quality_reason = "not_sampled";
    std::string audio_quality_measurement_point = "rtc_inbound_audio_stats";
    Availability audio_concealment_availability = Availability::Unknown;
    std::string audio_concealment_reason = "not_sampled";
    Availability audio_jitter_buffer_availability = Availability::Unknown;
    std::string audio_jitter_buffer_reason = "not_sampled";
    Availability audio_time_stretch_availability = Availability::Unknown;
    std::string audio_time_stretch_reason = "not_sampled";
    std::uint64_t audio_streams = 0;
    std::uint64_t audio_window_samples = 0;
    std::uint64_t audio_window_concealed_samples = 0;
    std::uint64_t audio_window_silent_concealed_samples = 0;
    std::uint64_t audio_window_concealment_events = 0;
    double audio_concealed_ratio = -1.0;
    double audio_non_silent_concealed_ratio = -1.0;
    double audio_jitter_buffer_delay_ms = -1.0;
    double audio_jitter_buffer_target_delay_ms = -1.0;
    double audio_jitter_buffer_minimum_delay_ms = -1.0;
    std::uint64_t audio_inserted_samples = 0;
    std::uint64_t audio_removed_samples = 0;
    double audio_inserted_ratio = -1.0;
    double audio_removed_ratio = -1.0;

    Availability reconnect_audio_availability = Availability::Unknown;
    std::string reconnect_audio_reason = "no_reconnect_episode";
    std::string reconnect_audio_measurement_point =
        "native_audio_sink_stable_pcm_delivery";
    std::uint64_t reconnect_audio_expected = 0;
    std::uint64_t reconnect_audio_recovered = 0;
    std::int64_t last_reconnect_first_audio_ms = -1;
    std::int64_t last_reconnect_stable_audio_ms = -1;
    std::int64_t last_reconnect_audio_interruption_ms = -1;

    Availability render_first_frame_availability = Availability::Unknown;
    std::string render_first_frame_reason = "no_expected_render_binding";
    std::string render_first_frame_measurement_point = "render_submit_not_registered";
    std::uint64_t render_bindings = 0;
    std::uint64_t unique_render_submits = 0;
    std::uint64_t stale_render_binding_drops = 0;
    std::int64_t last_decode_to_render_ms = -1;
    std::int64_t last_subscribe_to_first_render_ms = -1;
    std::int64_t last_connect_to_first_render_ms = -1;
    double render_average_interval_ms = -1.0;
    std::int64_t render_maximum_interval_ms = -1;

    Availability render_stall_availability = Availability::Unknown;
    std::string render_stall_reason = "no_expected_render_binding";
    std::string render_stall_algorithm = "render-stall-v1";
    std::uint64_t render_stall_count = 0;
    std::int64_t render_stall_duration_ms = 0;
    std::int64_t render_longest_stall_ms = 0;
    std::int64_t render_expected_duration_ms = 0;
    double render_stall_ratio = -1.0;
    bool render_stall_active = false;

    Availability reconnect_render_availability = Availability::Unknown;
    std::string reconnect_render_reason = "no_reconnect_episode";
    std::string reconnect_render_measurement_point = "visible_render_submit";
    std::uint64_t reconnect_render_expected = 0;
    std::uint64_t reconnect_render_recovered = 0;
    std::int64_t last_reconnect_first_render_ms = -1;
    std::int64_t last_reconnect_stable_render_ms = -1;
    std::int64_t last_reconnect_render_interruption_ms = -1;
};

class SessionTelemetry final : public std::enable_shared_from_this<SessionTelemetry> {
public:
    using Clock = std::chrono::steady_clock;
    using Strand = asio::strand<asio::io_context::executor_type>;
    using SnapshotPtr = std::shared_ptr<const Snapshot>;
    using SnapshotCallback = std::function<void(SnapshotPtr)>;
    using LateCompletion = std::function<void()>;
    using StatsProvider =
        std::function<asio::awaitable<RoomStatsReport>(LateCompletion)>;
    using ResourceProvider = std::function<ProcessResourceSample()>;
    using UiProbeDispatcher = std::function<void(
        std::uint64_t session_generation,
        std::uint64_t probe_id,
        Clock::time_point dispatched_at)>;

    static constexpr std::size_t kDefaultQueueCapacity = 256;
    static constexpr std::size_t kMaxSeries = 256;
    static constexpr std::size_t kMaxOperations = 256;
    static constexpr std::size_t kResourceTrendCapacity = 300;
    static constexpr std::size_t kResourceTrendMinimumSamples = 20;
    static constexpr std::chrono::milliseconds kRecoveryStableWindow{250};
    static constexpr std::chrono::seconds kRecoveryObservationWindow{10};

    SessionTelemetry(Strand strand,
                     std::uint64_t session_generation,
                     std::size_t queue_capacity = kDefaultQueueCapacity);

    SessionTelemetry(const SessionTelemetry&) = delete;
    SessionTelemetry& operator=(const SessionTelemetry&) = delete;

    std::uint64_t generation() const noexcept { return session_generation_; }
    bool Submit(Event event);
    std::string StartOperation(
        OperationKind kind,
        std::string id_prefix = {},
        Clock::time_point source_time = Clock::now());
    bool FinishOperation(
        const std::string& operation_id,
        OperationKind kind,
        OperationOutcome outcome,
        Clock::time_point source_time = Clock::now());
    bool RecordRoomConnectAccepted(
        std::uint64_t room_generation,
        Clock::time_point source_time = Clock::now());
    bool RegisterRemoteVideoBinding(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        Clock::time_point subscription_accepted,
        bool expected_receive,
        bool continuous_video,
        std::shared_ptr<VideoActivityProbe> probe,
        Clock::time_point source_time = Clock::now());
    bool SetRemoteVideoExpected(
        std::string series_key,
        bool expected_receive,
        MediaExpectationReason reason,
        Clock::time_point source_time = Clock::now());
    bool RecordRemoteVideoFrame(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        std::uint64_t recovery_epoch,
        bool recovery_stable,
        std::uint32_t width,
        std::uint32_t height,
        Clock::time_point source_time = Clock::now(),
        Clock::time_point last_good_at = {});
    bool RegisterRemoteAudioBinding(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        Clock::time_point subscription_accepted,
        bool expected_receive,
        std::shared_ptr<AudioActivityProbe> probe,
        Clock::time_point source_time = Clock::now());
    bool SetRemoteAudioExpected(
        std::string series_key,
        bool expected_receive,
        MediaExpectationReason reason,
        Clock::time_point source_time = Clock::now());
    bool RecordRemoteAudioFrame(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        std::uint64_t recovery_epoch,
        bool recovery_stable,
        std::uint32_t sample_rate,
        std::uint32_t channels,
        Clock::time_point source_time = Clock::now(),
        Clock::time_point last_good_at = {});
    bool RegisterRemoteVideoRenderBinding(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        Clock::time_point subscription_accepted,
        bool expected_render,
        bool continuous_video,
        std::chrono::milliseconds target_interval,
        std::shared_ptr<RenderActivityProbe> probe,
        Clock::time_point source_time = Clock::now());
    bool SetRemoteVideoRenderExpected(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        std::shared_ptr<RenderActivityProbe> probe,
        bool expected_render,
        MediaExpectationReason reason,
        Clock::time_point source_time = Clock::now());
    bool RecordRemoteVideoRenderSubmit(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        std::uint64_t frame_token,
        Clock::time_point decoded_at,
        std::string measurement_point,
        std::shared_ptr<RenderActivityProbe> probe,
        Clock::time_point source_time = Clock::now());
    std::uint64_t ActiveRecoveryEpoch() const noexcept {
        return active_recovery_epoch_.load(std::memory_order_acquire);
    }

    void SetSnapshotCallbackOnStrand(SnapshotCallback callback);
    void StartStatsSamplingOnStrand(
        StatsProvider provider,
        std::chrono::milliseconds interval = std::chrono::seconds(1));
    void RequestStatsSampleOnStrand();
    void StartRuntimeSamplingOnStrand(
        ResourceProvider resource_provider,
        UiProbeDispatcher ui_probe_dispatcher,
        std::chrono::milliseconds interval = std::chrono::seconds(1));
    void RequestRuntimeSampleOnStrand(
        Clock::time_point scheduled_at = Clock::now(),
        Clock::time_point observed_at = Clock::now());
    bool CompleteUiLagProbe(
        std::uint64_t session_generation,
        std::uint64_t probe_id,
        Clock::time_point executed_at = Clock::now());
    void RefreshOnStrand(Clock::time_point now = Clock::now());
    void StopOnStrand(std::function<void()> on_stopped = {});
    SnapshotPtr SnapshotOnStrand(Clock::time_point now = Clock::now());

private:
    struct SeriesState {
        std::uint64_t epoch = 0;
        std::uint64_t last_sequence = 0;
        std::int64_t counter_value = 0;
        bool has_counter = false;
    };

    struct OperationState {
        bool terminal = false;
        std::uint64_t sequence = 0;
        OperationKind kind = OperationKind::Unknown;
        OperationOutcome outcome = OperationOutcome::None;
        Clock::time_point started_at{};
        std::uint64_t recovery_epoch = 0;
    };

    struct MediaState {
        std::uint64_t room_generation = 0;
        std::uint64_t binding_epoch = 0;
        bool expected_receive = false;
        bool continuous_video = true;
        bool first_frame_seen = false;
        Clock::time_point subscription_accepted{};
        Clock::time_point first_frame_at{};
        std::shared_ptr<VideoActivityProbe> probe;
    };

    struct AudioMediaState {
        std::uint64_t room_generation = 0;
        std::uint64_t binding_epoch = 0;
        bool expected_receive = false;
        bool first_frame_seen = false;
        Clock::time_point subscription_accepted{};
        std::shared_ptr<AudioActivityProbe> probe;
    };

    struct RenderState {
        std::uint64_t room_generation = 0;
        std::uint64_t binding_epoch = 0;
        bool expected_render = false;
        bool continuous_video = true;
        bool first_submit_seen = false;
        Clock::time_point subscription_accepted{};
        Clock::time_point expected_since{};
        std::chrono::nanoseconds expected_accumulated{0};
        std::shared_ptr<RenderActivityProbe> probe;
    };

    struct AudioStatsBaseline {
        std::uint64_t total_samples = 0;
        std::uint64_t concealed = 0;
        std::uint64_t silent_concealed = 0;
        std::uint64_t concealment_events = 0;
        std::uint64_t inserted = 0;
        std::uint64_t removed = 0;
        std::uint64_t jitter_emitted = 0;
        double jitter_delay = 0.0;
        double jitter_target = 0.0;
        double jitter_minimum = 0.0;
        bool concealment_initialized = false;
        bool jitter_initialized = false;
        bool time_stretch_initialized = false;
    };

    struct RecoveryTrackState {
        Clock::time_point last_good_at{};
        Clock::time_point first_recovered_at{};
        Clock::time_point stable_recovered_at{};
        bool recovered = false;
    };

    struct RecoveryState {
        bool active = false;
        bool signaling_restored = false;
        bool expectation_changed = false;
        bool video_expectation_changed = false;
        bool audio_expectation_changed = false;
        bool render_expectation_changed = false;
        std::uint64_t epoch = 0;
        std::string operation_id;
        Clock::time_point outage_started_at{};
        Clock::time_point signaling_restored_at{};
        std::map<std::string, RecoveryTrackState> tracks;
        std::map<std::string, RecoveryTrackState> audio_tracks;
        std::map<std::string, RecoveryTrackState> render_tracks;
    };

    struct ResourceTrendSample {
        Clock::time_point captured_at{};
        std::optional<std::uint64_t> working_set_bytes;
        std::optional<std::uint64_t> private_bytes;
        std::optional<std::uint32_t> thread_count;
        std::optional<std::uint32_t> handle_count;
    };

    void AssertOnStrand() const;
    void ScheduleDrain();
    void DrainOnStrand();
    void ApplyOnStrand(const Event& event);
    void FinishOperationOnStrand(
        OperationState& operation,
        OperationOutcome outcome,
        Clock::time_point finished_at);
    void MaybeFinishReconnectOnStrand(Clock::time_point now);
    void UpdateFirstFrameAvailabilityOnStrand();
    void UpdateAudioFirstFrameAvailabilityOnStrand();
    void ReconcileAudioQualityExpectationOnStrand(bool reset_window);
    void UpdateRenderAvailabilityOnStrand(Clock::time_point now);
    void UpdateVideoStatsOnStrand(
        const RoomStatsReport& report,
        Clock::time_point received_at);
    void UpdateAudioStatsOnStrand(
        const RoomStatsReport& report,
        Clock::time_point received_at);
    void CompleteStatsOnStrand(
        RoomStatsReport report,
        Clock::time_point request_started,
        bool provider_failed);
    void ApplyProcessResourceSampleOnStrand(
        ProcessResourceSample sample,
        Clock::time_point observed_at);
    void UpdateResourceTrendOnStrand(const ProcessResourceSample& sample);
    void FinalizeResourceSessionOnStrand();
    void PopulateTelemetryCostOnSnapshot(
        Snapshot& snapshot,
        Clock::time_point now) const;
    void ScheduleNextStatsTickOnStrand();
    void ScheduleNextRuntimeTickOnStrand();
    void FinalizeStopOnStrand();
    void PublishSnapshotOnStrand(Clock::time_point now);
    Snapshot BuildSnapshotOnStrand(Clock::time_point now) const;

    Strand strand_;
    const std::uint64_t session_generation_;
    const std::size_t queue_capacity_;

    mutable std::mutex queue_mutex_;
    std::deque<Event> queue_;
    bool drain_posted_ = false;
    std::atomic<bool> accepting_{true};
    std::atomic<std::uint64_t> next_event_sequence_{1};
    std::atomic<std::uint64_t> next_operation_id_{1};
    std::atomic<std::uint64_t> producer_capacity_drops_{0};
    std::atomic<std::uint64_t> producer_stopped_drops_{0};
    std::atomic<std::size_t> producer_high_water_{0};
    std::atomic<std::uint64_t> next_recovery_epoch_{1};
    std::atomic<std::uint64_t> active_recovery_epoch_{0};

    Snapshot state_;
    std::map<std::string, SeriesState> series_;
    std::map<std::string, OperationState> operations_;
    std::map<std::uint64_t, Clock::time_point> room_connect_starts_;
    std::map<std::string, MediaState> media_;
    std::map<std::string, AudioMediaState> audio_media_;
    std::map<std::string, RenderState> render_media_;
    std::map<std::string, AudioStatsBaseline> audio_stats_baselines_;
    RecoveryState recovery_;
    SnapshotCallback snapshot_callback_;
    SnapshotPtr latest_snapshot_;

    StatsProvider stats_provider_;
    asio::steady_timer stats_timer_;
    std::chrono::milliseconds stats_interval_{std::chrono::seconds(1)};
    std::chrono::milliseconds stale_after_{std::chrono::seconds(3)};
    bool stats_started_ = false;
    bool stats_in_flight_ = false;
    ResourceProvider resource_provider_;
    UiProbeDispatcher ui_probe_dispatcher_;
    asio::steady_timer runtime_timer_;
    std::chrono::milliseconds runtime_interval_{std::chrono::seconds(1)};
    std::chrono::milliseconds runtime_stale_after_{std::chrono::seconds(3)};
    bool runtime_started_ = false;
    std::uint64_t next_ui_probe_id_ = 1;
    std::uint64_t ui_probe_in_flight_id_ = 0;
    Clock::time_point ui_probe_dispatched_at_{};
    std::deque<ResourceTrendSample> resource_trend_;
    std::optional<ResourceTrendSample> resource_baseline_;
    Clock::time_point resource_observation_started_at_{};
    bool stopping_ = false;
    bool stop_finalized_ = false;
    std::vector<std::function<void()>> stop_callbacks_;
};

} // namespace livekit::telemetry
