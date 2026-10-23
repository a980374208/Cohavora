#pragma once

// Diagnostic-only single-slot request, consumed on PilotDiagnosticsProbe's worker.
// It never accepts paths or changes heap state. The old terminal cache is separate.
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace MeetingApp::released_heap_window {
using Json = nlohmann::json;
struct Context {
    std::string run_id, process_run_id;
    std::uint64_t pid = 0, creation_filetime = 0, generation = 0;
    bool complete = false, closing = false;
    std::size_t cleanup_pending = 0;
};
inline bool HexId(const Json& value) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool Integer(const Json& value) {
    return value.is_number_integer() && (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()));
}
inline bool SafeUtc(const Json& value) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() != 28) return false; // DateTime.UtcNow.ToString("o")
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char separator = i == 4 || i == 7 ? '-' : i == 10 ? 'T' :
            i == 13 || i == 16 ? ':' : i == 19 ? '.' : i == 27 ? 'Z' : '\0';
        if (separator ? s[i] != separator : s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}
inline Json ReadRequest(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > 4096) return nullptr;
        std::ifstream input(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(size)) || input.peek() != EOF) return nullptr;
        return Json::parse(bytes, nullptr, false);
    } catch (...) { return nullptr; }
}
class Sampler final {
public:
    template <class Capture, class Clock>
    const Json& Observe(const Json& request, const Context& context, Capture capture, Clock clock) {
        try {
            // Exact schema: no arbitrary paths, extra text or variable-size payloads.
            static constexpr const char* fields[] = {"schema", "run_id", "process_run_id", "pid",
                "process_creation_filetime_100ns", "cycle", "cycle_id", "request_id", "session_generation",
                "leave_utc", "leave_observed_utc_ms", "window_start_utc_ms", "requested_utc_ms", "deadline_utc_ms"};
            if (context.closing || !context.complete || context.cleanup_pending || !context.creation_filetime ||
                !request.is_object() || request.size() != std::size(fields)) return last_;
            for (const auto* field : fields) if (!request.contains(field)) return last_;
            if (request["schema"] != "released_heap_window_v1" || !HexId(request["run_id"]) ||
                !HexId(request["process_run_id"]) || !HexId(request["cycle_id"]) || !HexId(request["request_id"]) ||
                request["run_id"] != context.run_id || request["process_run_id"] != context.process_run_id) return last_;
            for (const auto* field : {"pid", "process_creation_filetime_100ns", "cycle", "session_generation",
                    "leave_observed_utc_ms", "window_start_utc_ms", "requested_utc_ms", "deadline_utc_ms"})
                if (!Integer(request[field])) return last_;
            if (!SafeUtc(request["leave_utc"])) return last_;
            const auto cycle = request["cycle"].get<std::int64_t>();
            const auto leave = request["leave_observed_utc_ms"].get<std::int64_t>();
            const auto start = request["window_start_utc_ms"].get<std::int64_t>();
            const auto requested = request["requested_utc_ms"].get<std::int64_t>();
            const auto deadline = request["deadline_utc_ms"].get<std::int64_t>();
            // Encode cycle in the ID prefix: cross-cycle ID reuse is impossible
            // without a set or a growing queue. Within a cycle it is one-shot.
            std::uint64_t id_cycle = 0;
            const auto& id = request["request_id"].get_ref<const std::string&>();
            for (std::size_t i = 0; i < 8; ++i)
                id_cycle = id_cycle * 16 + (id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
            const auto now = clock();
            if (cycle <= last_cycle_ || cycle > 1000 || id_cycle != static_cast<std::uint64_t>(cycle) || leave <= 0 ||
                leave > (std::numeric_limits<std::int64_t>::max)() - 15001 ||
                start != leave + 5001 || requested < start || requested <= last_requested_ ||
                requested > now || deadline < requested || deadline - requested > 10000 || now > deadline ||
                request["pid"] != context.pid || request["process_creation_filetime_100ns"] != context.creation_filetime ||
                request["session_generation"] != context.generation) return last_;
            last_cycle_ = cycle;
            last_requested_ = requested;
            const auto began = clock();
            Json values;
            try { values = capture(); } catch (...) { values = {{"available", false}}; }
            const auto ended = clock();
            last_ = {{"schema", "released_heap_window_sample_v1"}, {"request", request},
                {"capture_started_utc_ms", began}, {"capture_finished_utc_ms", ended},
                {"status", began >= start && began >= requested && ended >= began && ended <= deadline ?
                    "CAPTURED_DIAGNOSTIC_ONLY" : "OUTSIDE_REQUEST_WINDOW"},
                {"values", std::move(values)}, {"qualification_credit", 0}, {"formal_credit", 0}};
        } catch (...) { /* Malformed diagnostic requests cannot affect the product. */ }
        return last_;
    }
private:
    std::int64_t last_cycle_ = 0, last_requested_ = 0;
    Json last_;
};
}
