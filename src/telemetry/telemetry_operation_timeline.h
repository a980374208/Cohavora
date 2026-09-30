#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include "diagnostic_event.h"

namespace livekit::telemetry {

struct OperationTimelineEntry {
    std::string event_name;
    std::string process_run_id;
    std::string operation_id;
    std::string parent_operation_id;
    std::string request_id;
    std::string stage;
    std::string outcome;
    std::string error_code;
    std::string error_layer;
    std::string leave_reason;
    std::string media_kind;
    std::string measurement_point;
    std::string endpoint_id;
    std::string previous_endpoint_id;
    std::uint16_t http_status = 0;
    std::int64_t occurred_at_utc_ms = 0;
    std::uint64_t monotonic_us = 0;
    std::uint64_t event_sequence = 0;
    std::uint64_t source_monotonic_us = 0;
    std::uint64_t binding_epoch = 0;
    std::uint64_t begin_us = 0, end_us = 0, threshold_us = 0;
    std::string boundary;
    std::uint32_t attempt = 0;
    std::uint32_t expected_endpoints = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t room_generation = 0;
    std::uint64_t recovery_epoch = 0;
    bool has_expected_endpoints = false;
};

struct OperationTimeline {
    std::vector<OperationTimelineEntry> entries;
    std::string build_id = "unknown";
    std::string symbol_identity = "unknown";
    std::uint64_t omitted = 0;
    std::uint64_t invalid_lines = 0;
    std::uint64_t skipped_files = 0;
    bool source_available = false;
};

OperationTimeline ReadOperationTimeline(
    const std::filesystem::path& diagnostic_root,
    const std::string& anonymous_session_id,
    std::size_t maximum_entries = 1024,
    const std::string& expected_run_id = {});

std::optional<OperationTimelineEntry> ProjectTimelineEvent(const diagnostic::Event& event);

} // namespace livekit::telemetry
