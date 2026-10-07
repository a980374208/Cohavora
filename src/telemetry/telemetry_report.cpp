#include "telemetry_report.h"
#include "telemetry_checkpoint.h"
#include "diagnostic_bundle.h"

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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;

constexpr std::array<const char*, 4> kReportFiles = {
    "manifest.json", "session.json", "metrics.jsonl", "metrics.csv"};
constexpr std::array<const char*, 4> kReportTemporaryFiles = {
    "manifest.json.tmp", "session.json.tmp", "metrics.jsonl.tmp",
    "metrics.csv.tmp"};

bool IsLegacyTemporaryDirectory(std::string_view name) {
    constexpr std::string_view prefix = ".cohavora-telemetry-tmp-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 32)
        return false;
    return std::all_of(name.begin() + prefix.size(), name.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f'); });
}

std::mutex g_installed_mutex;
std::weak_ptr<TelemetryHistoryStore> g_installed_store;

std::int64_t UtcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Keep this retained-allocation census in sync with Snapshot's owning fields.
// Charge capacities (including shared snapshots conservatively once per job),
// not the serialized JSON scratch buffer that exists only on the worker.
std::size_t SnapshotRetainedBytes(const Snapshot& snapshot) {
    constexpr std::string Snapshot::* text_fields[] = {
        &Snapshot::reason,
        &Snapshot::session_duration_reason,
        &Snapshot::session_duration_measurement_point,
        &Snapshot::usable_duration_reason,
        &Snapshot::usable_duration_measurement_point,
        &Snapshot::admission_to_usable_reason,
        &Snapshot::admission_to_usable_measurement_point,
        &Snapshot::event_queue_lag_reason,
        &Snapshot::resource_reason,
        &Snapshot::cpu_reason,
        &Snapshot::memory_reason,
        &Snapshot::thread_count_reason,
        &Snapshot::handle_count_reason,
        &Snapshot::gpu_resource_reason,
        &Snapshot::resource_trend_reason,
        &Snapshot::resource_session_delta_reason,
        &Snapshot::resource_final_delta_reason,
        &Snapshot::resource_return_reason,
        &Snapshot::internal_resource_reason,
        &Snapshot::router_queue_reason,
        &Snapshot::export_queue_reason,
        &Snapshot::telemetry_cost_reason,
        &Snapshot::telemetry_ab_reason,
        &Snapshot::strand_lag_reason,
        &Snapshot::ui_lag_reason,
        &Snapshot::local_publish_media_reason,
        &Snapshot::local_publish_media_algorithm,
        &Snapshot::local_video_injection_reason,
        &Snapshot::local_video_injection_measurement_point,
        &Snapshot::local_video_encode_reason,
        &Snapshot::local_video_encode_measurement_point,
        &Snapshot::local_rtp_send_reason,
        &Snapshot::local_rtp_send_measurement_point,
        &Snapshot::subscription_media_reason,
        &Snapshot::subscription_media_measurement_point,
        &Snapshot::inbound_rtp_traffic_reason,
        &Snapshot::outbound_rtp_traffic_reason,
        &Snapshot::rtp_traffic_measurement_point,
        &Snapshot::inbound_packet_loss_reason,
        &Snapshot::inbound_packet_loss_measurement_point,
        &Snapshot::inbound_jitter_reason,
        &Snapshot::remote_rtcp_reason,
        &Snapshot::remote_rtcp_measurement_point,
        &Snapshot::network_recovery_reason,
        &Snapshot::network_recovery_measurement_point,
        &Snapshot::network_retransmit_ratio_denominator,
        &Snapshot::inbound_retransmission_reason,
        &Snapshot::inbound_fec_reason,
        &Snapshot::inbound_feedback_reason,
        &Snapshot::outbound_retransmission_reason,
        &Snapshot::outbound_feedback_reason,
        &Snapshot::media_path_reason,
        &Snapshot::media_path_measurement_point,
        &Snapshot::local_candidate_types,
        &Snapshot::remote_candidate_types,
        &Snapshot::local_network_types,
        &Snapshot::media_protocols,
        &Snapshot::relay_protocols,
        &Snapshot::tcp_types,
        &Snapshot::media_path_rtt_reason,
        &Snapshot::media_bandwidth_reason,
        &Snapshot::transport_traffic_reason,
        &Snapshot::transport_traffic_measurement_point,
        &Snapshot::transport_state_reason,
        &Snapshot::transport_dtls_states,
        &Snapshot::transport_connectivity_states,
        &Snapshot::transport_roles,
        &Snapshot::video_quality_limitation_reason,
        &Snapshot::video_quality_limitation_measurement_point,
        &Snapshot::video_quality_limitation_current,
        &Snapshot::video_pipeline_reason,
        &Snapshot::video_pipeline_measurement_point,
        &Snapshot::video_codec_reason,
        &Snapshot::video_codec_measurement_point,
        &Snapshot::inbound_video_codecs,
        &Snapshot::outbound_video_codecs,
        &Snapshot::decoder_implementations,
        &Snapshot::encoder_implementations,
        &Snapshot::decoder_power_efficiency,
        &Snapshot::encoder_power_efficiency,
        &Snapshot::outbound_video_layers,
        &Snapshot::video_publish_plan_reason,
        &Snapshot::video_publish_plan_measurement_point,
        &Snapshot::video_publish_requested_codecs,
        &Snapshot::video_publish_effective_codecs,
        &Snapshot::video_publish_observed_codecs,
        &Snapshot::video_publish_fallback_reasons,
        &Snapshot::video_publish_sources,
        &Snapshot::video_publish_direction,
        &Snapshot::video_publish_generations,
        &Snapshot::video_publish_modes,
        &Snapshot::video_publish_resolved_profiles,
        &Snapshot::video_publish_observed_profiles,
        &Snapshot::video_publish_encoder_implementations,
        &Snapshot::video_publish_resolved_scalability,
        &Snapshot::video_publish_observed_scalability,
        &Snapshot::video_processing_reason,
        &Snapshot::video_processing_measurement_point,
        &Snapshot::local_device_continuity_reason,
        &Snapshot::local_device_continuity_measurement_point,
        &Snapshot::local_device_continuity_algorithm,
        &Snapshot::device_open_reason,
        &Snapshot::device_open_measurement_point,
        &Snapshot::device_hotplug_reason,
        &Snapshot::device_hotplug_measurement_point,
        &Snapshot::device_failure_reason,
        &Snapshot::device_state_reason,
        &Snapshot::device_state_measurement_point,
        &Snapshot::remote_video_first_frame_reason,
        &Snapshot::remote_video_first_frame_measurement_point,
        &Snapshot::native_video_freeze_reason,
        &Snapshot::native_video_freeze_measurement_point,
        &Snapshot::reconnect_video_reason,
        &Snapshot::reconnect_video_measurement_point,
        &Snapshot::remote_audio_first_frame_reason,
        &Snapshot::remote_audio_first_frame_measurement_point,
        &Snapshot::audio_quality_reason,
        &Snapshot::audio_quality_measurement_point,
        &Snapshot::audio_concealment_reason,
        &Snapshot::audio_jitter_buffer_reason,
        &Snapshot::audio_time_stretch_reason,
        &Snapshot::reconnect_audio_reason,
        &Snapshot::reconnect_audio_measurement_point,
        &Snapshot::render_first_frame_reason,
        &Snapshot::render_first_frame_measurement_point,
        &Snapshot::render_window_reason,
        &Snapshot::render_frame_age_reason,
        &Snapshot::render_frame_age_measurement_point,
        &Snapshot::render_pipeline_reason,
        &Snapshot::render_pipeline_measurement_point,
        &Snapshot::render_requested_backend,
        &Snapshot::render_actual_backend,
        &Snapshot::render_gpu_failure,
        &Snapshot::render_fallback_reason,
        &Snapshot::video_policy_reason,
        &Snapshot::video_policy_measurement_point,
        &Snapshot::video_policy_stage_content,
        &Snapshot::video_policy_selection_reason,
        &Snapshot::render_stall_reason,
        &Snapshot::render_stall_algorithm,
        &Snapshot::render_stage_reason,
        &Snapshot::render_stage_measurement_point,
        &Snapshot::render_gpu_execution_reason,
        &Snapshot::reconnect_render_reason,
        &Snapshot::reconnect_render_measurement_point,
        &Snapshot::reconnect_density_reason,
        &Snapshot::stability_anomaly_density_reason,
        &Snapshot::stability_anomaly_density_algorithm,
    };
    std::size_t bytes = sizeof(Snapshot) + 64 +
        snapshot.render_fine_interval_histogram.capacity() * sizeof(std::uint64_t) +
        snapshot.render_window_fine_interval_histogram.capacity() * sizeof(std::uint64_t);
    for (const auto field : text_fields) bytes += (snapshot.*field).capacity() + 1;
    bytes += snapshot.metric_product_chains.capacity() * sizeof(MetricProductChainStatus);
    for (const auto& chain : snapshot.metric_product_chains)
        bytes += chain.metric_id.capacity() + chain.reason.capacity() + 2;
    bytes += snapshot.operation_summaries.capacity() * sizeof(OperationSummary);
    return bytes;
}

std::size_t StabilityRetainedBytes(const StabilitySummary& stability) {
    return stability.ledger_availability.capacity() + stability.ledger_reason.capacity() +
        stability.confirmed_crash_availability.capacity() + stability.confirmed_crash_reason.capacity() +
        stability.unknown_termination_availability.capacity() + stability.unknown_termination_reason.capacity() + 6;
}

std::size_t PendingCharge(const TelemetryCheckpointRecord& record) {
    return 2 * (sizeof(record) + record.jsonl.capacity() + 64);
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

void AppendJsonString(std::string& output, const std::string& value) {
    // Typed metric names/reasons are usually printable ASCII. Only that
    // escape-free subset can be appended directly; all other text still goes
    // through the JSON library's escaping and UTF-8 validation.
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= 0x20 && c < 0x7f && c != '"' && c != '\\';
        })) {
        output.push_back('"');
        output += value;
        output.push_back('"');
    } else {
        output += Json(value).dump();
    }
}

void AppendJsonValue(std::string& output, const MetricValue& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) output += "null";
        else if constexpr (std::is_same_v<T, bool>) output += item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) AppendJsonString(output, SafeText(item));
        else if constexpr (std::is_integral_v<T>) output += std::to_string(item);
        else output += Json(item).dump();
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

MetricValue RatioWhenSupported(double value, Availability availability) {
    return availability == Availability::Unsupported ||
           availability == Availability::Unknown ||
           availability == Availability::NotExpected
        ? MetricValue{std::monostate{}}
        : Ratio(value);
}

MetricValue ValueWhenSupported(std::uint64_t value, Availability availability) {
    return availability == Availability::Unsupported ||
           availability == Availability::Unknown ||
           availability == Availability::NotExpected
        ? MetricValue{std::monostate{}}
        : MetricValue{value};
}

MetricValue SignedValueWhenSupported(std::int64_t value, Availability availability) {
    return availability == Availability::Unsupported ||
           availability == Availability::Unknown ||
           availability == Availability::NotExpected
        ? MetricValue{std::monostate{}}
        : MetricValue{value};
}

MetricValue WindowValue(std::uint64_t value, Availability availability) {
    return availability == Availability::Valid || availability == Availability::Stale
        ? MetricValue{value} : MetricValue{std::monostate{}};
}

MetricValue SignedWindowValue(std::int64_t value, Availability availability) {
    return availability == Availability::Valid ||
           availability == Availability::Stale ||
           availability == Availability::Invalid
        ? MetricValue{value} : MetricValue{std::monostate{}};
}

MetricValue CorrectableWindowValue(
        std::uint64_t value,
        Availability availability) {
    return availability == Availability::Valid ||
           availability == Availability::Stale ||
           availability == Availability::Invalid
        ? MetricValue{value} : MetricValue{std::monostate{}};
}

Availability ParseAvailability(const std::string& value) {
    if (value == "VALID") return Availability::Valid;
    if (value == "WARMING_UP") return Availability::WarmingUp;
    if (value == "NOT_EXPECTED") return Availability::NotExpected;
    if (value == "UNSUPPORTED") return Availability::Unsupported;
    if (value == "TIMEOUT") return Availability::Timeout;
    if (value == "STALE") return Availability::Stale;
    if (value == "INVALID") return Availability::Invalid;
    return Availability::Unknown;
}

Availability ProductChainAvailability(ProductChainStatus status) {
    switch (status) {
    case ProductChainStatus::Implemented: return Availability::Valid;
    case ProductChainStatus::Partial: return Availability::Unknown;
    case ProductChainStatus::Unsupported: return Availability::Unsupported;
    case ProductChainStatus::ControlledHarnessOnly:
    case ProductChainStatus::DeferredExternal:
        return Availability::NotExpected;
    }
    return Availability::Unknown;
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
    for (const auto* name : kReportTemporaryFiles) {
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
#if defined(_WIN32)
    HANDLE handle = nullptr;
    if (recognized) {
        handle = CreateFileW((directory / L"manifest.json").c_str(),
            GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
    }
    auto close = std::unique_ptr<void, decltype(&CloseHandle)>(
        handle, &CloseHandle);
#endif
    error.clear();
    for (const auto* name : kReportFiles) {
        std::filesystem::remove(directory / name, error);
        error.clear();
    }
    for (const auto* name : kReportTemporaryFiles) {
        std::filesystem::remove(directory / name, error);
        error.clear();
    }
#if defined(_WIN32)
    close.reset();
#endif
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
    rows.reserve(320);
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
    group("session.duration", Signed(s.session_duration_ms), "ms",
          s.session_duration_availability, s.session_duration_reason,
          s.session_duration_measurement_point);
    group("session.usable_duration", Signed(s.usable_duration_ms), "ms",
          s.usable_duration_availability, s.usable_duration_reason,
          s.usable_duration_measurement_point);
    group("session.admission_to_usable", Signed(s.admission_to_usable_ms), "ms",
          s.admission_to_usable_availability, s.admission_to_usable_reason,
          s.admission_to_usable_measurement_point);
    for (std::size_t index = 0;
         index < s.metric_product_chains.size() && index < 64; ++index) {
        const auto& capability = s.metric_product_chains[index];
        if (!IsKnownMetricProductChain(capability)) continue;
        const auto availability = ProductChainAvailability(capability.status);
        group("product_chain." + capability.metric_id,
              std::string(ProductChainStatusName(capability.status)), "status",
              availability, capability.reason, "s9b_fifth_batch_audit");
    }
    base("queue.capacity", static_cast<std::uint64_t>(s.queue_capacity), "events");
    base("queue.depth", static_cast<std::uint64_t>(s.queue_depth), "events");
    base("queue.high_water", static_cast<std::uint64_t>(s.queue_high_water), "events");
    base("queue.capacity_drops", s.capacity_drops, "events");
    base("queue.stopped_drops", s.stopped_drops, "events");
    base("queue.stale_generation_drops", s.stale_generation_drops, "events");
    base("queue.out_of_order_drops", s.out_of_order_drops, "events");
    base("late_callbacks", s.late_callbacks, "callbacks");
    base("mapping_failures", s.mapping_failures, "events");
    base("render.timeline.event_drops", s.render_timeline_event_drops, "events");
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
    group("resource.internal.native_bindings", s.active_native_bindings, "bindings",
          s.internal_resource_availability, s.internal_resource_reason,
          "session_owned_lifecycle_registry");
    group("resource.internal.local_media_streams", s.active_local_media_streams,
          "streams", s.internal_resource_availability, s.internal_resource_reason,
          "session_owned_lifecycle_registry");
    group("resource.internal.router_slots", s.active_router_slots, "slots",
          s.router_queue_availability, s.router_queue_reason,
          "bounded_latest_frame_router");
    group("resource.queue.router.submitted", s.router_frames_submitted, "frames",
          s.router_queue_availability, s.router_queue_reason,
          "bounded_latest_frame_router");
    group("resource.queue.router.replaced", s.router_frames_replaced, "frames",
          s.router_queue_availability, s.router_queue_reason,
          "bounded_latest_frame_router");
    group("resource.queue.router.capacity_drops", s.router_capacity_drops, "frames",
          s.router_queue_availability, s.router_queue_reason,
          "bounded_latest_frame_router");
    group("resource.queue.export", std::monostate{}, "items",
          s.export_queue_availability, s.export_queue_reason,
          "telemetry_history_exporter");

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

    group("publish.media.publications", s.local_publications, "publications",
          s.local_publish_media_availability, s.local_publish_media_reason,
          s.local_publish_media_algorithm);
    group("publish.media.active", s.active_local_publications, "publications",
          s.local_publish_media_availability, s.local_publish_media_reason,
          s.local_publish_media_algorithm);
    group("publish.media.expected", s.expected_local_publications, "publications",
          s.local_publish_media_availability, s.local_publish_media_reason,
          s.local_publish_media_algorithm);
    group("publish.media.no_media", s.local_publish_no_media, "publications",
          s.local_publish_media_availability, s.local_publish_media_reason,
          s.local_publish_media_algorithm);
    group("publish.media.stale_callback_drops", s.stale_local_publication_drops,
          "callbacks", s.local_publish_media_availability,
          s.local_publish_media_reason, s.local_publish_media_algorithm);
    group("publish.video.first_injections", s.local_video_first_injections, "frames",
          s.local_video_injection_availability,
          s.local_video_injection_reason,
          s.local_video_injection_measurement_point);
    group("publish.video.accepted_to_injection",
          Signed(s.last_publish_to_video_injection_ms), "ms",
          s.local_video_injection_availability,
          s.local_video_injection_reason,
          s.local_video_injection_measurement_point);
    group("publish.video.first_encodes", s.local_video_first_encodes, "frames",
          s.local_video_encode_availability,
          s.local_video_encode_reason,
          s.local_video_encode_measurement_point);
    group("publish.video.accepted_to_encode",
          Signed(s.last_publish_to_video_encode_ms), "ms",
          s.local_video_encode_availability,
          s.local_video_encode_reason,
          s.local_video_encode_measurement_point);
    group("publish.rtp.first_sends", s.local_first_rtp_sends, "streams",
          s.local_rtp_send_availability, s.local_rtp_send_reason,
          s.local_rtp_send_measurement_point);
    group("publish.rtp.accepted_to_send",
          Signed(s.last_publish_to_rtp_send_ms), "ms",
          s.local_rtp_send_availability, s.local_rtp_send_reason,
          s.local_rtp_send_measurement_point);
    group("publish.stats.uncertainty",
          Signed(s.local_publish_stats_uncertainty_ms), "ms",
          s.local_rtp_send_availability, s.local_rtp_send_reason,
          s.local_rtp_send_measurement_point);
    group("subscribe.media.expected", s.expected_remote_subscriptions,
          "subscriptions", s.subscription_media_availability,
          s.subscription_media_reason, s.subscription_media_measurement_point);
    group("subscribe.media.delivered", s.delivered_remote_subscriptions,
          "subscriptions", s.subscription_media_availability,
          s.subscription_media_reason, s.subscription_media_measurement_point);
    group("subscribe.media.no_media", s.remote_subscription_no_media,
          "subscriptions", s.subscription_media_availability,
          s.subscription_media_reason, s.subscription_media_measurement_point);
    group("subscribe.media.longest_wait",
          Signed(s.longest_subscription_media_wait_ms), "ms",
          s.subscription_media_availability, s.subscription_media_reason,
          s.subscription_media_measurement_point);

    group("network.rtp.inbound.streams", s.inbound_rtp_streams, "streams",
          s.inbound_rtp_traffic_availability, s.inbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.inbound.bytes.cumulative",
          ValueWhenSupported(s.inbound_rtp_bytes,
              s.inbound_rtp_traffic_availability), "bytes",
          s.inbound_rtp_traffic_availability, s.inbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.inbound.bytes.window",
          WindowValue(s.window_inbound_rtp_bytes,
              s.inbound_rtp_traffic_availability), "bytes",
          s.inbound_rtp_traffic_availability, s.inbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.inbound.bitrate",
          RatioWhenSupported(s.inbound_rtp_bitrate_bps,
              s.inbound_rtp_traffic_availability), "bps",
          s.inbound_rtp_traffic_availability, s.inbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.outbound.streams", s.outbound_rtp_streams, "streams",
          s.outbound_rtp_traffic_availability, s.outbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.outbound.bytes.cumulative",
          ValueWhenSupported(s.outbound_rtp_bytes,
              s.outbound_rtp_traffic_availability), "bytes",
          s.outbound_rtp_traffic_availability, s.outbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.outbound.bytes.window",
          WindowValue(s.window_outbound_rtp_bytes,
              s.outbound_rtp_traffic_availability), "bytes",
          s.outbound_rtp_traffic_availability, s.outbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.rtp.outbound.bitrate",
          RatioWhenSupported(s.outbound_rtp_bitrate_bps,
              s.outbound_rtp_traffic_availability), "bps",
          s.outbound_rtp_traffic_availability, s.outbound_rtp_traffic_reason,
          s.rtp_traffic_measurement_point);
    group("network.inbound.loss.cumulative",
          SignedValueWhenSupported(s.inbound_packets_lost,
              s.inbound_packet_loss_availability), "packets",
          s.inbound_packet_loss_availability, s.inbound_packet_loss_reason,
          s.inbound_packet_loss_measurement_point);
    group("network.inbound.loss.window",
          SignedWindowValue(s.window_inbound_packets_lost,
              s.inbound_packet_loss_availability), "packets",
          s.inbound_packet_loss_availability, s.inbound_packet_loss_reason,
          s.inbound_packet_loss_measurement_point);
    group("network.inbound.received.window",
          CorrectableWindowValue(s.window_inbound_packets_received,
              s.inbound_packet_loss_availability), "packets",
          s.inbound_packet_loss_availability,
          s.inbound_packet_loss_reason, s.inbound_packet_loss_measurement_point);
    group("network.inbound.loss.ratio",
          RatioWhenSupported(s.inbound_packet_loss_ratio,
              s.inbound_packet_loss_availability), "ratio",
          s.inbound_packet_loss_availability, s.inbound_packet_loss_reason,
          s.inbound_packet_loss_measurement_point);
    group("network.inbound.jitter.maximum",
          RatioWhenSupported(s.inbound_jitter_max_ms,
              s.inbound_jitter_availability), "ms",
          s.inbound_jitter_availability, s.inbound_jitter_reason,
          "rtc_inbound_rtp_jitter_current");
    group("network.remote_rtcp.streams", s.remote_rtcp_streams, "streams",
          s.remote_rtcp_availability, s.remote_rtcp_reason,
          s.remote_rtcp_measurement_point);
    group("network.remote_rtcp.current_rtt.maximum",
          RatioWhenSupported(s.remote_rtcp_current_rtt_max_ms,
              s.remote_rtcp_availability), "ms",
          s.remote_rtcp_availability, s.remote_rtcp_reason,
          s.remote_rtcp_measurement_point);
    group("network.remote_rtcp.window_rtt.average",
          RatioWhenSupported(s.remote_rtcp_window_average_rtt_ms,
              s.remote_rtcp_availability), "ms",
          s.remote_rtcp_availability, s.remote_rtcp_reason,
          s.remote_rtcp_measurement_point);
    group("network.remote_rtcp.fraction_lost.maximum",
          RatioWhenSupported(s.remote_rtcp_fraction_lost_max,
              s.remote_rtcp_availability), "ratio",
          s.remote_rtcp_availability, s.remote_rtcp_reason,
          s.remote_rtcp_measurement_point);

    group("network.recovery.streams", s.network_recovery_streams, "streams",
          s.network_recovery_availability, s.network_recovery_reason,
          s.network_recovery_measurement_point);
    group("network.recovery.ratio_denominator",
          SafeText(s.network_retransmit_ratio_denominator), "definition",
          s.network_recovery_availability, s.network_recovery_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.retransmitted_packets.cumulative",
          ValueWhenSupported(s.inbound_retransmitted_packets,
              s.inbound_retransmission_availability), "packets",
          s.inbound_retransmission_availability, s.inbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.retransmitted_bytes.cumulative",
          ValueWhenSupported(s.inbound_retransmitted_bytes,
              s.inbound_retransmission_availability), "bytes",
          s.inbound_retransmission_availability, s.inbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fec_packets.cumulative",
          ValueWhenSupported(s.inbound_fec_packets, s.inbound_fec_availability), "packets",
          s.inbound_fec_availability, s.inbound_fec_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fec_bytes.cumulative",
          ValueWhenSupported(s.inbound_fec_bytes, s.inbound_fec_availability), "bytes",
          s.inbound_fec_availability, s.inbound_fec_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fec_discarded.cumulative",
          ValueWhenSupported(s.inbound_fec_discarded_packets,
              s.inbound_fec_availability), "packets",
          s.inbound_fec_availability, s.inbound_fec_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.nack.cumulative",
          ValueWhenSupported(s.inbound_nack_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.pli.cumulative",
          ValueWhenSupported(s.inbound_pli_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fir.cumulative",
          ValueWhenSupported(s.inbound_fir_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.retransmitted_packets.cumulative",
          ValueWhenSupported(s.outbound_retransmitted_packets,
              s.outbound_retransmission_availability), "packets",
          s.outbound_retransmission_availability, s.outbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.retransmitted_bytes.cumulative",
          ValueWhenSupported(s.outbound_retransmitted_bytes,
              s.outbound_retransmission_availability), "bytes",
          s.outbound_retransmission_availability, s.outbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.nack.cumulative",
          ValueWhenSupported(s.outbound_nack_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.pli.cumulative",
          ValueWhenSupported(s.outbound_pli_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.fir.cumulative",
          ValueWhenSupported(s.outbound_fir_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.packets.window",
          WindowValue(s.window_inbound_packets, s.inbound_retransmission_availability), "packets",
          s.inbound_retransmission_availability, s.inbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.retransmitted_packets.window",
          WindowValue(s.window_inbound_retransmitted_packets,
              s.inbound_retransmission_availability), "packets",
          s.inbound_retransmission_availability, s.inbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fec_packets.window",
          WindowValue(s.window_inbound_fec_packets, s.inbound_fec_availability), "packets",
          s.inbound_fec_availability, s.inbound_fec_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.nack.window",
          WindowValue(s.window_inbound_nack_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.pli.window",
          WindowValue(s.window_inbound_pli_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.fir.window",
          WindowValue(s.window_inbound_fir_count, s.inbound_feedback_availability), "events",
          s.inbound_feedback_availability, s.inbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.inbound.retransmitted_packet_ratio",
          Ratio(s.inbound_retransmitted_packet_ratio), "ratio",
          s.inbound_retransmission_availability, s.inbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.packets.window",
          WindowValue(s.window_outbound_packets, s.outbound_retransmission_availability), "packets",
          s.outbound_retransmission_availability, s.outbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.retransmitted_packets.window",
          WindowValue(s.window_outbound_retransmitted_packets,
              s.outbound_retransmission_availability), "packets",
          s.outbound_retransmission_availability, s.outbound_retransmission_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.nack.window",
          WindowValue(s.window_outbound_nack_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.pli.window",
          WindowValue(s.window_outbound_pli_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.fir.window",
          WindowValue(s.window_outbound_fir_count, s.outbound_feedback_availability), "events",
          s.outbound_feedback_availability, s.outbound_feedback_reason,
          s.network_recovery_measurement_point);
    group("network.outbound.retransmitted_packet_ratio",
          Ratio(s.outbound_retransmitted_packet_ratio), "ratio",
          s.outbound_retransmission_availability, s.outbound_retransmission_reason,
          s.network_recovery_measurement_point);

    group("network.path.selected_transports", s.selected_media_transports, "transports",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.switches", s.media_path_switches, "switches",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.local_candidate_types", SafeText(s.local_candidate_types), "types",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.remote_candidate_types", SafeText(s.remote_candidate_types), "types",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.local_network_types", SafeText(s.local_network_types), "types",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.media_protocols", SafeText(s.media_protocols), "protocols",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.relay_protocols", SafeText(s.relay_protocols), "protocols",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.tcp_types", SafeText(s.tcp_types), "types",
          s.media_path_availability, s.media_path_reason,
          s.media_path_measurement_point);
    group("network.path.rtt.maximum",
          RatioWhenSupported(s.media_path_rtt_max_ms,
              s.media_path_rtt_availability), "ms",
          s.media_path_rtt_availability, s.media_path_rtt_reason,
          s.media_path_measurement_point);
    group("network.path.available_outgoing_bitrate.maximum",
          RatioWhenSupported(s.media_available_outgoing_bitrate_bps,
              s.media_bandwidth_availability), "bps",
          s.media_bandwidth_availability, s.media_bandwidth_reason,
          s.media_path_measurement_point);
    group("network.path.available_incoming_bitrate.maximum",
          RatioWhenSupported(s.media_available_incoming_bitrate_bps,
              s.media_bandwidth_availability), "bps",
          s.media_bandwidth_availability, s.media_bandwidth_reason,
          s.media_path_measurement_point);
    group("network.transport.count", s.transport_stats_count, "transports",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.bytes_sent.cumulative",
          ValueWhenSupported(s.transport_bytes_sent,
              s.transport_traffic_availability), "bytes",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.bytes_received.cumulative",
          ValueWhenSupported(s.transport_bytes_received,
              s.transport_traffic_availability), "bytes",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.packets_sent.cumulative",
          ValueWhenSupported(s.transport_packets_sent,
              s.transport_traffic_availability), "packets",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.packets_received.cumulative",
          ValueWhenSupported(s.transport_packets_received,
              s.transport_traffic_availability), "packets",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.bytes_sent.window",
          WindowValue(s.window_transport_bytes_sent,
              s.transport_traffic_availability), "bytes",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.bytes_received.window",
          WindowValue(s.window_transport_bytes_received,
              s.transport_traffic_availability), "bytes",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.packets_sent.window",
          WindowValue(s.window_transport_packets_sent,
              s.transport_traffic_availability), "packets",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.packets_received.window",
          WindowValue(s.window_transport_packets_received,
              s.transport_traffic_availability), "packets",
          s.transport_traffic_availability, s.transport_traffic_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.dtls_states", SafeText(s.transport_dtls_states),
          "states", s.transport_state_availability, s.transport_state_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.connectivity_states",
          SafeText(s.transport_connectivity_states), "states",
          s.transport_state_availability, s.transport_state_reason,
          s.transport_traffic_measurement_point);
    group("network.transport.roles", SafeText(s.transport_roles), "roles",
          s.transport_state_availability, s.transport_state_reason,
          s.transport_traffic_measurement_point);

    group("device.local.expected_streams", s.expected_local_device_streams, "streams",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          s.local_device_continuity_measurement_point);
    group("device.local.active_streams", s.active_local_device_streams, "streams",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          s.local_device_continuity_measurement_point);
    group("device.local.unexpected_stops", s.local_device_unexpected_stops, "events",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          s.local_device_continuity_algorithm);
    group("device.local.interruption_duration",
          s.local_device_interruption_duration_ms, "ms",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          s.local_device_continuity_algorithm);
    group("device.local.format_changes", s.local_device_format_changes, "events",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          s.local_device_continuity_measurement_point);
    group("device.local.video_clock_resets", s.local_device_clock_resets, "events",
          s.local_device_continuity_availability,
          s.local_device_continuity_reason,
          "rtc_local_video_capture_timestamp");
    group("device.open", std::monostate{}, "operations",
          s.device_open_availability, s.device_open_reason,
          s.device_open_measurement_point);
    group("device.hotplug", std::monostate{}, "events",
          s.device_hotplug_availability, s.device_hotplug_reason,
          s.device_hotplug_measurement_point);
    group("device.switch.attempts", s.device_switch_attempts, "operations",
          s.device_failure_availability, s.device_failure_reason,
          "typed_device_switch_operation_ledger");
    group("device.switch.successes", s.device_switch_successes, "operations",
          s.device_failure_availability, s.device_failure_reason,
          "typed_device_switch_operation_ledger");
    group("device.switch.failures", s.device_switch_failures, "operations",
          s.device_failure_availability, s.device_failure_reason,
          "typed_device_switch_operation_ledger");
    group("device.switch.timeouts", s.device_switch_timeouts, "operations",
          s.device_failure_availability, s.device_failure_reason,
          "typed_device_switch_operation_ledger");
    group("device.state.microphone_requested", s.microphone_requested, "bool",
          s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.microphone_effective", s.microphone_effective, "bool",
          s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.camera_requested", s.camera_requested, "bool",
          s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.camera_effective", s.camera_effective, "bool",
          s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.capture_width", static_cast<std::uint64_t>(s.actual_capture_width),
          "pixels", s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.capture_height", static_cast<std::uint64_t>(s.actual_capture_height),
          "pixels", s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.sample_rate", static_cast<std::uint64_t>(s.actual_capture_sample_rate),
          "Hz", s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);
    group("device.state.channels", static_cast<std::uint64_t>(s.actual_capture_channels),
          "channels", s.device_state_availability, s.device_state_reason,
          s.device_state_measurement_point);

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
    group("video.quality.streams", s.outbound_video_streams, "streams",
          s.video_quality_limitation_availability,
          s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.current_reasons",
          SafeText(s.video_quality_limitation_current), "native_reasons",
          s.video_quality_limitation_availability,
          s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.duration.none", Signed(s.video_quality_none_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.duration.cpu", Signed(s.video_quality_cpu_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.duration.bandwidth", Signed(s.video_quality_bandwidth_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.duration.other", Signed(s.video_quality_other_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.window.none", Signed(s.window_video_quality_none_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.window.cpu", Signed(s.window_video_quality_cpu_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.window.bandwidth", Signed(s.window_video_quality_bandwidth_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.window.other", Signed(s.window_video_quality_other_duration_ms), "ms",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.resolution_changes", Signed(s.video_quality_resolution_changes), "changes",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.window_resolution_changes",
          Signed(s.window_video_quality_resolution_changes), "changes",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.output_width", static_cast<std::uint64_t>(s.outbound_video_width), "pixels",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.output_height", static_cast<std::uint64_t>(s.outbound_video_height), "pixels",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.quality.output_fps", Ratio(s.outbound_video_fps), "frames/s",
          s.video_quality_limitation_availability, s.video_quality_limitation_reason,
          s.video_quality_limitation_measurement_point);
    group("video.pipeline.inbound_received", s.inbound_video_frames_received, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_decoded", s.inbound_video_frames_decoded, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_rtp_streams", s.inbound_video_rtp_streams, "streams",
          s.video_pipeline_availability, s.video_pipeline_reason, "rtc_inbound_video_rtp");
    group("video.pipeline.active_decode_streams", Signed(s.inbound_video_active_decode_streams), "streams",
          s.inbound_video_active_decode_streams < 0 ? Availability::Unknown : s.video_pipeline_availability,
          s.inbound_video_active_decode_streams < 0 ? "decode_stream_baseline_incomplete" : s.video_pipeline_reason,
          "rtc_inbound_rtp_positive_frames_decoded_delta");
    group("video.pipeline.inbound_dropped", s.inbound_video_frames_dropped, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.outbound_encoded", s.outbound_video_frames_encoded, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.outbound_sent", s.outbound_video_frames_sent, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.window_inbound_received",
          s.window_inbound_video_frames_received, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.window_inbound_decoded",
          s.window_inbound_video_frames_decoded, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.window_inbound_dropped",
          s.window_inbound_video_frames_dropped, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.window_outbound_encoded",
          s.window_outbound_video_frames_encoded, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.window_outbound_sent",
          s.window_outbound_video_frames_sent, "frames",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_drop_ratio", Ratio(s.inbound_video_frame_drop_ratio),
          "ratio", s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_width", static_cast<std::uint64_t>(s.inbound_video_width),
          "pixels", s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_height", static_cast<std::uint64_t>(s.inbound_video_height),
          "pixels", s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.pipeline.inbound_fps", Ratio(s.inbound_video_fps), "frames/s",
          s.video_pipeline_availability, s.video_pipeline_reason,
          s.video_pipeline_measurement_point);
    group("video.codec.inbound", SafeText(s.inbound_video_codecs), "codecs",
          s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.outbound", SafeText(s.outbound_video_codecs), "codecs",
          s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.decoders", SafeText(s.decoder_implementations), "implementations",
          s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.encoders", SafeText(s.encoder_implementations), "implementations",
          s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.decoder_power_efficient", SafeText(s.decoder_power_efficiency),
          "states", s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.encoder_power_efficient", SafeText(s.encoder_power_efficiency),
          "states", s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.codec.layers", SafeText(s.outbound_video_layers), "layers",
          s.video_codec_availability, s.video_codec_reason,
          s.video_codec_measurement_point);
    group("video.publish.requested", SafeText(s.video_publish_requested_codecs),
          "codecs", s.video_publish_plan_availability,
          s.video_publish_plan_reason, s.video_publish_plan_measurement_point);
    group("video.publish.effective", SafeText(s.video_publish_effective_codecs),
          "codecs", s.video_publish_plan_availability,
          s.video_publish_plan_reason, s.video_publish_plan_measurement_point);
    group("video.publish.observed", SafeText(s.video_publish_observed_codecs),
          "codecs", s.video_publish_plan_availability,
          s.video_publish_plan_reason, s.video_publish_plan_measurement_point);
    group("video.publish.fallback", SafeText(s.video_publish_fallback_reasons),
          "reason", s.video_publish_plan_availability,
          s.video_publish_plan_reason, s.video_publish_plan_measurement_point);
    group("video.publish.source", SafeText(s.video_publish_sources), "source",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.direction", SafeText(s.video_publish_direction), "direction",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.generation", SafeText(s.video_publish_generations), "generation",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.mode", SafeText(s.video_publish_modes), "mode",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.resolved_profile",
          SafeText(s.video_publish_resolved_profiles), "profile",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.observed_profile",
          SafeText(s.video_publish_observed_profiles), "profile",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.encoder_implementation",
          SafeText(s.video_publish_encoder_implementations), "implementations",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.resolved_scalability",
          SafeText(s.video_publish_resolved_scalability), "mode",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.publish.observed_scalability",
          SafeText(s.video_publish_observed_scalability), "mode",
          s.video_publish_plan_availability, s.video_publish_plan_reason,
          s.video_publish_plan_measurement_point);
    group("video.processing.decode_average", Ratio(s.video_decode_ms_per_frame),
          "ms/frame", s.video_processing_availability, s.video_processing_reason,
          s.video_processing_measurement_point);
    group("video.processing.encode_average", Ratio(s.video_encode_ms_per_frame),
          "ms/frame", s.video_processing_availability, s.video_processing_reason,
          s.video_processing_measurement_point);

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
    group("render.admission_to_first",
          Signed(s.last_admission_to_first_render_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.average", Ratio(s.render_average_interval_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.maximum", Signed(s.render_maximum_interval_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.p50", Ratio(s.render_interval_p50_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.interval.p95", Ratio(s.render_interval_p95_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    for (std::size_t i = 0; i < s.render_interval_histogram.size(); ++i)
        group("render.interval.bucket." + std::to_string(i), s.render_interval_histogram[i], "intervals",
              s.render_first_frame_availability, s.render_first_frame_reason,
              s.render_first_frame_measurement_point);
    group("render.interval.p99", Ratio(s.render_interval_p99_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    group("render.submit_fps", Ratio(s.render_submit_fps), "frames/s",
          s.render_first_frame_availability, s.render_first_frame_reason,
          s.render_first_frame_measurement_point);
    constexpr auto window_point = "aggregate_expected_continuous_render_submit_window_v1";
    const auto window_meta = s.render_window_scope_epoch ? Availability::Valid : Availability::Unknown;
    group("render.window.scope_epoch", s.render_window_scope_epoch, "epoch",
          window_meta, s.render_window_reason, window_point);
    group("render.window.begin", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              s.render_window_begin.time_since_epoch()).count()), "monotonic_us",
          window_meta, s.render_window_reason, window_point);
    group("render.window.end", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              s.render_window_end.time_since_epoch()).count()), "monotonic_us",
          window_meta, s.render_window_reason, window_point);
    group("render.window.bindings", s.render_window_bindings, "bindings",
          window_meta, s.render_window_reason, window_point);
    group("render.window.submits", s.render_window_submits, "frames",
          s.render_window_availability, s.render_window_reason, window_point);
    group("render.window.submit_fps", Ratio(s.render_window_submit_fps), "frames/s",
          s.render_window_availability, s.render_window_reason, window_point);
    for (std::size_t i = 0; i < s.render_window_interval_histogram.size(); ++i)
        group("render.window.interval.bucket." + std::to_string(i), s.render_window_interval_histogram[i], "intervals",
              s.render_window_availability, s.render_window_reason, window_point);
    group("render.window.fine.bindings", s.render_window_fine_bindings, "bindings",
          window_meta, s.render_window_reason, window_point);
    for (std::size_t i = 0; i < s.render_window_fine_interval_histogram.size(); ++i)
        group("render.window.fine.bucket." + std::to_string(i), s.render_window_fine_interval_histogram[i], "intervals",
              s.render_window_availability,
              s.render_window_fine_bindings == s.render_window_bindings ? s.render_window_reason : "render_fine_partial_coverage",
              window_point);
    group("render.frame_age.average", Ratio(s.render_average_frame_age_ms), "ms",
          s.render_frame_age_availability, s.render_frame_age_reason,
          s.render_frame_age_measurement_point);
    group("render.frame_age.maximum", Signed(s.render_maximum_frame_age_ms), "ms",
          s.render_frame_age_availability, s.render_frame_age_reason,
          s.render_frame_age_measurement_point);
    group("render.policy.target_interval", Signed(s.render_target_interval_ms), "ms",
          s.render_first_frame_availability, s.render_first_frame_reason,
          "render_binding_target_cadence");
    group("render.policy.expected_bindings", s.render_expected_bindings, "bindings",
          s.render_first_frame_availability, s.render_first_frame_reason,
          "render_visibility_expectation");
    group("render.policy.hidden_bindings", s.render_hidden_bindings, "bindings",
          s.render_first_frame_availability, s.render_first_frame_reason,
          "render_visibility_expectation");
    group("render.policy.minimized_bindings", s.render_minimized_bindings, "bindings",
          s.render_first_frame_availability, s.render_first_frame_reason,
          "render_visibility_expectation");
    group("render.policy.latest_frame_replacements", s.render_policy_skipped_frames,
          "frames", s.router_queue_availability, s.router_queue_reason,
          "bounded_latest_frame_router");
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
    group("render.stage.convert.samples", s.render_convert_samples, "samples",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.convert.total", s.render_convert_total_us, "us",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.convert.maximum", Signed(s.render_convert_max_us), "us",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.upload.samples", s.render_upload_samples, "samples",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.upload.total", s.render_upload_total_us, "us",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.upload.maximum", Signed(s.render_upload_max_us), "us",
          s.render_stage_availability, s.render_stage_reason,
          s.render_stage_measurement_point);
    group("render.stage.draw.samples", s.render_draw_samples, "samples",
          s.render_stage_availability, s.render_stage_reason,
          "qt_tile_paint_or_canvas_draw_cpu_span_v2");
    group("render.stage.draw.total", s.render_draw_total_us, "us",
          s.render_stage_availability, s.render_stage_reason,
          "qt_tile_paint_or_canvas_draw_cpu_span_v2");
    group("render.stage.draw.maximum", Signed(s.render_draw_max_us), "us",
          s.render_stage_availability, s.render_stage_reason,
          "qt_tile_paint_or_canvas_draw_cpu_span_v2");
    group("render.stage.present_block.samples",
          s.render_present_block_samples, "samples",
          s.render_stage_availability, s.render_stage_reason,
          "canvas_gl_swap_or_dx11_render_present_cpu_span_v2");
    group("render.stage.present_block.total", s.render_present_block_total_us, "us",
          s.render_stage_availability, s.render_stage_reason,
          "canvas_gl_swap_or_dx11_render_present_cpu_span_v2");
    group("render.stage.present_block.maximum",
          Signed(s.render_present_block_max_us), "us",
          s.render_stage_availability, s.render_stage_reason,
          "canvas_gl_swap_or_dx11_render_present_cpu_span_v2");
    group("render.stage.gpu_execution", std::monostate{}, "us",
          s.render_gpu_execution_availability,
          s.render_gpu_execution_reason,
          "gpu_timestamp_query");
    group("render.pipeline.router_submitted", s.render_router_submitted, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.router_replaced", s.render_router_replaced, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.router_rejected_generation",
          s.render_router_rejected_generation, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.router_rejected_binding",
          s.render_router_rejected_binding, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.router_dropped_invalid", s.render_router_dropped_invalid,
          "frames", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.router_dropped_capacity", s.render_router_dropped_capacity,
          "frames", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.qt_conversion_failures", s.render_qt_conversion_failures,
          "frames", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.delivered_gpu", s.render_delivered_to_gpu, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.delivered_qt_cpu", s.render_delivered_to_qt_cpu, "frames",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.rejected_track_attachments",
          s.render_rejected_track_attachments, "bindings",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.attached_tracks", s.render_attached_track_count, "bindings",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.requested_backend", SafeText(s.render_requested_backend),
          "backend", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.actual_backend", SafeText(s.render_actual_backend),
          "backend", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.gpu_failure", SafeText(s.render_gpu_failure), "reason",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.fallback_reason", SafeText(s.render_fallback_reason),
          "reason", s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.backend_failures", s.render_backend_failures, "events",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("render.pipeline.backend_fallbacks", s.render_backend_fallbacks, "events",
          s.render_pipeline_availability, s.render_pipeline_reason,
          s.render_pipeline_measurement_point);
    group("video.policy.coordinator_session", s.video_policy_coordinator_session,
          "generation", s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.native_room_generation",
          s.video_policy_native_room_generation, "generation",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.catalog_revision", s.video_policy_catalog_revision,
          "revision", s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.revision", s.video_policy_revision, "revision",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.stage_content", SafeText(s.video_policy_stage_content),
          "state", s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.selection_reason",
          SafeText(s.video_policy_selection_reason), "reason",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.retired", s.video_policy_retired, "bool",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.requested", s.video_policy_requested, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.selected", s.video_policy_selected, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.actual", s.video_policy_actual, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.bound", s.video_policy_bound, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.selected_not_requested",
          s.video_policy_selected_not_requested, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.selected_not_actual",
          s.video_policy_selected_not_actual, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.actual_not_selected",
          s.video_policy_actual_not_selected, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.selected_not_bound",
          s.video_policy_selected_not_bound, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.bound_not_selected",
          s.video_policy_bound_not_selected, "tracks",
          s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
    group("video.policy.stale_updates", s.video_policy_stale_updates,
          "updates", s.video_policy_availability, s.video_policy_reason,
          s.video_policy_measurement_point);
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
    group("reconnect.episodes", s.reconnect_episodes, "episodes",
          s.reconnect_density_availability, s.reconnect_density_reason);
    group("reconnect.episodes_per_hour", Ratio(s.reconnect_episodes_per_hour),
          "episodes/hour", s.reconnect_density_availability,
          s.reconnect_density_reason);

    group("stability.anomaly.operation_failures",
          s.stability_operation_failures, "events",
          s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);
    group("stability.anomaly.sampler_interruptions",
          s.stability_sampler_interruptions, "events",
          s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);
    group("stability.anomaly.device_stops", s.stability_device_stops, "events",
          s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);
    group("stability.anomaly.media_failures", s.stability_media_failures, "events",
          s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);
    group("stability.anomaly.total", s.stability_anomalies, "events",
          s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);
    group("stability.anomaly.per_hour", Ratio(s.stability_anomalies_per_hour),
          "events/hour", s.stability_anomaly_density_availability,
          s.stability_anomaly_density_reason,
          s.stability_anomaly_density_algorithm);

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
    const auto unknown_termination_availability =
        ParseAvailability(st.unknown_termination_availability);
    const auto confirmed_crash_availability =
        ParseAvailability(st.confirmed_crash_availability);
    group("stability.process_runs_started", st.process_runs_started, "runs",
          availability, st.ledger_reason);
    group("stability.process_runs_terminal", st.process_runs_terminal, "runs",
          availability, st.ledger_reason);
    group("stability.clean_process_exits", st.clean_process_exits, "runs",
          availability, st.ledger_reason);
    group("stability.unknown_process_terminations", st.unknown_process_terminations,
          "runs", unknown_termination_availability,
          st.unknown_termination_reason);
    group("stability.confirmed_process_crashes",
          ValueWhenSupported(st.confirmed_process_crashes,
                             confirmed_crash_availability),
          "runs", confirmed_crash_availability, st.confirmed_crash_reason);
    group("stability.sessions_started", st.sessions_started, "sessions",
          availability, st.ledger_reason);
    group("stability.sessions_terminal", st.sessions_terminal, "sessions",
          availability, st.ledger_reason);
    group("stability.unknown_session_terminations", st.unknown_session_terminations,
          "sessions", unknown_termination_availability,
          st.unknown_termination_reason);
    group("stability.unknown_process_termination_ratio",
          Ratio(st.unknown_process_termination_ratio), "ratio",
          unknown_termination_availability, st.unknown_termination_reason);
    group("stability.confirmed_process_crash_ratio",
          RatioWhenSupported(st.confirmed_process_crash_ratio,
                             confirmed_crash_availability),
          "ratio", confirmed_crash_availability, st.confirmed_crash_reason);
    group("stability.corrupt_inputs", st.corrupt_inputs, "records",
          availability, st.ledger_reason);
    group("stability.write_failures", st.write_failures, "writes",
          availability, st.ledger_reason);
    group("stability.duplicate_terminals", st.duplicate_terminals, "events",
          availability, st.ledger_reason);
    return rows;
}

std::string SerializeSafeTelemetryCheckpointRecord(
    const SafeTelemetryRecord& record) {
    // The metadata is identical for every metric in this snapshot. Avoid
    // allocating an eleven-member JSON object for each row; the JSON library
    // still owns string escaping, UTF-8 validation and typed value formatting.
    const auto definition = ",\"definition_version\":" +
        std::to_string(record.definition_version) + ",\"key\":";
    const auto metadata = ",\"revision\":" + std::to_string(record.snapshot.revision) +
        ",\"session_generation\":" + std::to_string(record.snapshot.session_generation) +
        ",\"source_monotonic_us\":" + std::to_string(record.source_monotonic_us) +
        ",\"source_utc_ms\":" + std::to_string(record.source_utc_ms != 0
            ? record.source_utc_ms : record.captured_utc_ms) + ",\"unit\":";
    std::string output;
    output.reserve(160 * 1024);
    for (const auto& metric : BuildSafeMetricRows(record)) {
        output += "{\"availability\":";
        AppendJsonString(output, metric.availability);
        output += definition;
        AppendJsonString(output, metric.key);
        output += ",\"measurement_point\":";
        AppendJsonString(output, metric.measurement_point);
        output += ",\"reason\":";
        AppendJsonString(output, metric.reason);
        output += metadata;
        AppendJsonString(output, metric.unit);
        output += ",\"value\":";
        AppendJsonValue(output, metric.value);
        output += "}\n";
    }
    // Retained checkpoints are charged by capacity; release unused reserve.
    output.shrink_to_fit();
    return output;
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
        {"session_duration_ms", final_record.snapshot.session_duration_ms >= 0
            ? Json(final_record.snapshot.session_duration_ms) : Json(nullptr)},
        {"usable_duration_ms", final_record.snapshot.usable_duration_ms >= 0
            ? Json(final_record.snapshot.usable_duration_ms) : Json(nullptr)},
        {"local_publish_media", {
            {"availability", AvailabilityName(
                final_record.snapshot.local_publish_media_availability)},
            {"reason", SafeText(
                final_record.snapshot.local_publish_media_reason)},
            {"algorithm", SafeText(
                final_record.snapshot.local_publish_media_algorithm)},
            {"publications", final_record.snapshot.local_publications},
            {"no_media", final_record.snapshot.local_publish_no_media},
        }},
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

struct TelemetryHistoryStore::WorkerContext final {
    using ExportCallback = TelemetryHistoryStore::ExportCallback;
    using MutationCallback = TelemetryHistoryStore::MutationCallback;
    WorkerContext(std::filesystem::path root, std::size_t memory_buckets,
        std::size_t queue_capacity, std::uint64_t maximum_bytes,
        std::chrono::hours retention, std::size_t queue_byte_capacity,
        std::string process_run_id, std::filesystem::path diagnostic_root,
        std::shared_ptr<TelemetryCheckpointControl> control);
    bool SubmitSnapshot(
        SessionTelemetry::SnapshotPtr snapshot,
        StabilitySummary stability = {},
        std::string anonymous_session_id = {});
    bool ExportCurrent(
        std::filesystem::path destination_root,
        ExportCallback callback,
        std::shared_ptr<std::atomic_bool> cancelled = {});
    bool ExportReport(
        std::string record_id,
        std::filesystem::path destination_root,
        ExportCallback callback,
        std::shared_ptr<std::atomic_bool> cancelled = {},
        std::int64_t first_utc_ms = 0,
        std::int64_t last_utc_ms = 0);
    bool ClearReport(std::string record_id, MutationCallback callback = {});
    bool RetryCheckpoint(std::string record_id, MutationCallback callback = {});
    void SetHistoryEnabled(bool enabled);

    std::shared_ptr<const TelemetryStoreStatus> Status() const;
    std::vector<SafeTelemetryRecordPtr> CurrentRecords() const;

    struct PendingReport {
        std::string id;
        std::uint64_t generation = 0;
        std::vector<TelemetryCheckpointRecord> records;
        std::size_t bytes = 0;
        std::size_t payload_bytes = 0;
        bool terminal = false;
        std::uint64_t committed_revision = 0;
        unsigned retry_count = 0;
        std::chrono::steady_clock::time_point first_failure{};
        std::chrono::steady_clock::time_point next_attempt{};
    };

    struct CurrentRecord {
        SafeTelemetryRecordPtr record;
        std::size_t charge = 0;
    };

    struct FlushTiming {
        WorkerContext& context;
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
        ~FlushTiming() {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - began).count());
            std::lock_guard lock(context.mutex_);
            ++context.status_.flush_count;
            context.status_.flush_total_us += elapsed;
            context.status_.flush_max_us = (std::max)(context.status_.flush_max_us, elapsed);
        }
    };

    enum class JobKind { Snapshot, Export, ExportReport, ClearReport, RetryCheckpoint,
                         SetHistoryEnabled };
    struct Job {
        JobKind kind = JobKind::Snapshot;
        SessionTelemetry::SnapshotPtr snapshot;
        StabilitySummary stability;
        TelemetryCheckpointRecord checkpoint;
        std::string anonymous_session_id;
        std::filesystem::path destination;
        std::string record_id;
        std::int64_t first_utc_ms = 0;
        std::int64_t last_utc_ms = 0;
        ExportCallback export_callback;
        MutationCallback mutation_callback;
        std::shared_ptr<std::atomic_bool> cancelled;
        bool enabled = true;
        std::int64_t captured_utc_ms = 0;
        std::uint64_t source_monotonic_us = 0;
        std::size_t charge = 0;
    };

    void Run();
    void RunLoop();
    void HandleSnapshot(Job job);
    void HandleExport(Job job);
    void HandleClear(Job job);
    void HandleRetry(Job job);
    void RefreshAndPruneReports(std::uint64_t reserve_bytes = 0);
    void FlushPendingReports(bool force);
    void FlushLossRanges(bool force);
    void RecordLossLocked(const std::string& session_id,
                          const TelemetryCheckpointRecord& record);
    void PublishPendingStatus();
    void PublishStatusLocked();
    bool Enqueue(Job job, bool terminal_priority);
    bool EnqueueLocked(Job job, bool terminal_priority);
    static std::string NewOpaqueId();

    const std::filesystem::path root_;
    const std::filesystem::path diagnostic_root_;
    const std::string process_run_id_;
    const std::size_t memory_buckets_;
    const std::size_t queue_capacity_;
    const std::size_t queue_byte_capacity_;
    const std::uint64_t maximum_bytes_;
    const std::chrono::hours retention_;

    std::mutex submit_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::size_t inflight_jobs_ = 0;
    std::size_t export_jobs_ = 0;
    std::deque<CurrentRecord> current_records_;
    std::size_t current_record_bytes_ = 0;
    // Worker-owned; previous sessions survive current_records_ replacement.
    std::deque<PendingReport> pending_reports_;
    std::chrono::steady_clock::time_point next_history_refresh_{};
    std::deque<TelemetryLossRange> loss_ranges_pending_;
    unsigned loss_retry_count_ = 0;
    std::chrono::steady_clock::time_point loss_first_failure_{};
    std::chrono::steady_clock::time_point loss_next_attempt_{};
    std::unique_ptr<TelemetryRunLease> run_lease_;
    std::string current_session_id_;
    std::uint64_t current_generation_ = 0;
    bool stopping_ = false;
    TelemetryStoreStatus status_;
    mutable std::shared_ptr<const TelemetryStoreStatus> status_cache_;

    const std::shared_ptr<TelemetryCheckpointControl> control_;
    std::condition_variable done_;
    bool finished_ = false;
    bool worker_failed_ = false;
    bool unconfirmed_commit_ = false;
    bool status_frozen_ = false;
    bool Stopped() const noexcept { return control_->abandoned.load(std::memory_order_acquire); }
};

TelemetryHistoryStore::WorkerContext::WorkerContext(
    std::filesystem::path root,
    std::size_t memory_buckets,
    std::size_t queue_capacity,
    std::uint64_t maximum_bytes,
    std::chrono::hours retention,
    std::size_t queue_byte_capacity,
    std::string process_run_id,
    std::filesystem::path diagnostic_root,
    std::shared_ptr<TelemetryCheckpointControl> control)
    : root_(std::move(root))
    , diagnostic_root_(std::move(diagnostic_root))
    , process_run_id_(std::move(process_run_id))
    , memory_buckets_((std::max)(std::size_t{1}, memory_buckets))
    , queue_capacity_((std::clamp)(queue_capacity,
        std::size_t{4}, kDefaultQueueCapacity))
    , queue_byte_capacity_((std::clamp)(queue_byte_capacity,
        std::size_t{4096}, kDefaultQueueBytes))
    , maximum_bytes_((std::max)(std::uint64_t{1024 * 1024}, maximum_bytes))
    , retention_(retention), control_(std::move(control)) {
    status_.queue_capacity = queue_capacity_;
    status_.queue_byte_capacity = queue_byte_capacity_;
    status_cache_ = std::make_shared<const TelemetryStoreStatus>(status_);
}

bool TelemetryHistoryStore::WorkerContext::SubmitSnapshot(
    SessionTelemetry::SnapshotPtr snapshot,
    StabilitySummary stability,
    std::string anonymous_session_id) {
    if (!snapshot) return false;
    if (snapshot->metric_product_chains.size() > 64 ||
        snapshot->operation_summaries.size() > 64) return false;
    if (!anonymous_session_id.empty() &&
        (anonymous_session_id.size() != 32 ||
         !std::all_of(anonymous_session_id.begin(), anonymous_session_id.end(),
             [](char ch) { return (ch >= '0' && ch <= '9') ||
                 (ch >= 'a' && ch <= 'f'); }))) return false;
    bool admitted = false;
    try {
        std::lock_guard submit_lock(submit_mutex_);
        const auto generation = snapshot->session_generation;
        bool new_session = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            new_session = current_session_id_.empty() ||
                current_generation_ != generation;
            if (anonymous_session_id.empty() && !new_session)
                anonymous_session_id = current_session_id_;
            else if (!new_session &&
                       anonymous_session_id != current_session_id_) {
                ++status_.queue_drops;
                status_.availability = Availability::Invalid;
                status_.reason = "met_06_session_id_mismatch";
                PublishStatusLocked();
                return false;
            }
        }
        if (anonymous_session_id.empty()) anonymous_session_id = NewOpaqueId();
        const auto submitted_at = Snapshot::Clock::now();
        const auto source_at = snapshot->generated_at ==
                Snapshot::Clock::time_point{} ||
            snapshot->generated_at > submitted_at
            ? submitted_at : snapshot->generated_at;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(submitted_at - source_at);
        Job job;
        job.kind = JobKind::Snapshot;
        job.snapshot = std::move(snapshot);
        job.stability = std::move(stability);
        job.captured_utc_ms = UtcNowMs();
        job.source_monotonic_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                source_at.time_since_epoch()).count();
        job.checkpoint.generation = generation;
        job.checkpoint.revision = job.snapshot->revision;
        job.checkpoint.source_utc_ms = job.captured_utc_ms - elapsed.count();
        job.checkpoint.complete = job.snapshot->session_complete;
        job.anonymous_session_id = anonymous_session_id;
        const bool terminal = job.checkpoint.complete;
        std::lock_guard lock(mutex_);
        if (!EnqueueLocked(std::move(job), terminal))
            return false;
        admitted = true;
        if (new_session) {
            current_records_.clear();
            current_record_bytes_ = 0;
            current_session_id_ = anonymous_session_id;
            current_generation_ = generation;
            status_.memory_records = 0;
            status_.memory_bytes = 0;
        }
        ++status_.snapshots_accepted;
        if (status_.availability != Availability::Invalid) {
            status_.availability = Availability::Valid;
            status_.reason = status_.history_enabled
                ? "bounded_history_valid" : "history_disabled_by_user";
        }
        if (terminal && !status_.history_enabled) {
            current_records_.clear();
            current_record_bytes_ = 0;
            status_.memory_records = 0;
            status_.memory_bytes = 0;
        }
        PublishStatusLocked();
        return true;
    } catch (...) {
        std::lock_guard lock(mutex_);
        if (!admitted) ++status_.queue_drops;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_snapshot_admission_failed";
        PublishStatusLocked();
        return admitted;
    }
}

bool TelemetryHistoryStore::WorkerContext::ExportCurrent(
    std::filesystem::path destination_root,
    ExportCallback callback,
    std::shared_ptr<std::atomic_bool> cancelled) {
    Job job;
    job.kind = JobKind::Export;
    job.destination = std::move(destination_root);
    job.export_callback = callback;
    job.cancelled = std::move(cancelled);
    if (Enqueue(std::move(job), true)) return true;
    try { if (callback) callback({false, false, "export_rejected", {}}); }
    catch (...) {}
    return false;
}

bool TelemetryHistoryStore::WorkerContext::ExportReport(
    std::string record_id,
    std::filesystem::path destination_root,
    ExportCallback callback,
    std::shared_ptr<std::atomic_bool> cancelled,
    std::int64_t first_utc_ms,
    std::int64_t last_utc_ms) {
    constexpr std::string_view v2 = "cohavora-telemetry-v2-";
    constexpr std::string_view v1 = "cohavora-telemetry-v1-";
    const auto suffix = record_id.starts_with(v2)
        ? std::string_view(record_id).substr(v2.size())
        : record_id.starts_with(v1)
            ? std::string_view(record_id).substr(v1.size())
            : std::string_view{};
    const auto legacy_dash = suffix.find('-');
    const bool valid_v1 = record_id.starts_with(v1) &&
        legacy_dash != std::string_view::npos && legacy_dash > 0 &&
        std::all_of(suffix.begin(), suffix.begin() + legacy_dash,
            [](char ch) { return ch >= '0' && ch <= '9'; }) &&
        suffix.size() == legacy_dash + 1 + 32;
    const auto session = valid_v1
        ? suffix.substr(legacy_dash + 1) : suffix;
    if ((!record_id.starts_with(v2) && !valid_v1) ||
        session.size() != 32 ||
        !std::all_of(session.begin(), session.end(),
            [](char ch) { return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f'); }) ||
        first_utc_ms < 0 || last_utc_ms < 0 ||
        (first_utc_ms && last_utc_ms && first_utc_ms > last_utc_ms)) {
        try { if (callback) callback({false, false, "invalid_report_id", {}}); }
        catch (...) {}
        return false;
    }
    Job job;
    job.kind = JobKind::ExportReport;
    job.record_id = std::move(record_id);
    job.first_utc_ms = first_utc_ms;
    job.last_utc_ms = last_utc_ms;
    job.destination = std::move(destination_root);
    job.export_callback = callback;
    job.cancelled = std::move(cancelled);
    if (Enqueue(std::move(job), true)) return true;
    try { if (callback) callback({false, false, "export_rejected", {}}); }
    catch (...) {}
    return false;
}

bool TelemetryHistoryStore::WorkerContext::ClearReport(
    std::string record_id,
    MutationCallback callback) {
    if (record_id.empty() || record_id.find_first_of("/\\") != std::string::npos) {
        try { if (callback) callback(false, "invalid_report_id"); }
        catch (...) {}
        return false;
    }
    Job job;
    job.kind = JobKind::ClearReport;
    job.record_id = std::move(record_id);
    job.mutation_callback = callback;
    if (Enqueue(std::move(job), true)) return true;
    try { if (callback) callback(false, "clear_rejected"); }
    catch (...) {}
    return false;
}

bool TelemetryHistoryStore::WorkerContext::RetryCheckpoint(
    std::string record_id, MutationCallback callback) {
    if (record_id.empty() || record_id.find_first_of("/\\") != std::string::npos) {
        try { if (callback) callback(false, "invalid_report_id"); }
        catch (...) {}
        return false;
    }
    Job job;
    job.kind = JobKind::RetryCheckpoint;
    job.record_id = std::move(record_id);
    job.mutation_callback = callback;
    if (Enqueue(std::move(job), true)) return true;
    try { if (callback) callback(false, "retry_rejected"); }
    catch (...) {}
    return false;
}

void TelemetryHistoryStore::WorkerContext::SetHistoryEnabled(bool enabled) {
    Job job;
    job.kind = JobKind::SetHistoryEnabled;
    job.enabled = enabled;
    Enqueue(std::move(job), true);
}

std::shared_ptr<const TelemetryStoreStatus> TelemetryHistoryStore::WorkerContext::Status() const {
    return std::atomic_load_explicit(&status_cache_, std::memory_order_acquire);
}

std::vector<SafeTelemetryRecordPtr> TelemetryHistoryStore::WorkerContext::CurrentRecords() const {
    std::lock_guard lock(mutex_);
    std::vector<SafeTelemetryRecordPtr> result;
    result.reserve(current_records_.size());
    for (const auto& item : current_records_)
        result.push_back(item.record);
    return result;
}

bool TelemetryHistoryStore::WorkerContext::Enqueue(Job job, bool terminal_priority) {
    try {
        std::lock_guard lock(mutex_);
        return EnqueueLocked(std::move(job), terminal_priority);
    } catch (...) {
        return false;
    }
}

bool TelemetryHistoryStore::WorkerContext::EnqueueLocked(Job job, bool terminal_priority) {
    if (stopping_) return false;
    const bool export_job = job.kind == JobKind::Export ||
        job.kind == JobKind::ExportReport;
    if (export_job && export_jobs_ >= 2) return false;
    constexpr std::size_t kControlCallbackReservation = 4096;
    job.charge = sizeof(Job) + job.checkpoint.jsonl.capacity() +
        // Include the worker's SafeTelemetryRecord snapshot copy as well.
        (job.snapshot ? 2 * SnapshotRetainedBytes(*job.snapshot) : 0) +
        StabilityRetainedBytes(job.stability) +
        (job.export_callback || job.mutation_callback
            ? kControlCallbackReservation : 0) +
        job.anonymous_session_id.capacity() + job.record_id.capacity() +
        job.destination.native().capacity() *
            sizeof(std::filesystem::path::value_type);
    if (job.charge > queue_byte_capacity_) {
        ++status_.queue_byte_limit_hits;
        if (job.kind == JobKind::Snapshot)
            RecordLossLocked(job.anonymous_session_id, job.checkpoint);
        ++status_.queue_drops;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_queue_bytes_exceeded";
        PublishStatusLocked();
        return false;
    }
    while (jobs_.size() + inflight_jobs_ >= queue_capacity_ ||
           status_.queue_bytes > queue_byte_capacity_ - job.charge) {
        if (jobs_.size() + inflight_jobs_ >= queue_capacity_)
            ++status_.queue_job_limit_hits;
        if (status_.queue_bytes > queue_byte_capacity_ - job.charge)
            ++status_.queue_byte_limit_hits;
        if (terminal_priority) {
            const auto found = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& queued) {
                return queued.kind == JobKind::Snapshot &&
                    !queued.checkpoint.complete;
            });
            if (found != jobs_.end()) {
                RecordLossLocked(found->anonymous_session_id, found->checkpoint);
                status_.queue_bytes -= found->charge;
                jobs_.erase(found);
                ++status_.queue_drops;
            } else {
                if (job.kind == JobKind::Snapshot)
                    RecordLossLocked(job.anonymous_session_id, job.checkpoint);
                ++status_.queue_drops;
                status_.availability = Availability::Invalid;
                status_.reason = "met_06_queue_capacity_exceeded";
                PublishStatusLocked();
                return false;
            }
        } else {
            if (job.kind == JobKind::Snapshot)
                RecordLossLocked(job.anonymous_session_id, job.checkpoint);
            ++status_.queue_drops;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_queue_capacity_exceeded";
            PublishStatusLocked();
            return false;
        }
    }
    const auto charge = job.charge;
    jobs_.push_back(std::move(job));
    if (export_job) ++export_jobs_;
    status_.queue_bytes += charge;
    status_.queue_peak_jobs = (std::max)(status_.queue_peak_jobs,
        jobs_.size() + inflight_jobs_);
    status_.queue_peak_bytes = (std::max)(status_.queue_peak_bytes, status_.queue_bytes);
    status_.queue_depth = jobs_.size();
    PublishStatusLocked();
    condition_.notify_one();
    return true;
}

void TelemetryHistoryStore::WorkerContext::RunLoop() {
    RefreshAndPruneReports();
    while (true) {
        Job job;
        bool has_job = false;
        bool stopping = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::seconds(1),
                                [this] { return stopping_ || !jobs_.empty(); });
            stopping = stopping_;
            if (Stopped() || (jobs_.empty() && stopping)) break;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                ++inflight_jobs_;
                status_.inflight_jobs = inflight_jobs_;
                has_job = true;
                status_.queue_depth = jobs_.size();
                PublishStatusLocked();
            }
        }
        const auto completed_charge = job.charge;
        const auto job_began = std::chrono::steady_clock::now();
        try {
            if (!has_job) {
                FlushPendingReports(false);
                FlushLossRanges(false);
                continue;
            }
            switch (job.kind) {
            case JobKind::Snapshot: {
                const auto began = std::chrono::steady_clock::now();
                HandleSnapshot(std::move(job));
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - began).count();
                std::lock_guard lock(mutex_);
                status_.snapshot_max_us = (std::max)(status_.snapshot_max_us,
                    static_cast<std::uint64_t>(elapsed));
                status_.snapshot_total_us += static_cast<std::uint64_t>(elapsed);
                ++status_.snapshot_count;
                break;
            }
            case JobKind::Export: HandleExport(std::move(job)); break;
            case JobKind::ExportReport: HandleExport(std::move(job)); break;
            case JobKind::ClearReport: HandleClear(std::move(job)); break;
            case JobKind::RetryCheckpoint: HandleRetry(std::move(job)); break;
            case JobKind::SetHistoryEnabled: {
                std::vector<std::string> report_ids;
                {
                    std::lock_guard lock(mutex_);
                    status_.history_enabled = job.enabled;
                    if (!job.enabled) {
                        current_records_.clear();
                        current_record_bytes_ = 0;
                        pending_reports_.clear();
                        loss_ranges_pending_.clear();
                        status_.loss_ranges_pending = 0;
                        for (const auto& entry : status_.reports) {
                            report_ids.push_back(entry.record_id);
                        }
                    }
                    status_.memory_records = current_records_.size();
                    status_.memory_bytes = current_record_bytes_;
                    status_.availability = Availability::Valid;
                    status_.reason = job.enabled
                        ? "history_enabled" : "history_disabled_by_user";
                    PublishStatusLocked();
                }
                if (!job.enabled) {
                    for (const auto& id : report_ids) {
                        if (id.starts_with("cohavora-telemetry-v2-"))
                            RemoveTelemetryCheckpoint(root_ / id);
                        else
                            RemoveKnownReport(root_ / id);
                    }
                    if (!ClearTelemetryLossSummary(root_)) {
                        std::lock_guard lock(mutex_);
                        ++status_.write_failures;
                        status_.availability = Availability::Invalid;
                        status_.reason = "met_06_loss_summary_clear_failed";
                        PublishStatusLocked();
                    }
                    RefreshAndPruneReports();
                }
                break;
            }
            }
            if (!Stopped()) FlushPendingReports(false);
            if (!Stopped()) FlushLossRanges(false);
        } catch (...) {
            std::lock_guard lock(mutex_);
            ++status_.write_failures;
            status_.availability = Availability::Invalid;
            worker_failed_ = true;
            status_.reason = "met_06_worker_exception";
            PublishStatusLocked();
        }
        {
            std::lock_guard lock(mutex_);
            const auto job_elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - job_began).count());
            status_.worker_job_max_us = (std::max)(status_.worker_job_max_us, job_elapsed);
            status_.worker_job_total_us += job_elapsed;
            ++status_.worker_job_count;
            status_.queue_bytes -= completed_charge;
            --inflight_jobs_;
            status_.inflight_jobs = inflight_jobs_;
            PublishStatusLocked();
        }
    }
    if (!Stopped()) FlushPendingReports(true);
    if (!Stopped()) FlushLossRanges(true);
}

void TelemetryHistoryStore::WorkerContext::RecordLossLocked(
    const std::string& session_id,
    const TelemetryCheckpointRecord& record) {
    try {
        if (session_id.size() != 32 || record.revision == 0) return;
        if (!loss_ranges_pending_.empty() &&
            loss_ranges_pending_.back().anonymous_session_id == session_id) {
            auto& last = loss_ranges_pending_.back();
            ++last.dropped_records;
            last.first_revision = (std::min)(last.first_revision, record.revision);
            last.last_revision = (std::max)(last.last_revision, record.revision);
            last.first_source_utc_ms = (std::min)(
                last.first_source_utc_ms, record.source_utc_ms);
            last.last_source_utc_ms = (std::max)(
                last.last_source_utc_ms, record.source_utc_ms);
        } else if (loss_ranges_pending_.size() < 256) {
            loss_ranges_pending_.push_back({session_id, 1, record.revision,
                record.revision, record.source_utc_ms, record.source_utc_ms});
        } else {
            ++status_.loss_ranges_omitted;
        }
        status_.loss_ranges_pending = loss_ranges_pending_.size();
    } catch (...) { ++status_.loss_ranges_omitted; }
}

void TelemetryHistoryStore::WorkerContext::FlushLossRanges(bool force) {
    if (Stopped()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < loss_next_attempt_) return;
    std::deque<TelemetryLossRange> pending;
    {
        std::lock_guard lock(mutex_);
        if (loss_ranges_pending_.empty()) return;
        pending.swap(loss_ranges_pending_);
        status_.loss_ranges_pending = 0;
        status_.loss_ranges_inflight = pending.size();
        PublishStatusLocked();
    }
    bool persisted = false;
    try {
        // A loss burst must not trigger a full history scan after every
        // consumed job and amplify the admission stall that caused it.
        const auto owned = TelemetryHistoryOwnedBytes(root_);
        if (!owned || *owned > maximum_bytes_ ||
            64 * 1024 > maximum_bytes_ - *owned || now >= next_history_refresh_)
            RefreshAndPruneReports(64 * 1024);
        const std::vector<TelemetryLossRange> batch(
            pending.begin(), pending.end());
        if (!Stopped()) persisted = PersistTelemetryLossRanges(root_, batch, maximum_bytes_);
    } catch (...) {}
    if (persisted) {
        loss_retry_count_ = 0;
        loss_first_failure_ = {};
        loss_next_attempt_ = now + std::chrono::seconds(1);
        std::lock_guard lock(mutex_);
        status_.loss_ranges_inflight = 0;
        status_.loss_ranges_persisted += pending.size();
        PublishStatusLocked();
        return;
    }
    ++loss_retry_count_;
    if (loss_first_failure_ == std::chrono::steady_clock::time_point{})
        loss_first_failure_ = now;
    constexpr std::array delays{1, 5, 30, 60};
    loss_next_attempt_ = now + std::chrono::seconds(delays[(std::min)(
        loss_retry_count_ - 1, static_cast<unsigned>(delays.size() - 1))]);
    if (now - loss_first_failure_ >= std::chrono::minutes(5))
        loss_next_attempt_ = std::chrono::steady_clock::time_point::max();
    std::lock_guard lock(mutex_);
    status_.loss_ranges_inflight = 0;
    while (!pending.empty()) {
        if (loss_ranges_pending_.size() == 256) {
            status_.loss_ranges_omitted += pending.size();
            break;
        }
        loss_ranges_pending_.push_front(std::move(pending.back()));
        pending.pop_back();
    }
    status_.loss_ranges_pending = loss_ranges_pending_.size();
    ++status_.loss_persist_failures;
    status_.availability = Availability::Invalid;
    status_.reason = "met_06_loss_summary_write_failed";
    PublishStatusLocked();
}

void TelemetryHistoryStore::WorkerContext::HandleSnapshot(Job job) {
    if (!job.snapshot) return;
    try {
        auto record = std::make_shared<SafeTelemetryRecord>();
        record->anonymous_session_id = job.anonymous_session_id;
        record->captured_utc_ms = job.captured_utc_ms;
        record->source_utc_ms = job.checkpoint.source_utc_ms;
        record->source_monotonic_us = job.source_monotonic_us;
        record->complete = job.checkpoint.complete;
        record->snapshot = *job.snapshot;
        record->stability = std::move(job.stability);
        job.checkpoint = MakeTelemetryCheckpointRecord(*record);
        {
            std::lock_guard lock(mutex_);
            if (job.anonymous_session_id == current_session_id_ &&
                (status_.history_enabled || !job.checkpoint.complete) &&
                job.checkpoint.generation == current_generation_) {
                const auto bucket = record->source_utc_ms / 1000;
                // The JSONL belongs to pending_reports_, not current_records_.
                // Charging it here evicts retained snapshots based on a second
                // buffer's size and overstates the memory exposed to the UI.
                const auto charge = sizeof(SafeTelemetryRecord) +
                    SnapshotRetainedBytes(record->snapshot) +
                    StabilityRetainedBytes(record->stability);
                if (!current_records_.empty() &&
                    current_records_.back().record->source_utc_ms / 1000 == bucket) {
                    current_record_bytes_ -= current_records_.back().charge;
                    current_records_.back() = {std::move(record), charge};
                    current_record_bytes_ += charge;
                    ++status_.one_second_buckets_coalesced;
                } else {
                    current_records_.push_back({std::move(record), charge});
                    current_record_bytes_ += charge;
                }
                while (current_records_.size() > memory_buckets_ ||
                       current_record_bytes_ > kDefaultMemoryBytes) {
                    current_record_bytes_ -= current_records_.front().charge;
                    current_records_.pop_front();
                    ++status_.memory_records_evicted;
                }
                status_.memory_records = current_records_.size();
                status_.memory_bytes = current_record_bytes_;
            }
            PublishStatusLocked();
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        RecordLossLocked(job.anonymous_session_id, job.checkpoint);
        ++status_.queue_drops;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_snapshot_projection_failed";
        PublishStatusLocked();
        return;
    }
    if (job.checkpoint.jsonl.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (!status_.history_enabled) return;
    }
    constexpr std::size_t kMaximumPendingBytes = 8 * 1024 * 1024;
    constexpr std::size_t kMaximumSegmentPayload = 4 * 1024 * 1024;
    bool needs_headroom = false;
    {
        std::size_t total = 0;
        std::size_t payload = job.checkpoint.jsonl.size();
        std::size_t replaced_charge = 0;
        bool existing = false;
        for (const auto& pending : pending_reports_) {
            total += pending.bytes;
            if (pending.id != job.anonymous_session_id) continue;
            existing = true;
            payload += pending.payload_bytes;
            for (const auto& record : pending.records) {
                if (record.revision != job.checkpoint.revision) continue;
                replaced_charge = PendingCharge(record);
                payload -= record.jsonl.size();
                break;
            }
        }
        needs_headroom = total - replaced_charge + PendingCharge(job.checkpoint) >
            kMaximumPendingBytes || payload > kMaximumSegmentPayload ||
            (!existing && pending_reports_.size() >= 4);
    }
    if (needs_headroom) {
        // Flush healthy reports before admitting the next record, including
        // pressure shared by multiple sessions. Never override IO retry backoff
        // and never keep a report reference across a flush that may erase it.
        for (auto& pending : pending_reports_)
            if (pending.retry_count == 0 && !pending.records.empty())
                pending.next_attempt = std::chrono::steady_clock::now();
        FlushPendingReports(false);
    }
    const auto found = std::find_if(pending_reports_.begin(), pending_reports_.end(),
        [&](const PendingReport& report) {
            return report.id == job.anonymous_session_id;
        });
    if (found == pending_reports_.end()) {
        if (pending_reports_.size() >= 4) {
            const auto& oldest = pending_reports_.front();
            std::lock_guard lock(mutex_);
            for (const auto& lost : oldest.records)
                RecordLossLocked(oldest.id, lost);
            status_.pending_records_dropped += oldest.records.size();
            ++status_.pending_reports_dropped;
            pending_reports_.pop_front();
        }
        pending_reports_.push_back(PendingReport{
            .id = job.anonymous_session_id,
            .generation = job.checkpoint.generation,
            .next_attempt = std::chrono::steady_clock::now() +
                std::chrono::seconds(15)});
    }
    auto& report = *std::find_if(pending_reports_.begin(), pending_reports_.end(),
        [&](const PendingReport& item) {
            return item.id == job.anonymous_session_id;
        });
    auto compact = std::move(job.checkpoint);
    const bool terminal = compact.complete;
    const auto bytes = PendingCharge(compact);
    std::size_t total = 0;
    for (const auto& item : pending_reports_) total += item.bytes;
    const auto same_revision = std::find_if(report.records.begin(),
        report.records.end(), [&](const auto& record) {
            return record.revision == compact.revision;
        });
    const auto replaced_bytes = same_revision == report.records.end()
        ? std::size_t{0} : PendingCharge(*same_revision);
    const auto replaced_payload = same_revision == report.records.end()
        ? std::size_t{0} : same_revision->jsonl.size();
    if (bytes > kMaximumPendingBytes ||
        total - replaced_bytes + bytes > kMaximumPendingBytes ||
        report.payload_bytes - replaced_payload + compact.jsonl.size() > kMaximumSegmentPayload) {
        std::lock_guard lock(mutex_);
        RecordLossLocked(job.anonymous_session_id, compact);
        ++status_.pending_records_dropped;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_pending_capacity_exceeded";
        PublishStatusLocked();
        return;
    }
    report.payload_bytes = report.payload_bytes - replaced_payload + compact.jsonl.size();
    if (same_revision != report.records.end()) {
        report.bytes -= replaced_bytes;
        *same_revision = std::move(compact);
    } else {
        report.records.push_back(std::move(compact));
    }
    report.bytes += bytes;
    report.terminal = report.terminal || terminal;
    // The time-based flush alone can fill the bounded pending buffer during
    // normal high-frequency snapshots. Batch by actual segment payload rather
    // than its doubled allocation reservation; preflight above protects the
    // pending and segment headroom before the next record is retained.
    if ((report.terminal || report.payload_bytes >= kDefaultMemoryBytes / 4) &&
        report.retry_count == 0)
        report.next_attempt = std::chrono::steady_clock::now();
    PublishPendingStatus();
}

void TelemetryHistoryStore::WorkerContext::PublishPendingStatus() {
    std::size_t bytes = 0;
    for (const auto& report : pending_reports_) bytes += report.bytes;
    std::lock_guard lock(mutex_);
    status_.pending_records = 0;
    for (const auto& report : pending_reports_) status_.pending_records += report.records.size();
    status_.pending_reports = pending_reports_.size();
    status_.pending_bytes = bytes;
    status_.pending_report_ids.clear();
    for (const auto& report : pending_reports_)
        status_.pending_report_ids.push_back(report.id);
    PublishStatusLocked();
}

void TelemetryHistoryStore::WorkerContext::FlushPendingReports(bool force) {
    const FlushTiming timing{*this};
    if (Stopped()) return;
    if (!pending_reports_.empty() && !process_run_id_.empty() &&
        (!run_lease_ || !run_lease_->acquired())) {
        run_lease_ = std::make_unique<TelemetryRunLease>(root_, process_run_id_);
        if (!run_lease_->acquired()) {
            std::lock_guard lock(mutex_);
            ++status_.write_failures;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_run_lease_unavailable";
            PublishStatusLocked();
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_reports_.begin(); it != pending_reports_.end();) {
        if (Stopped()) return;
        bool superseded = false;
        {
            std::lock_guard lock(mutex_);
            superseded = it->generation != current_generation_;
        }
        if (it->records.empty() && superseded) {
            it = pending_reports_.erase(it);
            continue;
        }
        if (it->records.empty() || (!force && now < it->next_attempt)) {
            ++it;
            continue;
        }
        std::uint64_t reserve_bytes = 16384;
        // Atomic append retains the old manifest until its replacement commits.
        // Include growth of an existing manifest instead of assuming 16 KiB;
        // bounded allowance covers the new segment and new missing-range rows.
        std::error_code manifest_error;
        const auto manifest_bytes = std::filesystem::file_size(root_ /
            ("cohavora-telemetry-v2-" + it->id) / "manifest.json", manifest_error);
        if (!manifest_error) {
            reserve_bytes = (std::max)(reserve_bytes,
                (std::min)(static_cast<std::uint64_t>(kTelemetryCheckpointMaximumManifestBytes),
                    manifest_bytes + 1024 + 512 * static_cast<std::uint64_t>(it->records.size())));
        }
        for (const auto& record : it->records)
            reserve_bytes += record.jsonl.size();
        // Append performs its own locked quota check and full checkpoint
        // integrity check. Scan unrelated reports only for retention/index
        // refresh or when actual owned bytes require pruning for this write.
        const auto owned_began = std::chrono::steady_clock::now();
        const auto owned = TelemetryHistoryOwnedBytes(root_);
        {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - owned_began).count());
            std::lock_guard lock(mutex_);
            ++status_.owned_scan_count;
            status_.owned_scan_total_us += elapsed;
            status_.owned_scan_max_us = (std::max)(status_.owned_scan_max_us, elapsed);
        }
        if (!owned || *owned > maximum_bytes_ ||
            reserve_bytes > maximum_bytes_ - *owned || now >= next_history_refresh_)
            RefreshAndPruneReports(reserve_bytes);
        if (Stopped()) return;
        const auto append_began = std::chrono::steady_clock::now();
        const auto result = AppendTelemetryCheckpoint(
            root_, it->id, it->records,
            (std::min)(maximum_bytes_ / 2, 64ull * 1024ull * 1024ull),
            process_run_id_, maximum_bytes_, control_.get());
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - append_began).count();
            std::lock_guard lock(mutex_);
            status_.checkpoint_max_us = (std::max)(status_.checkpoint_max_us,
                static_cast<std::uint64_t>(elapsed));
            status_.checkpoint_total_us += static_cast<std::uint64_t>(elapsed);
            ++status_.checkpoint_count;
        }
        if (result.success) {
            const auto watermark = result.status.last_committed_revision;
            it->committed_revision = watermark;
            it->retry_count = 0;
            it->first_failure = {};
            for (const auto& record : it->records) {
                if (record.revision <= watermark) {
                    it->bytes -= PendingCharge(record);
                    it->payload_bytes -= record.jsonl.size();
                }
            }
            it->records.erase(std::remove_if(it->records.begin(), it->records.end(),
                [watermark](const auto& record) {
                    return record.revision <= watermark;
                }), it->records.end());
            {
                std::lock_guard lock(mutex_);
                status_.checkpoint_revision = watermark;
                const auto commit = std::find_if(status_.confirmed_commits.begin(),
                    status_.confirmed_commits.end(), [&](const auto& v) { return v.session_id == it->id; });
                if (commit != status_.confirmed_commits.end()) commit->revision = watermark;
                else {
                    // Bounded confirmation history, newest sessions retained.
                    if (status_.confirmed_commits.size() == 256) status_.confirmed_commits.erase(status_.confirmed_commits.begin());
                    status_.confirmed_commits.push_back({it->id, it->generation, watermark});
                }
                status_.availability = Availability::Valid;
                status_.reason = "checkpoint_committed";
                const auto& verified = result.status;
                const auto id = result.report_directory.filename().string();
                const auto entry = std::find_if(status_.reports.begin(), status_.reports.end(),
                    [&](const auto& report) { return report.record_id == id; });
                const TelemetryReportEntry updated{id, verified.created_utc_ms,
                    verified.size_bytes, verified.record_count, verified.session_complete};
                if (entry == status_.reports.end()) status_.reports.push_back(updated);
                else *entry = updated;
                std::sort(status_.reports.begin(), status_.reports.end(),
                    [](const auto& a, const auto& b) { return a.created_utc_ms > b.created_utc_ms; });
                PublishStatusLocked();
            }
            if (it->terminal && it->records.empty()) {
                it = pending_reports_.erase(it);
                continue;
            }
            it->next_attempt = now + std::chrono::seconds(15);
        } else {
            ++it->retry_count;
            if (it->first_failure == std::chrono::steady_clock::time_point{})
                it->first_failure = now;
            constexpr std::array delays{1, 5, 30, 60};
            const auto delay = delays[(std::min)(it->retry_count - 1,
                static_cast<unsigned>(delays.size() - 1))];
            it->next_attempt = now + std::chrono::seconds(delay);
            if (now - it->first_failure >= std::chrono::minutes(5))
                it->next_attempt = std::chrono::steady_clock::time_point::max();
            std::lock_guard lock(mutex_);
            ++status_.write_failures;
            status_.availability = Availability::Invalid;
            status_.reason = "met_06_" + result.reason;
            unconfirmed_commit_ = unconfirmed_commit_ || result.manifest_replaced;
            PublishStatusLocked();
        }
        ++it;
    }
    PublishPendingStatus();
}

void TelemetryHistoryStore::WorkerContext::HandleExport(Job job) {
    TelemetryExportResult result;
    try {
        if (job.kind == JobKind::ExportReport) {
            std::optional<TelemetryReportEntry> selected;
            {
                std::lock_guard lock(mutex_);
                const auto found = std::find_if(status_.reports.begin(),
                    status_.reports.end(), [&](const auto& entry) {
                        return entry.record_id == job.record_id;
                    });
                if (found != status_.reports.end()) selected = *found;
            }
            if (selected) result = WriteTelemetryDiagnosticBundle(
                root_, *selected, diagnostic_root_, job.destination,
                job.cancelled, {job.first_utc_ms, job.last_utc_ms});
            else result.reason = "report_not_indexed";
        } else {
            std::vector<SafeTelemetryRecordPtr> records;
            {
                std::lock_guard lock(mutex_);
                records.reserve(current_records_.size());
                for (const auto& item : current_records_)
                    records.push_back(item.record);
            }
            result = WriteTelemetryReportAtomically(
                records, job.destination, false, job.cancelled);
        }
    } catch (...) {
        result.reason = "export_exception";
    }
    try {
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
    } catch (...) {}
    try { if (!Stopped() && job.export_callback) job.export_callback(std::move(result)); }
    catch (...) {}
    {
        std::lock_guard lock(mutex_);
        if (export_jobs_ != 0) --export_jobs_;
    }
}

void TelemetryHistoryStore::WorkerContext::HandleClear(Job job) {
    bool removed = false;
    std::string reason = "report_not_removed";
    try {
        bool indexed = false;
        {
            std::lock_guard lock(mutex_);
            indexed = std::any_of(status_.reports.begin(), status_.reports.end(),
                [&](const auto& entry) { return entry.record_id == job.record_id; });
        }
        if (indexed) {
            removed = job.record_id.starts_with("cohavora-telemetry-v2-")
                ? RemoveTelemetryCheckpoint(root_ / job.record_id)
                : RemoveKnownReport(root_ / job.record_id);
            if (removed) reason = "report_cleared";
        }
        RefreshAndPruneReports();
    } catch (...) {
        reason = "clear_exception";
    }
    try { if (!Stopped() && job.mutation_callback) job.mutation_callback(removed, reason); }
    catch (...) {}
}

void TelemetryHistoryStore::WorkerContext::HandleRetry(Job job) {
    bool success = false;
    std::string reason = "report_not_pending";
    try {
        const auto found = std::find_if(pending_reports_.begin(),
            pending_reports_.end(), [&](const auto& report) {
                return report.id == job.record_id && !report.records.empty();
            });
        if (found != pending_reports_.end()) {
            found->retry_count = 0;
            found->first_failure = {};
            found->next_attempt = std::chrono::steady_clock::now();
            FlushPendingReports(false);
            const auto remaining = std::find_if(pending_reports_.begin(),
                pending_reports_.end(), [&](const auto& report) {
                    return report.id == job.record_id;
                });
            success = remaining == pending_reports_.end() || remaining->records.empty();
            reason = success ? "checkpoint_committed" : "checkpoint_retry_failed";
        }
    } catch (...) {
        reason = "retry_exception";
    }
    try { if (!Stopped() && job.mutation_callback) job.mutation_callback(success, reason); }
    catch (...) {}
}

void TelemetryHistoryStore::WorkerContext::RefreshAndPruneReports(
    std::uint64_t reserve_bytes) {
    if (Stopped()) return;
    const auto refresh_began = std::chrono::steady_clock::now();
    next_history_refresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    struct Candidate {
        TelemetryReportEntry entry;
        std::filesystem::path path;
        bool active_run = false;
    };
    struct CorruptCandidate {
        std::filesystem::path path;
        std::uint64_t bytes = 0;
        std::filesystem::file_time_type modified{};
    };
    struct LegacyTemporary {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
    };
    std::vector<Candidate> candidates;
    std::vector<CorruptCandidate> corrupt_candidates;
    std::vector<LegacyTemporary> legacy_temporary;
    std::uint64_t corrupt = 0;
    std::uint64_t unsupported = 0;
    std::uint64_t corrupt_bytes = 0;
    std::uint64_t corrupt_pruned = 0;
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (!error) {
        for (std::filesystem::directory_iterator it(root_, error), end;
             !error && it != end; it.increment(error)) {
            if (Stopped()) return;
            if (it->is_symlink(error) || error) {
                error.clear();
                continue;
            }
            if (!it->is_directory(error) || error) {
                error.clear();
                continue;
            }
            const auto id = it->path().filename().string();
            if (IsLegacyTemporaryDirectory(id)) {
                const auto modified = it->last_write_time(error);
                if (!error) legacy_temporary.push_back({it->path(), modified});
                error.clear();
                continue;
            }
            if (id.starts_with("cohavora-telemetry-v2-")) {
                PruneTelemetryCheckpointOrphans(it->path());
                const auto lock_path = it->path() / "writer.lock";
                const bool has_lock = std::filesystem::exists(lock_path, error);
                if (error) {
                    error.clear();
                    continue;
                }
                TelemetryCheckpointReadLease reader(it->path());
                if (has_lock && !reader.acquired()) continue;
                const auto inspected = InspectTelemetryCheckpoint(it->path());
                if (!inspected.valid) {
                    if (inspected.reason == "unsupported_version")
                        ++unsupported;
                    else {
                        const auto bytes = TelemetryCheckpointArtifactBytes(it->path());
                        if (bytes == 0) continue;
                        ++corrupt;
                        const auto modified = it->last_write_time(error);
                        if (error) error.clear();
                        corrupt_candidates.push_back({it->path(), bytes, modified});
                        corrupt_bytes += bytes;
                    }
                    continue;
                }
                TelemetryReportEntry entry;
                entry.record_id = id;
                entry.created_utc_ms = inspected.created_utc_ms;
                entry.record_count = inspected.record_count;
                entry.complete = inspected.session_complete;
                entry.size_bytes = inspected.size_bytes;
                const bool active_run = !entry.complete &&
                    IsTelemetryRunActive(root_, inspected.process_run_id);
                candidates.push_back({std::move(entry), it->path(), active_run});
                continue;
            }
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
    std::sort(corrupt_candidates.begin(), corrupt_candidates.end(),
        [](const auto& a, const auto& b) { return a.modified < b.modified; });
    constexpr std::uint64_t kCorruptBudget = 8ull * 1024ull * 1024ull;
    const auto corrupt_cutoff = std::filesystem::file_time_type::clock::now() -
        retention_;
    for (const auto& item : legacy_temporary) {
        if (Stopped()) return;
        if (item.modified < corrupt_cutoff)
            RemoveKnownReport(item.path);
    }
    for (const auto& item : corrupt_candidates) {
        if (Stopped()) return;
        if (corrupt_bytes <= kCorruptBudget &&
            item.modified >= corrupt_cutoff) break;
        if (DiscardCorruptTelemetryCheckpointArtifacts(item.path)) {
            corrupt_bytes -= (std::min)(corrupt_bytes, item.bytes);
            ++corrupt_pruned;
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.entry.created_utc_ms < b.entry.created_utc_ms;
    });
    const auto owned_bytes = TelemetryHistoryOwnedBytes(root_);
    std::uint64_t total = owned_bytes.value_or(0);
    bool accounting_failed = !owned_bytes;
    const auto cutoff = UtcNowMs() -
        std::chrono::duration_cast<std::chrono::milliseconds>(retention_).count();
    for (auto& item : candidates) {
        if (Stopped()) return;
        if (accounting_failed) break;
        // An in-flight session owns its checkpoint even when its first sample
        // predates the retention window; per-report rolling bounds its size.
        if (item.active_run ||
            std::any_of(pending_reports_.begin(), pending_reports_.end(),
                [&](const PendingReport& pending) {
                    return item.entry.record_id ==
                        "cohavora-telemetry-v2-" + pending.id;
                })) continue;
        if (item.entry.created_utc_ms >= cutoff &&
            total <= maximum_bytes_ &&
            reserve_bytes <= maximum_bytes_ - total) continue;
        const bool removed = item.entry.record_id.starts_with("cohavora-telemetry-v2-")
            ? RemoveTelemetryCheckpoint(item.path)
            : RemoveKnownReport(item.path);
        if (removed) {
            // Other runs may have written since the scan, and the index size
            // need not equal the owned bytes actually removed.
            const auto remaining = TelemetryHistoryOwnedBytes(root_);
            if (!remaining) accounting_failed = true;
            else total = *remaining;
            item.entry.record_id.clear();
        }
    }
    std::lock_guard lock(mutex_);
    status_.reports.clear();
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (!it->entry.record_id.empty()) status_.reports.push_back(it->entry);
    }
    status_.corrupt_reports = corrupt;
    status_.unsupported_reports = unsupported;
    status_.corrupt_artifacts_pruned += corrupt_pruned;
    ++status_.history_refresh_count;
    status_.history_refresh_last_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - refresh_began).count());
    status_.history_refresh_max_us = (std::max)(status_.history_refresh_max_us,
        status_.history_refresh_last_us);
    status_.history_refresh_total_us += status_.history_refresh_last_us;
    if (error || accounting_failed) {
        ++status_.write_failures;
        status_.availability = Availability::Invalid;
        status_.reason = "met_06_history_scan_failed";
    } else if (status_.availability == Availability::WarmingUp) {
        status_.availability = Availability::Valid;
        status_.reason = "bounded_history_valid";
    }
    PublishStatusLocked();
}

void TelemetryHistoryStore::WorkerContext::PublishStatusLocked() {
    if (status_frozen_) return;
    try {
        std::atomic_store_explicit(
            &status_cache_,
            std::make_shared<const TelemetryStoreStatus>(status_),
            std::memory_order_release);
    } catch (...) {}
}

std::string TelemetryHistoryStore::WorkerContext::NewOpaqueId() {
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

TelemetryHistoryStore::TelemetryHistoryStore(std::filesystem::path root,
    std::size_t memory_buckets, std::size_t queue_capacity, std::uint64_t maximum_bytes,
    std::chrono::hours retention, std::size_t queue_byte_capacity,
    std::string process_run_id, std::filesystem::path diagnostic_root)
    : control_(std::make_shared<TelemetryCheckpointControl>()),
      context_(std::make_shared<WorkerContext>(std::move(root), memory_buckets,
          queue_capacity, maximum_bytes, retention, queue_byte_capacity,
          std::move(process_run_id), std::move(diagnostic_root), control_)),
      worker_([context = context_] { context->Run(); }) {}
TelemetryHistoryStore::TelemetryHistoryStore(std::filesystem::path root,
    std::string run, std::filesystem::path diagnostics)
    : TelemetryHistoryStore(std::move(root), kDefaultMemoryBuckets, kDefaultQueueCapacity,
        kDefaultMaximumBytes, kDefaultRetention, kDefaultQueueBytes,
        std::move(run), std::move(diagnostics)) {}
TelemetryHistoryStore::~TelemetryHistoryStore() { Close(); }

void TelemetryHistoryStore::WorkerContext::Run() {
    try { RunLoop(); }
    catch (...) {
        std::lock_guard lock(mutex_);
        ++status_.write_failures;
        status_.availability = Availability::Invalid;
        worker_failed_ = true;
        status_.reason = "met_06_worker_exception";
        PublishStatusLocked();
    }
    // Lease destruction belongs to this worker even after owner timeout/destruction.
    run_lease_.reset();
    {
        std::lock_guard lock(mutex_);
        // Include aggregate timings from the final forced flush before Close
        // captures and freezes the immutable completion status.
        PublishStatusLocked();
        finished_ = true;
    }
    done_.notify_all();
}

TelemetryCloseResult TelemetryHistoryStore::Close(std::chrono::milliseconds budget) {
    std::lock_guard closing(close_mutex_);
    if (closed_) return close_result_;
    const auto& c = context_;
    std::unique_lock lock(c->mutex_);
    c->stopping_ = true;
    c->condition_.notify_all();
    const bool finished = c->done_.wait_for(lock, (std::max)(budget, std::chrono::milliseconds::zero()),
        [&] { return c->finished_; });
    if (!finished) {
        control_->abandoned.store(true, std::memory_order_release);
        c->status_.reason = "history_close_timed_out";
        c->status_.availability = Availability::Invalid;
        c->PublishStatusLocked();
        close_result_.state = TelemetryCloseState::TimedOut;
        close_result_.disk_outcome_unknown = true;
    } else {
        close_result_.state = c->worker_failed_ || c->status_.pending_records ||
            c->status_.loss_ranges_pending || c->status_.loss_ranges_inflight || c->status_.queue_depth || c->status_.inflight_jobs ||
            c->status_.queue_drops || c->status_.pending_records_dropped || c->status_.loss_ranges_omitted
            ? TelemetryCloseState::Failed : TelemetryCloseState::Completed;
        // Failure after replacement cannot be reported as proof of no disk commit.
        close_result_.disk_outcome_unknown = close_result_.state == TelemetryCloseState::Failed &&
            c->unconfirmed_commit_;
    }
    close_result_.status = c->Status();
    c->status_frozen_ = true;
    closed_ = true;
    lock.unlock();
    if (finished) worker_.join();
    else { c->condition_.notify_all(); worker_.detach(); }
    return close_result_;
}

bool TelemetryHistoryStore::SubmitSnapshot(SessionTelemetry::SnapshotPtr s, StabilitySummary b, std::string id) {
    return context_->SubmitSnapshot(std::move(s), std::move(b), std::move(id));
}
bool TelemetryHistoryStore::ExportCurrent(std::filesystem::path p, ExportCallback f, std::shared_ptr<std::atomic_bool> c) {
    return context_->ExportCurrent(std::move(p), std::move(f), std::move(c));
}
bool TelemetryHistoryStore::ExportReport(std::string id, std::filesystem::path p, ExportCallback f,
    std::shared_ptr<std::atomic_bool> c, std::int64_t first, std::int64_t last) {
    return context_->ExportReport(std::move(id), std::move(p), std::move(f), std::move(c), first, last);
}
bool TelemetryHistoryStore::ClearReport(std::string id, MutationCallback f) { return context_->ClearReport(std::move(id), std::move(f)); }
bool TelemetryHistoryStore::RetryCheckpoint(std::string id, MutationCallback f) { return context_->RetryCheckpoint(std::move(id), std::move(f)); }
void TelemetryHistoryStore::SetHistoryEnabled(bool enabled) { context_->SetHistoryEnabled(enabled); }
std::shared_ptr<const TelemetryStoreStatus> TelemetryHistoryStore::Status() const { return context_->Status(); }
std::vector<SafeTelemetryRecordPtr> TelemetryHistoryStore::CurrentRecords() const { return context_->CurrentRecords(); }

void InstallTelemetryHistoryStore(std::shared_ptr<TelemetryHistoryStore> store) {
    std::lock_guard lock(g_installed_mutex);
    g_installed_store = std::move(store);
}

std::shared_ptr<TelemetryHistoryStore> InstalledTelemetryHistoryStore() {
    std::lock_guard lock(g_installed_mutex);
    return g_installed_store.lock();
}

} // namespace livekit::telemetry
