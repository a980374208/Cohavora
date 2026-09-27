#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/telemetry_checkpoint.h"
#include "src/telemetry/build_identity.h"
#include "src/telemetry/diagnostic_bundle.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;
using livekit::telemetry::Availability;
using livekit::telemetry::ProductChainStatus;
using livekit::telemetry::SafeTelemetryRecord;
using livekit::telemetry::SafeTelemetryRecordPtr;
using livekit::telemetry::TelemetryHistoryStore;
using livekit::telemetry::AppendTelemetryCheckpoint;
using livekit::telemetry::InspectTelemetryCheckpoint;
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

std::string Sha256(const std::string& content) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
    SHA256(reinterpret_cast<const unsigned char*>(content.data()),
        content.size(), bytes.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
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
    unsafe->snapshot.metric_product_chains.push_back({
        "privatecanary", ProductChainStatus::Implemented, "privatecanary"});
    const auto result = WriteTelemetryReportAtomically(
        {unsafe}, directory.path(), false);
    TEST_CHECK(result.success);
    const auto session = ReadAll(result.report_directory / "session.json");
    const auto jsonl = ReadAll(result.report_directory / "metrics.jsonl");
    const auto csv = ReadAll(result.report_directory / "metrics.csv");
    const auto checkpoint = livekit::telemetry::
        SerializeSafeTelemetryCheckpointRecord(*unsafe);
    for (const auto* secret : {
             "super-secret-token", "10.0.0.7", "https://conference.invalid",
             "privatecanary"}) {
        TEST_CHECK(session.find(secret) == std::string::npos);
        TEST_CHECK(jsonl.find(secret) == std::string::npos);
        TEST_CHECK(csv.find(secret) == std::string::npos);
        TEST_CHECK(checkpoint.find(secret) == std::string::npos);
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
        const auto source_at = livekit::telemetry::Snapshot::Clock::now() -
            std::chrono::seconds(3);
        for (std::uint64_t revision = 1; revision <= 8; ++revision) {
            auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
                Record(revision, revision == 8)->snapshot);
            snapshot->generated_at = source_at;
            TEST_CHECK(store.SubmitSnapshot(std::move(snapshot)));
        }
        for (int i = 0; i != 100; ++i) {
            const auto status = store.Status();
            if (status->snapshots_accepted == 8 &&
                status->one_second_buckets_coalesced >= 7) break;
            std::this_thread::sleep_for(10ms);
        }
        const auto status = store.Status();
        TEST_CHECK(status->snapshots_accepted == 8);
        TEST_CHECK(status->one_second_buckets_coalesced >= 7);
        TEST_CHECK(status->memory_records <= 3);
        TEST_CHECK(status->memory_bytes <=
            TelemetryHistoryStore::kDefaultMemoryBytes);
        const auto latest = store.CurrentRecords().back();
        const auto wall_now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        TEST_CHECK(latest->source_monotonic_us > 0);
        TEST_CHECK(wall_now - latest->source_utc_ms >= 2500);
        TEST_CHECK(wall_now - latest->source_utc_ms < 5000);
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

void FailedAutomaticHistoryWriteCanRetrySameSession() {
    TemporaryDirectory directory("cohavora-telemetry-retry");
    const auto history_root = directory.path() / "history";
    std::ofstream(history_root) << "blocked";
    TEST_CHECK(std::filesystem::is_regular_file(history_root));

    TelemetryHistoryStore store(history_root, 3, 16, 1024 * 1024,
                                std::chrono::hours(24 * 7));
    const auto terminal = std::make_shared<livekit::telemetry::Snapshot>(
        Record(1, true)->snapshot);
    TEST_CHECK(store.SubmitSnapshot(terminal));
    for (int i = 0; i != 100; ++i) {
        if (store.Status()->write_failures > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->write_failures > 0);
    TEST_CHECK(store.Status()->availability == Availability::Invalid);
    TEST_CHECK(!store.Status()->reason.empty());
    TEST_CHECK(store.Status()->reports.empty());

    TEST_CHECK(std::filesystem::remove(history_root));
    std::filesystem::create_directory(history_root);
    TEST_CHECK(store.SubmitSnapshot(terminal));
    for (int i = 0; i != 100; ++i) {
        if (store.Status()->reports.size() == 1) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->reports.size() == 1);
    TEST_CHECK(store.Status()->reports.front().complete);

    TEST_CHECK(store.SubmitSnapshot(terminal));
    for (int i = 0; i != 100; ++i) {
        if (store.Status()->snapshots_accepted == 3) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->snapshots_accepted == 3);
    store.Close();
    TEST_CHECK(store.Status()->reports.size() == 1);
    std::size_t report_directories = 0;
    for (const auto& entry : std::filesystem::directory_iterator(history_root))
        if (entry.is_directory()) ++report_directories;
    TEST_CHECK(report_directories == 1);
}

void CheckpointAppendRetryAndIntegrity() {
    TemporaryDirectory directory("cohavora-telemetry-checkpoint");
    const auto legacy = WriteTelemetryReportAtomically(
        {Record(1, true)}, directory.path(), true);
    TEST_CHECK(legacy.success);

    const auto first = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false), Record(2, false)});
    if (!first.success) std::fprintf(stderr, "checkpoint: %s\n", first.reason.c_str());
    TEST_CHECK(first.success);
    TEST_CHECK(first.status.last_committed_revision == 2);
    TEST_CHECK(!first.status.session_complete);
    const auto retry = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false), Record(2, false)});
    TEST_CHECK(retry.success);
    TEST_CHECK(retry.status.segment_count == 1);

    const auto terminal = AppendTelemetryCheckpoint(
        directory.path(), {Record(2, false), Record(3, true)});
    TEST_CHECK(terminal.success);
    TEST_CHECK(terminal.status.last_committed_revision == 3);
    TEST_CHECK(terminal.status.segment_count == 2);
    TEST_CHECK(terminal.status.session_complete);
    TEST_CHECK(std::filesystem::exists(legacy.report_directory / "manifest.json"));

    const auto manifest_path = terminal.report_directory / "manifest.json";
    const auto manifest = nlohmann::json::parse(ReadAll(manifest_path));
    const auto segment = terminal.report_directory /
        manifest.at("segments").at(0).at("file").get<std::string>();
    const auto original = ReadAll(segment);
    TEST_CHECK(original.find("\"source_utc_ms\":") != std::string::npos);
    std::ofstream(segment, std::ios::binary | std::ios::trunc) << original.substr(0, 15);
    const auto corrupt = InspectTelemetryCheckpoint(terminal.report_directory);
    TEST_CHECK(!corrupt.valid);
    TEST_CHECK(corrupt.reason == "segment_missing_or_corrupt");
    TEST_CHECK(!AppendTelemetryCheckpoint(directory.path(), {Record(4, true)}).success);
    std::ofstream(segment, std::ios::binary | std::ios::trunc) << original;
    TEST_CHECK(InspectTelemetryCheckpoint(terminal.report_directory).valid);
    std::filesystem::remove(segment);
    TEST_CHECK(!InspectTelemetryCheckpoint(terminal.report_directory).valid);
}

void ManifestReplacementFailureKeepsCommittedRevision() {
    TemporaryDirectory directory("cohavora-telemetry-rename-failure");
    const auto root = directory.path() / "history";
    const auto first = AppendTelemetryCheckpoint(root, {Record(1, false)});
    TEST_CHECK(first.success);
#if defined(_WIN32)
    const auto manifest = first.report_directory / L"manifest.json";
    const HANDLE reader = CreateFileW(manifest.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(reader != INVALID_HANDLE_VALUE);
    const auto blocked = AppendTelemetryCheckpoint(root, {Record(2, false)});
    CloseHandle(reader);
    TEST_CHECK(!blocked.success && blocked.reason == "manifest_commit_failed");
    const auto committed = InspectTelemetryCheckpoint(first.report_directory);
    TEST_CHECK(committed.valid && committed.last_committed_revision == 1);
    const auto retry = AppendTelemetryCheckpoint(root, {Record(2, false)});
    TEST_CHECK(retry.success && retry.status.last_committed_revision == 2);
#endif
}

void PendingSessionsRecoverIndependently() {
    TemporaryDirectory directory("cohavora-telemetry-pending");
    const auto root = directory.path() / "history";
    std::ofstream(root) << "blocked";
    {
        TelemetryHistoryStore store(root, 3, 16, 1024 * 1024,
                                    std::chrono::hours(24 * 7));
        for (std::uint64_t generation : {42ull, 43ull}) {
            auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
                Record(1, true)->snapshot);
            snapshot->session_generation = generation;
            TEST_CHECK(store.SubmitSnapshot(snapshot));
        }
        for (int i = 0; i != 200; ++i) {
            if (store.Status()->snapshots_accepted == 2 &&
                store.Status()->pending_reports == 2) break;
            std::this_thread::sleep_for(10ms);
        }
        TEST_CHECK(store.Status()->pending_reports == 2);
        TEST_CHECK(store.Status()->reports.empty());
        std::filesystem::remove(root);
        std::filesystem::create_directory(root);
        for (int i = 0; i != 500; ++i) {
            if (store.Status()->reports.size() == 2) break;
            std::this_thread::sleep_for(10ms);
        }
        TEST_CHECK(store.Status()->reports.size() == 2);
        TEST_CHECK(store.Status()->pending_reports == 0);
        TEST_CHECK(store.Status()->reports[0].record_id !=
                   store.Status()->reports[1].record_id);
        store.Close();
    }
    {
        TelemetryHistoryStore reopened(root);
        for (int i = 0; i != 200; ++i) {
            if (reopened.Status()->reports.size() == 2) break;
            std::this_thread::sleep_for(10ms);
        }
        TEST_CHECK(reopened.Status()->reports.size() == 2);
        TEST_CHECK(reopened.Status()->reports[0].complete);
        TEST_CHECK(reopened.Status()->reports[1].complete);
    }
}

void NonterminalCheckpointRunsWithoutNewJobs() {
    TemporaryDirectory directory("cohavora-telemetry-timed");
    TelemetryHistoryStore store(directory.path(), 300, 64,
        100ull * 1024ull * 1024ull, std::chrono::hours(0));
    auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
        Record(1, false)->snapshot);
    TEST_CHECK(store.SubmitSnapshot(snapshot));
    for (int i = 0; i != 190; ++i) {
        if (store.Status()->reports.size() == 1) break;
        std::this_thread::sleep_for(100ms);
    }
    const auto status = store.Status();
    TEST_CHECK(status->reports.size() == 1);
    TEST_CHECK(!status->reports.front().complete);
    TEST_CHECK(status->checkpoint_revision == 1);
    TEST_CHECK(InspectTelemetryCheckpoint(
        directory.path() / status->reports.front().record_id).valid);
}

void CheckpointRollsAndRecoversOrphanSegment() {
    TemporaryDirectory directory("cohavora-telemetry-roll");
    const auto bytes = livekit::telemetry::SerializeSafeTelemetryCheckpointRecord(
        *Record(1, false)).size();
    const auto budget = static_cast<std::uint64_t>(bytes * 2 + 16);
    const auto first = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false)}, budget);
    TEST_CHECK(first.success);
    const auto orphan = first.report_directory /
        "segment-00000000000000000002.jsonl";
    std::ofstream(orphan, std::ios::binary) << "incomplete";
    const auto second = AppendTelemetryCheckpoint(
        directory.path(), {Record(2, false)}, budget);
    TEST_CHECK(second.success);
    TEST_CHECK(InspectTelemetryCheckpoint(first.report_directory).valid);
    const auto third = AppendTelemetryCheckpoint(
        directory.path(), {Record(3, true)}, budget);
    TEST_CHECK(third.success);
    TEST_CHECK(third.status.segment_count == 2);
    TEST_CHECK(third.status.pruned_segments == 1);
    TEST_CHECK(third.status.pruned_records == 1);
    TEST_CHECK(third.status.record_count == 3);
    TEST_CHECK(third.status.session_complete);
    TEST_CHECK(!std::filesystem::exists(first.report_directory /
        "segment-00000000000000000001.jsonl"));
    const auto orphan4 = first.report_directory /
        "segment-00000000000000000004.jsonl";
    std::ofstream(orphan4, std::ios::binary) << "abandoned";
    const auto unknown = first.report_directory / "user-owned.bin";
    std::ofstream(unknown, std::ios::binary) << "keep";
    TEST_CHECK(InspectTelemetryCheckpoint(first.report_directory).orphan_files == 1);
    TEST_CHECK(livekit::telemetry::PruneTelemetryCheckpointOrphans(
        first.report_directory));
    TEST_CHECK(InspectTelemetryCheckpoint(first.report_directory).orphan_files == 0);
    TEST_CHECK(!std::filesystem::exists(orphan4));
    TEST_CHECK(std::filesystem::exists(unknown));
}

void ControlCallbacksHaveOneTerminalResult() {
    TemporaryDirectory directory("cohavora-telemetry-controls");
    TelemetryHistoryStore store(directory.path());
    std::atomic<int> exports{0};
    std::atomic<int> clears{0};
    std::atomic<int> retries{0};
    TEST_CHECK(store.ExportCurrent(directory.path() / "export",
        [&](auto result) {
            TEST_CHECK(!result.success);
            ++exports;
        }));
    TEST_CHECK(store.ClearReport("missing", [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "report_not_removed");
        ++clears;
    }));
    TEST_CHECK(store.RetryCheckpoint("missing", [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "report_not_pending");
        ++retries;
    }));
    store.Close();
    TEST_CHECK(exports == 1 && clears == 1 && retries == 1);
    TEST_CHECK(!store.ExportCurrent(directory.path() / "later",
        [&](auto result) {
            TEST_CHECK(!result.success && result.reason == "export_rejected");
            ++exports;
        }));
    TEST_CHECK(!store.ClearReport("missing", [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "clear_rejected");
        ++clears;
    }));
    TEST_CHECK(!store.RetryCheckpoint("missing", [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "retry_rejected");
        ++retries;
    }));
    TEST_CHECK(exports == 2 && clears == 2 && retries == 2);
    TEST_CHECK(!store.ClearReport("../invalid", [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "invalid_report_id");
        ++clears;
    }));
    TEST_CHECK(clears == 3);
}

void FutureCheckpointVersionIsPreserved() {
    TemporaryDirectory directory("cohavora-telemetry-future");
    const auto written = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, true)});
    TEST_CHECK(written.success);
    const auto manifest_path = written.report_directory / "manifest.json";
    auto manifest = nlohmann::json::parse(ReadAll(manifest_path));
    manifest["schema_version"] = 3;
    std::ofstream(manifest_path, std::ios::binary | std::ios::trunc)
        << manifest.dump();
    TEST_CHECK(InspectTelemetryCheckpoint(written.report_directory).reason ==
               "unsupported_version");
    TelemetryHistoryStore store(directory.path());
    for (int i = 0; i != 200; ++i) {
        if (store.Status()->unsupported_reports == 1) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->unsupported_reports == 1);
    TEST_CHECK(store.Status()->corrupt_reports == 0);
    TEST_CHECK(store.Status()->reports.empty());
    TEST_CHECK(std::filesystem::exists(manifest_path));
}

void CorruptOwnedArtifactsRespectQuarantineBudget() {
    TemporaryDirectory directory("cohavora-telemetry-corrupt-budget");
    const auto report = directory.path() /
        "cohavora-telemetry-v2-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    std::filesystem::create_directory(report);
    const auto segment = report / "segment-00000000000000000001.jsonl";
    {
        std::ofstream output(segment, std::ios::binary);
        const std::string data(9 * 1024 * 1024, 'x');
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    const auto unknown = report / "user-owned.bin";
    std::ofstream(unknown) << "keep";
    TelemetryHistoryStore store(directory.path());
    for (int i = 0; i != 200; ++i) {
        if (store.Status()->corrupt_artifacts_pruned == 1) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->corrupt_artifacts_pruned == 1);
    TEST_CHECK(!std::filesystem::exists(segment));
    TEST_CHECK(std::filesystem::exists(unknown));
}

void SuppliedSessionIdIsUsedForHistory() {
    TemporaryDirectory directory("cohavora-telemetry-correlation");
    TelemetryHistoryStore store(directory.path(), std::string(32, 'c'));
    auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
        Record(1, true)->snapshot);
    const std::string id = "abcdefabcdefabcdefabcdefabcdefab";
    TEST_CHECK(!store.SubmitSnapshot(snapshot, {}, "invalid-id"));
    TEST_CHECK(store.SubmitSnapshot(snapshot, {}, id));
    for (int i = 0; i != 200; ++i) {
        if (!store.Status()->reports.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->reports.size() == 1);
    TEST_CHECK(store.Status()->reports.front().record_id ==
        "cohavora-telemetry-v2-" + id);
    TEST_CHECK(InspectTelemetryCheckpoint(directory.path() /
        store.Status()->reports.front().record_id).process_run_id ==
        std::string(32, 'c'));
}

void CheckpointRunIdentityIsImmutable() {
    TemporaryDirectory directory("cohavora-telemetry-run-id");
    const auto first = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *Record(1, false));
    const auto second = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *Record(2, true));
    const auto id = Record(1, false)->anonymous_session_id;
    const auto run = std::string(32, 'a');
    const auto written = AppendTelemetryCheckpoint(
        directory.path(), id, {first}, 64ull * 1024ull * 1024ull, run);
    TEST_CHECK(written.success);
    TEST_CHECK(InspectTelemetryCheckpoint(
        written.report_directory).process_run_id == run);
    const auto wrong_run = AppendTelemetryCheckpoint(
        directory.path(), id, {second}, 64ull * 1024ull * 1024ull,
        std::string(32, 'b'));
    TEST_CHECK(!wrong_run.success &&
               wrong_run.reason == "process_run_mismatch");
    TEST_CHECK(AppendTelemetryCheckpoint(
        directory.path(), id, {second}, 64ull * 1024ull * 1024ull,
        run).success);
}

void CheckpointMutationRespectsStoreLease() {
#if defined(_WIN32)
    TemporaryDirectory directory("cohavora-telemetry-store-lease");
    const auto written = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, true)});
    TEST_CHECK(written.success);
    const auto lock_path = directory.path() / L"store.lock";
    const HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(handle != INVALID_HANDLE_VALUE);
    TEST_CHECK(!livekit::telemetry::RemoveTelemetryCheckpoint(
        written.report_directory));
    const auto retry = AppendTelemetryCheckpoint(
        directory.path(), {Record(2, true)});
    TEST_CHECK(!retry.success && retry.reason == "store_busy");
    TEST_CHECK(InspectTelemetryCheckpoint(written.report_directory).valid);
    CloseHandle(handle);
    TEST_CHECK(livekit::telemetry::RemoveTelemetryCheckpoint(
        written.report_directory));
    TEST_CHECK(!std::filesystem::exists(written.report_directory));
#endif
}

void CheckpointReadLeaseBlocksMutation() {
    TemporaryDirectory directory("cohavora-telemetry-read-lease");
    const auto written = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false)});
    TEST_CHECK(written.success);
    {
        livekit::telemetry::TelemetryCheckpointReadLease reader(
            written.report_directory);
        TEST_CHECK(reader.acquired());
#if defined(_WIN32)
        TEST_CHECK(!livekit::telemetry::RemoveTelemetryCheckpoint(
            written.report_directory));
        const auto next = AppendTelemetryCheckpoint(
            directory.path(), {Record(2, true)});
        TEST_CHECK(!next.success && next.reason == "writer_busy");
#endif
        TEST_CHECK(InspectTelemetryCheckpoint(written.report_directory).valid);
    }
    TEST_CHECK(AppendTelemetryCheckpoint(
        directory.path(), {Record(2, true)}).success);
    TEST_CHECK(livekit::telemetry::RemoveTelemetryCheckpoint(
        written.report_directory));
}

void CheckpointPersistsRevisionLossRange() {
    TemporaryDirectory directory("cohavora-telemetry-loss-range");
    const auto first = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false)});
    TEST_CHECK(first.success);
    const auto next = AppendTelemetryCheckpoint(
        directory.path(), {Record(3, true)});
    TEST_CHECK(next.success);
    TEST_CHECK(next.status.missing_revisions == 1);
    const auto manifest = nlohmann::json::parse(
        ReadAll(next.report_directory / "manifest.json"));
    TEST_CHECK(manifest.at("missing_ranges").size() == 1);
    TEST_CHECK(manifest.at("missing_ranges")[0].at("first_revision") == 2);
    TEST_CHECK(manifest.at("missing_ranges")[0].at("last_revision") == 2);
    TEST_CHECK(InspectTelemetryCheckpoint(next.report_directory)
        .missing_revisions == 1);
}

void CheckpointBudgetIncludesOwnedOrphans() {
    TemporaryDirectory directory("cohavora-telemetry-global-budget");
    const auto sample = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *Record(1, false));
    const auto next = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *Record(2, true));
    const auto id = Record(1, false)->anonymous_session_id;
    const auto budget = sample.jsonl.size() * 2 + 4096;
    const auto first = AppendTelemetryCheckpoint(directory.path(), id,
        {sample}, 64ull * 1024ull * 1024ull, {}, budget);
    TEST_CHECK(first.success);
    const auto orphan = first.report_directory /
        "segment-00000000000000000003.jsonl.tmp";
    {
        std::ofstream output(orphan, std::ios::binary);
        const std::string bytes(sample.jsonl.size(), 'x');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto blocked = AppendTelemetryCheckpoint(directory.path(), id,
        {next}, 64ull * 1024ull * 1024ull, {}, budget);
    TEST_CHECK(!blocked.success && blocked.reason == "history_budget_exceeded");
    TEST_CHECK(InspectTelemetryCheckpoint(first.report_directory).valid);
    TEST_CHECK(livekit::telemetry::PruneTelemetryCheckpointOrphans(
        first.report_directory));
    TEST_CHECK(AppendTelemetryCheckpoint(directory.path(), id, {next},
        64ull * 1024ull * 1024ull, {}, budget).success);
    TEST_CHECK(*livekit::telemetry::TelemetryHistoryOwnedBytes(
        directory.path()) <= budget);
}

void FailedBudgetAccountingPreservesReports() {
    TemporaryDirectory directory("cohavora-telemetry-budget-scan-failure");
    const auto report = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, true)});
    TEST_CHECK(report.success);
    std::filesystem::create_directory(directory.path() / "losses-v1.json");
    TelemetryHistoryStore store(directory.path(), 300, 64,
        1024 * 1024, std::chrono::hours(0));
    store.Close();
    TEST_CHECK(store.Status()->availability == Availability::Invalid);
    TEST_CHECK(store.Status()->reason == "met_06_history_scan_failed");
    TEST_CHECK(InspectTelemetryCheckpoint(report.report_directory).valid);
}

void ActiveRunCheckpointSurvivesConcurrentPrune() {
    TemporaryDirectory directory("cohavora-telemetry-active-run");
    const auto run_id = std::string(32, 'a');
    const auto checkpoint = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *Record(1, false));
    const auto id = Record(1, false)->anonymous_session_id;
    const auto written = AppendTelemetryCheckpoint(directory.path(), id,
        {checkpoint}, 64ull * 1024ull * 1024ull, run_id);
    TEST_CHECK(written.success);
    {
        livekit::telemetry::TelemetryRunLease active(directory.path(), run_id);
        TEST_CHECK(active.acquired());
#if defined(_WIN32)
        TEST_CHECK(livekit::telemetry::IsTelemetryRunActive(
            directory.path(), run_id));
#endif
        TelemetryHistoryStore observer(directory.path(), 300, 64,
            100ull * 1024ull * 1024ull, std::chrono::hours(0));
        observer.Close();
        TEST_CHECK(InspectTelemetryCheckpoint(written.report_directory).valid);
    }
    TEST_CHECK(!livekit::telemetry::IsTelemetryRunActive(
        directory.path(), run_id));
    TelemetryHistoryStore observer(directory.path(), 300, 64,
        100ull * 1024ull * 1024ull, std::chrono::hours(0));
    observer.Close();
    TEST_CHECK(!std::filesystem::exists(written.report_directory));
}

void BusyCheckpointIsNotClassifiedCorrupt() {
#if defined(_WIN32)
    TemporaryDirectory directory("cohavora-telemetry-busy-scan");
    const auto written = AppendTelemetryCheckpoint(
        directory.path(), {Record(1, false)});
    TEST_CHECK(written.success);
    const auto manifest_path = written.report_directory / "manifest.json";
    const auto content = ReadAll(manifest_path);
    const auto lock_path = written.report_directory / L"writer.lock";
    const HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(handle != INVALID_HANDLE_VALUE);
    std::ofstream(manifest_path, std::ios::trunc) << "partial";
    {
        TelemetryHistoryStore observer(directory.path());
        observer.Close();
        TEST_CHECK(observer.Status()->corrupt_reports == 0);
        TEST_CHECK(std::filesystem::exists(manifest_path));
    }
    std::ofstream(manifest_path, std::ios::trunc) << content;
    CloseHandle(handle);
    TEST_CHECK(InspectTelemetryCheckpoint(written.report_directory).valid);
#endif
}

void HistoricalBundleIsSafeAndVerifiable() {
    TemporaryDirectory directory("cohavora-telemetry-bundle");
    const auto root = directory.path() / "telemetry" / "reports";
    const auto diagnostics = directory.path() / "diagnostics";
    const auto destination = directory.path() / "export";
    const auto run_id = std::string(32, 'b');
    const auto session_id = Record(1, true)->anonymous_session_id;
    auto safe_record = std::make_shared<SafeTelemetryRecord>(*Record(1, true));
    safe_record->source_utc_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now()
            .time_since_epoch()).count();
    auto checkpoint = livekit::telemetry::MakeTelemetryCheckpointRecord(
        *safe_record);
    constexpr auto canary = "Aster Quill Lantern";
    {
        std::istringstream input(checkpoint.jsonl);
        std::string line;
        std::string altered;
        bool changed = false;
        while (std::getline(input, line)) {
            auto row = nlohmann::json::parse(line);
            if (!changed && row.at("key") == "coverage") {
                row["value"] = canary;
                changed = true;
            }
            altered += row.dump() + '\n';
        }
        TEST_CHECK(changed);
        checkpoint.jsonl = std::move(altered);
    }
    const auto written = AppendTelemetryCheckpoint(root, session_id,
        {checkpoint}, 64ull * 1024ull * 1024ull, run_id);
    TEST_CHECK(written.success);
    const auto run_directory = diagnostics / ("run-" + run_id);
    std::filesystem::create_directories(run_directory);
    const auto build_id = std::string(64, 'c');
    const auto symbol_identity =
        std::string("01234567-89ab-cdef-0123456789abcdef-1");
    std::ofstream events(run_directory / "segment-000000.jsonl");
    events << nlohmann::json{
        {"schema_version", 1}, {"event_name", "process.started"},
        {"process_run_id", run_id},
        {"attributes", {{"build_id", build_id},
                        {"symbol_identity", symbol_identity}}},
    }.dump() << '\n';
    events << nlohmann::json{
        {"schema_version", 1},
        {"event_name", "room.connect.terminal"},
        {"process_run_id", run_id},
        {"anonymous_session_id", session_id},
        {"operation_id", "room_connect_42"},
        {"request_id", std::string(32, 'd')},
        {"stage", "connecting_room"},
        {"outcome", "failure"},
        {"error_code", "join_timeout"},
        {"occurred_at_utc_ms", 1234},
        {"monotonic_us", 5678},
        {"event_sequence", 2},
    }.dump() << '\n';
    events << nlohmann::json{
        {"schema_version", 1},
        {"event_name", "reconnect.attempt.terminal"},
        {"process_run_id", run_id},
        {"anonymous_session_id", session_id},
        {"operation_id", "reconnect_attempt_42_1"},
        {"parent_operation_id", "room_connect_42"},
        {"stage", "full_restart"},
        {"outcome", "degraded_success"},
        {"occurred_at_utc_ms", 1250},
        {"monotonic_us", 5690},
        {"event_sequence", 3},
    }.dump() << '\n';
    events << nlohmann::json{
        {"schema_version", 1},
        {"event_name", "meeting.leave.requested"},
        {"process_run_id", run_id},
        {"anonymous_session_id", session_id},
        {"parent_operation_id", "room_connect_42"},
        {"attributes", {{"leave_reason", "leave"}}},
        {"occurred_at_utc_ms", 1290},
        {"monotonic_us", 5695},
        {"event_sequence", 4},
    }.dump() << '\n';
    events << nlohmann::json{
        {"schema_version", 1},
        {"event_name", "meeting.backend_notification.completed"},
        {"process_run_id", run_id},
        {"anonymous_session_id", session_id},
        {"parent_operation_id", "room_connect_42"},
        {"request_id", std::string(32, 'e')},
        {"outcome", "failure"},
        {"error_code", "unknown"},
        {"error_layer", "http"},
        {"attributes", {{"http_status", 503}}},
        {"occurred_at_utc_ms", 1300},
        {"monotonic_us", 5700},
        {"event_sequence", 5},
    }.dump() << '\n';
    events.close();
    std::filesystem::create_directories(root.parent_path());
    std::ofstream(root.parent_path() / "stability-ledger-v1.json") <<
        nlohmann::json{{"schema", "cohavora-stability-ledger"},
            {"version", 1}, {"records", nlohmann::json::array({
                {{"kind", "process"}, {"id", run_id},
                 {"build_id", build_id}, {"status", "UNKNOWN_TERMINATION"},
                 {"started_utc_ms", 1000}, {"terminal_utc_ms", 2000},
                 {"crash_evidence_checked", true}},
                {{"kind", "session"}, {"id", session_id},
                 {"process_run_id", std::string(32, 'f')},
                 {"status", "COMPLETED"}, {"started_utc_ms", 900}},
                {{"kind", "session"}, {"id", session_id},
                 {"process_run_id", run_id}, {"status", "ADMISSION_FAILURE"},
                 {"started_utc_ms", 1100}, {"terminal_utc_ms", 1400}}
            })}}.dump();
    TEST_CHECK(livekit::telemetry::PersistTelemetryLossRanges(root,
        {{session_id, 3, 2, 4, 1000, 2000}}));
    livekit::telemetry::TelemetryReportEntry entry;
    entry.record_id = written.report_directory.filename().string();
    const auto result = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, diagnostics, destination);
    TEST_CHECK(result.success);
    const auto bundle = result.report_directory;
    const auto manifest = nlohmann::json::parse(ReadAll(bundle / "manifest.json"));
    TEST_CHECK(manifest.at("includes_memory_dump") == false);
    TEST_CHECK(manifest.at("process_run_id").get<std::string>() == run_id);
    TEST_CHECK(nlohmann::json::parse(ReadAll(bundle / "build.json"))
        .at("build_id").get<std::string>() == build_id);
    TEST_CHECK(nlohmann::json::parse(ReadAll(bundle / "build.json"))
        .at("symbol_identity").get<std::string>() == symbol_identity);
    TEST_CHECK(ReadAll(bundle / "sessions" / session_id / "events.jsonl")
        .find("room.connect.terminal") != std::string::npos);
    const auto exported_events = ReadAll(bundle / "sessions" / session_id /
        "events.jsonl");
    TEST_CHECK(exported_events.find("join_timeout") != std::string::npos);
    TEST_CHECK(exported_events.find("full_restart") != std::string::npos);
    TEST_CHECK(exported_events.find("reconnect_attempt_42_1") !=
        std::string::npos);
    TEST_CHECK(exported_events.find(std::string(32, 'd')) !=
        std::string::npos);
    bool leave_requested_found = false;
    bool backend_failure_found = false;
    std::istringstream event_rows(exported_events);
    for (std::string row; std::getline(event_rows, row);) {
        const auto event = nlohmann::json::parse(row);
        if (event.at("event_name") == "meeting.leave.requested") {
            TEST_CHECK(event.at("leave_reason") == "leave");
            leave_requested_found = true;
        }
        if (event.at("event_name") != "meeting.backend_notification.completed")
            continue;
        TEST_CHECK(event.at("outcome") == "failure");
        TEST_CHECK(event.at("error_layer") == "http");
        TEST_CHECK(event.at("http_status") == 503);
        TEST_CHECK(event.at("request_id") == std::string(32, 'e'));
        backend_failure_found = true;
    }
    TEST_CHECK(leave_requested_found && backend_failure_found);
    TEST_CHECK(manifest.at("missing").dump().find(
        "recovered_media_endpoint_identity") != std::string::npos);
    {
        std::ofstream recovery_events(run_directory / "segment-000000.jsonl",
            std::ios::app);
        recovery_events << nlohmann::json{
            {"schema_version", 1}, {"event_name", "media.endpoint.recovered"},
            {"process_run_id", run_id}, {"anonymous_session_id", session_id},
            {"operation_id", "reconnect_42_1"}, {"session_generation", 1},
            {"room_generation", 2},
            {"recovery_epoch", 1},
            {"attributes", {{"media_kind", "video"},
                {"measurement_point", "decoded_video_stably_recovered"},
                {"endpoint_id", std::string(32, '1')},
                {"previous_endpoint_id", std::string(32, '2')}}},
            {"occurred_at_utc_ms", 1310}, {"monotonic_us", 5710},
            {"event_sequence", 6},
        }.dump() << '\n';
        recovery_events << nlohmann::json{
            {"schema_version", 1}, {"event_name", "media.endpoint.recovered"},
            {"process_run_id", run_id}, {"anonymous_session_id", session_id},
            {"operation_id", "reconnect_42_1"}, {"session_generation", 2},
            {"room_generation", 3}, {"recovery_epoch", 1},
            {"attributes", {{"media_kind", "video"},
                {"measurement_point", "decoded_video_stably_recovered"},
                {"endpoint_id", std::string(32, '3')}}},
            {"occurred_at_utc_ms", 1315}, {"monotonic_us", 5715},
            {"event_sequence", 7},
        }.dump() << '\n';
        recovery_events << nlohmann::json{
            {"schema_version", 1}, {"event_name", "media.recovery.milestone"},
            {"process_run_id", run_id}, {"anonymous_session_id", session_id},
            {"operation_id", "reconnect_42_1"}, {"session_generation", 1},
            {"recovery_epoch", 1},
            {"attributes", {{"media_kind", "video"},
                {"measurement_point", "decoded_video_stably_recovered"},
                {"expected_endpoints", 1u}}},
            {"occurred_at_utc_ms", 1320}, {"monotonic_us", 5720},
            {"event_sequence", 8},
        }.dump() << '\n';
    }
    const auto complete_bundle = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, diagnostics, directory.path() / "export-complete");
    TEST_CHECK(complete_bundle.success);
    TEST_CHECK(ReadAll(complete_bundle.report_directory / "manifest.json").find(
        "recovered_media_endpoint_identity") == std::string::npos);
    TEST_CHECK(ReadAll(complete_bundle.report_directory / "sessions" /
        session_id / "events.jsonl").find(std::string(32, '1')) !=
        std::string::npos);
    {
        std::ofstream recovery_events(run_directory / "segment-000000.jsonl",
            std::ios::app);
        recovery_events << nlohmann::json{
            {"schema_version", 1}, {"event_name", "media.endpoint.recovered"},
            {"process_run_id", run_id}, {"anonymous_session_id", session_id},
            {"operation_id", "reconnect_42_1"}, {"session_generation", 1},
            {"room_generation", 2},
            {"recovery_epoch", 1},
            {"attributes", {{"media_kind", "video"},
                {"measurement_point", "decoded_video_stably_recovered"},
                {"endpoint_id", "invalid"}}},
            {"occurred_at_utc_ms", 1330}, {"monotonic_us", 5730},
            {"event_sequence", 9},
        }.dump() << '\n';
    }
    const auto incomplete_bundle = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, diagnostics, directory.path() / "export-incomplete");
    TEST_CHECK(incomplete_bundle.success);
    TEST_CHECK(ReadAll(incomplete_bundle.report_directory / "manifest.json").find(
        "recovered_media_endpoint_identity") != std::string::npos);
    const auto stability = nlohmann::json::parse(ReadAll(
        bundle / "sessions" / session_id / "stability.json"));
    TEST_CHECK(stability.at("process_run").at("status") ==
        "UNKNOWN_TERMINATION");
    TEST_CHECK(stability.at("session_detail").at("status") ==
        "ADMISSION_FAILURE");
    TEST_CHECK(manifest.at("missing").dump().find(
        "historical_stability_session_link") == std::string::npos);
    const auto health = nlohmann::json::parse(ReadAll(
        bundle / "diagnostics-health.json"));
    TEST_CHECK(health.at("persistent_losses").at("dropped_records") == 3);
    for (const auto& file : manifest.at("files")) {
        const auto path = bundle / file.at("path").get<std::string>();
        const auto content = ReadAll(path);
        TEST_CHECK(content.size() == file.at("size_bytes").get<std::size_t>());
        TEST_CHECK(Sha256(content) == file.at("sha256").get<std::string>());
        TEST_CHECK(content.find(canary) == std::string::npos);
    }
    TEST_CHECK(ReadAll(bundle / "manifest.json").find(canary) ==
        std::string::npos);
    const auto cancelled = std::make_shared<std::atomic_bool>(true);
    const auto denied = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, diagnostics, destination, cancelled);
    TEST_CHECK(!denied.success && denied.cancelled);
    TEST_CHECK(std::distance(std::filesystem::directory_iterator(destination),
        std::filesystem::directory_iterator{}) == 1);
    entry.record_id = "../outside";
    TEST_CHECK(livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, diagnostics, destination).reason == "invalid_report_id");
    TelemetryHistoryStore store(root, run_id, diagnostics);
    for (int i = 0; i != 200; ++i) {
        if (!store.Status()->reports.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->reports.size() == 1);
    std::atomic<int> callbacks{0};
    std::atomic<bool> exported{false};
    TEST_CHECK(store.ExportReport(store.Status()->reports.front().record_id,
        directory.path() / "queued-export",
        [&](livekit::telemetry::TelemetryExportResult answer) {
            exported = answer.success;
            ++callbacks;
        }));
    store.Close();
    TEST_CHECK(callbacks == 1 && exported);
}

void LegacyBundleIsSafeAndRangeBounded() {
    TemporaryDirectory directory("cohavora-telemetry-legacy-bundle");
    const auto root = directory.path() / "telemetry" / "reports";
    const auto first = Record(1, false, 1000);
    const auto second = Record(2, true, 2000);
    const auto written = WriteTelemetryReportAtomically(
        {first, second}, root, true);
    TEST_CHECK(written.success);
    const auto metrics_path = written.report_directory / "metrics.jsonl";
    std::istringstream input(ReadAll(metrics_path));
    std::string line;
    std::string altered;
    bool injected = false;
    while (std::getline(input, line)) {
        auto row = nlohmann::json::parse(line);
        if (!injected && row.at("key") == "coverage" &&
            row.at("revision") == 2) {
            row["value"] = "private legacy canary";
            injected = true;
        }
        altered += row.dump() + '\n';
    }
    TEST_CHECK(injected);
    std::ofstream(metrics_path, std::ios::trunc) << altered;
    livekit::telemetry::TelemetryReportEntry entry;
    entry.record_id = written.report_directory.filename().string();
    entry.size_bytes = std::filesystem::file_size(metrics_path);
    const auto result = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, directory.path() / "diagnostics",
        directory.path() / "export", {}, {1500, 2500});
    TEST_CHECK(result.success);
    const auto bundle = result.report_directory;
    const auto manifest = nlohmann::json::parse(ReadAll(bundle / "manifest.json"));
    TEST_CHECK(manifest.at("source_format") == "v1");
    TEST_CHECK(manifest.at("selected_first_utc_ms") == 1500);
    TEST_CHECK(manifest.at("selected_last_utc_ms") == 2500);
    TEST_CHECK(manifest.at("missing").dump().find(
        "legacy_source_integrity_and_run_identity") != std::string::npos);
    const auto metrics = ReadAll(bundle / "sessions" /
        first->anonymous_session_id / "telemetry" / "metrics.jsonl");
    TEST_CHECK(metrics.find("\"revision\":1") == std::string::npos);
    TEST_CHECK(metrics.find("\"revision\":2") != std::string::npos);
    TEST_CHECK(metrics.find("private legacy canary") == std::string::npos);
    TelemetryHistoryStore store(root);
    for (int i = 0; i != 200; ++i) {
        if (!store.Status()->reports.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->reports.size() == 1);
    std::atomic<bool> queued_exported{false};
    TEST_CHECK(store.ExportReport(entry.record_id,
        directory.path() / "queued-export",
        [&](livekit::telemetry::TelemetryExportResult answer) {
            queued_exported = answer.success;
        }, {}, 1500, 2500));
    store.Close();
    TEST_CHECK(queued_exported);
    std::ofstream(metrics_path, std::ios::app) <<
        std::string(20 * 1024, 'x') << '\n';
    const auto oversized = livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, {}, directory.path() / "export");
    TEST_CHECK(!oversized.success &&
        oversized.reason == "metric_line_oversized");
    TEST_CHECK(livekit::telemetry::WriteTelemetryDiagnosticBundle(
        root, entry, {}, directory.path() / "export", {},
        {2500, 1500}).reason == "invalid_time_range");
}

void LegacyExportLeaseBlocksClear() {
#if defined(_WIN32)
    TemporaryDirectory directory("cohavora-telemetry-legacy-lease");
    const auto root = directory.path() / "telemetry" / "reports";
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto written = WriteTelemetryReportAtomically(
        {Record(1, true, now)}, root, true);
    TEST_CHECK(written.success);
    TelemetryHistoryStore store(root);
    for (int i = 0; i != 200; ++i) {
        if (!store.Status()->reports.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->reports.size() == 1);
    const auto handle = CreateFileW(
        (written.report_directory / L"manifest.json").c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(handle != INVALID_HANDLE_VALUE);
    std::atomic<bool> removed{true};
    TEST_CHECK(store.ClearReport(
        written.report_directory.filename().string(),
        [&](bool value, std::string) { removed = value; }));
    store.Close();
    TEST_CHECK(!removed && std::filesystem::exists(
        written.report_directory / "metrics.jsonl"));
    CloseHandle(handle);
#endif
}

void ExportAdmissionAllowsOneRunningAndOneWaiting() {
    TemporaryDirectory directory("cohavora-telemetry-export-budget");
    TelemetryHistoryStore store(directory.path() / "history");
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool release = false;
    std::atomic<int> callbacks{0};
    TEST_CHECK(store.ExportCurrent(directory.path() / "first",
        [&](auto) {
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return release; });
            ++callbacks;
        }));
    {
        std::unique_lock lock(mutex);
        TEST_CHECK(wake.wait_for(lock, 5s, [&] { return entered; }));
    }
    TEST_CHECK(store.ExportCurrent(directory.path() / "second",
        [&](auto) { ++callbacks; }));
    TEST_CHECK(!store.ExportCurrent(directory.path() / "third",
        [&](auto result) {
            TEST_CHECK(!result.success && result.reason == "export_rejected");
            ++callbacks;
        }));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    wake.notify_all();
    store.Close();
    TEST_CHECK(callbacks == 3);
}

void LegacyTemporaryFilesCountAndExpire() {
    TemporaryDirectory directory("cohavora-telemetry-v1-temporary");
    const auto temp = directory.path() /
        ".cohavora-telemetry-tmp-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    std::filesystem::create_directory(temp);
    const auto owned = temp / "metrics.jsonl";
    std::ofstream(owned) << std::string(8192, 'x');
    const auto unknown = temp / "user-owned.bin";
    std::ofstream(unknown) << "keep";
    TEST_CHECK(*livekit::telemetry::TelemetryHistoryOwnedBytes(
        directory.path()) >= 8192);
    std::filesystem::last_write_time(temp,
        std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(24 * 10));
    TelemetryHistoryStore store(directory.path());
    store.Close();
    TEST_CHECK(!std::filesystem::exists(owned));
    TEST_CHECK(std::filesystem::exists(unknown));
    TEST_CHECK(*livekit::telemetry::TelemetryHistoryOwnedBytes(
        directory.path()) == 0);
}

void DisablingHistoryRemovesLossSummary() {
    TemporaryDirectory directory("cohavora-telemetry-clear-losses");
    const auto root = directory.path() / "history";
    TEST_CHECK(livekit::telemetry::PersistTelemetryLossRanges(root, {
        {std::string(32, 'a'), 2, 4, 5, 1000, 2000},
    }));
    TEST_CHECK(std::filesystem::exists(root / "losses-v1.json"));
    TelemetryHistoryStore store(root);
    store.SetHistoryEnabled(false);
    store.Close();
    TEST_CHECK(!std::filesystem::exists(root / "losses-v1.json"));
    TEST_CHECK(!store.Status()->history_enabled);
}

void ManualRetryCommitsAfterStorageRecovery() {
    TemporaryDirectory directory("cohavora-telemetry-manual-retry");
    const auto root = directory.path() / "history";
    std::ofstream(root) << "blocked";
    TelemetryHistoryStore store(root);
    auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
        Record(1, true)->snapshot);
    const std::string id = "fedcbafedcbafedcbafedcbafedcbafe";
    TEST_CHECK(store.SubmitSnapshot(snapshot, {}, id));
    for (int i = 0; i != 200; ++i) {
        if (store.Status()->write_failures > 0 &&
            store.Status()->pending_reports == 1) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(store.Status()->pending_reports == 1);
    TEST_CHECK(store.Status()->reports.empty());
    std::atomic<int> failed_callbacks{0};
    TEST_CHECK(store.RetryCheckpoint(id, [&](bool success, std::string reason) {
        TEST_CHECK(!success && reason == "checkpoint_retry_failed");
        ++failed_callbacks;
    }));
    for (int i = 0; i != 200 && failed_callbacks == 0; ++i)
        std::this_thread::sleep_for(10ms);
    TEST_CHECK(failed_callbacks == 1);

    std::filesystem::remove(root);
    std::filesystem::create_directory(root);
    std::atomic<int> success_callbacks{0};
    TEST_CHECK(store.RetryCheckpoint(id, [&](bool success, std::string reason) {
        TEST_CHECK(success && reason == "checkpoint_committed");
        ++success_callbacks;
    }));
    for (int i = 0; i != 200 && success_callbacks == 0; ++i)
        std::this_thread::sleep_for(10ms);
    TEST_CHECK(success_callbacks == 1);
    TEST_CHECK(store.Status()->pending_reports == 0);
    TEST_CHECK(store.Status()->reports.size() == 1);
    TEST_CHECK(InspectTelemetryCheckpoint(root /
        ("cohavora-telemetry-v2-" + id)).valid);
}

void QueueBytesRejectBeforeJobCount() {
    TemporaryDirectory directory("cohavora-telemetry-queue-bytes");
    const auto sample = Record(1, false);
    const auto serialized = livekit::telemetry::
        SerializeSafeTelemetryCheckpointRecord(*sample);
    const auto budget = (std::max)(std::size_t{4 * 1024 * 1024},
                                   serialized.size() * 4);
    TelemetryHistoryStore store(directory.path() / "history", 300, 64,
        100ull * 1024ull * 1024ull, std::chrono::hours(24 * 7), budget);
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool entered = false;
    bool released = false;
    TEST_CHECK(store.ExportCurrent(directory.path() / "export",
        [&](auto) {
            std::unique_lock lock(gate_mutex);
            entered = true;
            gate.notify_all();
            gate.wait(lock, [&] { return released; });
        }));
    {
        std::unique_lock lock(gate_mutex);
        TEST_CHECK(gate.wait_for(lock, 5s, [&] { return entered; }));
    }
    auto snapshot = std::make_shared<livekit::telemetry::Snapshot>(
        sample->snapshot);
    std::size_t accepted = 0;
    for (int i = 0; i != 64; ++i)
        if (store.SubmitSnapshot(snapshot)) ++accepted;
    const auto queued = store.Status();
    TEST_CHECK(accepted > 0 && accepted < 64);
    TEST_CHECK(queued->queue_depth == accepted);
    TEST_CHECK(queued->queue_bytes <= queued->queue_byte_capacity);
    TEST_CHECK(queued->queue_byte_capacity == budget);
    TEST_CHECK(queued->queue_drops == 64 - accepted);
    {
        std::lock_guard lock(gate_mutex);
        released = true;
    }
    gate.notify_all();
    store.Close();
    TEST_CHECK(store.Status()->queue_bytes == 0);
    TEST_CHECK(store.Status()->loss_ranges_pending == 0);
    const auto losses = nlohmann::json::parse(ReadAll(
        directory.path() / "history" / "losses-v1.json"));
    TEST_CHECK(losses.at("schema") == "cohavora-telemetry-losses");
    TEST_CHECK(losses.at("sessions").size() == 1);
    TEST_CHECK(losses.at("sessions")[0].at("dropped_records") ==
        queued->queue_drops);
}

#if defined(_WIN32)
void CommittedCheckpointSurvivesForcedTermination(
    const std::filesystem::path& executable) {
    TemporaryDirectory directory("cohavora-telemetry-kill");
    const auto root = directory.path() / "history";
    const auto ready = directory.path() / "ready";
    const auto command = L"\"" + executable.wstring() + L"\" --checkpoint-child \"" +
        root.wstring() + L"\" \"" + ready.wstring() + L"\"";
    auto mutable_command = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    TEST_CHECK(CreateProcessW(executable.c_str(), mutable_command.data(),
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process));
    bool child_ready = false;
    for (int attempt = 0; attempt != 500; ++attempt) {
        if (std::filesystem::exists(ready)) {
            child_ready = true;
            break;
        }
        Sleep(10);
    }
    const bool killed = TerminateProcess(process.hProcess, 0xdead) != 0;
    const bool exited = WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    TEST_CHECK(child_ready && killed && exited);

    const auto report = root /
        "cohavora-telemetry-v2-0123456789abcdef0123456789abcdef";
    const auto recovered = InspectTelemetryCheckpoint(report);
    TEST_CHECK(recovered.valid);
    TEST_CHECK(!recovered.session_complete);
    TEST_CHECK(recovered.last_committed_revision == 1);
    TelemetryHistoryStore reopened(root);
    for (int attempt = 0; attempt != 200; ++attempt) {
        if (!reopened.Status()->reports.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(reopened.Status()->reports.size() == 1);
    TEST_CHECK(!reopened.Status()->reports.front().complete);
    reopened.Close();
}
#endif

} // namespace

int wmain(int argc, wchar_t** argv) {
#if defined(_WIN32)
    if (argc == 4 && std::wstring_view(argv[1]) == L"--checkpoint-child") {
        const auto result = AppendTelemetryCheckpoint(
            std::filesystem::path(argv[2]), {Record(1, false)});
        if (!result.success) return 2;
        std::ofstream(std::filesystem::path(argv[3])) << "ready";
        Sleep(15000);
        return 3;
    }
    wchar_t executable[MAX_PATH]{};
    TEST_CHECK(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
#endif
    const auto& build_id = livekit::telemetry::CurrentExecutableBuildId();
    TEST_CHECK(build_id.size() == 64);
    TEST_CHECK(std::all_of(build_id.begin(), build_id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }));
    JsonCsvShareValuesAndPreserveMissing();
    UnsafeTextAndCsvFormulaAreContained();
    CancellationAndUnwritableDestinationAreBounded();
    StoreCoalescesBucketsAndPreservesUnknownFiles();
    FailedAutomaticHistoryWriteCanRetrySameSession();
    CheckpointAppendRetryAndIntegrity();
    ManifestReplacementFailureKeepsCommittedRevision();
    PendingSessionsRecoverIndependently();
    NonterminalCheckpointRunsWithoutNewJobs();
    CheckpointRollsAndRecoversOrphanSegment();
    ControlCallbacksHaveOneTerminalResult();
    FutureCheckpointVersionIsPreserved();
    CorruptOwnedArtifactsRespectQuarantineBudget();
    SuppliedSessionIdIsUsedForHistory();
    CheckpointRunIdentityIsImmutable();
    CheckpointMutationRespectsStoreLease();
    CheckpointReadLeaseBlocksMutation();
    CheckpointPersistsRevisionLossRange();
    CheckpointBudgetIncludesOwnedOrphans();
    FailedBudgetAccountingPreservesReports();
    ActiveRunCheckpointSurvivesConcurrentPrune();
    BusyCheckpointIsNotClassifiedCorrupt();
    HistoricalBundleIsSafeAndVerifiable();
    LegacyBundleIsSafeAndRangeBounded();
    LegacyExportLeaseBlocksClear();
    ExportAdmissionAllowsOneRunningAndOneWaiting();
    LegacyTemporaryFilesCountAndExpire();
    DisablingHistoryRemovesLossSummary();
    ManualRetryCommitsAfterStorageRecovery();
    QueueBytesRejectBeforeJobCount();
#if defined(_WIN32)
    CommittedCheckpointSurvivesForcedTermination(executable);
#endif
    return 0;
}
