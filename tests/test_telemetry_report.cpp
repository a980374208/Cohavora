#include "src/telemetry/telemetry_report.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using livekit::telemetry::Availability;
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
    record->snapshot.render_stall_availability = Availability::Valid;
    record->snapshot.render_stall_reason = "render_window_valid";
    record->snapshot.render_stall_algorithm = "render-stall-v1";
    record->snapshot.render_stall_count = 0;
    record->snapshot.render_stall_duration_ms = 0;
    record->snapshot.render_stall_ratio = 0.0;
    record->snapshot.telemetry_cost_availability = Availability::Valid;
    record->snapshot.telemetry_cost_reason = "observed_sampler_snapshot_cost_valid";
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
    TEST_CHECK(csv.find("\"resource.cpu\",\"0\",\"percent\",\"VALID\"") != std::string::npos);
    TEST_CHECK(csv.find("\"stats.last_request_duration\",\"\",\"ms\",\"VALID\"") != std::string::npos);
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
