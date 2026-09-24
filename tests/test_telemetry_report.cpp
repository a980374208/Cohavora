#include "src/telemetry/telemetry_report.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using livekit::telemetry::Availability;
using livekit::telemetry::ProductChainStatus;
using livekit::telemetry::SafeTelemetryRecord;
using livekit::telemetry::SafeTelemetryRecordPtr;
using livekit::telemetry::TelemetryHistoryStore;
using livekit::telemetry::WriteTelemetryReportAtomically;

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(std::string name) {
        path_ = std::filesystem::temp_directory_path() /
            (std::move(name) + "-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

SafeTelemetryRecordPtr Record(
    std::uint64_t revision,
    bool complete,
    std::int64_t captured_utc_ms = 1000) {
    auto record = std::make_shared<SafeTelemetryRecord>();
    record->anonymous_session_id = "0123456789abcdef0123456789abcdef";
    record->captured_utc_ms = captured_utc_ms;
    record->complete = complete;
    record->snapshot.session_generation = 42;
    record->snapshot.revision = revision;
    record->snapshot.session_complete = complete;
    record->snapshot.availability = Availability::Valid;
    record->snapshot.reason = "stats_complete";
    record->snapshot.coverage = 1.0;
    record->snapshot.process_cpu_percent = 0.0;
    record->snapshot.cpu_availability = Availability::Valid;
    record->snapshot.cpu_reason = "process_cpu_sample_valid";
    record->snapshot.last_stats_request_ms = -1;
    record->snapshot.session_duration_availability = Availability::Valid;
    record->snapshot.session_duration_reason = "session_duration_complete";
    record->snapshot.session_duration_ms = 4321;
    record->snapshot.usable_duration_availability = Availability::Valid;
    record->snapshot.usable_duration_reason = "room_usable_duration_complete";
    record->snapshot.usable_duration_ms = 4000;
    record->snapshot.admission_to_usable_availability = Availability::Valid;
    record->snapshot.admission_to_usable_reason =
        "admission_to_startup_terminal_valid";
    record->snapshot.admission_to_usable_ms = 700;
    record->snapshot.metric_product_chains.push_back({
        "SES-03", ProductChainStatus::Implemented,
        "admission_to_startup_terminal_chain_implemented"});
    record->snapshot.local_publish_media_availability = Availability::Valid;
    record->snapshot.local_publish_media_reason =
        "all_expected_local_publications_sending";
    record->snapshot.local_publications = 1;
    record->snapshot.local_publish_no_media = 0;
    record->snapshot.local_video_injection_availability = Availability::Valid;
    record->snapshot.local_video_injection_reason =
        "local_video_injection_observed";
    record->snapshot.local_video_first_injections = 1;
    record->snapshot.last_publish_to_video_injection_ms = 12;
    record->snapshot.local_video_encode_availability = Availability::Unknown;
    record->snapshot.local_video_encode_reason =
        "outbound_video_mapping_unavailable";
    record->snapshot.last_publish_to_video_encode_ms = -1;
    record->snapshot.local_rtp_send_availability = Availability::Valid;
    record->snapshot.local_rtp_send_reason = "local_rtp_send_observed";
    record->snapshot.local_first_rtp_sends = 1;
    record->snapshot.last_publish_to_rtp_send_ms = 44;
    record->snapshot.subscription_media_availability = Availability::Valid;
    record->snapshot.subscription_media_reason =
        "all_expected_subscriptions_delivered_media";
    record->snapshot.expected_remote_subscriptions = 2;
    record->snapshot.delivered_remote_subscriptions = 2;
    record->snapshot.remote_subscription_no_media = 0;
    record->snapshot.longest_subscription_media_wait_ms = 52;
    record->snapshot.render_stall_availability = Availability::Valid;
    record->snapshot.render_stall_reason = "render_window_valid";
    record->snapshot.render_stall_algorithm = "render-stall-v1";
    record->snapshot.render_stall_count = 0;
    record->snapshot.render_stall_duration_ms = 0;
    record->snapshot.render_stall_ratio = 0.0;
    record->snapshot.last_admission_to_first_render_ms = 88;
    record->snapshot.inbound_rtp_traffic_availability = Availability::Valid;
    record->snapshot.inbound_rtp_traffic_reason =
        "inbound_rtp_bitrate_window_valid";
    record->snapshot.inbound_rtp_bitrate_bps = 800000.0;
    record->snapshot.window_inbound_rtp_bytes = 10000;
    record->snapshot.outbound_rtp_traffic_availability = Availability::Unsupported;
    record->snapshot.outbound_rtp_traffic_reason = "outbound_rtp_bytes_missing";
    record->snapshot.outbound_rtp_bitrate_bps = 123456.0;
    record->snapshot.inbound_packet_loss_availability = Availability::Invalid;
    record->snapshot.inbound_packet_loss_reason =
        "inbound_loss_late_packet_correction";
    record->snapshot.inbound_packets_lost = 6;
    record->snapshot.window_inbound_packets_lost = -1;
    record->snapshot.window_inbound_packets_received = 100;
    record->snapshot.inbound_packet_loss_ratio = -1.0;
    record->snapshot.inbound_jitter_availability = Availability::Valid;
    record->snapshot.inbound_jitter_reason = "inbound_jitter_current_valid";
    record->snapshot.inbound_jitter_max_ms = 4.0;
    record->snapshot.remote_rtcp_availability = Availability::Valid;
    record->snapshot.remote_rtcp_reason = "remote_rtcp_feedback_valid";
    record->snapshot.remote_rtcp_current_rtt_max_ms = 40.0;
    record->snapshot.remote_rtcp_window_average_rtt_ms = 250.0;
    record->snapshot.remote_rtcp_fraction_lost_max = 0.02;
    record->snapshot.network_recovery_availability = Availability::Valid;
    record->snapshot.network_recovery_reason = "recovery_counter_window_valid";
    record->snapshot.window_inbound_packets = 100;
    record->snapshot.window_inbound_retransmitted_packets = 2;
    record->snapshot.inbound_retransmitted_packet_ratio = 0.02;
    record->snapshot.media_path_availability = Availability::Valid;
    record->snapshot.media_path_reason = "selected_media_path_valid";
    record->snapshot.selected_media_transports = 1;
    record->snapshot.local_candidate_types = "relay";
    record->snapshot.media_protocols = "udp";
    record->snapshot.media_path_rtt_availability = Availability::Valid;
    record->snapshot.media_path_rtt_reason = "selected_media_path_rtt_valid";
    record->snapshot.media_path_rtt_max_ms = 25.0;
    record->snapshot.media_bandwidth_availability = Availability::Valid;
    record->snapshot.media_bandwidth_reason =
        "selected_media_path_bandwidth_valid";
    record->snapshot.media_available_outgoing_bitrate_bps = 2500000.0;
    record->snapshot.transport_traffic_availability = Availability::Valid;
    record->snapshot.transport_traffic_reason = "transport_traffic_window_valid";
    record->snapshot.transport_stats_count = 1;
    record->snapshot.window_transport_bytes_sent = 20000;
    record->snapshot.window_transport_bytes_received = 30000;
    record->snapshot.transport_state_availability = Availability::Valid;
    record->snapshot.transport_state_reason = "transport_state_valid";
    record->snapshot.transport_dtls_states = "connected";
    record->snapshot.video_quality_limitation_availability = Availability::Valid;
    record->snapshot.video_quality_limitation_reason =
        "quality_limitation_native_window_valid";
    record->snapshot.video_quality_limitation_current = "bandwidth";
    record->snapshot.window_video_quality_bandwidth_duration_ms = 250;
    record->snapshot.local_device_continuity_availability = Availability::Valid;
    record->snapshot.local_device_continuity_reason =
        "local_device_continuity_valid";
    record->snapshot.local_device_format_changes = 1;
    record->snapshot.render_stage_availability = Availability::Valid;
    record->snapshot.render_stage_reason = "render_cpu_stage_spans_valid";
    record->snapshot.render_convert_samples = 2;
    record->snapshot.render_convert_total_us = 100;
    record->snapshot.render_convert_max_us = 60;
    record->snapshot.render_draw_samples = 3;
    record->snapshot.render_draw_total_us = 240;
    record->snapshot.render_draw_max_us = 110;
    record->snapshot.render_present_block_samples = 2;
    record->snapshot.render_present_block_total_us = 160;
    record->snapshot.render_present_block_max_us = 90;
    record->snapshot.video_policy_availability = Availability::Valid;
    record->snapshot.video_policy_reason = "video_policy_snapshot_valid";
    record->snapshot.video_policy_coordinator_session = 42;
    record->snapshot.video_policy_native_room_generation = 8;
    record->snapshot.video_policy_catalog_revision = 12;
    record->snapshot.video_policy_revision = 4;
    record->snapshot.video_policy_stage_content = "video";
    record->snapshot.video_policy_selection_reason = "visible";
    record->snapshot.video_policy_requested = 4;
    record->snapshot.video_policy_selected = 3;
    record->snapshot.video_policy_actual = 2;
    record->snapshot.video_policy_bound = 1;
    record->snapshot.video_policy_selected_not_actual = 1;
    record->snapshot.video_policy_selected_not_bound = 2;
    record->snapshot.telemetry_cost_availability = Availability::Valid;
    record->snapshot.telemetry_cost_reason = "observed_sampler_snapshot_cost_valid";
    record->snapshot.reconnect_density_availability = Availability::Valid;
    record->snapshot.reconnect_density_reason =
        "reconnect_episode_density_valid";
    record->snapshot.reconnect_episodes = 1;
    record->snapshot.reconnect_episodes_per_hour = 0.5;
    record->snapshot.stability_anomaly_density_availability =
        Availability::Valid;
    record->snapshot.stability_anomaly_density_reason =
        "typed_anomaly_density_valid";
    record->snapshot.stability_operation_failures = 1;
    record->snapshot.stability_sampler_interruptions = 2;
    record->snapshot.stability_device_stops = 3;
    record->snapshot.stability_media_failures = 4;
    record->snapshot.stability_anomalies = 10;
    record->snapshot.stability_anomalies_per_hour = 5.0;
    record->stability.ledger_availability = "VALID";
    record->stability.ledger_reason = "atomic_bounded_ledger_valid";
    return record;
}

std::filesystem::path OnlyReportDirectory(const std::filesystem::path& root) {
    std::filesystem::path result;
    for (const auto& item : std::filesystem::directory_iterator(root)) {
        if (item.is_directory() &&
            item.path().filename().string().starts_with("cohavora-telemetry")) {
            TEST_CHECK(result.empty());
            result = item.path();
        }
    }
    TEST_CHECK(!result.empty());
    return result;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

nlohmann::json FindMetric(const std::string& jsonl, const std::string& key) {
    std::istringstream input(jsonl);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        auto row = nlohmann::json::parse(line);
        if (row.value("key", std::string{}) == key) return row;
    }
    TEST_CHECK(false && "metric key not found");
    return {};
}

void JsonCsvShareValuesAndPreserveMissing() {
    TemporaryDirectory directory("cohavora-telemetry-report");
    std::vector<SafeTelemetryRecordPtr> records{
        Record(1, false, 1000), Record(2, true, 2000)};
    const auto result = WriteTelemetryReportAtomically(
        records, directory.path(), false);
    TEST_CHECK(result.success);
    TEST_CHECK(std::filesystem::exists(result.report_directory / "manifest.json"));
    TEST_CHECK(std::filesystem::exists(result.report_directory / "session.json"));
    TEST_CHECK(std::filesystem::exists(result.report_directory / "metrics.jsonl"));
    TEST_CHECK(std::filesystem::exists(result.report_directory / "metrics.csv"));

    nlohmann::json manifest;
    std::ifstream(result.report_directory / "manifest.json") >> manifest;
    TEST_CHECK(manifest.at("schema") == "cohavora-telemetry-report");
    TEST_CHECK(manifest.at("record_count") == 2);
    TEST_CHECK(manifest.at("session_complete") == true);
    TEST_CHECK(manifest.at("integrity").at("status") == "complete");

    const auto jsonl = ReadAll(result.report_directory / "metrics.jsonl");
    const auto csv = ReadAll(result.report_directory / "metrics.csv");
    TEST_CHECK(jsonl.find("\"key\":\"resource.cpu\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"stats.last_request_duration\",\"measurement_point\":\"\",\"reason\":\"stats_complete\",\"revision\":2,\"session_generation\":42,\"unit\":\"ms\",\"value\":null") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"render.stall.count\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"session.duration\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "session.admission_to_usable").at("value") ==
               700);
    const auto product_chain = FindMetric(jsonl, "product_chain.SES-03");
    TEST_CHECK(product_chain.at("value") == "IMPLEMENTED_DETERMINISTIC");
    TEST_CHECK(product_chain.at("availability") == "VALID");
    TEST_CHECK(product_chain.at("reason") ==
               "admission_to_startup_terminal_chain_implemented");
    TEST_CHECK(jsonl.find("\"key\":\"publish.video.accepted_to_encode\",\"measurement_point\":\"webrtc_outbound_rtp_frames_encoded_sample\",\"reason\":\"outbound_video_mapping_unavailable\",\"revision\":2,\"session_generation\":42,\"unit\":\"ms\",\"value\":null") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"publish.rtp.accepted_to_send\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "subscribe.media.delivered").at("value") == 2);
    TEST_CHECK(FindMetric(jsonl, "subscribe.media.longest_wait").at("value") ==
               52);
    TEST_CHECK(jsonl.find("\"key\":\"network.inbound.retransmitted_packet_ratio\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"network.path.local_candidate_types\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "network.rtp.inbound.bitrate").at("value") ==
               800000.0);
    TEST_CHECK(FindMetric(jsonl, "network.rtp.outbound.bitrate")
                   .at("value").is_null());
    const auto corrected_loss =
        FindMetric(jsonl, "network.inbound.loss.window");
    TEST_CHECK(corrected_loss.at("availability") == "INVALID");
    TEST_CHECK(corrected_loss.at("value") == -1);
    TEST_CHECK(FindMetric(jsonl, "network.remote_rtcp.window_rtt.average")
                   .at("value") == 250.0);
    TEST_CHECK(FindMetric(jsonl, "network.path.rtt.maximum").at("value") ==
               25.0);
    TEST_CHECK(FindMetric(jsonl, "network.transport.bytes_sent.window")
                   .at("value") == 20000);
    const auto confirmed_crashes =
        FindMetric(jsonl, "stability.confirmed_process_crashes");
    TEST_CHECK(confirmed_crashes.at("availability") == "UNSUPPORTED");
    TEST_CHECK(confirmed_crashes.at("reason") ==
               "crash_evidence_provider_not_configured");
    TEST_CHECK(confirmed_crashes.at("value").is_null());
    const auto confirmed_crash_ratio =
        FindMetric(jsonl, "stability.confirmed_process_crash_ratio");
    TEST_CHECK(confirmed_crash_ratio.at("availability") == "UNSUPPORTED");
    TEST_CHECK(confirmed_crash_ratio.at("value").is_null());
    TEST_CHECK(FindMetric(jsonl, "stability.unknown_process_terminations")
                   .at("availability") == "WARMING_UP");
    TEST_CHECK(jsonl.find("\"key\":\"video.quality.window.bandwidth\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"video.pipeline.inbound_received\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"video.codec.inbound\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"video.processing.decode_average\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"device.local.format_changes\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"device.open\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"device.hotplug\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"render.stage.convert.maximum\"") != std::string::npos);
    const auto check_stage_metric = [&jsonl](
            const char* key, std::int64_t expected, const char* measurement_point) {
        const auto metric = FindMetric(jsonl, key);
        TEST_CHECK(metric.at("value") == expected);
        TEST_CHECK(metric.at("measurement_point") == measurement_point);
        TEST_CHECK(metric.at("availability") == "VALID");
    };
    constexpr auto draw_point = "qt_tile_paint_or_canvas_draw_cpu_span_v2";
    constexpr auto present_point = "canvas_gl_swap_or_dx11_render_present_cpu_span_v2";
    check_stage_metric("render.stage.draw.samples", 3, draw_point);
    check_stage_metric("render.stage.draw.total", 240, draw_point);
    check_stage_metric("render.stage.draw.maximum", 110, draw_point);
    check_stage_metric("render.stage.present_block.samples", 2, present_point);
    check_stage_metric("render.stage.present_block.total", 160, present_point);
    check_stage_metric("render.stage.present_block.maximum", 90, present_point);
    for (const char* key : {
            "render.stage.convert.samples", "render.stage.convert.total",
            "render.stage.convert.maximum", "render.stage.upload.samples",
            "render.stage.upload.total", "render.stage.upload.maximum"}) {
        TEST_CHECK(FindMetric(jsonl, key).at("measurement_point") ==
                   "render_cpu_submission_spans");
    }
    TEST_CHECK(jsonl.find("\"key\":\"render.stage.gpu_execution\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"render.interval.p99\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "render.admission_to_first").at("value") == 88);
    TEST_CHECK(jsonl.find("\"key\":\"render.frame_age.average\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"render.pipeline.actual_backend\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "video.policy.requested").at("value") == 4);
    TEST_CHECK(FindMetric(jsonl, "video.policy.selected").at("value") == 3);
    TEST_CHECK(FindMetric(jsonl, "video.policy.actual").at("value") == 2);
    TEST_CHECK(FindMetric(jsonl, "video.policy.bound").at("value") == 1);
    TEST_CHECK(FindMetric(jsonl, "video.policy.selected_not_actual").at("value") == 1);
    TEST_CHECK(FindMetric(jsonl, "video.policy.selected_not_bound").at("value") == 2);
    TEST_CHECK(FindMetric(jsonl, "video.policy.revision").at("value") == 4);
    TEST_CHECK(FindMetric(jsonl, "video.policy.stage_content").at("value") == "video");
    TEST_CHECK(FindMetric(jsonl, "video.policy.requested").at("measurement_point") ==
               "session_video_policy_convergence");
    TEST_CHECK(jsonl.find("\"key\":\"resource.internal.native_bindings\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"resource.queue.router.replaced\"") != std::string::npos);
    TEST_CHECK(jsonl.find("\"key\":\"resource.queue.export\"") != std::string::npos);
    TEST_CHECK(FindMetric(jsonl, "reconnect.episodes_per_hour").at("value") ==
               0.5);
    TEST_CHECK(FindMetric(jsonl, "stability.anomaly.total").at("value") == 10);
    TEST_CHECK(FindMetric(jsonl, "stability.anomaly.per_hour").at("value") ==
               5.0);
    TEST_CHECK(csv.find("\"resource.cpu\",\"0\",\"percent\",\"VALID\"") != std::string::npos);
    TEST_CHECK(csv.find("\"stats.last_request_duration\",\"\",\"ms\",\"VALID\"") != std::string::npos);
    TEST_CHECK(csv.find("\"session.duration\",\"4321\",\"ms\",\"VALID\"") != std::string::npos);
}

void UnsafeTextAndCsvFormulaAreContained() {
    TemporaryDirectory directory("cohavora-telemetry-security");
    auto unsafe = std::make_shared<SafeTelemetryRecord>(*Record(7, true));
    unsafe->snapshot.reason =
        "https://conference.invalid/join?token=super-secret-token";
    unsafe->snapshot.render_stall_algorithm = "=1+1";
    unsafe->snapshot.audio_quality_reason = "candidate:1 1 udp 1 10.0.0.7";
    const auto result = WriteTelemetryReportAtomically(
        {unsafe}, directory.path(), false);
    TEST_CHECK(result.success);
    const auto session = ReadAll(result.report_directory / "session.json");
    const auto jsonl = ReadAll(result.report_directory / "metrics.jsonl");
    const auto csv = ReadAll(result.report_directory / "metrics.csv");
    for (const auto* secret : {
             "super-secret-token", "10.0.0.7", "https://conference.invalid"}) {
        TEST_CHECK(session.find(secret) == std::string::npos);
        TEST_CHECK(jsonl.find(secret) == std::string::npos);
        TEST_CHECK(csv.find(secret) == std::string::npos);
    }
    TEST_CHECK(csv.find("\"'=1+1\"") != std::string::npos);
    TEST_CHECK(csv.find("\"=1+1\"") == std::string::npos);
}

void CancellationAndUnwritableDestinationAreBounded() {
    TemporaryDirectory directory("cohavora-telemetry-failure");
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    auto result = WriteTelemetryReportAtomically(
        {Record(1, true)}, directory.path(), false, cancelled);
    TEST_CHECK(!result.success);
    TEST_CHECK(result.cancelled);
    TEST_CHECK(result.reason == "export_cancelled");

    const auto file = directory.path() / "not-a-directory";
    std::ofstream(file) << "occupied";
    result = WriteTelemetryReportAtomically(
        {Record(2, true)}, file, false);
    TEST_CHECK(!result.success);
    TEST_CHECK(!result.reason.empty());

    auto incompatible = std::make_shared<SafeTelemetryRecord>(*Record(3, true));
    incompatible->definition_version = 2;
    result = WriteTelemetryReportAtomically(
        {Record(2, false), incompatible}, directory.path(), false);
    TEST_CHECK(!result.success);
    TEST_CHECK(result.reason == "incompatible_record_set");
}

void StoreCoalescesBucketsAndPreservesUnknownFiles() {
    TemporaryDirectory directory("cohavora-telemetry-store");
    const auto seed = WriteTelemetryReportAtomically(
        {Record(1, true)}, directory.path(), true);
    TEST_CHECK(seed.success);
    const auto unknown = seed.report_directory / "user-owned.bin";
    std::ofstream(unknown) << "keep";

    {
        TelemetryHistoryStore store(
            directory.path(), 3, 16, 1024 * 1024, std::chrono::hours(24 * 7));
        for (std::uint64_t revision = 1; revision <= 8; ++revision) {
            auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
                Record(revision, revision == 8)->snapshot);
            TEST_CHECK(store.SubmitSnapshot(std::move(snapshot)));
        }
        for (int i = 0; i != 100; ++i) {
            const auto status = store.Status();
            if (status->snapshots_accepted == 8) break;
            std::this_thread::sleep_for(10ms);
        }
        const auto status = store.Status();
        TEST_CHECK(status->snapshots_accepted == 8);
        TEST_CHECK(status->one_second_buckets_coalesced >= 7);
        TEST_CHECK(status->memory_records <= 3);
        store.SetHistoryEnabled(false);
        for (int i = 0; i != 100; ++i) {
            const auto disabled = store.Status();
            if (!disabled->history_enabled && disabled->reports.empty()) break;
            std::this_thread::sleep_for(10ms);
        }
        const auto disabled = store.Status();
        TEST_CHECK(!disabled->history_enabled);
        TEST_CHECK(disabled->reports.empty());
        TEST_CHECK(disabled->memory_records == 0);
    }
    TEST_CHECK(std::filesystem::exists(unknown));
    TEST_CHECK(!std::filesystem::exists(seed.report_directory / "manifest.json"));
}

} // namespace

int main() {
    JsonCsvShareValuesAndPreserveMissing();
    UnsafeTextAndCsvFormulaAreContained();
    CancellationAndUnwritableDestinationAreBounded();
    StoreCoalescesBucketsAndPreservesUnknownFiles();
    return 0;
}
