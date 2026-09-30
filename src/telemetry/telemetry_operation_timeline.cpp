#include "telemetry_operation_timeline.h"
#include "diagnostic_event.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <limits>
#include <string_view>

namespace livekit::telemetry {
namespace {

constexpr std::size_t kMaximumFiles = 2048;
constexpr std::uint64_t kMaximumInputBytes = 100ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumFileBytes = 10ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumLineBytes = 16 * 1024;

bool IsHexId(std::string_view value) noexcept {
    return value.size() == 32 && std::all_of(value.begin(), value.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f'); });
}

bool IsSegment(std::string_view name) noexcept {
    return name.size() == 20 && name.starts_with("segment-") &&
        name.ends_with(".jsonl") &&
        std::all_of(name.begin() + 8, name.begin() + 14,
            [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool IsTimelineEvent(std::string_view name) noexcept {
    constexpr std::array names{
        "http.request.started", "http.request.completed",
        "http.response.decode_failed", "admission.started",
        "admission.stage_changed", "admission.terminal",
        "room.connect.started", "room.connect.terminal", "startup.terminal",
        "media.publish.started", "media.publish.terminal",
        "media.publish_batch.started", "media.publish_batch.terminal",
        "media.unpublish.started", "media.unpublish.terminal",
        "media.recovery.milestone", "media.recovery.timeout",
        "media.endpoint.recovered",
        "reconnect.episode.started", "reconnect.episode.terminal",
        "reconnect.attempt.started", "reconnect.attempt.terminal",
        "reconnect.mode_changed", "device.switch.started",
        "device.switch.terminal", "meeting.leave.requested",
        "meeting.backend_notification.completed", "session.stopped",
        "media.first_observed", "render.stall.interval",
    };
    return std::any_of(names.begin(), names.end(),
        [name](const char* item) { return name == item; });
}

bool SafeToken(std::string_view value) noexcept {
    return value.size() <= 64 && std::all_of(value.begin(), value.end(),
        [](char ch) { return (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.'; });
}

bool IsTimelineOperationId(std::string_view value) noexcept {
    if (value.empty() || IsHexId(value)) return true;
    if (!diagnostic::IsSafeOperationId(value)) return false;
    const auto colon = value.find(':');
    if (colon != std::string_view::npos) return true;
    constexpr std::array prefixes{
        "room_connect_", "publish_", "reconnect_",
        "reconnect_attempt_",
    };
    for (const auto* prefix : prefixes) {
        const std::string_view start(prefix);
        if (!value.starts_with(start)) continue;
        auto tail = value.substr(start.size());
        const auto separator = tail.find('_');
        if (separator == std::string_view::npos && start != "room_connect_")
            continue;
        const auto digits = [](std::string_view part) {
            return !part.empty() && std::all_of(part.begin(), part.end(),
                [](char ch) { return ch >= '0' && ch <= '9'; });
        };
        if (separator == std::string_view::npos ? digits(tail) :
            digits(tail.substr(0, separator)) &&
            digits(tail.substr(separator + 1))) return true;
    }
    return false;
}

template <typename Enum>
bool IsNamedEnum(std::string_view value, Enum last,
                 std::string_view (*name)(Enum) noexcept) {
    if (value.empty()) return true;
    for (unsigned n = 0; n <= static_cast<unsigned>(last); ++n) {
        if (value == name(static_cast<Enum>(n))) return true;
    }
    return false;
}

std::string Token(const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || !value.at(key).is_string()) return {};
    const auto text = value.at(key).get<std::string>();
    return SafeToken(text) ? text : std::string{};
}

} // namespace

std::optional<OperationTimelineEntry> ProjectTimelineEvent(const diagnostic::Event& e) {
    if (!diagnostic::IsTimelineEvent(e.kind) || !diagnostic::IsValidEvent(e)) return {};
    OperationTimelineEntry r;
    r.event_name = diagnostic::EventName(e.kind);
    r.process_run_id = std::string(e.process_run_id.data(), 32);
    r.operation_id = e.context.operation_id.View();
    r.parent_operation_id = e.context.parent_operation_id.View();
    r.request_id = e.context.request_id.View();
    r.stage = diagnostic::StageName(e.stage);
    r.outcome = diagnostic::OutcomeName(e.outcome);
    r.error_code = diagnostic::ErrorCodeName(e.error_code);
    r.error_layer = diagnostic::ErrorLayerName(e.error_layer);
    r.occurred_at_utc_ms = e.occurred_at_utc_ms;
    r.monotonic_us = e.monotonic_us;
    r.source_monotonic_us = e.source_monotonic_us;
    r.event_sequence = e.event_sequence;
    r.session_generation = e.context.session_generation;
    r.room_generation = e.context.room_generation;
    r.recovery_epoch = e.context.recovery_epoch;
    r.attempt = e.attempt;
    r.http_status = e.http_status >= 100 && e.http_status <= 599 ? e.http_status : 0;
    r.media_kind = diagnostic::MediaKindName(e.media_kind);
    r.endpoint_id = e.media_endpoint_id.View();
    r.previous_endpoint_id = e.previous_media_endpoint_id.View();
    if (e.kind == diagnostic::EventKind::MediaFirstObserved ||
        e.kind == diagnostic::EventKind::RenderStallInterval) {
        r.binding_epoch = e.binding_epoch;
        if (e.kind == diagnostic::EventKind::MediaFirstObserved) {
            r.measurement_point = diagnostic::MediaObservationName(e.media_observation);
        } else {
            r.measurement_point = "render_submit_stall";
            r.begin_us = e.interval_begin_us; r.end_us = e.interval_end_us;
            r.threshold_us = e.stall_threshold_us;
            r.boundary = diagnostic::StallBoundaryName(e.stall_boundary);
        }
    } else if (e.kind == diagnostic::EventKind::MediaRecoveryMilestone ||
               e.kind == diagnostic::EventKind::MediaRecoveryTimeout ||
               e.kind == diagnostic::EventKind::MediaEndpointRecovered) {
        r.measurement_point = diagnostic::RecoveryMeasurementName(e.recovery_measurement);
        r.expected_endpoints = e.batch_track_count;
        r.has_expected_endpoints = e.kind == diagnostic::EventKind::MediaRecoveryMilestone;
    }
    return r;
}

OperationTimeline ReadOperationTimeline(
    const std::filesystem::path& root,
    const std::string& session_id,
    std::size_t maximum_entries,
    const std::string& expected_run_id) {
    OperationTimeline result;
    if (!IsHexId(session_id) || maximum_entries == 0 ||
        (!expected_run_id.empty() && !IsHexId(expected_run_id))) return result;
    maximum_entries = (std::min)(maximum_entries, std::size_t{1024});
    try {
        std::error_code error;
        if (std::filesystem::is_symlink(root, error) || error ||
            !std::filesystem::is_directory(root, error) || error) return result;
        struct Input {
            std::filesystem::path path;
            std::string run_id;
            std::uint64_t bytes;
            std::filesystem::file_time_type modified;
        };
        std::vector<Input> files;
        std::uint64_t input_bytes = 0;
        for (std::filesystem::directory_iterator runs(root, error), end;
             !error && runs != end; runs.increment(error)) {
            const auto name = runs->path().filename().string();
            if (!name.starts_with("run-") || !IsHexId(
                    std::string_view(name).substr(4)) ||
                (!expected_run_id.empty() && name.substr(4) != expected_run_id) ||
                runs->is_symlink(error) || error ||
                !runs->is_directory(error) || error) {
                error.clear();
                continue;
            }
            for (std::filesystem::directory_iterator segments(runs->path(), error);
                 !error && segments != end; segments.increment(error)) {
                if (!IsSegment(segments->path().filename().string()) ||
                    segments->is_symlink(error) || error ||
                    !segments->is_regular_file(error) || error) {
                    error.clear();
                    continue;
                }
                const auto size = segments->file_size(error);
                if (error || size > kMaximumFileBytes ||
                    files.size() == kMaximumFiles ||
                    input_bytes + size > kMaximumInputBytes) {
                    ++result.skipped_files;
                    error.clear();
                    continue;
                }
                const auto modified = segments->last_write_time(error);
                if (error) {
                    ++result.skipped_files;
                    error.clear();
                    continue;
                }
                files.push_back({segments->path(), name.substr(4), size, modified});
                input_bytes += size;
            }
        }
        if (error) ++result.skipped_files;
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a.modified == b.modified ? a.path < b.path :
                a.modified < b.modified;
        });
        std::deque<OperationTimelineEntry> recent;
        std::array<char, kMaximumLineBytes + 1> line{};
        for (const auto& file : files) {
            std::ifstream input(file.path, std::ios::binary);
            if (!input) { ++result.skipped_files; continue; }
            result.source_available = true;
            while (true) {
                input.getline(line.data(), static_cast<std::streamsize>(line.size()));
                if (input.eof() && input.gcount() == 0) break;
                if (input.fail() || input.eof()) {
                    ++result.invalid_lines;
                    if (input.eof()) break;
                    input.clear();
                    input.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
                    continue;
                }
                try {
                    const auto value = nlohmann::json::parse(line.data());
                    if (value.is_object() &&
                        value.value("event_name", std::string{}) == "process.started" &&
                        value.value("process_run_id", std::string{}) == file.run_id &&
                        value.contains("attributes") &&
                        value["attributes"].is_object()) {
                        const auto build = value["attributes"].value(
                            "build_id", std::string{});
                        if (build.size() == 64 && std::all_of(build.begin(),
                                build.end(), [](char ch) {
                                    return (ch >= '0' && ch <= '9') ||
                                        (ch >= 'a' && ch <= 'f');
                                })) {
                            result.build_id = build;
                            const auto symbol = value["attributes"].value(
                                "symbol_identity", std::string{});
                            if (symbol.size() >= 37 && symbol.size() <= 46 &&
                                symbol[8] == '-' && symbol[13] == '-' &&
                                symbol[18] == '-' && symbol[35] == '-' &&
                                std::all_of(symbol.begin() + 36, symbol.end(),
                                    [](char ch) { return ch >= '0' && ch <= '9'; }) &&
                                std::all_of(symbol.begin(), symbol.begin() + 35,
                                    [](char ch) {
                                        return (ch >= '0' && ch <= '9') ||
                                            (ch >= 'a' && ch <= 'f') || ch == '-';
                                    })) result.symbol_identity = symbol;
                        }
                    }
                    if (!value.is_object() ||
                        value.value("schema_version", 0u) != 1u ||
                        value.value("anonymous_session_id", std::string{}) != session_id)
                        continue;
                    const auto name = Token(value, "event_name");
                    if (!IsTimelineEvent(name)) continue;
                    const auto run_id = Token(value, "process_run_id");
                    if (run_id != file.run_id) {
                        ++result.invalid_lines;
                        continue;
                    }
                    OperationTimelineEntry entry;
                    entry.event_name = name;
                    entry.process_run_id = run_id;
                    const auto operation_id = value.value(
                        "operation_id", std::string{});
                    const auto parent_id = value.value(
                        "parent_operation_id", std::string{});
                    const auto request_id = value.value(
                        "request_id", std::string{});
                    if (!IsTimelineOperationId(operation_id) ||
                        !IsTimelineOperationId(parent_id) ||
                        (!request_id.empty() && !IsHexId(request_id))) {
                        ++result.invalid_lines;
                        continue;
                    }
                    entry.operation_id = operation_id;
                    entry.parent_operation_id = parent_id;
                    entry.request_id = request_id;
                    entry.session_generation = value.value("session_generation", std::uint64_t{0});
                    entry.room_generation = value.value("room_generation", std::uint64_t{0});
                    entry.recovery_epoch = value.value("recovery_epoch", std::uint64_t{0});
                    entry.source_monotonic_us = value.value("source_monotonic_us", std::uint64_t{0});
                    entry.stage = Token(value, "stage");
                    entry.outcome = Token(value, "outcome");
                    entry.error_code = Token(value, "error_code");
                    entry.error_layer = Token(value, "error_layer");
                    if (!IsNamedEnum(entry.stage,
                            diagnostic::Stage::SetRemoteDescription,
                            diagnostic::StageName)) entry.stage.clear();
                    if (!IsNamedEnum(entry.outcome,
                            diagnostic::Outcome::Cancelled,
                            diagnostic::OutcomeName)) entry.outcome.clear();
                    if (!IsNamedEnum(entry.error_code,
                            diagnostic::ErrorCode::InvalidResponse,
                            diagnostic::ErrorCodeName)) entry.error_code.clear();
                    if (!IsNamedEnum(entry.error_layer,
                            diagnostic::ErrorLayer::Rtc,
                            diagnostic::ErrorLayerName)) entry.error_layer.clear();
                    if (value.contains("attributes") &&
                        value.at("attributes").is_object()) {
                        const auto& attributes = value.at("attributes");
                        if (name == "media.first_observed" || name == "render.stall.interval") {
                            entry.endpoint_id = Token(attributes, "endpoint_id");
                            entry.media_kind = Token(attributes, "media_kind");
                            entry.measurement_point = Token(attributes, "measurement_point");
                            entry.binding_epoch = attributes.value("binding_epoch", std::uint64_t{0});
                            bool valid = value.at("source_monotonic_us").is_number_unsigned() &&
                                entry.source_monotonic_us <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
                                attributes.at("binding_epoch").is_number_unsigned() &&
                                IsHexId(entry.endpoint_id) && entry.session_generation &&
                                entry.room_generation && entry.binding_epoch && entry.source_monotonic_us &&
                                !entry.operation_id.empty();
                            if (name == "media.first_observed") {
                                valid &= (entry.measurement_point == "first_decoded" ||
                                    entry.measurement_point == "first_render_submit") ? entry.media_kind == "video" :
                                    entry.measurement_point == "first_pcm" && entry.media_kind == "audio";
                            } else {
                                entry.boundary = Token(attributes, "boundary");
                                entry.begin_us = attributes.value("begin_us", std::uint64_t{0});
                                entry.end_us = attributes.value("end_us", std::uint64_t{0});
                                entry.threshold_us = attributes.value("threshold_us", std::uint64_t{0});
                                valid &= attributes.at("begin_us").is_number_unsigned() &&
                                    attributes.at("end_us").is_number_unsigned() && attributes.at("threshold_us").is_number_unsigned() &&
                                    entry.begin_us <= entry.source_monotonic_us && entry.end_us <= entry.source_monotonic_us &&
                                    entry.media_kind == "video" && entry.measurement_point == "render_submit_stall" &&
                                    entry.begin_us && entry.threshold_us >= 500000 &&
                                    (entry.boundary == "open" ? entry.end_us == 0 :
                                        (entry.boundary == "recovered" || entry.boundary == "hidden" ||
                                         entry.boundary == "rebound" || entry.boundary == "stopped" || entry.boundary == "inactive") &&
                                        entry.end_us >= entry.begin_us);
                            }
                            if (!valid) { ++result.invalid_lines; continue; }
                        }
                        if (name == "meeting.leave.requested") {
                            const auto reason = Token(attributes, "leave_reason");
                            if (reason == "leave" || reason == "end_for_all")
                                entry.leave_reason = reason;
                        }
                        if (attributes.contains("http_status") &&
                            attributes.at("http_status").is_number_unsigned()) {
                            const auto status = attributes.at("http_status")
                                .get<std::uint64_t>();
                            if (status >= 100 && status <= 599)
                                entry.http_status = static_cast<std::uint16_t>(status);
                        }
                        if (name == "media.endpoint.recovered" ||
                            name == "media.recovery.milestone" ||
                            name == "media.recovery.timeout") {
                            entry.media_kind = Token(attributes, "media_kind");
                            entry.measurement_point = Token(
                                attributes, "measurement_point");
                            if ((entry.media_kind != "audio" &&
                                 entry.media_kind != "video") ||
                                (entry.measurement_point != "decoded_video_stably_recovered" &&
                                 entry.measurement_point != "pcm_stably_recovered" &&
                                 entry.measurement_point != "visible_render_stably_recovered")) {
                                ++result.invalid_lines;
                                continue;
                            }
                            entry.recovery_epoch = value.value(
                                "recovery_epoch", std::uint64_t{0});
                            if (entry.recovery_epoch == 0) {
                                ++result.invalid_lines;
                                continue;
                            }
                            if (name != "media.recovery.timeout") {
                                entry.session_generation = value.value(
                                    "session_generation", std::uint64_t{0});
                                if (entry.session_generation == 0 ||
                                    entry.operation_id.empty()) {
                                    ++result.invalid_lines;
                                    continue;
                                }
                            }
                            if (name == "media.endpoint.recovered") {
                                entry.endpoint_id = Token(attributes, "endpoint_id");
                                entry.previous_endpoint_id = Token(
                                    attributes, "previous_endpoint_id");
                                entry.room_generation = value.value(
                                    "room_generation", std::uint64_t{0});
                                if (!IsHexId(entry.endpoint_id) ||
                                    (!entry.previous_endpoint_id.empty() &&
                                     !IsHexId(entry.previous_endpoint_id)) ||
                                    entry.room_generation == 0) {
                                    ++result.invalid_lines;
                                    continue;
                                }
                            } else if (name == "media.recovery.milestone" &&
                                       attributes.contains("expected_endpoints") &&
                                       attributes.at("expected_endpoints").is_number_unsigned()) {
                                const auto expected = attributes.at("expected_endpoints")
                                    .get<std::uint64_t>();
                                if (expected == 0 || expected > 256) {
                                    ++result.invalid_lines;
                                    continue;
                                }
                                entry.expected_endpoints = static_cast<std::uint32_t>(expected);
                                entry.has_expected_endpoints = true;
                            }
                        }
                    }
                    entry.occurred_at_utc_ms =
                        value.at("occurred_at_utc_ms").get<std::int64_t>();
                    entry.monotonic_us = value.at("monotonic_us").get<std::uint64_t>();
                    entry.event_sequence =
                        value.at("event_sequence").get<std::uint64_t>();
                    if (value.contains("attributes") &&
                        value.at("attributes").is_object())
                        entry.attempt = value.at("attributes").value(
                            "attempt", std::uint32_t{0});
                    recent.push_back(std::move(entry));
                    if (recent.size() > maximum_entries) {
                        recent.pop_front();
                        ++result.omitted;
                    }
                } catch (...) { ++result.invalid_lines; }
            }
        }
        result.entries.assign(recent.begin(), recent.end());
        std::sort(result.entries.begin(), result.entries.end(),
            [](const auto& a, const auto& b) {
                if (a.process_run_id == b.process_run_id)
                    return a.monotonic_us == b.monotonic_us ? a.event_sequence < b.event_sequence
                        : a.monotonic_us < b.monotonic_us;
                return a.occurred_at_utc_ms == b.occurred_at_utc_ms
                    ? a.event_sequence < b.event_sequence
                    : a.occurred_at_utc_ms < b.occurred_at_utc_ms;
            });
    } catch (...) { ++result.skipped_files; }
    return result;
}

} // namespace livekit::telemetry
