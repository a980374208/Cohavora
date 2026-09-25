#pragma once

#include "stats.h"
#include "render/canvas_render_timing.h"

#include <asio.hpp>

#include <atomic>
#include <array>
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

class SessionTelemetry;

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
    LocalPublicationBinding,
    LocalPublicationEnded,
    LocalVideoFrameInjected,
    RenderPipelineSample,
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
    Disconnect,
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

enum class ProductChainStatus {
    Implemented,
    Partial,
    Unsupported,
    ControlledHarnessOnly,
    DeferredExternal,
};

const char* ProductChainStatusName(ProductChainStatus status) noexcept;

struct MetricProductChainStatus {
    std::string metric_id;
    ProductChainStatus status = ProductChainStatus::Partial;
    std::string reason;
};

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
    static constexpr std::size_t kIntervalHistogramBuckets = 9;
    struct StageAccumulator {
        std::atomic<std::uint64_t> samples{0};
        std::atomic<std::int64_t> total_us{0};
        std::atomic<std::int64_t> maximum_us{0};
    };

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
    std::array<std::atomic<std::uint64_t>, kIntervalHistogramBuckets>
        interval_histogram{};
    std::atomic<std::int64_t> frame_age_sum_ns{0};
    std::atomic<std::int64_t> maximum_frame_age_ns{0};
    std::atomic<std::uint64_t> frame_age_count{0};
    std::atomic<MediaExpectationReason> expectation_reason{
        MediaExpectationReason::SurfaceHidden};
    std::atomic<bool> first_event_submitted{false};
    std::atomic<std::uint64_t> recovery_epoch{0};
    std::atomic<std::int64_t> recovery_first_ns{0};
    std::atomic<std::int64_t> recovery_last_good_ns{0};
    std::atomic<bool> recovery_stable_submitted{false};
    StageAccumulator cpu_convert;
    StageAccumulator upload_submit;
    // Per-tile Qt CPU paint. Shared GPU scene draws use CanvasRenderProbe.
    StageAccumulator draw_submit;
    std::int64_t target_interval_ns = 0;
    bool continuous_video = true;
};

// Session-owned cumulative costs, independent of any individual stream binding.
// Retained scenes may hold this pure counter object after teardown; it owns no
// executor, Qt object or native resource and rejects timings after session stop.
struct CanvasRenderProbe final : render::CanvasRenderTimingObserver {
    std::atomic<bool> active{true};
    RenderActivityProbe::StageAccumulator draw_submit;
    RenderActivityProbe::StageAccumulator present_block;

    void OnCanvasStageTiming(
        render::CanvasRenderStage stage, std::chrono::microseconds duration) override;
};

struct RenderPipelineSample {
    std::uint64_t router_submitted = 0;
    std::uint64_t router_replaced_before_render = 0;
    std::uint64_t router_rejected_generation = 0;
    std::uint64_t router_rejected_binding = 0;
    std::uint64_t router_dropped_invalid = 0;
    std::uint64_t router_dropped_capacity = 0;
    std::uint64_t delivered_to_gpu = 0;
    std::uint64_t delivered_to_qt_cpu = 0;
    std::uint64_t qt_cpu_conversion_failures = 0;
    std::uint64_t rejected_track_attachments = 0;
    std::uint64_t attached_track_count = 0;
    std::string requested_backend = "none";
    std::string actual_backend = "qt-cpu";
    std::string gpu_failure = "none";
    std::string fallback_reason = "none";
};

struct VideoPolicySample {
    std::uint64_t coordinator_session = 0;
    std::uint64_t native_room_generation = 0;
    std::uint64_t catalog_revision = 0;
    std::uint64_t policy_revision = 0;
    std::string stage_content = "video";
    std::string policy_reason = "hidden";
    bool retired = false;
    std::uint64_t requested = 0;
    std::uint64_t selected = 0;
    std::uint64_t actual = 0;
    std::uint64_t bound = 0;
    std::uint64_t selected_not_requested = 0;
    std::uint64_t selected_not_actual = 0;
    std::uint64_t actual_not_selected = 0;
    std::uint64_t selected_not_bound = 0;
    std::uint64_t bound_not_selected = 0;
};

enum class LocalMediaKind {
    Unknown,
    Audio,
    Video,
};

struct LocalVideoPublishDescriptor {
    std::string requested_codec;
    std::string effective_codec;
    std::string fallback_reason;
    std::string source;
    std::string mode;
    std::string resolved_profile;
    std::string resolved_scalability;
    std::vector<std::string> sender_track_ids;
};

// Written on the local capture bridge and sampled on the session strand. The
// first accepted frame also submits one typed event; later frames stay lock-free.
struct LocalVideoActivityProbe {
    std::atomic<bool> active{false};
    std::atomic<std::int64_t> first_injected_ns{0};
    std::atomic<bool> first_event_submitted{false};
    std::atomic<std::int64_t> last_frame_ns{0};
    std::atomic<std::uint64_t> frame_count{0};
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};
    std::atomic<std::int64_t> capture_timestamp_us{0};
    std::atomic<std::uint64_t> format_changes{0};
    std::atomic<std::uint64_t> clock_resets{0};
    std::weak_ptr<SessionTelemetry> telemetry;
    std::string series_key;
    std::uint64_t room_generation = 0;
    std::uint64_t publication_epoch = 0;
};

// Audio capture counterpart to LocalVideoActivityProbe. AudioFrame has no
// device timestamp, so cadence and format are observable but clock resets are
// explicitly reported as only partially covered by DEV-06.
struct LocalAudioActivityProbe {
    std::atomic<bool> active{false};
    std::atomic<std::int64_t> last_frame_ns{0};
    std::atomic<std::uint64_t> frame_count{0};
    std::atomic<std::uint32_t> sample_rate{0};
    std::atomic<std::uint32_t> channels{0};
    std::atomic<std::uint32_t> samples_per_channel{0};
    std::atomic<std::uint64_t> format_changes{0};
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
    std::string rtc_track_id;
    LocalMediaKind local_media_kind = LocalMediaKind::Unknown;
    std::uint64_t publication_epoch = 0;
    MediaExpectationReason expectation_reason = MediaExpectationReason::BindingActive;
    std::shared_ptr<VideoActivityProbe> video_probe;
    std::shared_ptr<AudioActivityProbe> audio_probe;
    std::shared_ptr<RenderActivityProbe> render_probe;
    std::shared_ptr<LocalVideoActivityProbe> local_video_probe;
    std::shared_ptr<LocalAudioActivityProbe> local_audio_probe;
    LocalVideoPublishDescriptor local_video_publish;
    RenderPipelineSample render_pipeline;
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

    Availability session_duration_availability = Availability::Unknown;
    std::string session_duration_reason = "session_not_started";
    std::string session_duration_measurement_point = "session_telemetry_lifetime";
    std::int64_t session_duration_ms = -1;
    Availability usable_duration_availability = Availability::Unknown;
    std::string usable_duration_reason = "room_not_connected";
    std::string usable_duration_measurement_point =
        "connect_success_to_reconnect_or_disconnect";
    std::int64_t usable_duration_ms = -1;
    Availability admission_to_usable_availability = Availability::Unknown;
    std::string admission_to_usable_reason = "admission_not_observed";
    std::string admission_to_usable_measurement_point =
        "admission_accept_to_startup_terminal";
    std::int64_t admission_to_usable_ms = -1;

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

    std::vector<MetricProductChainStatus> metric_product_chains;

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

    Availability internal_resource_availability = Availability::Unknown;
    std::string internal_resource_reason = "not_sampled";
    std::uint64_t active_native_bindings = 0;
    std::uint64_t active_local_media_streams = 0;
    std::uint64_t active_router_slots = 0;
    Availability router_queue_availability = Availability::Unknown;
    std::string router_queue_reason = "render_pipeline_not_sampled";
    std::uint64_t router_frames_submitted = 0;
    std::uint64_t router_frames_replaced = 0;
    std::uint64_t router_capacity_drops = 0;
    Availability export_queue_availability = Availability::Unsupported;
    std::string export_queue_reason = "history_export_queue_depth_not_exposed";

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

    Availability local_publish_media_availability = Availability::Unknown;
    std::string local_publish_media_reason = "no_local_publication";
    std::string local_publish_media_algorithm = "publish-media-v1";
    std::uint64_t local_publications = 0;
    std::uint64_t active_local_publications = 0;
    std::uint64_t expected_local_publications = 0;
    std::uint64_t local_publish_no_media = 0;
    std::uint64_t stale_local_publication_drops = 0;
    Availability local_video_injection_availability = Availability::Unknown;
    std::string local_video_injection_reason = "no_local_video_publication";
    std::string local_video_injection_measurement_point =
        "rtc_local_video_source_onframe_submission";
    std::uint64_t local_video_first_injections = 0;
    std::int64_t last_publish_to_video_injection_ms = -1;
    Availability local_video_encode_availability = Availability::Unknown;
    std::string local_video_encode_reason = "no_local_video_publication";
    std::string local_video_encode_measurement_point =
        "webrtc_outbound_rtp_frames_encoded_sample";
    std::uint64_t local_video_first_encodes = 0;
    std::int64_t last_publish_to_video_encode_ms = -1;
    Availability local_rtp_send_availability = Availability::Unknown;
    std::string local_rtp_send_reason = "no_local_publication";
    std::string local_rtp_send_measurement_point =
        "webrtc_outbound_rtp_packets_sent_sample";
    std::uint64_t local_first_rtp_sends = 0;
    std::int64_t last_publish_to_rtp_send_ms = -1;
    std::int64_t local_publish_stats_uncertainty_ms = -1;

    Availability subscription_media_availability = Availability::Unknown;
    std::string subscription_media_reason = "no_remote_media_binding";
    std::string subscription_media_measurement_point =
        "subscription_intent_to_first_media";
    std::uint64_t expected_remote_subscriptions = 0;
    std::uint64_t delivered_remote_subscriptions = 0;
    std::uint64_t remote_subscription_no_media = 0;
    std::int64_t longest_subscription_media_wait_ms = -1;

    Availability inbound_rtp_traffic_availability = Availability::Unknown;
    std::string inbound_rtp_traffic_reason = "not_sampled";
    Availability outbound_rtp_traffic_availability = Availability::Unknown;
    std::string outbound_rtp_traffic_reason = "not_sampled";
    std::string rtp_traffic_measurement_point = "rtc_rtp_payload_stats_window";
    std::uint64_t inbound_rtp_streams = 0;
    std::uint64_t outbound_rtp_streams = 0;
    std::uint64_t inbound_rtp_bytes = 0;
    std::uint64_t outbound_rtp_bytes = 0;
    std::uint64_t window_inbound_rtp_bytes = 0;
    std::uint64_t window_outbound_rtp_bytes = 0;
    double inbound_rtp_bitrate_bps = -1.0;
    double outbound_rtp_bitrate_bps = -1.0;

    Availability inbound_packet_loss_availability = Availability::Unknown;
    std::string inbound_packet_loss_reason = "not_sampled";
    std::string inbound_packet_loss_measurement_point =
        "rtc_inbound_rtp_received_and_lost_window";
    std::int64_t inbound_packets_lost = 0;
    std::int64_t window_inbound_packets_lost = 0;
    std::uint64_t window_inbound_packets_received = 0;
    double inbound_packet_loss_ratio = -1.0;
    Availability inbound_jitter_availability = Availability::Unknown;
    std::string inbound_jitter_reason = "not_sampled";
    double inbound_jitter_max_ms = -1.0;

    Availability remote_rtcp_availability = Availability::Unknown;
    std::string remote_rtcp_reason = "not_sampled";
    std::string remote_rtcp_measurement_point =
        "rtc_remote_inbound_rtcp_feedback";
    std::uint64_t remote_rtcp_streams = 0;
    double remote_rtcp_current_rtt_max_ms = -1.0;
    double remote_rtcp_window_average_rtt_ms = -1.0;
    double remote_rtcp_fraction_lost_max = -1.0;

    Availability network_recovery_availability = Availability::Unknown;
    std::string network_recovery_reason = "not_sampled";
    std::string network_recovery_measurement_point = "rtc_rtp_stats_window";
    std::string network_retransmit_ratio_denominator =
        "rtp_packets_including_retransmissions";
    Availability inbound_retransmission_availability = Availability::Unknown;
    std::string inbound_retransmission_reason = "not_sampled";
    Availability inbound_fec_availability = Availability::Unknown;
    std::string inbound_fec_reason = "not_sampled";
    Availability inbound_feedback_availability = Availability::Unknown;
    std::string inbound_feedback_reason = "not_sampled";
    Availability outbound_retransmission_availability = Availability::Unknown;
    std::string outbound_retransmission_reason = "not_sampled";
    Availability outbound_feedback_availability = Availability::Unknown;
    std::string outbound_feedback_reason = "not_sampled";
    std::uint64_t network_recovery_streams = 0;
    std::uint64_t inbound_retransmitted_packets = 0;
    std::uint64_t inbound_retransmitted_bytes = 0;
    std::uint64_t inbound_fec_packets = 0;
    std::uint64_t inbound_fec_bytes = 0;
    std::uint64_t inbound_fec_discarded_packets = 0;
    std::uint64_t inbound_nack_count = 0;
    std::uint64_t inbound_pli_count = 0;
    std::uint64_t inbound_fir_count = 0;
    std::uint64_t outbound_retransmitted_packets = 0;
    std::uint64_t outbound_retransmitted_bytes = 0;
    std::uint64_t outbound_nack_count = 0;
    std::uint64_t outbound_pli_count = 0;
    std::uint64_t outbound_fir_count = 0;
    std::uint64_t window_inbound_packets = 0;
    std::uint64_t window_inbound_retransmitted_packets = 0;
    std::uint64_t window_inbound_fec_packets = 0;
    std::uint64_t window_inbound_nack_count = 0;
    std::uint64_t window_inbound_pli_count = 0;
    std::uint64_t window_inbound_fir_count = 0;
    std::uint64_t window_outbound_packets = 0;
    std::uint64_t window_outbound_retransmitted_packets = 0;
    std::uint64_t window_outbound_nack_count = 0;
    std::uint64_t window_outbound_pli_count = 0;
    std::uint64_t window_outbound_fir_count = 0;
    double inbound_retransmitted_packet_ratio = -1.0;
    double outbound_retransmitted_packet_ratio = -1.0;

    Availability media_path_availability = Availability::Unknown;
    std::string media_path_reason = "not_sampled";
    std::string media_path_measurement_point =
        "rtc_transport_selected_candidate_pair_id";
    std::uint64_t selected_media_transports = 0;
    std::uint64_t media_path_switches = 0;
    std::string local_candidate_types;
    std::string remote_candidate_types;
    std::string local_network_types;
    std::string media_protocols;
    std::string relay_protocols;
    std::string tcp_types;

    Availability media_path_rtt_availability = Availability::Unknown;
    std::string media_path_rtt_reason = "not_sampled";
    double media_path_rtt_max_ms = -1.0;
    Availability media_bandwidth_availability = Availability::Unknown;
    std::string media_bandwidth_reason = "not_sampled";
    double media_available_outgoing_bitrate_bps = -1.0;
    double media_available_incoming_bitrate_bps = -1.0;

    Availability transport_traffic_availability = Availability::Unknown;
    std::string transport_traffic_reason = "not_sampled";
    std::string transport_traffic_measurement_point = "rtc_transport_stats_window";
    std::uint64_t transport_stats_count = 0;
    std::uint64_t transport_bytes_sent = 0;
    std::uint64_t transport_bytes_received = 0;
    std::uint64_t transport_packets_sent = 0;
    std::uint64_t transport_packets_received = 0;
    std::uint64_t window_transport_bytes_sent = 0;
    std::uint64_t window_transport_bytes_received = 0;
    std::uint64_t window_transport_packets_sent = 0;
    std::uint64_t window_transport_packets_received = 0;
    Availability transport_state_availability = Availability::Unknown;
    std::string transport_state_reason = "not_sampled";
    std::string transport_dtls_states;
    std::string transport_connectivity_states;
    std::string transport_roles;

    Availability video_quality_limitation_availability = Availability::Unknown;
    std::string video_quality_limitation_reason = "not_sampled";
    std::string video_quality_limitation_measurement_point =
        "rtc_outbound_video_quality_limitation";
    std::uint64_t outbound_video_streams = 0;
    std::string video_quality_limitation_current;
    std::int64_t video_quality_none_duration_ms = -1;
    std::int64_t video_quality_cpu_duration_ms = -1;
    std::int64_t video_quality_bandwidth_duration_ms = -1;
    std::int64_t video_quality_other_duration_ms = -1;
    std::int64_t window_video_quality_none_duration_ms = -1;
    std::int64_t window_video_quality_cpu_duration_ms = -1;
    std::int64_t window_video_quality_bandwidth_duration_ms = -1;
    std::int64_t window_video_quality_other_duration_ms = -1;
    std::int64_t video_quality_resolution_changes = -1;
    std::int64_t window_video_quality_resolution_changes = -1;
    std::uint32_t outbound_video_width = 0;
    std::uint32_t outbound_video_height = 0;
    double outbound_video_fps = -1.0;

    Availability video_pipeline_availability = Availability::Unknown;
    std::string video_pipeline_reason = "not_sampled";
    std::string video_pipeline_measurement_point = "rtc_video_stats_window";
    std::uint64_t inbound_video_frames_received = 0;
    std::uint64_t inbound_video_frames_decoded = 0;
    std::uint64_t inbound_video_frames_dropped = 0;
    std::uint64_t outbound_video_frames_encoded = 0;
    std::uint64_t outbound_video_frames_sent = 0;
    std::uint64_t window_inbound_video_frames_received = 0;
    std::uint64_t window_inbound_video_frames_decoded = 0;
    std::uint64_t window_inbound_video_frames_dropped = 0;
    std::uint64_t window_outbound_video_frames_encoded = 0;
    std::uint64_t window_outbound_video_frames_sent = 0;
    double inbound_video_frame_drop_ratio = -1.0;
    std::uint32_t inbound_video_width = 0;
    std::uint32_t inbound_video_height = 0;
    double inbound_video_fps = -1.0;

    Availability video_codec_availability = Availability::Unknown;
    std::string video_codec_reason = "not_sampled";
    std::string video_codec_measurement_point = "rtc_codec_id_join";
    std::string inbound_video_codecs;
    std::string outbound_video_codecs;
    std::string decoder_implementations;
    std::string encoder_implementations;
    std::string decoder_power_efficiency;
    std::string encoder_power_efficiency;
    std::string outbound_video_layers;

    Availability video_publish_plan_availability = Availability::Unknown;
    std::string video_publish_plan_reason = "no_local_video_publication";
    std::string video_publish_plan_measurement_point =
        "resolved_publish_plan_and_outbound_rtp_stats";
    std::string video_publish_requested_codecs;
    std::string video_publish_effective_codecs;
    std::string video_publish_observed_codecs;
    std::string video_publish_fallback_reasons = "none";
    std::string video_publish_sources;
    std::string video_publish_direction;
    std::string video_publish_generations;
    std::string video_publish_modes;
    std::string video_publish_resolved_profiles;
    std::string video_publish_observed_profiles = "pending";
    std::string video_publish_encoder_implementations;
    std::string video_publish_resolved_scalability;
    std::string video_publish_observed_scalability;

    Availability video_processing_availability = Availability::Unknown;
    std::string video_processing_reason = "not_sampled";
    std::string video_processing_measurement_point =
        "rtc_total_processing_time_counter_delta";
    double video_decode_ms_per_frame = -1.0;
    double video_encode_ms_per_frame = -1.0;

    Availability local_device_continuity_availability = Availability::Unknown;
    std::string local_device_continuity_reason = "no_local_publication";
    std::string local_device_continuity_measurement_point =
        "rtc_local_source_submission_probe";
    std::string local_device_continuity_algorithm = "device-continuity-v1";
    std::uint64_t expected_local_device_streams = 0;
    std::uint64_t active_local_device_streams = 0;
    std::uint64_t local_device_unexpected_stops = 0;
    std::int64_t local_device_interruption_duration_ms = 0;
    std::uint64_t local_device_format_changes = 0;
    std::uint64_t local_device_clock_resets = 0;

    Availability device_open_availability = Availability::Unsupported;
    std::string device_open_reason = "native_device_open_milestone_not_exposed";
    std::string device_open_measurement_point = "native_capture_provider";
    Availability device_hotplug_availability = Availability::Unsupported;
    std::string device_hotplug_reason = "os_device_change_provider_not_installed";
    std::string device_hotplug_measurement_point = "os_device_notification";
    Availability device_failure_availability = Availability::Unknown;
    std::string device_failure_reason = "no_device_switch_operation";
    std::uint64_t device_switch_attempts = 0;
    std::uint64_t device_switch_successes = 0;
    std::uint64_t device_switch_failures = 0;
    std::uint64_t device_switch_timeouts = 0;
    Availability device_state_availability = Availability::Unknown;
    std::string device_state_reason = "no_local_publication";
    std::string device_state_measurement_point = "rtc_local_source_submission_probe";
    bool microphone_requested = false;
    bool microphone_effective = false;
    bool camera_requested = false;
    bool camera_effective = false;
    std::uint32_t actual_capture_width = 0;
    std::uint32_t actual_capture_height = 0;
    std::uint32_t actual_capture_sample_rate = 0;
    std::uint32_t actual_capture_channels = 0;

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
    std::int64_t last_admission_to_first_render_ms = -1;
    double render_average_interval_ms = -1.0;
    std::int64_t render_maximum_interval_ms = -1;
    double render_interval_p50_ms = -1.0;
    double render_interval_p95_ms = -1.0;
    double render_interval_p99_ms = -1.0;
    double render_submit_fps = -1.0;
    Availability render_frame_age_availability = Availability::Unknown;
    std::string render_frame_age_reason = "no_render_submit";
    std::string render_frame_age_measurement_point =
        "native_decode_to_actual_render_submit";
    double render_average_frame_age_ms = -1.0;
    std::int64_t render_maximum_frame_age_ms = -1;
    std::int64_t render_target_interval_ms = -1;
    std::uint64_t render_expected_bindings = 0;
    std::uint64_t render_hidden_bindings = 0;
    std::uint64_t render_minimized_bindings = 0;
    std::uint64_t render_policy_skipped_frames = 0;

    Availability render_pipeline_availability = Availability::Unknown;
    std::string render_pipeline_reason = "render_pipeline_not_sampled";
    std::string render_pipeline_measurement_point =
        "video_render_session_and_router_statistics";
    std::uint64_t render_router_submitted = 0;
    std::uint64_t render_router_replaced = 0;
    std::uint64_t render_router_rejected_generation = 0;
    std::uint64_t render_router_rejected_binding = 0;
    std::uint64_t render_router_dropped_invalid = 0;
    std::uint64_t render_router_dropped_capacity = 0;
    std::uint64_t render_delivered_to_gpu = 0;
    std::uint64_t render_delivered_to_qt_cpu = 0;
    std::uint64_t render_qt_conversion_failures = 0;
    std::uint64_t render_rejected_track_attachments = 0;
    std::uint64_t render_attached_track_count = 0;
    std::string render_requested_backend = "none";
    std::string render_actual_backend = "qt-cpu";
    std::string render_gpu_failure = "none";
    std::string render_fallback_reason = "none";
    std::uint64_t render_backend_failures = 0;
    std::uint64_t render_backend_fallbacks = 0;

    Availability video_policy_availability = Availability::Unknown;
    std::string video_policy_reason = "video_policy_not_observed";
    std::string video_policy_measurement_point =
        "session_video_policy_convergence";
    std::uint64_t video_policy_coordinator_session = 0;
    std::uint64_t video_policy_native_room_generation = 0;
    std::uint64_t video_policy_catalog_revision = 0;
    std::uint64_t video_policy_revision = 0;
    std::string video_policy_stage_content = "video";
    std::string video_policy_selection_reason = "hidden";
    bool video_policy_retired = false;
    std::uint64_t video_policy_requested = 0;
    std::uint64_t video_policy_selected = 0;
    std::uint64_t video_policy_actual = 0;
    std::uint64_t video_policy_bound = 0;
    std::uint64_t video_policy_selected_not_requested = 0;
    std::uint64_t video_policy_selected_not_actual = 0;
    std::uint64_t video_policy_actual_not_selected = 0;
    std::uint64_t video_policy_selected_not_bound = 0;
    std::uint64_t video_policy_bound_not_selected = 0;
    std::uint64_t video_policy_stale_updates = 0;

    Availability render_stall_availability = Availability::Unknown;
    std::string render_stall_reason = "no_expected_render_binding";
    std::string render_stall_algorithm = "render-stall-v1";
    std::uint64_t render_stall_count = 0;
    std::int64_t render_stall_duration_ms = 0;
    std::int64_t render_longest_stall_ms = 0;
    std::int64_t render_expected_duration_ms = 0;
    double render_stall_ratio = -1.0;
    bool render_stall_active = false;

    Availability render_stage_availability = Availability::Unknown;
    std::string render_stage_reason = "no_render_stage_samples";
    std::string render_stage_measurement_point = "render_cpu_submission_spans";
    // Convert/upload are per-resource CPU spans. Draw is a Qt tile paint or a
    // single GPU canvas draw. Present is one GL swap / DX11 Render+Present call
    // per participating session, never multiplied by its video resource count.
    std::uint64_t render_convert_samples = 0;
    std::int64_t render_convert_total_us = 0;
    std::int64_t render_convert_max_us = -1;
    std::uint64_t render_upload_samples = 0;
    std::int64_t render_upload_total_us = 0;
    std::int64_t render_upload_max_us = -1;
    std::uint64_t render_draw_samples = 0;
    std::int64_t render_draw_total_us = 0;
    std::int64_t render_draw_max_us = -1;
    std::uint64_t render_present_block_samples = 0;
    std::int64_t render_present_block_total_us = 0;
    std::int64_t render_present_block_max_us = -1;
    Availability render_gpu_execution_availability = Availability::Unsupported;
    std::string render_gpu_execution_reason = "gpu_timestamp_query_not_available";

    Availability reconnect_render_availability = Availability::Unknown;
    std::string reconnect_render_reason = "no_reconnect_episode";
    std::string reconnect_render_measurement_point = "visible_render_submit";
    std::uint64_t reconnect_render_expected = 0;
    std::uint64_t reconnect_render_recovered = 0;
    std::int64_t last_reconnect_first_render_ms = -1;
    std::int64_t last_reconnect_stable_render_ms = -1;
    std::int64_t last_reconnect_render_interruption_ms = -1;

    Availability reconnect_density_availability = Availability::Unknown;
    std::string reconnect_density_reason = "usable_duration_not_available";
    std::uint64_t reconnect_episodes = 0;
    double reconnect_episodes_per_hour = -1.0;

    Availability stability_anomaly_density_availability = Availability::Unknown;
    std::string stability_anomaly_density_reason =
        "usable_duration_not_available";
    std::string stability_anomaly_density_algorithm =
        "stability-anomaly-density-v1";
    std::uint64_t stability_operation_failures = 0;
    std::uint64_t stability_sampler_interruptions = 0;
    std::uint64_t stability_device_stops = 0;
    std::uint64_t stability_media_failures = 0;
    std::uint64_t stability_anomalies = 0;
    double stability_anomalies_per_hour = -1.0;
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
    static constexpr std::chrono::seconds kLocalPublishObservationWindow{5};
    static constexpr std::chrono::seconds kSubscriptionMediaObservationWindow{5};
    static constexpr std::chrono::seconds kLocalDeviceStallThreshold{1};

    SessionTelemetry(Strand strand,
                     std::uint64_t session_generation,
                     std::size_t queue_capacity = kDefaultQueueCapacity,
                     Clock::time_point session_started_at = Clock::now(),
                     std::shared_ptr<void> executor_lifetime = {});

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
    bool RegisterLocalPublication(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t publication_epoch,
        LocalMediaKind media_kind,
        std::string rtc_track_id,
        Clock::time_point publish_accepted_at,
        bool expected_send,
        std::shared_ptr<LocalVideoActivityProbe> video_probe = {},
        Clock::time_point committed_at = Clock::now(),
        std::shared_ptr<LocalAudioActivityProbe> audio_probe = {},
        LocalVideoPublishDescriptor video_publish = {});
    bool EndLocalPublication(
        std::string series_key,
        Clock::time_point source_time = Clock::now());
    bool RecordLocalVideoFrameInjected(
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t publication_epoch,
        Clock::time_point source_time = Clock::now());
    bool RecordRenderPipelineSample(
        RenderPipelineSample sample,
        Clock::time_point source_time = Clock::now());
    bool RecordVideoPolicySampleOnStrand(VideoPolicySample sample);
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

    // Immutable pointer identity; safe to obtain from render producer threads.
    std::shared_ptr<CanvasRenderProbe> canvasRenderProbe() const {
        return canvas_render_probe_;
    }

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
        Clock::time_point first_frame_at{};
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

    struct NetworkStatsBaseline {
        std::uint64_t bytes = 0;
        std::uint64_t packets = 0;
        std::int64_t packets_lost = 0;
        std::uint64_t retransmitted_packets = 0;
        std::uint64_t retransmitted_bytes = 0;
        std::uint64_t fec_packets = 0;
        std::uint64_t fec_bytes = 0;
        std::uint64_t fec_discarded = 0;
        std::uint64_t nack = 0;
        std::uint64_t pli = 0;
        std::uint64_t fir = 0;
        std::uint32_t availability_mask = 0;
        Clock::time_point observed_at{};
        bool initialized = false;
    };

    struct RemoteRtcpBaseline {
        double total_round_trip_time = 0.0;
        std::uint64_t round_trip_time_measurements = 0;
        bool initialized = false;
    };

    struct VideoQualityBaseline {
        std::unordered_map<std::string, double> durations;
        std::uint32_t resolution_changes = 0;
        bool resolution_changes_available = false;
        bool initialized = false;
    };

    struct VideoStatsBaseline {
        std::uint64_t primary_frames = 0;
        std::uint64_t secondary_frames = 0;
        std::uint64_t dropped_frames = 0;
        double processing_seconds = 0.0;
        Clock::time_point observed_at{};
        std::uint32_t availability_mask = 0;
        bool initialized = false;
    };

    struct TransportBaseline {
        std::string selected_pair_id;
        std::uint32_t selected_pair_changes = 0;
        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t packets_sent = 0;
        std::uint64_t packets_received = 0;
        std::uint32_t traffic_availability_mask = 0;
        Clock::time_point observed_at{};
        bool traffic_initialized = false;
        bool initialized = false;
        bool changes_available = false;
    };

    struct LocalPublicationState {
        std::uint64_t room_generation = 0;
        std::uint64_t publication_epoch = 0;
        LocalMediaKind media_kind = LocalMediaKind::Unknown;
        std::string rtc_track_id;
        bool active = true;
        bool expected_send = true;
        bool was_expected = true;
        bool sender_observed = false;
        bool sender_enabled = true;
        bool outbound_mapping_observed = false;
        bool encode_counter_observed = false;
        bool send_counter_observed = false;
        bool injection_timeout = false;
        bool encode_timeout = false;
        bool send_timeout = false;
        bool no_media_reported = false;
        Clock::time_point publish_accepted_at{};
        Clock::time_point committed_at{};
        Clock::time_point first_injected_at{};
        Clock::time_point first_encoded_at{};
        Clock::time_point first_sent_at{};
        std::shared_ptr<LocalVideoActivityProbe> video_probe;
        std::shared_ptr<LocalAudioActivityProbe> audio_probe;
        LocalVideoPublishDescriptor video_publish;
        bool device_stall_active = false;
        Clock::time_point device_stall_started_at{};
        std::chrono::nanoseconds device_stall_accumulated{0};
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
    void UpdateLocalPublishAvailabilityOnStrand(Clock::time_point now);
    void UpdateVideoPublishPlanOnStrand();
    void UpdateLocalPublishStatsOnStrand(
        const RoomStatsReport& report,
        Clock::time_point received_at);
    void CloseUsableIntervalOnStrand(Clock::time_point now);
    void UpdateVideoStatsOnStrand(
        const RoomStatsReport& report,
        Clock::time_point received_at);
    void UpdateNetworkStatsOnStrand(
        const RoomStatsReport& report,
        Clock::time_point received_at);
    void UpdateLocalDeviceStatsOnStrand(Clock::time_point now);
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

    // Declared first so timers/strand die before the shared executor lease.
    const std::shared_ptr<void> executor_lifetime_;
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
    const std::shared_ptr<CanvasRenderProbe> canvas_render_probe_ =
        std::make_shared<CanvasRenderProbe>();
    std::map<std::string, LocalPublicationState> local_publications_;
    std::map<std::string, AudioStatsBaseline> audio_stats_baselines_;
    std::map<std::string, NetworkStatsBaseline> inbound_network_baselines_;
    std::map<std::string, NetworkStatsBaseline> outbound_network_baselines_;
    std::map<std::string, RemoteRtcpBaseline> remote_rtcp_baselines_;
    std::map<std::string, VideoQualityBaseline> video_quality_baselines_;
    std::map<std::string, VideoStatsBaseline> video_stats_baselines_;
    std::map<std::string, TransportBaseline> transport_baselines_;
    std::uint64_t retired_device_format_changes_ = 0;
    std::uint64_t retired_device_clock_resets_ = 0;
    std::chrono::nanoseconds retired_device_interruption_{0};
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
    Clock::time_point session_started_at_{};
    Clock::time_point latest_admission_accepted_at_{};
    Clock::time_point session_stopped_at_{};
    Clock::time_point usable_since_{};
    std::chrono::milliseconds usable_accumulated_{0};
    bool room_was_usable_ = false;
    bool stopping_ = false;
    bool stop_finalized_ = false;
    std::vector<std::function<void()>> stop_callbacks_;
};

} // namespace livekit::telemetry
