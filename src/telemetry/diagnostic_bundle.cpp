#include "diagnostic_bundle.h"

#include "telemetry_checkpoint.h"
#include "telemetry_operation_timeline.h"
#include "crash_evidence_provider.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <map>
#include <optional>
#include <random>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMaximumBundleBytes = 100ull * 1024 * 1024;

bool IsId(std::string_view value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f'); });
}

std::string SessionFromReportId(std::string_view id, bool& legacy) {
    constexpr std::string_view v2 = "cohavora-telemetry-v2-";
    constexpr std::string_view v1 = "cohavora-telemetry-v1-";
    legacy = false;
    if (id.starts_with(v2)) {
        const auto session = id.substr(v2.size());
        return IsId(session) ? std::string(session) : std::string{};
    }
    if (!id.starts_with(v1)) return {};
    const auto suffix = id.substr(v1.size());
    const auto dash = suffix.find('-');
    if (dash == std::string_view::npos || dash == 0 ||
        !std::all_of(suffix.begin(), suffix.begin() + dash,
            [](char ch) { return ch >= '0' && ch <= '9'; }) ||
        !IsId(suffix.substr(dash + 1))) return {};
    legacy = true;
    return std::string(suffix.substr(dash + 1));
}

bool SafeFile(const std::filesystem::path& path, std::uint64_t maximum) {
    std::error_code error;
    return !std::filesystem::is_symlink(path, error) && !error &&
        std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) <= maximum && !error;
}

class LegacyReportReadLease final {
public:
    explicit LegacyReportReadLease(const std::filesystem::path& manifest) {
#if defined(_WIN32)
        handle_ = CreateFileW(manifest.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
            nullptr);
#else
        acquired_ = SafeFile(manifest, 1024 * 1024);
#endif
    }
    ~LegacyReportReadLease() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
    }
    bool acquired() const noexcept {
#if defined(_WIN32)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return acquired_;
#endif
    }
private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    bool acquired_ = false;
#endif
};

Json ReadStability(const std::filesystem::path& report_root,
                   std::string_view run_id, std::string_view build_id) {
    Json summary{{"availability", "missing"}};
    if (!IsId(run_id)) return summary;
    const auto path = report_root.parent_path() / "stability-ledger-v1.json";
    if (!SafeFile(path, 256 * 1024)) return summary;
    try {
        std::ifstream input(path, std::ios::binary);
        const auto ledger = Json::parse(input);
        if (!input || ledger.value("schema", std::string{}) !=
                "cohavora-stability-ledger" ||
            ledger.value("version", 0u) != 1u ||
            !ledger.at("records").is_array()) return summary;
        for (const auto& record : ledger.at("records")) {
            if (!record.is_object() ||
                record.value("kind", std::string{}) != "process" ||
                record.value("id", std::string{}) != run_id) continue;
            const auto recorded_build = record.value("build_id", std::string{});
            if (build_id != "unknown" && recorded_build != build_id) {
                return {{"availability", "build_mismatch"}};
            }
            const auto status = record.value("status", std::string{});
            static const std::set<std::string> allowed{
                "STARTED", "CLEAN_EXIT", "UNKNOWN_TERMINATION",
                "CONFIRMED_CRASH"};
            if (!allowed.contains(status)) return summary;
            summary = {{"availability", "valid"}, {"status", status},
                {"started_utc_ms", record.value("started_utc_ms", std::int64_t{0})},
                {"terminal_utc_ms", record.value("terminal_utc_ms", std::int64_t{0})},
                {"crash_evidence_checked", record.value("crash_evidence_checked", false)}};
            break;
        }
    } catch (...) {}
    return summary;
}

Json ReadStabilitySession(const std::filesystem::path& report_root,
                          std::string_view run_id,
                          std::string_view session_id) {
    Json summary{{"availability", "missing"}};
    if (!IsId(run_id) || !IsId(session_id)) return summary;
    const auto path = report_root.parent_path() / "stability-ledger-v1.json";
    if (!SafeFile(path, 256 * 1024)) return summary;
    try {
        std::ifstream input(path, std::ios::binary);
        const auto ledger = Json::parse(input);
        if (!input || ledger.value("schema", std::string{}) !=
                "cohavora-stability-ledger" ||
            ledger.value("version", 0u) != 1u ||
            !ledger.at("records").is_array()) return summary;
        for (const auto& record : ledger.at("records")) {
            if (!record.is_object() ||
                record.value("kind", std::string{}) != "session" ||
                record.value("id", std::string{}) != session_id ||
                record.value("process_run_id", std::string{}) != run_id)
                continue;
            const auto status = record.value("status", std::string{});
            static const std::set<std::string> allowed{
                "STARTED", "COMPLETED", "DEGRADED_COMPLETED",
                "ADMISSION_FAILURE", "TIMEOUT", "CANCELLED", "STOPPED"};
            if (!allowed.contains(status)) return summary;
            return {{"availability", "valid"}, {"status", status},
                {"started_utc_ms", record.value("started_utc_ms", std::int64_t{0})},
                {"terminal_utc_ms", record.value("terminal_utc_ms", std::int64_t{0})}};
        }
    } catch (...) {}
    return summary;
}

Json ReadLosses(const std::filesystem::path& report_root,
                std::string_view session_id) {
    Json summary{{"availability", "missing"}};
    const auto path = report_root / "losses-v1.json";
    if (!SafeFile(path, 64 * 1024)) return summary;
    try {
        std::ifstream input(path, std::ios::binary);
        const auto losses = Json::parse(input);
        if (!input || losses.value("schema", std::string{}) !=
                "cohavora-telemetry-losses" ||
            losses.value("schema_version", 0u) != 1u ||
            !losses.at("sessions").is_array() ||
            losses.at("sessions").size() > 128) return summary;
        summary = {{"availability", "valid"}, {"dropped_records", 0},
            {"first_revision", nullptr}, {"last_revision", nullptr}};
        for (const auto& item : losses.at("sessions")) {
            if (item.value("anonymous_session_id", std::string{}) != session_id)
                continue;
            summary["dropped_records"] = item.at("dropped_records").get<std::uint64_t>();
            summary["first_revision"] = item.at("first_revision").get<std::uint64_t>();
            summary["last_revision"] = item.at("last_revision").get<std::uint64_t>();
            summary["first_source_utc_ms"] = item.value("first_source_utc_ms", std::int64_t{0});
            summary["last_source_utc_ms"] = item.value("last_source_utc_ms", std::int64_t{0});
            break;
        }
    } catch (...) { return {{"availability", "invalid"}}; }
    return summary;
}

bool IsSegment(std::string_view name) {
    return name.size() == 34 && name.starts_with("segment-") &&
        name.ends_with(".jsonl") &&
        std::all_of(name.begin() + 8, name.begin() + 28,
            [](char ch) { return ch >= '0' && ch <= '9'; });
}

std::string NewId() {
    std::random_device source;
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (char& ch : result) ch = digits[source() & 15];
    return result;
}

std::string HashFile(const std::filesystem::path& path) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    std::ifstream input(path, std::ios::binary);
    if (!digest || !input ||
        EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return {};
    std::array<char, 64 * 1024> chunk{};
    while (input.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
           input.gcount() > 0) {
        if (EVP_DigestUpdate(digest.get(), chunk.data(),
                static_cast<std::size_t>(input.gcount())) != 1) return {};
    }
    if (input.bad()) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned size = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &size) != 1 || size != 32)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned i = 0; i < size; ++i) {
        result.push_back(digits[hash[i] >> 4]);
        result.push_back(digits[hash[i] & 15]);
    }
    return result;
}

bool WriteJson(const std::filesystem::path& path, const Json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2);
    output.flush();
    return static_cast<bool>(output);
}

const std::set<std::string>& MetricKeys() {
    static const auto keys = [] {
        std::set<std::string> result;
        for (const auto& row : BuildSafeMetricRows(SafeTelemetryRecord{}))
            result.insert(row.key);
        return result;
    }();
    return keys;
}

const std::string& MetricUnit(const std::string& key) {
    static const auto units = [] {
        std::map<std::string, std::string> result;
        for (const auto& row : BuildSafeMetricRows(SafeTelemetryRecord{}))
            result.emplace(row.key, row.unit);
        return result;
    }();
    static const std::string empty;
    const auto found = units.find(key);
    return found == units.end() ? empty : found->second;
}

bool IsAvailability(std::string_view value) {
    constexpr std::array names{"VALID", "WARMING_UP", "NOT_EXPECTED",
        "UNSUPPORTED", "TIMEOUT", "STALE", "INVALID", "UNKNOWN"};
    return std::any_of(names.begin(), names.end(),
        [&](const char* name) { return value == name; });
}

} // namespace

TelemetryExportResult WriteTelemetryDiagnosticBundle(
    const std::filesystem::path& report_root,
    const TelemetryReportEntry& report,
    const std::filesystem::path& diagnostic_root,
    const std::filesystem::path& destination_root,
    const std::shared_ptr<std::atomic_bool>& cancelled,
    TelemetryExportRange range) {
    TelemetryExportResult result;
    result.reason = "invalid_report_id";
    bool legacy = false;
    const auto session_id = SessionFromReportId(report.record_id, legacy);
    if (session_id.empty()) return result;
    if (range.first_utc_ms < 0 || range.last_utc_ms < 0 ||
        (range.first_utc_ms && range.last_utc_ms &&
         range.first_utc_ms > range.last_utc_ms)) {
        result.reason = "invalid_time_range";
        return result;
    }
    const auto source = report_root / report.record_id;
    const auto is_cancelled = [&] {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };
    std::filesystem::path temporary;
    bool created = false;
    const auto cleanup = [&] {
        if (!created) return;
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
    };
    try {
        std::error_code error;
        if (std::filesystem::is_symlink(report_root, error) || error ||
            std::filesystem::is_symlink(source, error) || error ||
            !std::filesystem::is_directory(source, error) || error) {
            result.reason = "source_path_invalid";
            return result;
        }
        std::unique_ptr<TelemetryCheckpointReadLease> reader;
        std::unique_ptr<LegacyReportReadLease> legacy_reader;
        TelemetryCheckpointStatus checkpoint;
        if (!legacy) {
            reader = std::make_unique<TelemetryCheckpointReadLease>(source);
            if (!reader->acquired()) {
                result.reason = "report_busy";
                return result;
            }
            checkpoint = InspectTelemetryCheckpoint(source);
            if (!checkpoint.valid ||
                checkpoint.anonymous_session_id != session_id) {
                result.reason = checkpoint.reason;
                return result;
            }
        }
        if (legacy) {
            legacy_reader = std::make_unique<LegacyReportReadLease>(
                source / "manifest.json");
            if (!legacy_reader->acquired()) {
                result.reason = "report_busy";
                return result;
            }
        }
        if (!SafeFile(source / "manifest.json", 1024 * 1024)) {
            result.reason = "manifest_unreadable";
            return result;
        }
        std::ifstream source_manifest(source / "manifest.json", std::ios::binary);
        Json index;
        source_manifest >> index;
        if (!source_manifest || !index.is_object()) {
            result.reason = "manifest_unreadable";
            return result;
        }
        if (legacy) {
            if (index.value("schema", std::string{}) !=
                    kTelemetryReportSchema ||
                index.value("schema_version", 0u) !=
                    kTelemetryReportSchemaVersion ||
                index.value("anonymous_session_id", std::string{}) != session_id ||
                index.value("integrity", Json::object()).value(
                    "status", std::string{}) != "complete" ||
                !SafeFile(source / "metrics.jsonl", 100ull * 1024 * 1024)) {
                result.reason = "legacy_report_invalid";
                return result;
            }
            checkpoint.valid = true;
            checkpoint.anonymous_session_id = session_id;
            checkpoint.session_complete = index.value("session_complete", false);
            checkpoint.record_count = index.value("record_count", std::uint64_t{0});
            checkpoint.first_retained_source_utc_ms =
                index.value("first_sample_utc_ms", std::int64_t{0});
            checkpoint.last_retained_source_utc_ms =
                index.value("last_sample_utc_ms", std::int64_t{0});
            checkpoint.size_bytes = std::filesystem::file_size(
                source / "manifest.json", error);
            if (error) {
                result.reason = "legacy_report_invalid";
                return result;
            }
            checkpoint.size_bytes += std::filesystem::file_size(
                source / "metrics.jsonl", error);
            if (error || checkpoint.size_bytes > 100ull * 1024 * 1024) {
                result.reason = "legacy_report_oversized";
                return result;
            }
            index["segments"] = Json::array({{{"file", "metrics.jsonl"}}});
        } else if (!index.contains("segments") ||
                   !index["segments"].is_array()) {
            result.reason = "manifest_unreadable";
            return result;
        }
        if (is_cancelled()) {
            result.cancelled = true;
            result.reason = "export_cancelled";
            return result;
        }
        const bool destination_exists =
            std::filesystem::exists(destination_root, error);
        if (error || (destination_exists &&
            std::filesystem::is_symlink(destination_root, error)) || error) {
            result.reason = "destination_path_invalid";
            return result;
        }
        std::filesystem::create_directories(destination_root, error);
        if (error) {
            result.reason = "destination_not_writable";
            return result;
        }
        const auto free_space = std::filesystem::space(destination_root, error);
        if (!error && free_space.available < checkpoint.size_bytes + 4 * 1024 * 1024) {
            result.reason = "destination_space_insufficient";
            return result;
        }
        const auto suffix = NewId();
        temporary = destination_root / (".cohavora-bundle-tmp-" + suffix);
        const auto final = destination_root / ("cohavora-diagnostic-bundle-" + suffix);
        if (std::filesystem::exists(temporary, error) || error ||
            std::filesystem::exists(final, error) || error) {
            result.reason = "destination_collision";
            return result;
        }
        created = std::filesystem::create_directory(temporary, error);
        if (!created || error) {
            result.reason = "destination_not_writable";
            return result;
        }
        const auto session = temporary / "sessions" / session_id;
        std::filesystem::create_directories(session / "telemetry", error);
        if (error) {
            cleanup();
            result.reason = "destination_not_writable";
            return result;
        }
        std::uint64_t omitted_metric_rows = 0;
        std::uint64_t omitted_string_values = 0;
        std::uint64_t omitted_by_range = 0;
        std::ofstream metrics(session / "telemetry" / "metrics.jsonl",
            std::ios::binary | std::ios::trunc);
        if (!metrics) {
            cleanup();
            result.reason = "write_failed";
            return result;
        }
        for (const auto& segment : index["segments"]) {
            const auto name = segment.at("file").get<std::string>();
            if (legacy ? name != "metrics.jsonl" : !IsSegment(name)) {
                result.reason = "segment_index_invalid";
                break;
            }
            if (!SafeFile(source / name, 100ull * 1024 * 1024)) {
                result.reason = "segment_missing_or_corrupt";
                break;
            }
            std::ifstream input(source / name, std::ios::binary);
            if (!input) {
                result.reason = "segment_missing_or_corrupt";
                break;
            }
            std::array<char, 16 * 1024 + 2> line_buffer{};
            for (;;) {
                input.getline(line_buffer.data(),
                    static_cast<std::streamsize>(line_buffer.size()));
                const auto extracted = input.gcount();
                if (input.fail() && !input.eof()) {
                    result.reason = "metric_line_oversized";
                    break;
                }
                if (extracted == 0) break;
                const auto length = std::char_traits<char>::length(
                    line_buffer.data());
                const auto expected = static_cast<std::size_t>(
                    input.eof() ? extracted : extracted - 1);
                if (length != expected || length > 16 * 1024) {
                    result.reason = "metric_line_invalid";
                    break;
                }
                if (is_cancelled()) {
                    result.cancelled = true;
                    result.reason = "export_cancelled";
                    break;
                }
                const auto row = Json::parse(std::string_view(
                    line_buffer.data(), length));
                const auto source_time = legacy
                    ? row.at("captured_utc_ms").get<std::int64_t>()
                    : row.at("source_utc_ms").get<std::int64_t>();
                if ((range.first_utc_ms && source_time < range.first_utc_ms) ||
                    (range.last_utc_ms && source_time > range.last_utc_ms)) {
                    ++omitted_by_range;
                    continue;
                }
                const auto key = row.at("key").get<std::string>();
                if (!MetricKeys().contains(key)) {
                    ++omitted_metric_rows;
                    continue;
                }
                auto value = row.at("value");
                if (value.is_string() || value.is_object() || value.is_array()) {
                    value = nullptr;
                    ++omitted_string_values;
                }
                const auto availability = row.value("availability", std::string{});
                const Json safe{
                    {"source_utc_ms", source_time},
                    {"source_monotonic_us", legacy ? Json(nullptr) : Json(
                        row.at("source_monotonic_us").get<std::uint64_t>())},
                    {"session_generation", row.at("session_generation").get<std::uint64_t>()},
                    {"revision", row.at("revision").get<std::uint64_t>()},
                    {"key", key}, {"value", value},
                    {"unit", MetricUnit(key)},
                    {"availability", IsAvailability(availability)
                        ? availability : "UNKNOWN"},
                };
                metrics << safe.dump() << '\n';
                if (!metrics) {
                    result.reason = "write_failed";
                    break;
                }
                if (metrics.tellp() > static_cast<std::streamoff>(
                        kMaximumBundleBytes - 4 * 1024 * 1024)) {
                    result.reason = "bundle_budget_exceeded";
                    break;
                }
                if (input.eof()) break;
            }
            if (result.reason != "invalid_report_id") break;
            if (input.bad()) {
                result.reason = "segment_read_failed";
                break;
            }
        }
        metrics.flush();
        if (!metrics && result.reason == "invalid_report_id")
            result.reason = "write_failed";
        metrics.close();
        if (result.reason != "invalid_report_id") {
            cleanup();
            return result;
        }
        const auto timeline = checkpoint.process_run_id.empty()
            ? OperationTimeline{} : ReadOperationTimeline(
                diagnostic_root, session_id, 1024, checkpoint.process_run_id);
        std::ofstream events(session / "events.jsonl",
            std::ios::binary | std::ios::trunc);
        std::uint64_t omitted_events_by_range = 0;
        for (const auto& event : timeline.entries) {
            if ((range.first_utc_ms &&
                 event.occurred_at_utc_ms < range.first_utc_ms) ||
                (range.last_utc_ms &&
                 event.occurred_at_utc_ms > range.last_utc_ms)) {
                ++omitted_events_by_range;
                continue;
            }
            events << Json{
                {"event_name", event.event_name},
                {"process_run_id", event.process_run_id},
                {"operation_id", event.operation_id},
                {"parent_operation_id", event.parent_operation_id},
                {"request_id", event.request_id},
                {"stage", event.stage}, {"outcome", event.outcome},
                {"error_code", event.error_code},
                {"error_layer", event.error_layer},
                {"leave_reason", event.leave_reason},
                {"media_kind", event.media_kind},
                {"measurement_point", event.measurement_point},
                {"endpoint_id", event.endpoint_id},
                {"previous_endpoint_id", event.previous_endpoint_id},
                {"session_generation", event.session_generation},
                {"room_generation", event.room_generation},
                {"recovery_epoch", event.recovery_epoch},
                {"expected_endpoints", event.expected_endpoints},
                {"http_status", event.http_status},
                {"occurred_at_utc_ms", event.occurred_at_utc_ms},
                {"monotonic_us", event.monotonic_us},
                {"event_sequence", event.event_sequence},
                {"attempt", event.attempt},
            }.dump() << '\n';
        }
        events.flush();
        if (!events || is_cancelled()) {
            result.cancelled = is_cancelled();
            result.reason = result.cancelled ? "export_cancelled" : "write_failed";
            events.close();
            cleanup();
            return result;
        }
        events.close();
        const bool application_history = report_root.filename() == "reports" &&
            report_root.parent_path().filename() == "telemetry";
        const auto stability = application_history
            ? ReadStability(report_root, checkpoint.process_run_id,
                timeline.build_id)
            : Json{{"availability", "missing"}};
        const auto stability_session = application_history
            ? ReadStabilitySession(report_root, checkpoint.process_run_id,
                session_id)
            : Json{{"availability", "missing"}};
        auto crash = Json{{"availability", "missing"}};
        if (stability.value("status", std::string{}) == "CLEAN_EXIT")
            crash = {{"availability", "not_expected"}};
        if (application_history &&
            IsId(checkpoint.process_run_id) &&
            timeline.build_id != "unknown") {
            const auto scan = CrashEvidenceProvider::Scan(
                report_root.parent_path().parent_path() / "crash-evidence");
            for (const auto& evidence : scan.records) {
                if (evidence.process_run_id != checkpoint.process_run_id ||
                    evidence.build_id != timeline.build_id) continue;
                crash = {{"availability",
                    stability.value("status", std::string{}) ==
                        "CONFIRMED_CRASH" ? "confirmed" : "unconfirmed"},
                    {"exception_code", evidence.code},
                    {"main_module_address", evidence.main_module_address},
                    {"main_module_rva", evidence.main_module_rva}};
                break;
            }
        }
        const auto losses = ReadLosses(report_root, session_id);
        const Json stability_summary{
            {"process_run", stability}, {"crash_metadata", crash},
            {"session_detail", stability_session},
        };
        const Json checkpoint_summary{
            {"schema", "cohavora-telemetry-checkpoint-summary"},
            {"schema_version", 1},
            {"anonymous_session_id", session_id},
            {"process_run_id", checkpoint.process_run_id},
            {"session_generation", checkpoint.session_generation},
            {"session_complete", checkpoint.session_complete},
            {"last_committed_revision", checkpoint.last_committed_revision},
            {"record_count", checkpoint.record_count},
            {"pruned_records", checkpoint.pruned_records},
            {"pruned_segments", checkpoint.pruned_segments},
            {"pruned_through_utc_ms", checkpoint.pruned_through_utc_ms},
            {"missing_revisions", checkpoint.missing_revisions},
            {"missing_ranges_omitted", checkpoint.missing_ranges_omitted},
            {"first_retained_source_utc_ms", checkpoint.first_retained_source_utc_ms},
            {"last_retained_source_utc_ms", checkpoint.last_retained_source_utc_ms},
            {"omitted_metric_rows", omitted_metric_rows},
            {"omitted_string_values", omitted_string_values},
            {"omitted_by_selected_range", omitted_by_range},
            {"source_format", legacy ? "v1" : "v2"},
            {"source_integrity", legacy ? "legacy_unverified" : "sha256_verified"},
        };
        if (!WriteJson(session / "telemetry" / "checkpoint.json", checkpoint_summary) ||
            !WriteJson(session / "stability.json", stability_summary) ||
            !WriteJson(temporary / "build.json", Json{
                {"build_id", timeline.build_id},
                {"process_run_id", checkpoint.process_run_id},
                {"symbol_identity", timeline.symbol_identity}}) ||
            !WriteJson(temporary / "environment.json", Json{
                {"platform", "windows"}, {"architecture", "x64"},
                {"render_backend", "unknown"},
                {"device_capabilities", "unknown"}}) ||
            !WriteJson(temporary / "diagnostics-health.json", Json{
                {"timeline_available", timeline.source_available},
                {"timeline_omitted", timeline.omitted},
                {"timeline_invalid_lines", timeline.invalid_lines},
                {"timeline_skipped_files", timeline.skipped_files},
                {"checkpoint_missing_revisions", checkpoint.missing_revisions},
                {"checkpoint_pruned_records", checkpoint.pruned_records},
                {"persistent_losses", losses},
                {"omitted_by_selected_range", omitted_by_range},
                {"timeline_omitted_by_selected_range", omitted_events_by_range}})) {
            result.reason = "write_failed";
            cleanup();
            return result;
        }
        Json files = Json::array();
        std::uint64_t bundle_bytes = 0;
        const std::array relative{
            std::filesystem::path("build.json"),
            std::filesystem::path("environment.json"),
            std::filesystem::path("diagnostics-health.json"),
            std::filesystem::path("sessions") / session_id / "events.jsonl",
            std::filesystem::path("sessions") / session_id / "stability.json",
            std::filesystem::path("sessions") / session_id / "telemetry" /
                "checkpoint.json",
            std::filesystem::path("sessions") / session_id / "telemetry" /
                "metrics.jsonl",
        };
        for (const auto& path : relative) {
            if (is_cancelled()) {
                result.cancelled = true;
                result.reason = "export_cancelled";
                cleanup();
                return result;
            }
            const auto hash = HashFile(temporary / path);
            const auto size = std::filesystem::file_size(temporary / path, error);
            if (hash.empty() || error) {
                result.reason = "bundle_hash_failed";
                cleanup();
                return result;
            }
            if (size > kMaximumBundleBytes - bundle_bytes) {
                result.reason = "bundle_budget_exceeded";
                cleanup();
                return result;
            }
            bundle_bytes += size;
            files.push_back({{"path", path.generic_string()},
                {"size_bytes", size}, {"sha256", hash}});
        }
        Json missing = Json::array();
        if (timeline.build_id == "unknown") missing.push_back("build_id");
        if (timeline.symbol_identity == "unknown")
            missing.push_back("symbol_identity");
        if (!timeline.source_available) missing.push_back("operation_timeline");
        if (stability.value("availability", std::string{}) != "valid")
            missing.push_back("historical_stability_run_detail");
        if (stability_session.value("availability", std::string{}) != "valid")
            missing.push_back("historical_stability_session_link");
        if (crash.value("availability", std::string{}) == "missing")
            missing.push_back("confirmed_crash_metadata");
        if (losses.value("availability", std::string{}) != "valid")
            missing.push_back("persistent_loss_summary");
        if (legacy) missing.push_back("legacy_source_integrity_and_run_identity");
        const bool has_reconnect = std::any_of(
            timeline.entries.begin(), timeline.entries.end(),
            [](const auto& event) {
                return event.event_name.starts_with("reconnect.");
            });
        bool has_milestone = false;
        bool endpoints_complete = timeline.omitted == 0 &&
            timeline.invalid_lines == 0 && timeline.skipped_files == 0 &&
            omitted_events_by_range == 0;
        for (const auto& milestone : timeline.entries) {
            if (milestone.event_name == "media.recovery.timeout")
                endpoints_complete = false;
            if (milestone.event_name != "media.recovery.milestone") continue;
            has_milestone = true;
            if (!milestone.has_expected_endpoints) {
                endpoints_complete = false;
                continue;
            }
            std::set<std::string> recovered;
            for (const auto& endpoint : timeline.entries) {
                if (endpoint.event_name == "media.endpoint.recovered" &&
                    endpoint.operation_id == milestone.operation_id &&
                    endpoint.session_generation == milestone.session_generation &&
                    endpoint.recovery_epoch == milestone.recovery_epoch &&
                    endpoint.media_kind == milestone.media_kind &&
                    endpoint.measurement_point == milestone.measurement_point)
                    recovered.insert(endpoint.endpoint_id);
            }
            if (recovered.size() != milestone.expected_endpoints)
                endpoints_complete = false;
        }
        if (has_reconnect && (!has_milestone || !endpoints_complete))
            missing.push_back("recovered_media_endpoint_identity");
        if (!WriteJson(temporary / "manifest.json", Json{
                {"schema", "cohavora-diagnostic-bundle"},
                {"schema_version", 1},
                {"anonymous_session_id", session_id},
                {"process_run_id", checkpoint.process_run_id},
                {"selected_first_utc_ms", range.first_utc_ms},
                {"selected_last_utc_ms", range.last_utc_ms},
                {"source_format", legacy ? "v1" : "v2"},
                {"omitted_metrics_by_selected_range", omitted_by_range},
                {"omitted_events_by_selected_range", omitted_events_by_range},
                {"session_complete", checkpoint.session_complete},
                {"files", files}, {"missing", missing},
                {"includes_memory_dump", false},
                {"default_upload", false}})) {
            result.reason = "write_failed";
            cleanup();
            return result;
        }
        std::filesystem::rename(temporary, final, error);
        if (error) {
            result.reason = "atomic_replace_failed";
            cleanup();
            return result;
        }
        created = false;
        result.success = true;
        result.reason = "export_complete";
        result.report_directory = final;
        return result;
    } catch (...) {
        cleanup();
        result.reason = "export_exception";
        return result;
    }
}

} // namespace livekit::telemetry
