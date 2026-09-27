#include "telemetry_checkpoint.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;
constexpr std::string_view kDirectoryPrefix = "cohavora-telemetry-v2-";
constexpr std::size_t kMaximumManifestBytes = 1024 * 1024;
constexpr std::size_t kMaximumSegmentBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumSegments = 4096;

bool IsOpaqueId(std::string_view value) noexcept {
    if (value.size() != 32) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::string SegmentName(std::uint64_t revision) {
    std::ostringstream output;
    output << "segment-" << std::setw(20) << std::setfill('0') << revision
           << ".jsonl";
    return output.str();
}

bool IsSegmentFile(std::string_view name) noexcept {
    if (name.size() != 34 || !name.starts_with("segment-") ||
        !name.ends_with(".jsonl")) return false;
    return std::all_of(name.begin() + 8, name.begin() + 28,
        [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool IsOwnedFile(std::string_view name) noexcept {
    if (name == "manifest.json" || name == "manifest.json.tmp" ||
        name == "writer.lock" || IsSegmentFile(name)) return true;
    return name.size() > 4 && name.ends_with(".tmp") &&
        IsSegmentFile(name.substr(0, name.size() - 4));
}

bool IsLegacyOwnedFile(std::string_view name) noexcept {
    return name == "manifest.json" || name == "session.json" ||
        name == "metrics.jsonl" || name == "metrics.csv" ||
        name == "manifest.json.tmp" || name == "session.json.tmp" ||
        name == "metrics.jsonl.tmp" || name == "metrics.csv.tmp";
}

std::uint64_t SaturatingAdd(std::uint64_t left,
                            std::uint64_t right) noexcept {
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
        ? (std::numeric_limits<std::uint64_t>::max)() : left + right;
}

std::optional<std::uint64_t> OwnedBytes(const std::filesystem::path& root) {
    std::error_code error;
    if (std::filesystem::is_symlink(root, error) || error ||
        !std::filesystem::is_directory(root, error) || error) return std::nullopt;
    std::uint64_t total = 0;
    std::size_t directories = 0;
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name == "losses-v1.json" || name == "losses-v1.json.tmp") {
            if (it->is_symlink(error) || error ||
                !it->is_regular_file(error) || error) return std::nullopt;
            total = SaturatingAdd(total, it->file_size(error));
            if (error) return std::nullopt;
            continue;
        }
        const bool v2 = name.starts_with("cohavora-telemetry-v2-");
        const bool v1 = name.starts_with("cohavora-telemetry-v1-") ||
            (name.starts_with(".cohavora-telemetry-tmp-") &&
             IsOpaqueId(std::string_view(name).substr(24)));
        if (!v1 && !v2) continue;
        if (it->is_symlink(error) || error || !it->is_directory(error) || error ||
            ++directories > 4096) return std::nullopt;
        for (std::filesystem::directory_iterator files(it->path(), error);
             !error && files != end; files.increment(error)) {
            const auto file = files->path().filename().string();
            if (!(v2 ? IsOwnedFile(file) : IsLegacyOwnedFile(file))) continue;
            if (files->is_symlink(error) || error ||
                !files->is_regular_file(error) || error) return std::nullopt;
            const auto size = files->file_size(error);
            if (error || size > (std::numeric_limits<std::uint64_t>::max)() - total)
                return std::nullopt;
            total += size;
        }
        if (error) return std::nullopt;
    }
    return error ? std::nullopt : std::optional<std::uint64_t>(total);
}

std::string Sha256(std::string_view content) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(content.data()),
           content.size(), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

bool ReadBounded(const std::filesystem::path& path, std::size_t maximum,
                 std::string& content) {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || error ||
        !std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    content.resize(static_cast<std::size_t>(size));
    input.read(content.data(), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size) &&
        !input.bad() && input.peek() == std::char_traits<char>::eof();
}

bool CommitManifest(const std::filesystem::path& temporary,
                    const std::filesystem::path& final) {
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(), final.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, final, error);
    return !error;
#endif
}

bool WriteText(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    return static_cast<bool>(output);
}

TelemetryCheckpointStatus Invalid(std::string reason) {
    TelemetryCheckpointStatus result;
    result.reason = std::move(reason);
    return result;
}

class WriterLease final {
public:
    explicit WriterLease(const std::filesystem::path& directory,
                         const std::filesystem::path& name = L"writer.lock") {
#if defined(_WIN32)
        handle_ = CreateFileW((directory / name).c_str(),
                              GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        (void)directory;
        (void)name;
#endif
    }
    ~WriterLease() { Release(); }
    void Release() noexcept {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#endif
    }
    bool acquired() const noexcept {
#if defined(_WIN32)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return true;
#endif
    }
private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

} // namespace

TelemetryCheckpointReadLease::TelemetryCheckpointReadLease(
    const std::filesystem::path& directory) {
    std::error_code error;
    if (std::filesystem::is_symlink(directory, error) || error ||
        !std::filesystem::is_directory(directory, error) || error) return;
#if defined(_WIN32)
    const auto handle = CreateFileW((directory / L"writer.lock").c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    handle_ = handle;
#else
    (void)directory;
#endif
    acquired_ = true;
}

TelemetryCheckpointReadLease::~TelemetryCheckpointReadLease() {
#if defined(_WIN32)
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#endif
}

bool TelemetryCheckpointReadLease::acquired() const noexcept {
    return acquired_;
}

TelemetryRunLease::TelemetryRunLease(
    const std::filesystem::path& root, std::string_view run_id) {
    if (!IsOpaqueId(run_id)) return;
    std::error_code error;
    const bool exists = std::filesystem::exists(root, error);
    if (error || (exists && std::filesystem::is_symlink(root, error)) || error)
        return;
    std::filesystem::create_directories(root, error);
    if (error) return;
    path_ = root / ("run-" + std::string(run_id) + ".lock");
#if defined(_WIN32)
    const auto handle = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    handle_ = handle;
#endif
    acquired_ = true;
}

TelemetryRunLease::~TelemetryRunLease() {
    if (!acquired_) return;
#if defined(_WIN32)
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#endif
    std::error_code error;
    std::filesystem::remove(path_, error);
}

bool TelemetryRunLease::acquired() const noexcept { return acquired_; }

bool IsTelemetryRunActive(const std::filesystem::path& root,
                          std::string_view run_id) {
    if (!IsOpaqueId(run_id)) return true;
    const auto path = root / ("run-" + std::string(run_id) + ".lock");
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return true;
    if (!exists) return false;
    if (std::filesystem::is_symlink(path, error) || error) return true;
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return true;
    CloseHandle(handle);
#endif
    return false;
}

TelemetryCheckpointRecord MakeTelemetryCheckpointRecord(
    const SafeTelemetryRecord& record) {
    return {
        .generation = record.snapshot.session_generation,
        .revision = record.snapshot.revision,
        .source_utc_ms = record.source_utc_ms,
        .complete = record.complete,
        .jsonl = SerializeSafeTelemetryCheckpointRecord(record),
    };
}

TelemetryCheckpointStatus InspectTelemetryCheckpoint(
    const std::filesystem::path& directory) {
    try {
        std::error_code error;
        if (std::filesystem::is_symlink(directory, error) || error ||
            !std::filesystem::is_directory(directory, error) || error)
            return Invalid("checkpoint_directory_invalid");
        const auto name = directory.filename().string();
        if (!name.starts_with(kDirectoryPrefix) ||
            !IsOpaqueId(std::string_view(name).substr(kDirectoryPrefix.size())))
            return Invalid("invalid_directory_id");
        std::string content;
        if (!ReadBounded(directory / "manifest.json", kMaximumManifestBytes, content))
            return Invalid("manifest_missing_or_oversized");
        const auto manifest = Json::parse(content);
        if (manifest.is_object() &&
            manifest.value("schema", std::string{}) ==
                "cohavora-telemetry-checkpoint" &&
            manifest.value("schema_version", 0u) > 2u)
            return Invalid("unsupported_version");
        if (!manifest.is_object() ||
            manifest.value("schema", std::string{}) != "cohavora-telemetry-checkpoint" ||
            manifest.value("schema_version", 0u) != 2u ||
            manifest.value("anonymous_session_id", std::string{}) !=
                name.substr(kDirectoryPrefix.size()) ||
            !manifest.contains("segments") || !manifest["segments"].is_array() ||
            manifest["segments"].size() > kMaximumSegments)
            return Invalid("manifest_incompatible");
        TelemetryCheckpointStatus result;
        result.anonymous_session_id = manifest.at("anonymous_session_id").get<std::string>();
        result.process_run_id = manifest.value("process_run_id", std::string{});
        if (!result.process_run_id.empty() &&
            !IsOpaqueId(result.process_run_id))
            return Invalid("run_id_invalid");
        result.session_generation = manifest.at("session_generation").get<std::uint64_t>();
        result.created_utc_ms = manifest.at("created_utc_ms").get<std::int64_t>();
        result.record_count = manifest.value("record_count", std::uint64_t{0});
        result.pruned_records = manifest.value("pruned_records", std::uint64_t{0});
        result.pruned_segments = manifest.value("pruned_segments", std::uint64_t{0});
        result.pruned_through_utc_ms =
            manifest.value("pruned_through_utc_ms", std::int64_t{0});
        result.missing_revisions =
            manifest.value("missing_revisions", std::uint64_t{0});
        result.missing_ranges_omitted =
            manifest.value("missing_ranges_omitted", std::uint64_t{0});
        if (manifest.contains("missing_ranges") &&
            (!manifest["missing_ranges"].is_array() ||
             manifest["missing_ranges"].size() > 128))
            return Invalid("missing_ranges_invalid");
        result.last_committed_revision = manifest.at("last_committed_revision").get<std::uint64_t>();
        result.session_complete = manifest.value("session_complete", false);
        std::uint64_t previous_revision = 0;
        std::set<std::string> names;
        for (const auto& segment : manifest["segments"]) {
            const auto file = segment.at("file").get<std::string>();
            const auto first = segment.at("first_revision").get<std::uint64_t>();
            const auto last = segment.at("last_revision").get<std::uint64_t>();
            const auto expected_size = segment.at("size_bytes").get<std::uint64_t>();
            const auto expected_hash = segment.at("sha256").get<std::string>();
            if (first <= previous_revision || last < first ||
                file != SegmentName(last) || !names.insert(file).second ||
                expected_size == 0 || expected_size > kMaximumSegmentBytes ||
                expected_hash.size() != SHA256_DIGEST_LENGTH * 2)
                return Invalid("segment_index_invalid");
            std::string payload;
            if (!ReadBounded(directory / file, kMaximumSegmentBytes, payload) ||
                payload.size() != expected_size || payload.back() != '\n' ||
                Sha256(payload) != expected_hash)
                return Invalid("segment_missing_or_corrupt");
            std::istringstream lines(payload);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) return Invalid("segment_line_invalid");
                const auto row = Json::parse(line);
                if (!row.is_object() ||
                    row.at("revision").get<std::uint64_t>() < first ||
                    row.at("revision").get<std::uint64_t>() > last ||
                    row.at("session_generation").get<std::uint64_t>() !=
                        result.session_generation)
                    return Invalid("segment_line_invalid");
            }
            previous_revision = last;
            if (result.segment_count == 0)
                result.first_retained_source_utc_ms =
                    segment.value("first_source_utc_ms", std::int64_t{0});
            result.last_retained_source_utc_ms =
                segment.value("last_source_utc_ms", std::int64_t{0});
            result.size_bytes += expected_size;
            ++result.segment_count;
        }
        if (previous_revision != result.last_committed_revision ||
            (result.segment_count == 0 && result.last_committed_revision != 0))
            return Invalid("revision_watermark_invalid");
        result.size_bytes = 0;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            const auto file = it->path().filename().string();
            if (!IsOwnedFile(file)) continue;
            if (it->is_symlink(error) || error ||
                !it->is_regular_file(error) || error)
                return Invalid("owned_file_invalid");
            const auto size = it->file_size(error);
            if (error) return Invalid("owned_file_unreadable");
            result.size_bytes += size;
            if (file == "manifest.json.tmp" ||
                (file != "manifest.json" && file != "writer.lock" &&
                 !names.contains(file))) {
                ++result.orphan_files;
                result.orphan_bytes += size;
            }
        }
        if (error) return Invalid("checkpoint_scan_failed");
        result.valid = true;
        result.reason = result.session_complete ? "complete" : "checkpoint";
        return result;
    } catch (...) {
        return Invalid("checkpoint_parse_failed");
    }
}

TelemetryCheckpointResult AppendTelemetryCheckpoint(
    const std::filesystem::path& root,
    std::string_view anonymous_session_id,
    const std::vector<TelemetryCheckpointRecord>& records,
    std::uint64_t maximum_report_bytes,
    std::string_view process_run_id,
    std::uint64_t maximum_history_bytes) {
    TelemetryCheckpointResult result;
    result.reason = "no_records";
    if (records.empty()) return result;
    const auto& final = records.back();
    const std::string id(anonymous_session_id);
    if (!IsOpaqueId(id)) {
        result.reason = "invalid_session_id";
        return result;
    }
    if (!process_run_id.empty() && !IsOpaqueId(process_run_id)) {
        result.reason = "invalid_run_id";
        return result;
    }
    if (!std::all_of(records.begin(), records.end(), [&](const auto& item) {
            return item.generation == final.generation &&
                item.revision != 0 && !item.jsonl.empty() &&
                item.jsonl.back() == '\n' &&
                item.jsonl.size() <= kMaximumSegmentBytes;
        })) {
        result.reason = "incompatible_record_set";
        return result;
    }
    try {
        const auto directory = root / (std::string(kDirectoryPrefix) + id);
        result.report_directory = directory;
        std::error_code error;
        const bool root_exists = std::filesystem::exists(root, error);
        if (error || (root_exists && std::filesystem::is_symlink(root, error)) ||
            error) {
            result.reason = "root_link_rejected";
            return result;
        }
        error.clear();
        const bool directory_exists = std::filesystem::exists(directory, error);
        if (error || (directory_exists &&
            std::filesystem::is_symlink(directory, error)) || error) {
            result.reason = "directory_link_rejected";
            return result;
        }
        error.clear();
        std::filesystem::create_directories(root, error);
        if (error) {
            result.reason = "directory_unavailable";
            return result;
        }
        WriterLease root_lease(root, L"store.lock");
        if (!root_lease.acquired()) {
            result.reason = "store_busy";
            return result;
        }
        std::filesystem::create_directories(directory, error);
        if (error) {
            result.reason = "directory_unavailable";
            return result;
        }
        WriterLease lease(directory);
        if (!lease.acquired()) {
            result.reason = "writer_busy";
            return result;
        }
        const auto manifest_path = directory / "manifest.json";
        Json manifest;
        if (std::filesystem::exists(manifest_path)) {
            const auto inspected = InspectTelemetryCheckpoint(directory);
            if (!inspected.valid || inspected.session_generation !=
                    final.generation) {
                result.reason = inspected.valid ? "session_generation_mismatch" : inspected.reason;
                return result;
            }
            if (!inspected.process_run_id.empty() &&
                inspected.process_run_id != process_run_id) {
                result.reason = "process_run_mismatch";
                return result;
            }
            std::string content;
            if (!ReadBounded(manifest_path, kMaximumManifestBytes, content)) {
                result.reason = "manifest_unreadable";
                return result;
            }
            manifest = Json::parse(content);
        } else {
            manifest = {
                {"schema", "cohavora-telemetry-checkpoint"},
                {"schema_version", 2},
                {"definition_version", kTelemetryDefinitionVersion},
                {"anonymous_session_id", id},
                {"process_run_id", std::string(process_run_id)},
                {"session_generation", final.generation},
                {"created_utc_ms", final.source_utc_ms},
                {"record_count", 0},
                {"pruned_records", 0},
                {"pruned_segments", 0},
                {"pruned_through_utc_ms", 0},
                {"missing_revisions", 0},
                {"missing_ranges_omitted", 0},
                {"missing_ranges", Json::array()},
                {"last_committed_revision", 0},
                {"session_complete", false},
                {"segments", Json::array()},
            };
        }
        const auto watermark = manifest.at("last_committed_revision").get<std::uint64_t>();
        if (manifest.value("session_complete", false) &&
            final.revision > watermark) {
            result.reason = "session_already_complete";
            return result;
        }
        std::string payload;
        std::uint64_t first_revision = 0;
        std::uint64_t last_revision = watermark;
        std::int64_t previous_source = 0;
        if (!manifest["segments"].empty())
            previous_source = manifest["segments"].back().value(
                "last_source_utc_ms", std::int64_t{0});
        std::int64_t first_source = 0;
        std::int64_t last_source = 0;
        std::uint64_t added_records = 0;
        std::vector<std::string> obsolete_segments;
        std::filesystem::path segment_path;
        std::filesystem::path segment_temp;
        for (const auto& record : records) {
            if (record.revision <= watermark) continue;
            if (record.revision <= last_revision) {
                result.reason = "revision_order_invalid";
                return result;
            }
            if (record.revision - last_revision > 1) {
                const auto missing = record.revision - last_revision - 1;
                manifest["missing_revisions"] =
                    manifest.value("missing_revisions", std::uint64_t{0}) + missing;
                if (!manifest.contains("missing_ranges"))
                    manifest["missing_ranges"] = Json::array();
                if (manifest["missing_ranges"].size() < 128) {
                    manifest["missing_ranges"].push_back({
                        {"first_revision", last_revision + 1},
                        {"last_revision", record.revision - 1},
                        {"after_source_utc_ms", previous_source},
                        {"before_source_utc_ms", record.source_utc_ms},
                    });
                } else {
                    manifest["missing_ranges_omitted"] =
                        manifest.value("missing_ranges_omitted", std::uint64_t{0}) + 1;
                }
            }
            if (first_revision == 0) {
                first_revision = record.revision;
                first_source = record.source_utc_ms;
            }
            last_revision = record.revision;
            last_source = record.source_utc_ms;
            previous_source = record.source_utc_ms;
            ++added_records;
            payload += record.jsonl;
            if (payload.size() > kMaximumSegmentBytes) {
                result.reason = "segment_too_large";
                return result;
            }
        }
        if (!payload.empty()) {
            if (payload.size() > maximum_report_bytes) {
                result.reason = "report_budget_too_small";
                return result;
            }
            const auto name = SegmentName(last_revision);
            segment_path = directory / name;
            segment_temp = directory / (name + ".tmp");
            const auto hash = Sha256(payload);
            manifest["segments"].push_back({
                {"file", name}, {"size_bytes", payload.size()}, {"sha256", hash},
                {"first_revision", first_revision}, {"last_revision", last_revision},
                {"record_count", added_records},
                {"first_source_utc_ms", first_source},
                {"last_source_utc_ms", last_source},
            });
            manifest["last_committed_revision"] = last_revision;
            manifest["record_count"] =
                manifest.value("record_count", std::uint64_t{0}) + added_records;
            std::uint64_t retained_bytes = 0;
            for (const auto& segment : manifest["segments"])
                retained_bytes += segment.at("size_bytes").get<std::uint64_t>();
            while (manifest["segments"].size() > 1 &&
                   (manifest["segments"].size() > kMaximumSegments ||
                    retained_bytes > maximum_report_bytes)) {
                const auto removed = manifest["segments"].front();
                obsolete_segments.push_back(removed.at("file").get<std::string>());
                retained_bytes -= removed.at("size_bytes").get<std::uint64_t>();
                manifest["pruned_records"] =
                    manifest.value("pruned_records", std::uint64_t{0}) +
                    removed.value("record_count", std::uint64_t{0});
                manifest["pruned_segments"] =
                    manifest.value("pruned_segments", std::uint64_t{0}) + 1;
                manifest["pruned_through_utc_ms"] =
                    removed.at("last_source_utc_ms").get<std::int64_t>();
                manifest["segments"].erase(manifest["segments"].begin());
            }
        }
        if (final.complete && final.revision <= last_revision)
            manifest["session_complete"] = true;
        manifest["integrity"] = {
            {"status", manifest.value("session_complete", false)
                ? "complete" : "checkpoint"},
            {"segment_count", manifest["segments"].size()},
        };
        const auto manifest_temp = directory / "manifest.json.tmp";
        std::filesystem::remove(manifest_temp, error);
        error.clear();
        if (!segment_temp.empty()) {
            std::filesystem::remove(segment_temp, error);
            error.clear();
        }
        const auto manifest_content = manifest.dump(2);
        if (manifest_content.size() > kMaximumManifestBytes) {
            result.reason = "manifest_write_failed";
            return result;
        }
        const auto used = OwnedBytes(root);
        if (!used || *used > maximum_history_bytes ||
            payload.size() + manifest_content.size() >
                maximum_history_bytes - *used) {
            result.reason = "history_budget_exceeded";
            return result;
        }
        if (!payload.empty()) {
            bool already_committed = false;
            if (std::filesystem::exists(segment_path)) {
                std::string existing;
                already_committed = ReadBounded(segment_path,
                    kMaximumSegmentBytes, existing) && existing == payload;
            }
            if (!already_committed) {
                if (!WriteText(segment_temp, payload) ||
                    !CommitManifest(segment_temp, segment_path)) {
                    result.reason = "segment_commit_failed";
                    return result;
                }
            }
        }
        if (!WriteText(manifest_temp, manifest_content)) {
            result.reason = "manifest_write_failed";
            return result;
        }
        if (!CommitManifest(manifest_temp, manifest_path)) {
            result.reason = "manifest_commit_failed";
            return result;
        }
        for (const auto& name : obsolete_segments) {
            error.clear();
            std::filesystem::remove(directory / name, error);
        }
        result.status = InspectTelemetryCheckpoint(directory);
        result.success = result.status.valid;
        result.reason = result.success ? "checkpoint_committed" : result.status.reason;
        return result;
    } catch (...) {
        result.reason = "checkpoint_exception";
        return result;
    }
}

std::optional<std::uint64_t> TelemetryHistoryOwnedBytes(
    const std::filesystem::path& root) {
    return OwnedBytes(root);
}

bool PersistTelemetryLossRanges(
    const std::filesystem::path& root,
    const std::vector<TelemetryLossRange>& losses,
    std::uint64_t maximum_history_bytes) {
    if (losses.empty()) return true;
    for (const auto& loss : losses)
        if (!IsOpaqueId(loss.anonymous_session_id) ||
            loss.dropped_records == 0 || loss.first_revision == 0 ||
            loss.last_revision < loss.first_revision) return false;
    try {
        std::error_code error;
        const bool root_exists = std::filesystem::exists(root, error);
        if (error || (root_exists &&
            std::filesystem::is_symlink(root, error)) || error) return false;
        std::filesystem::create_directories(root, error);
        if (error) return false;
        WriterLease lease(root, L"store.lock");
        if (!lease.acquired()) return false;
        const auto path = root / "losses-v1.json";
        const auto temp = root / "losses-v1.json.tmp";
        Json entries = Json::array();
        std::uint64_t omitted_reports = 0;
        std::uint64_t omitted_records = 0;
        const bool exists = std::filesystem::exists(path, error);
        if (error) return false;
        if (exists) {
            std::string existing;
            if (!ReadBounded(path, 64 * 1024, existing)) return false;
            const auto saved = Json::parse(existing);
            if (!saved.is_object() ||
                saved.value("schema", std::string{}) !=
                    "cohavora-telemetry-losses" ||
                saved.value("schema_version", 0u) != 1u ||
                !saved.contains("sessions") ||
                !saved["sessions"].is_array() ||
                saved["sessions"].size() > 128) return false;
            omitted_reports = saved.value("omitted_reports", std::uint64_t{0});
            omitted_records = saved.value("omitted_records", std::uint64_t{0});
            for (const auto& item : saved["sessions"]) {
                const auto id = item.at("anonymous_session_id").get<std::string>();
                const auto count = item.at("dropped_records").get<std::uint64_t>();
                const auto first = item.at("first_revision").get<std::uint64_t>();
                const auto last = item.at("last_revision").get<std::uint64_t>();
                if (!IsOpaqueId(id) || count == 0 || first == 0 || last < first)
                    return false;
                entries.push_back({
                    {"anonymous_session_id", id}, {"dropped_records", count},
                    {"first_revision", first}, {"last_revision", last},
                    {"first_source_utc_ms", item.value(
                        "first_source_utc_ms", std::int64_t{0})},
                    {"last_source_utc_ms", item.value(
                        "last_source_utc_ms", std::int64_t{0})},
                });
            }
        }
        for (const auto& loss : losses) {
            const auto found = std::find_if(entries.begin(), entries.end(),
                [&](const Json& item) {
                    return item.value("anonymous_session_id", std::string{}) ==
                        loss.anonymous_session_id;
                });
            if (found == entries.end()) {
                if (entries.size() == 128) {
                    omitted_reports = SaturatingAdd(omitted_reports, 1);
                    omitted_records = SaturatingAdd(omitted_records,
                        loss.dropped_records);
                    continue;
                }
                entries.push_back({
                    {"anonymous_session_id", loss.anonymous_session_id},
                    {"dropped_records", loss.dropped_records},
                    {"first_revision", loss.first_revision},
                    {"last_revision", loss.last_revision},
                    {"first_source_utc_ms", loss.first_source_utc_ms},
                    {"last_source_utc_ms", loss.last_source_utc_ms},
                });
                continue;
            }
            (*found)["dropped_records"] = SaturatingAdd(
                found->at("dropped_records").get<std::uint64_t>(),
                loss.dropped_records);
            (*found)["first_revision"] = (std::min)(
                found->at("first_revision").get<std::uint64_t>(),
                loss.first_revision);
            (*found)["last_revision"] = (std::max)(
                found->at("last_revision").get<std::uint64_t>(),
                loss.last_revision);
            (*found)["first_source_utc_ms"] = (std::min)(
                found->at("first_source_utc_ms").get<std::int64_t>(),
                loss.first_source_utc_ms);
            (*found)["last_source_utc_ms"] = (std::max)(
                found->at("last_source_utc_ms").get<std::int64_t>(),
                loss.last_source_utc_ms);
        }
        const Json output{
            {"schema", "cohavora-telemetry-losses"},
            {"schema_version", 1},
            {"omitted_reports", omitted_reports},
            {"omitted_records", omitted_records},
            {"sessions", entries},
        };
        const auto content = output.dump(2);
        if (content.size() > 64 * 1024) return false;
        std::filesystem::remove(temp, error);
        error.clear();
        const auto used = OwnedBytes(root);
        if (!used || *used > maximum_history_bytes ||
            content.size() > maximum_history_bytes - *used) return false;
        return WriteText(temp, content) && CommitManifest(temp, path);
    } catch (...) { return false; }
}

bool ClearTelemetryLossSummary(const std::filesystem::path& root) {
    try {
        std::error_code error;
        const bool exists = std::filesystem::exists(root, error);
        if (error) return false;
        if (!exists) return true;
        if (std::filesystem::is_symlink(root, error) || error ||
            !std::filesystem::is_directory(root, error) || error) return false;
        WriterLease lease(root, L"store.lock");
        if (!lease.acquired()) return false;
        for (const auto* name : {"losses-v1.json", "losses-v1.json.tmp"}) {
            const auto path = root / name;
            const bool present = std::filesystem::exists(path, error);
            if (error) return false;
            if (!present) continue;
            if (std::filesystem::is_symlink(path, error) || error ||
                !std::filesystem::is_regular_file(path, error) || error)
                return false;
            std::filesystem::remove(path, error);
            if (error) return false;
        }
        return true;
    } catch (...) { return false; }
}

TelemetryCheckpointResult AppendTelemetryCheckpoint(
    const std::filesystem::path& root,
    const std::vector<SafeTelemetryRecordPtr>& records,
    std::uint64_t maximum_report_bytes) {
    TelemetryCheckpointResult result;
    result.reason = "no_records";
    if (records.empty() || !records.back()) return result;
    const auto& last = *records.back();
    std::vector<TelemetryCheckpointRecord> safe;
    safe.reserve(records.size());
    for (const auto& record : records) {
        if (!record || record->anonymous_session_id != last.anonymous_session_id ||
            record->snapshot.session_generation != last.snapshot.session_generation ||
            record->schema_version != kTelemetryReportSchemaVersion ||
            record->definition_version != kTelemetryDefinitionVersion) {
            result.reason = "incompatible_record_set";
            return result;
        }
        safe.push_back(MakeTelemetryCheckpointRecord(*record));
    }
    return AppendTelemetryCheckpoint(root, last.anonymous_session_id,
                                     safe, maximum_report_bytes);
}

bool RemoveTelemetryCheckpoint(const std::filesystem::path& directory) {
    WriterLease root_lease(directory.parent_path(), L"store.lock");
    if (!root_lease.acquired()) return false;
    WriterLease lease(directory);
    if (!lease.acquired() || !InspectTelemetryCheckpoint(directory).valid)
        return false;
    try {
        std::string content;
        if (!ReadBounded(directory / "manifest.json", kMaximumManifestBytes, content))
            return false;
        const auto manifest = Json::parse(content);
        std::set<std::string> segments;
        for (const auto& segment : manifest.at("segments")) {
            const auto file = segment.at("file").get<std::string>();
            if (file != SegmentName(segment.at("last_revision").get<std::uint64_t>()))
                return false;
            segments.insert(file);
        }
        std::error_code error;
        {
            for (std::filesystem::directory_iterator it(directory, error), end;
                 !error && it != end; it.increment(error)) {
                const auto file = it->path().filename().string();
                if (!IsOwnedFile(file) || file == "writer.lock") continue;
                if (it->is_symlink(error) || error ||
                    !it->is_regular_file(error) || error) return false;
                if (file != "manifest.json" && !segments.contains(file) &&
                    file != "manifest.json.tmp" &&
                    !(file.size() > 4 && file.ends_with(".tmp") &&
                      IsSegmentFile(std::string_view(file).substr(0, file.size() - 4))))
                    return false;
                std::filesystem::remove(it->path(), error);
                if (error) return false;
            }
        }
        if (error) return false;
        lease.Release();
        std::filesystem::remove(directory / "writer.lock", error);
        if (error) return false;
        error.clear();
        std::filesystem::remove(directory, error);
        return !std::filesystem::exists(directory / "manifest.json");
    } catch (...) {
        return false;
    }
}

bool PruneTelemetryCheckpointOrphans(const std::filesystem::path& directory) {
    if (!InspectTelemetryCheckpoint(directory).valid) return false;
    WriterLease root_lease(directory.parent_path(), L"store.lock");
    if (!root_lease.acquired()) return false;
    WriterLease lease(directory);
    if (!lease.acquired()) return false;
    try {
        std::string content;
        if (!ReadBounded(directory / "manifest.json", kMaximumManifestBytes, content))
            return false;
        const auto manifest = Json::parse(content);
        std::set<std::string> retained;
        for (const auto& segment : manifest.at("segments"))
            retained.insert(segment.at("file").get<std::string>());
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            const auto file = it->path().filename().string();
            if (!IsOwnedFile(file) || file == "manifest.json" ||
                file == "writer.lock" || retained.contains(file)) continue;
            if (it->is_symlink(error) || error ||
                !it->is_regular_file(error) || error) return false;
            std::filesystem::remove(it->path(), error);
            if (error) return false;
        }
        return !error;
    } catch (...) {
        return false;
    }
}

std::uint64_t TelemetryCheckpointArtifactBytes(
    const std::filesystem::path& directory) {
    std::error_code error;
    if (std::filesystem::is_symlink(directory, error) || error ||
        !std::filesystem::is_directory(directory, error) || error)
        return 0;
    std::uint64_t bytes = 0;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        if (!IsOwnedFile(it->path().filename().string()) ||
            it->is_symlink(error) || error ||
            !it->is_regular_file(error) || error) continue;
        bytes += it->file_size(error);
        if (error) return 0;
    }
    return error ? 0 : bytes;
}

bool DiscardCorruptTelemetryCheckpointArtifacts(
    const std::filesystem::path& directory) {
    try {
        const auto name = directory.filename().string();
        if (!name.starts_with(kDirectoryPrefix) ||
            !IsOpaqueId(std::string_view(name).substr(kDirectoryPrefix.size())))
            return false;
        const auto initial = InspectTelemetryCheckpoint(directory);
        if (initial.valid || initial.reason == "unsupported_version") return false;
        WriterLease root_lease(directory.parent_path(), L"store.lock");
        if (!root_lease.acquired()) return false;
        WriterLease lease(directory);
        if (!lease.acquired()) return false;
        const auto locked = InspectTelemetryCheckpoint(directory);
        if (locked.valid || locked.reason == "unsupported_version") return false;
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            if (!IsOwnedFile(it->path().filename().string()) ||
                it->path().filename() == "writer.lock") continue;
            if (it->is_symlink(error) || error ||
                !it->is_regular_file(error) || error) return false;
            files.push_back(it->path());
        }
        if (error) return false;
        for (const auto& file : files) {
            std::filesystem::remove(file, error);
            if (error) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace livekit::telemetry
