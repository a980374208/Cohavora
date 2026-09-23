#include "telemetry_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;

constexpr std::array<const char*, 4> kReportFiles = {
    "manifest.json", "session.json", "metrics.jsonl", "metrics.csv"};

std::mutex g_installed_mutex;
std::weak_ptr<TelemetryHistoryStore> g_installed_store;

std::int64_t UtcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string SafeText(std::string value) {
    if (value.size() > 160) value.resize(160);
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    }), value.end());
    std::string folded = value;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const auto unsafe = folded.find("://") != std::string::npos ||
        folded.find("token=") != std::string::npos ||
        folded.find("candidate:") != std::string::npos ||
        folded.find("a=ice") != std::string::npos ||
        folded.find("sdp") != std::string::npos ||
        value.find('\\') != std::string::npos;
    if (unsafe) return "redacted_unsafe_text";
    if (!value.empty() &&
        (value.front() == '=' || value.front() == '+' ||
         value.front() == '-' || value.front() == '@')) {
        value.insert(value.begin(), '\'');
    }
    return value;
}

std::string CsvCell(std::string value) {
    value = SafeText(std::move(value));
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        if (ch == '"') escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

Json JsonValue(const MetricValue& value) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<T, std::string>) return SafeText(item);
        else return item;
    }, value);
}

std::string CsvValue(const MetricValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return {};
        else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return SafeText(item);
        else {
            std::ostringstream output;
            if constexpr (std::is_same_v<T, double>) {
                output << std::setprecision(15);
            }
            output << item;
            return output.str();
        }
    }, value);
}

void AddMetric(
    std::vector<SafeMetricRow>& rows,
    std::string key,
    MetricValue value,
    std::string unit,
    Availability availability,
    const std::string& reason,
    const std::string& measurement_point = {}) {
    rows.push_back({
        std::move(key), std::move(value), std::move(unit),
        AvailabilityName(availability), SafeText(reason),
        SafeText(measurement_point)});
}

MetricValue Signed(std::int64_t value) {
    return value < 0 ? MetricValue{std::monostate{}} : MetricValue{value};
}

MetricValue Ratio(double value) {
    return value < 0.0 ? MetricValue{std::monostate{}} : MetricValue{value};
}

std::uint64_t DirectoryKnownSize(const std::filesystem::path& directory) {
    std::uint64_t total = 0;
    std::error_code error;
    for (const auto* name : kReportFiles) {
        const auto path = directory / name;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            error.clear();
            continue;
        }
        total += std::filesystem::file_size(path, error);
        if (error) error.clear();
    }
    return total;
}

bool RemoveKnownReport(const std::filesystem::path& directory) {
    std::error_code error;
    const bool recognized = std::filesystem::is_regular_file(
        directory / "manifest.json", error) && !error;
    error.clear();
    for (const auto* name : kReportFiles) {
        std::filesystem::remove(directory / name, error);
        error.clear();
    }
    std::filesystem::remove(directory, error);
    error.clear();
    const auto known_files_gone = std::all_of(
        kReportFiles.begin(), kReportFiles.end(), [&](const char* name) {
            return !std::filesystem::exists(directory / name);
        });
    return recognized && known_files_gone;
}

bool WriteText(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.flush();
    return static_cast<bool>(output);
}

Json OperationJson(const OperationSummary& operation) {
    return {
        {"kind", OperationKindName(operation.kind)},
        {"started", operation.started},
        {"terminal", operation.terminal},
        {"success", operation.success},
        {"degraded_success", operation.degraded_success},
        {"failure", operation.failure},
        {"timeout", operation.timeout},
        {"cancelled", operation.cancelled},
        {"inflight", operation.inflight},
        {"last_duration_ms", operation.last_duration_ms < 0
            ? Json(nullptr) : Json(operation.last_duration_ms)},
    };
}

} // namespace

std::vector<SafeMetricRow> BuildSafeMetricRows(
    const SafeTelemetryRecord& record) {
    const auto& s = record.snapshot;
    std::vector<SafeMetricRow> rows;
    rows.reserve(150);
    const auto base = [&](std::string key, MetricValue value, std::string unit = {}) {
        AddMetric(rows, std::move(key), std::move(value), std::move(unit),
                  s.availability, s.reason);
    };
    const auto group = [&](std::string key, MetricValue value, std::string unit,
                           Availability availability, const std::string& reason,
                           const std::string& point = std::string{}) {
        AddMetric(rows, std::move(key), std::move(value), std::move(unit),
                  availability, reason, point);
    };

    base("coverage", s.coverage, "ratio");
    base("sample_age", Signed(s.sample_age_ms), "ms");
    base("queue.capacity", static_cast<std::uint64_t>(s.queue_capacity), "events");
    base("queue.depth", static_cast<std::uint64_t>(s.queue_depth), "events");
    base("queue.high_water", static_cast<std::uint64_t>(s.queue_high_water), "events");
    base("queue.capacity_drops", s.capacity_drops, "events");
    base("queue.stopped_drops", s.stopped_drops, "events");
    base("queue.stale_generation_drops", s.stale_generation_drops, "events");
    base("queue.out_of_order_drops", s.out_of_order_drops, "events");
    base("late_callbacks", s.late_callbacks, "callbacks");
    base("mapping_failures", s.mapping_failures, "events");
    base("counter_resets", s.counter_resets, "events");
    base("samples.valid", s.valid_samples, "samples");
    base("samples.unavailable", s.unavailable_samples, "samples");

    group("queue.lag.samples", s.event_queue_lag_samples, "samples",
          s.event_queue_lag_availability, s.event_queue_lag_reason);
    group("queue.lag.last", Signed(s.last_event_queue_lag_ms), "ms",
          s.event_queue_lag_availability, s.event_queue_lag_reason);
    group("queue.lag.maximum", Signed(s.maximum_event_queue_lag_ms), "ms",
          s.event_queue_lag_availability, s.event_queue_lag_reason);

    group("resource.sample_age", Signed(s.resource_sample_age_ms), "ms",
          s.resource_availability, s.resource_reason);
    group("resource.samples", s.resource_samples, "samples",
          s.resource_availability, s.resource_reason);
    group("resource.sample_failures", s.resource_sample_failures, "samples",
          s.resource_availability, s.resource_reason);
    group("resource.sample_duration", Signed(s.last_resource_sample_us), "us",
          s.resource_availability, s.resource_reason);
    group("resource.cpu", Ratio(s.process_cpu_percent), "percent",
          s.cpu_availability, s.cpu_reason);
    group("resource.logical_processors", static_cast<std::uint64_t>(s.logical_processor_count),
          "processors", s.cpu_availability, s.cpu_reason);
    group("resource.working_set", s.working_set_bytes, "bytes",
          s.memory_availability, s.memory_reason);
    group("resource.peak_working_set", s.peak_working_set_bytes, "bytes",
          s.memory_availability, s.memory_reason);
    group("resource.private_bytes", s.private_bytes, "bytes",
          s.memory_availability, s.memory_reason);
    group("resource.thread_count", static_cast<std::uint64_t>(s.process_thread_count),
          "threads", s.thread_count_availability, s.thread_count_reason);
    group("resource.thread_count_age", Signed(s.thread_count_sample_age_ms), "ms",
          s.thread_count_availability, s.thread_count_reason);
    group("resource.handle_count", static_cast<std::uint64_t>(s.process_handle_count),
          "handles", s.handle_count_availability, s.handle_count_reason);
    group("resource.gpu", std::monostate{}, "",
          s.gpu_resource_availability, s.gpu_resource_reason);

    group("resource.trend.samples", s.resource_trend_samples, "samples",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.span", Signed(s.resource_trend_span_ms), "ms",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.coverage", s.resource_trend_coverage, "ratio",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.working_set_min", s.minimum_working_set_bytes, "bytes",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.working_set_max", s.maximum_working_set_bytes, "bytes",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.private_bytes_min", s.minimum_private_bytes, "bytes",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.private_bytes_max", s.maximum_private_bytes, "bytes",
          s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.thread_count_min", static_cast<std::uint64_t>(s.minimum_thread_count),
          "threads", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.thread_count_max", static_cast<std::uint64_t>(s.maximum_thread_count),
          "threads", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.handle_count_min", static_cast<std::uint64_t>(s.minimum_handle_count),
          "handles", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.handle_count_max", static_cast<std::uint64_t>(s.maximum_handle_count),
          "handles", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.private_growth", s.private_bytes_growth_mib_per_minute,
          "MiB/min", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.thread_growth", s.thread_growth_per_hour,
          "threads/hour", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.trend.handle_growth", s.handle_growth_per_hour,
          "handles/hour", s.resource_trend_availability, s.resource_trend_reason);
    group("resource.session.working_set_delta", s.working_set_delta_bytes, "bytes",
          s.resource_session_delta_availability, s.resource_session_delta_reason);
    group("resource.session.private_bytes_delta", s.private_bytes_delta, "bytes",
          s.resource_session_delta_availability, s.resource_session_delta_reason);
    group("resource.session.thread_count_delta", s.thread_count_delta, "threads",
          s.resource_session_delta_availability, s.resource_session_delta_reason);
    group("resource.session.handle_count_delta", s.handle_count_delta, "handles",
          s.resource_session_delta_availability, s.resource_session_delta_reason);
    group("resource.final_delta", std::monostate{}, "",
          s.resource_final_delta_availability, s.resource_final_delta_reason);
    group("resource.return", std::monostate{}, "",
          s.resource_return_availability, s.resource_return_reason);

    group("runtime.strand_lag.samples", s.strand_lag_samples, "samples",
          s.strand_lag_availability, s.strand_lag_reason);
    group("runtime.strand_lag.last", Signed(s.last_strand_lag_ms), "ms",
          s.strand_lag_availability, s.strand_lag_reason);
    group("runtime.strand_lag.maximum", Signed(s.maximum_strand_lag_ms), "ms",
          s.strand_lag_availability, s.strand_lag_reason);
    group("runtime.ui_lag.samples", s.ui_lag_samples, "samples",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_lag.last", Signed(s.last_ui_lag_ms), "ms",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_lag.maximum", Signed(s.maximum_ui_lag_ms), "ms",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_probe.timeouts", s.ui_probe_timeouts, "probes",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_probe.skipped", s.ui_probe_skipped, "probes",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_probe.late_callbacks", s.ui_probe_late_callbacks, "callbacks",
          s.ui_lag_availability, s.ui_lag_reason);
    group("runtime.ui_probe.in_flight", s.ui_probe_in_flight, "bool",
          s.ui_lag_availability, s.ui_lag_reason);

    base("stats.in_flight", s.stats_in_flight, "bool");
    base("stats.actual_peer_connections", static_cast<std::uint64_t>(s.actual_pc_count), "connections");
    base("stats.successful_peer_connections", static_cast<std::uint64_t>(s.successful_pc_count), "connections");
    base("stats.requests_started", s.stats_requests_started, "requests");
    base("stats.requests_completed", s.stats_requests_completed, "requests");
    base("stats.request_timeouts", s.stats_request_timeouts, "requests");
    base("stats.request_rejections", s.stats_request_rejections, "requests");
    base("stats.requests_skipped", s.stats_requests_skipped, "requests");
    base("stats.last_request_duration", Signed(s.last_stats_request_ms), "ms");
    base("operations.started", s.operations_started, "operations");
    base("operations.terminal", s.operations_terminal, "operations");
    base("operations.inflight", s.operations_inflight, "operations");
    base("operations.missing_start", s.operations_missing_start, "operations");
    base("operations.duplicate_terminal", s.operations_duplicate_terminal, "operations");
    base("operations.kind_mismatch", s.operations_kind_mismatch, "operations");

    group("video.first_frame.bindings", s.remote_video_bindings, "bindings",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.count", s.remote_video_first_frames, "frames",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.stale_binding_drops", s.stale_binding_frame_drops, "frames",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.connect_to_decoded", Signed(s.room_connect_to_first_decoded_ms), "ms",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.last_connect_to_decoded", Signed(s.last_connect_to_first_decoded_ms), "ms",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.last_subscribe_to_decoded", Signed(s.last_subscribe_to_first_decoded_ms), "ms",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.width", static_cast<std::uint64_t>(s.last_decoded_width), "pixels",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.first_frame.height", static_cast<std::uint64_t>(s.last_decoded_height), "pixels",
          s.remote_video_first_frame_availability, s.remote_video_first_frame_reason,
          s.remote_video_first_frame_measurement_point);
    group("video.native_freeze.streams", s.native_video_streams, "streams",
          s.native_video_freeze_availability, s.native_video_freeze_reason,
          s.native_video_freeze_measurement_point);
    group("video.native_freeze.count", s.native_video_freeze_count, "freezes",
          s.native_video_freeze_availability, s.native_video_freeze_reason,
          s.native_video_freeze_measurement_point);
    group("video.native_freeze.duration", Signed(s.native_video_freeze_duration_ms), "ms",
          s.native_video_freeze_availability, s.native_video_freeze_reason,
          s.native_video_freeze_measurement_point);

    group("reconnect.video.expected", s.reconnect_video_expected, "recoveries",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.video.recovered", s.reconnect_video_recovered, "recoveries",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.expectation_changes", s.reconnect_expectation_changes, "events",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.media_timeouts", s.reconnect_media_timeouts, "episodes",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.signaling", Signed(s.last_reconnect_signaling_ms), "ms",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.video.first", Signed(s.last_reconnect_first_video_ms), "ms",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.video.stable", Signed(s.last_reconnect_stable_video_ms), "ms",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);
    group("reconnect.video.interruption", Signed(s.last_reconnect_media_interruption_ms), "ms",
          s.reconnect_video_availability, s.reconnect_video_reason,
          s.reconnect_video_measurement_point);

    group("audio.first_frame.bindings", s.remote_audio_bindings, "bindings",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.first_frame.count", s.remote_audio_first_frames, "frames",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.first_frame.stale_binding_drops", s.stale_audio_binding_drops, "frames",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.first_frame.subscribe_to_pcm", Signed(s.last_subscribe_to_first_pcm_ms), "ms",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.first_frame.sample_rate", static_cast<std::uint64_t>(s.last_audio_sample_rate), "Hz",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.first_frame.channels", static_cast<std::uint64_t>(s.last_audio_channels), "channels",
          s.remote_audio_first_frame_availability, s.remote_audio_first_frame_reason,
          s.remote_audio_first_frame_measurement_point);
    group("audio.streams", s.audio_streams, "streams",
          s.audio_quality_availability, s.audio_quality_reason,
          s.audio_quality_measurement_point);
    group("audio.window.samples", s.audio_window_samples, "samples",
          s.audio_quality_availability, s.audio_quality_reason,
          s.audio_quality_measurement_point);
    group("audio.concealment.samples", s.audio_window_concealed_samples, "samples",
          s.audio_concealment_availability, s.audio_concealment_reason,
          s.audio_quality_measurement_point);
    group("audio.concealment.silent_samples", s.audio_window_silent_concealed_samples, "samples",
          s.audio_concealment_availability, s.audio_concealment_reason,
          s.audio_quality_measurement_point);
    group("audio.concealment.events", s.audio_window_concealment_events, "events",
          s.audio_concealment_availability, s.audio_concealment_reason,
          s.audio_quality_measurement_point);
    group("audio.concealment.ratio", Ratio(s.audio_concealed_ratio), "ratio",
          s.audio_concealment_availability, s.audio_concealment_reason,
          s.audio_quality_measurement_point);
    group("audio.concealment.non_silent_ratio", Ratio(s.audio_non_silent_concealed_ratio), "ratio",
          s.audio_concealment_availability, s.audio_concealment_reason,
          s.audio_quality_measurement_point);
    group("audio.jitter_buffer.delay", Ratio(s.audio_jitter_buffer_delay_ms), "ms",
          s.audio_jitter_buffer_availability, s.audio_jitter_buffer_reason,
          s.audio_quality_measurement_point);
    group("audio.jitter_buffer.target_delay", Ratio(s.audio_jitter_buffer_target_delay_ms), "ms",
          s.audio_jitter_buffer_availability, s.audio_jitter_buffer_reason,
          s.audio_quality_measurement_point);
    group("audio.jitter_buffer.minimum_delay", Ratio(s.audio_jitter_buffer_minimum_delay_ms), "ms",
          s.audio_jitter_buffer_availability, s.audio_jitter_buffer_reason,
          s.audio_quality_measurement_point);
    group("audio.time_stretch.inserted_samples", s.audio_inserted_samples, "samples",
          s.audio_time_stretch_availability, s.audio_time_stretch_reason,
          s.audio_quality_measurement_point);
    group("audio.time_stretch.removed_samples", s.audio_removed_samples, "samples",
          s.audio_time_stretch_availability, s.audio_time_stretch_reason,
          s.audio_quality_measurement_point);
    group("audio.time_stretch.inserted_ratio", Ratio(s.audio_inserted_ratio), "ratio",
          s.audio_time_stretch_availability, s.audio_time_stretch_reason,
          s.audio_quality_measurement_point);
    group("audio.time_stretch.removed_ratio", Ratio(s.audio_removed_ratio), "ratio",
          s.audio_time_stretch_availability, s.audio_time_stretch_reason,
          s.audio_quality_measurement_point);
    group("reconnect.audio.expected", s.reconnect_audio_expected, "recoveries",
          s.reconnect_audio_availability, s.reconnect_audio_reason,
          s.reconnect_audio_measurement_point);
    group("reconnect.audio.recovered", s.reconnect_audio_recovered, "recoveries",
          s.reconnect_audio_availability, s.reconnect_audio_reason,
          s.reconnect_audio_measurement_point);
    group("reconnect.audio.first", Signed(s.last_reconnect_first_audio_ms), "ms",
          s.reconnect_audio_availability, s.reconnect_audio_reason,
          s.reconnect_audio_measurement_point);
    group("reconnect.audio.stable", Signed(s.last_reconnect_stable_audio_ms), "ms",
          s.reconnect_audio_availability, s.reconnect_audio_reason,
          s.reconnect_audio_measurement_point);
    group("reconnect.audio.interruption", Signed(s.last_reconnect_audio_interruption_ms), "ms",
          s.reconnect_audio_availability, s.reconnect_audio_reason,
          s.reconnect_audio_measurement_point);

    group("render.first_frame.bindings", s.render_bindings, "bindings",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.submits", s.unique_render_submits, "frames",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.stale_binding_drops", s.stale_render_binding_drops, "frames",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.decode_to_first", Signed(s.last_decode_to_render_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.subscribe_to_first", Signed(s.last_subscribe_to_first_render_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.connect_to_first", Signed(s.last_connect_to_first_render_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.average", Ratio(s.render_average_interval_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.maximum", Signed(s.render_maximum_interval_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.algorithm", SafeText(s.render_stall_algorithm), "version",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.count", s.render_stall_count, "stalls",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.duration", s.render_stall_duration_ms, "ms",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.longest", s.render_longest_stall_ms, "ms",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.expected_duration", s.render_expected_duration_ms, "ms",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.ratio", Ratio(s.render_stall_ratio), "ratio",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("render.stall.active", s.render_stall_active, "bool",
          s.render_stall_availability, s.render_stall_reason,
          s.render_first_frame_measurement_point);
    group("reconnect.render.expected", s.reconnect_render_expected, "recoveries",
          s.reconnect_render_availability, s.reconnect_render_reason,
          s.reconnect_render_measurement_point);
    group("reconnect.render.recovered", s.reconnect_render_recovered, "recoveries",
          s.reconnect_render_availability, s.reconnect_render_reason,
          s.reconnect_render_measurement_point);
    group("reconnect.render.first", Signed(s.last_reconnect_first_render_ms), "ms",
          s.reconnect_render_availability, s.reconnect_render_reason,
          s.reconnect_render_measurement_point);
    group("reconnect.render.stable", Signed(s.last_reconnect_stable_render_ms), "ms",
          s.reconnect_render_availability, s.reconnect_render_reason,
          s.reconnect_render_measurement_point);
    group("reconnect.render.interruption", Signed(s.last_reconnect_render_interruption_ms), "ms",
          s.reconnect_render_availability, s.reconnect_render_reason,
          s.reconnect_render_measurement_point);

    group("telemetry.snapshot_publications", s.telemetry_snapshot_publications, "snapshots",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.resource_sample.total", s.total_resource_sample_us, "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.resource_sample.maximum", Signed(s.maximum_resource_sample_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.resource_sample.average", Ratio(s.average_resource_sample_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_build.last", Signed(s.last_snapshot_build_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_build.maximum", Signed(s.maximum_snapshot_build_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_build.total", s.total_snapshot_build_us, "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_callback.last", Signed(s.last_snapshot_callback_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_callback.maximum", Signed(s.maximum_snapshot_callback_us), "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.snapshot_callback.total", s.total_snapshot_callback_us, "us",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.observed_cost", Ratio(s.telemetry_observed_cost_ratio), "ratio",
          s.telemetry_cost_availability, s.telemetry_cost_reason);
    group("telemetry.ab", std::monostate{}, "",
          s.telemetry_ab_availability, s.telemetry_ab_reason);

    const auto& st = record.stability;
    const auto availability = st.ledger_availability == "VALID"
        ? Availability::Valid : Availability::Invalid;
    group("stability.process_runs_started", st.process_runs_started, "runs",
          availability, st.ledger_reason);
    group("stability.process_runs_terminal", st.process_runs_terminal, "runs",
          availability, st.ledger_reason);
    group("stability.clean_process_exits", st.clean_process_exits, "runs",
          availability, st.ledger_reason);
    group("stability.unknown_process_terminations", st.unknown_process_terminations,
          "runs", availability, st.unknown_termination_reason);
    group("stability.confirmed_process_crashes", st.confirmed_process_crashes,
          "runs", availability, st.confirmed_crash_reason);
    group("stability.sessions_started", st.sessions_started, "sessions",
          availability, st.ledger_reason);
    group("stability.sessions_terminal", st.sessions_terminal, "sessions",
          availability, st.ledger_reason);
    group("stability.unknown_session_terminations", st.unknown_session_terminations,
          "sessions", availability, st.unknown_termination_reason);
    group("stability.unknown_process_termination_ratio",
          Ratio(st.unknown_process_termination_ratio), "ratio",
          availability, st.unknown_termination_reason);
    group("stability.confirmed_process_crash_ratio",
          Ratio(st.confirmed_process_crash_ratio), "ratio",
          availability, st.confirmed_crash_reason);
    group("stability.corrupt_inputs", st.corrupt_inputs, "records",
          availability, st.ledger_reason);
    group("stability.write_failures", st.write_failures, "writes",
          availability, st.ledger_reason);
    group("stability.duplicate_terminals", st.duplicate_terminals, "events",
          availability, st.ledger_reason);
    return rows;
}

TelemetryExportResult WriteTelemetryReportAtomically(
    const std::vector<SafeTelemetryRecordPtr>& records,
    const std::filesystem::path& destination_root,
    bool managed_history,
    const std::shared_ptr<std::atomic_bool>& cancelled) {
    TelemetryExportResult result;
    result.reason = "no_records";
    if (records.empty() || !records.back()) return result;
    const auto& expected = *records.back();
    const auto compatible = std::all_of(
        records.begin(), records.end(), [&](const auto& record) {
            return record &&
                record->schema_version == kTelemetryReportSchemaVersion &&
                record->definition_version == kTelemetryDefinitionVersion &&
                record->anonymous_session_id == expected.anonymous_session_id &&
                record->snapshot.session_generation ==
                    expected.snapshot.session_generation;
        });
    if (!compatible) {
        result.reason = "incompatible_record_set";
        return result;
    }
    const auto is_cancelled = [&] {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };
    if (is_cancelled()) {
        result.cancelled = true;
        result.reason = "export_cancelled";
        return result;
    }

    std::error_code error;
    std::filesystem::create_directories(destination_root, error);
    if (error) {
        result.reason = "destination_not_writable";
        return result;
    }
    const auto& final_record = expected;
    const auto prefix = managed_history
        ? "cohavora-telemetry-v1-" : "cohavora-telemetry-export-v1-";
    const auto name = prefix + std::to_string(final_record.captured_utc_ms) + "-" +
        final_record.anonymous_session_id;
    const auto final_directory = destination_root / name;
    const auto temporary = destination_root /
        (".cohavora-telemetry-tmp-" + final_record.anonymous_session_id);
    if (std::filesystem::exists(final_directory, error) ||
        std::filesystem::exists(temporary, error)) {
        result.reason = "destination_collision";
        return result;
    }
    std::filesystem::create_directory(temporary, error);
    if (error) {
        result.reason = "destination_not_writable";
        return result;
    }
    const auto cleanup = [&] { RemoveKnownReport(temporary); };

    Json manifest{
        {"schema", kTelemetryReportSchema},
        {"schema_version", kTelemetryReportSchemaVersion},
        {"definition_version", kTelemetryDefinitionVersion},
        {"anonymous_session_id", final_record.anonymous_session_id},
        {"created_utc_ms", UtcNowMs()},
        {"first_sample_utc_ms", records.front()->captured_utc_ms},
        {"last_sample_utc_ms", final_record.captured_utc_ms},
        {"record_count", records.size()},
        {"session_complete", final_record.complete},
        {"integrity", {
            {"status", "complete"},
            {"final_revision", final_record.snapshot.revision},
            {"expected_files", kReportFiles.size()},
        }},
        {"privacy", {
            {"default_upload", false},
            {"raw_identity", false},
            {"raw_network_detail", false},
            {"raw_media", false},
        }},
    };

    Json operations = Json::array();
    for (const auto& item : final_record.snapshot.operation_summaries) {
        operations.push_back(OperationJson(item));
    }
    Json session{
        {"schema", "cohavora-telemetry-session"},
        {"schema_version", kTelemetryReportSchemaVersion},
        {"definition_version", kTelemetryDefinitionVersion},
        {"anonymous_session_id", final_record.anonymous_session_id},
        {"session_generation", final_record.snapshot.session_generation},
        {"final_revision", final_record.snapshot.revision},
        {"complete", final_record.complete},
        {"availability", AvailabilityName(final_record.snapshot.availability)},
        {"reason", SafeText(final_record.snapshot.reason)},
        {"coverage", final_record.snapshot.coverage},
        {"operations", std::move(operations)},
    };

    if (!WriteText(temporary / "manifest.json", manifest.dump(2)) ||
        !WriteText(temporary / "session.json", session.dump(2))) {
        cleanup();
        result.reason = "write_failed";
        return result;
    }

    std::ofstream jsonl(temporary / "metrics.jsonl", std::ios::binary | std::ios::trunc);
    std::ofstream csv(temporary / "metrics.csv", std::ios::binary | std::ios::trunc);
    csv << "captured_utc_ms,session_generation,revision,key,value,unit,availability,reason,measurement_point,definition_version\r\n";
    for (const auto& pointer : records) {
        if (!pointer) continue;
        if (is_cancelled()) {
            jsonl.close();
            csv.close();
            cleanup();
            result.cancelled = true;
            result.reason = "export_cancelled";
            return result;
        }
        for (const auto& metric : BuildSafeMetricRows(*pointer)) {
            Json line{
                {"captured_utc_ms", pointer->captured_utc_ms},
                {"session_generation", pointer->snapshot.session_generation},
                {"revision", pointer->snapshot.revision},
                {"key", metric.key},
                {"value", JsonValue(metric.value)},
                {"unit", metric.unit},
                {"availability", metric.availability},
                {"reason", metric.reason},
                {"measurement_point", metric.measurement_point},
                {"definition_version", pointer->definition_version},
            };
            jsonl << line.dump() << '\n';
            csv << pointer->captured_utc_ms << ','
                << pointer->snapshot.session_generation << ','
                << pointer->snapshot.revision << ','
                << CsvCell(metric.key) << ','
                << CsvCell(CsvValue(metric.value)) << ','
                << CsvCell(metric.unit) << ','
                << CsvCell(metric.availability) << ','
                << CsvCell(metric.reason) << ','
                << CsvCell(metric.measurement_point) << ','
                << pointer->definition_version << "\r\n";
        }
    }
    jsonl.flush();
    csv.flush();
    if (!jsonl || !csv) {
        jsonl.close();
        csv.close();
        cleanup();
        result.reason = "write_failed";
        return result;
    }
    jsonl.close();
    csv.close();
    std::filesystem::rename(temporary, final_directory, error);
    if (error) {
        cleanup();
        result.reason = "atomic_replace_failed";
        return result;
    }
    result.success = true;
    result.reason = "export_complete";
    result.report_directory = final_directory;
    return result;
}

TelemetryHistoryStore::TelemetryHistoryStore(
    std::filesystem::path root,
    std::size_t memory_buckets,
    std::size_t queue_capacity,
    std::uint64_t maximum_bytes,
    std::chrono::hours retention)
    : root_(std::move(root))
    , memory_buckets_((std::max)(std::size_t{1}, memory_buckets))
    , queue_capacity_((std::max)(std::size_t{4}, queue_capacity))
    , maximum_bytes_((std::max)(std::uint64_t{1024 * 1024}, maximum_bytes))
    , retention_(retention) {
    status_.queue_capacity = queue_capacity_;
    status_cache_ = std::make_shared<const TelemetryStoreStatus>(status_);
    worker_ = std::thread([this] { Run(); });
}

TelemetryHistoryStore::~TelemetryHistoryStore() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

bool TelemetryHistoryStore::SubmitSnapshot(
    SessionTelemetry::SnapshotPtr snapshot,
    StabilitySummary stability) {
    if (!snapshot) return false;
    const bool terminal = snapshot->session_complete;
    Job job;
    job.kind = JobKind::Snapshot;
    job.snapshot = std::move(snapshot);
    job.stability = std::move(stability);
    return Enqueue(std::move(job), terminal);
}

bool TelemetryHistoryStore::ExportCurrent(
    std::filesystem::path destination_root,
    ExportCallback callback,
    std::shared_ptr<std::atomic_bool> cancelled) {
    Job job;
    job.kind = JobKind::Export;
    job.destination = std::move(destination_root);
    job.export_callback = std::move(callback);
    job.cancelled = std::move(cancelled);
    return Enqueue(std::move(job), true);
}

bool TelemetryHistoryStore::ClearReport(
    std::string record_id,
    MutationCallback callback) {
    if (record_id.empty() || record_id.find_first_of("/\\") != std::string::npos) {
        return false;
    }
    Job job;
    job.kind = JobKind::ClearReport;
    job.record_id = std::move(record_id);
    job.mutation_callback = std::move(callback);
    return Enqueue(std::move(job), true);
}

void TelemetryHistoryStore::SetHistoryEnabled(bool enabled) {
    Job job;
    job.kind = JobKind::SetHistoryEnabled;
    job.enabled = enabled;
    Enqueue(std::move(job), true);
}

std::shared_ptr<const TelemetryStoreStatus> TelemetryHistoryStore::Status() const {
    return std::atomic_load_explicit(&status_cache_, std::memory_order_acquire);
}

std::vector<SafeTelemetryRecordPtr> TelemetryHistoryStore::CurrentRecords() const {
    std::lock_guard lock(mutex_);
    return {current_records_.begin(), current_records_.end()};
}

bool TelemetryHistoryStore::Enqueue(Job job, bool terminal_priority) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    if (jobs_.size() >= queue_capacity_) {
        if (terminal_priority) {
            const auto found = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& queued) {
                return queued.kind == JobKind::Snapshot && queued.snapshot &&
                    !queued.snapshot->session_complete;
            });
            if (found != jobs_.end()) {
                jobs_.erase(found);
                ++status_.queue_drops;
            } else {
                ++status_.queue_drops;
                status_.availability = Availability::Invalid;
                status_.reason = "met_06_queue_capacity_exceeded";
                PublishStatusLocked();
                return false;
            }
        } else {
            ++status_.queue_drops;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_queue_capacity_exceeded";
            PublishStatusLocked();
            return false;
        }
    }
    jobs_.push_back(std::move(job));
    status_.queue_depth = jobs_.size();
    PublishStatusLocked();
    condition_.notify_one();
    return true;
}

void TelemetryHistoryStore::Run() {
    RefreshAndPruneReports();
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty() && stopping_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            status_.queue_depth = jobs_.size();
            PublishStatusLocked();
        }
        try {
            switch (job.kind) {
            case JobKind::Snapshot: HandleSnapshot(std::move(job)); break;
            case JobKind::Export: HandleExport(std::move(job)); break;
            case JobKind::ClearReport: HandleClear(std::move(job)); break;
            case JobKind::SetHistoryEnabled: {
                std::vector<std::string> report_ids;
                {
                    std::lock_guard lock(mutex_);
                    status_.history_enabled = job.enabled;
                    if (!job.enabled) {
                        current_records_.clear();
                        for (const auto& entry : status_.reports) {
                            report_ids.push_back(entry.record_id);
                        }
                    }
                    status_.memory_records = current_records_.size();
                    status_.availability = Availability::Valid;
                    status_.reason = job.enabled
                        ? "history_enabled" : "history_disabled_by_user";
                    PublishStatusLocked();
                }
                if (!job.enabled) {
                    for (const auto& id : report_ids) RemoveKnownReport(root_ / id);
                    RefreshAndPruneReports();
                }
                break;
            }
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            ++status_.write_failures;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_worker_exception";
            PublishStatusLocked();
        }
    }
}

void TelemetryHistoryStore::HandleSnapshot(Job job) {
    const auto captured = UtcNowMs();
    std::vector<SafeTelemetryRecordPtr> completed;
    {
        std::lock_guard lock(mutex_);
        if (!job.snapshot) return;
        if (current_generation_ != job.snapshot->session_generation) {
            current_records_.clear();
            current_session_id_ = NewOpaqueId();
            current_generation_ = job.snapshot->session_generation;
            current_persisted_ = false;
        }
        if (current_session_id_.empty()) current_session_id_ = NewOpaqueId();
        auto record = std::make_shared<SafeTelemetryRecord>();
        record->anonymous_session_id = current_session_id_;
        record->captured_utc_ms = captured;
        record->complete = job.snapshot->session_complete;
        record->snapshot = *job.snapshot;
        record->stability = std::move(job.stability);
        const auto bucket = captured / 1000;
        if (!current_records_.empty() &&
            current_records_.back()->captured_utc_ms / 1000 == bucket) {
            current_records_.back() = std::move(record);
            ++status_.one_second_buckets_coalesced;
        } else {
            current_records_.push_back(std::move(record));
        }
        while (current_records_.size() > memory_buckets_) {
            current_records_.pop_front();
        }
        ++status_.snapshots_accepted;
        status_.memory_records = current_records_.size();
        status_.availability = Availability::Valid;
        status_.reason = status_.history_enabled
            ? "bounded_history_valid" : "history_disabled_by_user";
        if (job.snapshot->session_complete && status_.history_enabled &&
            !current_persisted_) {
            completed.assign(current_records_.begin(), current_records_.end());
            current_persisted_ = true;
        } else if (job.snapshot->session_complete && !status_.history_enabled) {
            current_records_.clear();
            status_.memory_records = 0;
        }
        PublishStatusLocked();
    }
    if (!completed.empty()) {
        const auto result = WriteTelemetryReportAtomically(
            completed, root_, true, {});
        {
            std::lock_guard lock(mutex_);
            if (!result.success) {
                ++status_.write_failures;
                status_.availability = Availability::Invalid;
                status_.reason = "met_06_" + result.reason;
            }
            PublishStatusLocked();
        }
        RefreshAndPruneReports();
    }
}

void TelemetryHistoryStore::HandleExport(Job job) {
    std::vector<SafeTelemetryRecordPtr> records;
    {
        std::lock_guard lock(mutex_);
        records.assign(current_records_.begin(), current_records_.end());
    }
    auto result = WriteTelemetryReportAtomically(
        records, job.destination, false, job.cancelled);
    {
        std::lock_guard lock(mutex_);
        if (result.success) {
            ++status_.exports_succeeded;
            status_.availability = Availability::Valid;
            status_.reason = "export_complete";
        } else if (result.cancelled) {
            ++status_.exports_cancelled;
            status_.reason = "export_cancelled";
        } else {
            ++status_.write_failures;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_" + result.reason;
        }
        PublishStatusLocked();
    }
    if (job.export_callback) job.export_callback(std::move(result));
}

void TelemetryHistoryStore::HandleClear(Job job) {
    bool removed = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            status_.reports.begin(), status_.reports.end(), [&](const auto& entry) {
                return entry.record_id == job.record_id;
            });
        if (found != status_.reports.end()) {
            removed = RemoveKnownReport(root_ / found->record_id);
        }
    }
    RefreshAndPruneReports();
    if (job.mutation_callback) {
        job.mutation_callback(removed, removed ? "report_cleared" : "report_not_removed");
    }
}

void TelemetryHistoryStore::RefreshAndPruneReports() {
    struct Candidate { TelemetryReportEntry entry; std::filesystem::path path; };
    std::vector<Candidate> candidates;
    std::uint64_t corrupt = 0;
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (!error) {
        for (std::filesystem::directory_iterator it(root_, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_directory(error) || error) {
                error.clear();
                continue;
            }
            const auto id = it->path().filename().string();
            if (!id.starts_with("cohavora-telemetry-v1-")) continue;
            try {
                const auto manifest_path = it->path() / "manifest.json";
                if (!std::filesystem::is_regular_file(manifest_path) ||
                    std::filesystem::file_size(manifest_path) > 1024 * 1024) {
                    ++corrupt;
                    continue;
                }
                std::ifstream input(manifest_path, std::ios::binary);
                Json manifest;
                input >> manifest;
                if (!input || manifest.value("schema", std::string{}) != kTelemetryReportSchema ||
                    manifest.value("schema_version", 0u) != kTelemetryReportSchemaVersion ||
                    manifest.value("integrity", Json::object()).value(
                        "status", std::string{}) != "complete") {
                    ++corrupt;
                    continue;
                }
                TelemetryReportEntry entry;
                entry.record_id = id;
                entry.created_utc_ms = manifest.value("created_utc_ms", std::int64_t{0});
                entry.record_count = manifest.value("record_count", std::uint64_t{0});
                entry.complete = manifest.value("session_complete", false);
                entry.size_bytes = DirectoryKnownSize(it->path());
                candidates.push_back({std::move(entry), it->path()});
            } catch (...) {
                ++corrupt;
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.entry.created_utc_ms < b.entry.created_utc_ms;
    });
    std::uint64_t total = 0;
    for (const auto& item : candidates) total += item.entry.size_bytes;
    const auto cutoff = UtcNowMs() -
        std::chrono::duration_cast<std::chrono::milliseconds>(retention_).count();
    for (auto& item : candidates) {
        if (item.entry.created_utc_ms >= cutoff && total <= maximum_bytes_) continue;
        if (RemoveKnownReport(item.path)) {
            total -= (std::min)(total, item.entry.size_bytes);
            item.entry.record_id.clear();
        }
    }
    std::lock_guard lock(mutex_);
    status_.reports.clear();
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (!it->entry.record_id.empty()) status_.reports.push_back(it->entry);
    }
    status_.corrupt_reports = corrupt;
    if (error) {
        ++status_.write_failures;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_history_scan_failed";
    } else if (status_.availability == Availability::WarmingUp) {
        status_.availability = Availability::Valid;
        status_.reason = "bounded_history_valid";
    }
    PublishStatusLocked();
}

void TelemetryHistoryStore::PublishStatusLocked() {
    std::atomic_store_explicit(
        &status_cache_,
        std::make_shared<const TelemetryStoreStatus>(status_),
        std::memory_order_release);
}

std::string TelemetryHistoryStore::NewOpaqueId() {
    std::array<std::uint64_t, 2> words{};
    std::random_device source;
    for (auto& word : words) {
        word = (static_cast<std::uint64_t>(source()) << 32) ^ source();
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(16) << words[0]
           << std::setw(16) << words[1];
    return output.str();
}

void InstallTelemetryHistoryStore(std::shared_ptr<TelemetryHistoryStore> store) {
    std::lock_guard lock(g_installed_mutex);
    g_installed_store = std::move(store);
}

std::shared_ptr<TelemetryHistoryStore> InstalledTelemetryHistoryStore() {
    std::lock_guard lock(g_installed_mutex);
    return g_installed_store.lock();
}

} // namespace livekit::telemetry
