#include "stability_ledger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace livekit::telemetry {
namespace {

using Json = nlohmann::json;

constexpr auto kSchema = "cohavora-stability-ledger";
constexpr std::uint32_t kSchemaVersion = 1;

std::mutex g_installed_mutex;
std::weak_ptr<StabilityLedger> g_installed_ledger;

std::int64_t UtcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* SessionTerminalName(StabilitySessionTerminal terminal) noexcept {
    switch (terminal) {
    case StabilitySessionTerminal::Completed: return "COMPLETED";
    case StabilitySessionTerminal::DegradedCompleted: return "DEGRADED_COMPLETED";
    case StabilitySessionTerminal::AdmissionFailure: return "ADMISSION_FAILURE";
    case StabilitySessionTerminal::Timeout: return "TIMEOUT";
    case StabilitySessionTerminal::Cancelled: return "CANCELLED";
    case StabilitySessionTerminal::Stopped: return "STOPPED";
    }
    return "CANCELLED";
}

bool IsConfirmedEvidenceSource(const std::string& source) {
    return source == "windows_wer" || source == "external_supervisor";
}

Json EmptyDocument() {
    return Json{
        {"schema", kSchema},
        {"version", kSchemaVersion},
        {"diagnostics", {
            {"corrupt_inputs", 0},
            {"write_failures", 0},
            {"duplicate_terminals", 0},
        }},
        {"records", Json::array()},
    };
}

std::uint64_t UnsignedValue(
    const Json& object,
    const char* key) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_number_unsigned()) {
        return 0;
    }
    return object.at(key).get<std::uint64_t>();
}

void IncrementDiagnostic(Json& document, const char* key) {
    auto& diagnostics = document["diagnostics"];
    diagnostics[key] = UnsignedValue(diagnostics, key) + 1;
}

bool AtomicReplace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#if defined(_WIN32)
    return ::MoveFileExW(
        temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

} // namespace

struct StabilityLedger::Impl {
    Json document = EmptyDocument();
    StabilitySummary summary;
    std::string process_run_id;
    Clock::time_point process_started_at{};
    std::unordered_map<std::string, Clock::time_point> session_started_at;
    bool loaded = false;
    bool process_started = false;
    bool process_terminal = false;
    bool last_write_ok = true;
    bool evidence_provider_configured = false;
};

StabilityLedger::StabilityLedger(
    std::filesystem::path path,
    std::size_t max_records,
    std::size_t max_serialized_bytes)
    : path_(std::move(path))
    , max_records_((std::max)(std::size_t{8}, max_records))
    , max_serialized_bytes_((std::max)(std::size_t{4096}, max_serialized_bytes))
    , impl_(std::make_unique<Impl>()) {
    summary_cache_ = std::make_shared<const StabilitySummary>(impl_->summary);
}

StabilityLedger::~StabilityLedger() = default;

std::string StabilityLedger::NewOpaqueId() {
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

bool StabilityLedger::LoadAndRecoverLocked(
    const StabilityRecoveryEvidence& evidence) {
    impl_->document = EmptyDocument();
    impl_->evidence_provider_configured = evidence.provider_configured;
    bool input_corrupt = false;

    std::error_code file_error;
    if (std::filesystem::exists(path_, file_error) && !file_error) {
        const auto size = std::filesystem::file_size(path_, file_error);
        if (file_error || size > max_serialized_bytes_) {
            input_corrupt = true;
        } else {
            try {
                std::ifstream input(path_, std::ios::binary);
                Json parsed;
                input >> parsed;
                if (!input || !parsed.is_object() ||
                    parsed.value("schema", std::string{}) != kSchema ||
                    parsed.value("version", 0u) != kSchemaVersion ||
                    !parsed.contains("records") ||
                    !parsed.at("records").is_array()) {
                    input_corrupt = true;
                } else {
                    impl_->document = std::move(parsed);
                    if (!impl_->document.contains("diagnostics") ||
                        !impl_->document.at("diagnostics").is_object()) {
                        impl_->document["diagnostics"] = EmptyDocument()["diagnostics"];
                    }
                }
            } catch (...) {
                input_corrupt = true;
            }
        }
    } else if (file_error) {
        impl_->last_write_ok = false;
    }

    if (input_corrupt) {
        impl_->document = EmptyDocument();
        IncrementDiagnostic(impl_->document, "corrupt_inputs");
    }

    std::unordered_map<std::string, std::string> confirmed;
    if (evidence.provider_configured) {
        for (const auto& item : evidence.confirmed_crashes) {
            if (!item.process_run_id.empty() &&
                IsConfirmedEvidenceSource(item.source)) {
                confirmed.emplace(item.process_run_id, item.source);
            }
        }
    }

    const auto recovered_at = UtcNowMs();
    for (auto& record : impl_->document["records"]) {
        if (!record.is_object()) continue;
        const auto kind = record.value("kind", std::string{});
        const auto status = record.value("status", std::string{});
        if (kind == "process" && status == "UNKNOWN_TERMINATION" &&
            evidence.provider_configured) {
            const auto id = record.value("id", std::string{});
            const auto found = confirmed.find(id);
            if (found != confirmed.end()) {
                record["status"] = "CONFIRMED_CRASH";
                record["evidence_source"] = found->second;
            }
            record["crash_evidence_checked"] = true;
            continue;
        }
        if (status != "STARTED") continue;
        if (kind == "process") {
            const auto id = record.value("id", std::string{});
            const auto found = confirmed.find(id);
            if (found != confirmed.end()) {
                record["status"] = "CONFIRMED_CRASH";
                record["evidence_source"] = found->second;
                record["crash_evidence_checked"] = true;
            } else {
                record["status"] = "UNKNOWN_TERMINATION";
                record["crash_evidence_checked"] = evidence.provider_configured;
            }
            record["terminal_utc_ms"] = recovered_at;
        } else if (kind == "session") {
            record["status"] = "UNKNOWN_TERMINATION";
            record["terminal_utc_ms"] = recovered_at;
        }
    }
    impl_->loaded = true;
    return true;
}

bool StabilityLedger::BeginProcessRun(
    const StabilityRecoveryEvidence& evidence) {
    std::lock_guard lock(mutex_);
    if (impl_->process_started && !impl_->process_terminal) return false;
    LoadAndRecoverLocked(evidence);

    impl_->process_run_id = NewOpaqueId();
    impl_->process_started_at = Clock::now();
    impl_->process_started = true;
    impl_->process_terminal = false;
    impl_->session_started_at.clear();
    impl_->document["records"].push_back({
        {"kind", "process"},
        {"id", impl_->process_run_id},
        {"status", "STARTED"},
        {"started_utc_ms", UtcNowMs()},
        {"crash_evidence_checked", false},
    });
    const bool persisted = PersistLocked();
    if (!persisted) {
        impl_->document["records"].erase(
            std::prev(impl_->document["records"].end()));
        impl_->process_run_id.clear();
        impl_->process_started = false;
        impl_->process_terminal = false;
    }
    RebuildSummaryLocked();
    return persisted;
}

bool StabilityLedger::FinishProcessRunClean() {
    std::lock_guard lock(mutex_);
    if (!impl_->process_started || impl_->process_terminal) {
        IncrementDiagnostic(impl_->document, "duplicate_terminals");
        RebuildSummaryLocked();
        return false;
    }
    const auto now = Clock::now();
    const auto terminal_utc_ms = UtcNowMs();
    for (auto& record : impl_->document["records"]) {
        if (!record.is_object()) continue;
        if (record.value("kind", std::string{}) == "session" &&
            record.value("process_run_id", std::string{}) == impl_->process_run_id &&
            record.value("status", std::string{}) == "STARTED") {
            record["status"] = "UNKNOWN_TERMINATION";
            record["reason"] = "process_clean_exit_with_unclosed_session";
            record["terminal_utc_ms"] = terminal_utc_ms;
            const auto id = record.value("id", std::string{});
            if (const auto started = impl_->session_started_at.find(id);
                started != impl_->session_started_at.end()) {
                record["duration_ms"] = std::chrono::duration_cast<
                    std::chrono::milliseconds>(now - started->second).count();
            }
        }
        if (record.value("kind", std::string{}) == "process" &&
            record.value("id", std::string{}) == impl_->process_run_id &&
            record.value("status", std::string{}) == "STARTED") {
            record["status"] = "CLEAN_EXIT";
            record["terminal_utc_ms"] = terminal_utc_ms;
            record["duration_ms"] = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - impl_->process_started_at).count();
        }
    }
    impl_->process_terminal = true;
    const bool persisted = PersistLocked();
    RebuildSummaryLocked();
    return persisted;
}

std::string StabilityLedger::BeginSession() {
    std::lock_guard lock(mutex_);
    if (!impl_->process_started || impl_->process_terminal) return {};
    const auto id = NewOpaqueId();
    impl_->session_started_at[id] = Clock::now();
    impl_->document["records"].push_back({
        {"kind", "session"},
        {"id", id},
        {"process_run_id", impl_->process_run_id},
        {"status", "STARTED"},
        {"started_utc_ms", UtcNowMs()},
    });
    if (!PersistLocked()) {
        impl_->session_started_at.erase(id);
        impl_->document["records"].erase(
            std::prev(impl_->document["records"].end()));
        RebuildSummaryLocked();
        return {};
    }
    RebuildSummaryLocked();
    return id;
}

bool StabilityLedger::FinishSession(
    const std::string& session_id,
    StabilitySessionTerminal terminal) {
    if (session_id.empty()) return false;
    std::lock_guard lock(mutex_);
    for (auto& record : impl_->document["records"]) {
        if (!record.is_object() ||
            record.value("kind", std::string{}) != "session" ||
            record.value("id", std::string{}) != session_id) {
            continue;
        }
        if (record.value("status", std::string{}) != "STARTED") {
            IncrementDiagnostic(impl_->document, "duplicate_terminals");
            RebuildSummaryLocked();
            return false;
        }
        record["status"] = SessionTerminalName(terminal);
        record["terminal_utc_ms"] = UtcNowMs();
        if (const auto started = impl_->session_started_at.find(session_id);
            started != impl_->session_started_at.end()) {
            record["duration_ms"] = std::chrono::duration_cast<
                std::chrono::milliseconds>(Clock::now() - started->second).count();
            impl_->session_started_at.erase(started);
        }
        const bool persisted = PersistLocked();
        RebuildSummaryLocked();
        return persisted;
    }
    IncrementDiagnostic(impl_->document, "duplicate_terminals");
    RebuildSummaryLocked();
    return false;
}

bool StabilityLedger::PersistLocked() {
    auto& records = impl_->document["records"];
    const auto prune_one = [&]() {
        auto found = std::find_if(records.begin(), records.end(),
            [](const Json& record) {
                return !record.is_object() ||
                    (record.value("kind", std::string{}) == "session" &&
                     record.value("status", std::string{}) != "STARTED");
            });
        if (found == records.end()) {
            found = std::find_if(records.begin(), records.end(),
                [this](const Json& record) {
                    if (!record.is_object()) return true;
                    const auto current_process =
                        record.value("kind", std::string{}) == "process" &&
                        record.value("id", std::string{}) == impl_->process_run_id &&
                        record.value("status", std::string{}) == "STARTED";
                    return !current_process &&
                        record.value("status", std::string{}) != "STARTED";
                });
        }
        if (found == records.end()) return false;
        records.erase(found);
        return true;
    };
    while (records.size() > max_records_) {
        if (!prune_one()) break;
    }

    std::string serialized = impl_->document.dump(2);
    while (serialized.size() > max_serialized_bytes_ && records.size() > 1) {
        if (!prune_one()) break;
        serialized = impl_->document.dump(2);
    }
    if (serialized.size() > max_serialized_bytes_) {
        impl_->last_write_ok = false;
        IncrementDiagnostic(impl_->document, "write_failures");
        return false;
    }

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) {
        impl_->last_write_ok = false;
        IncrementDiagnostic(impl_->document, "write_failures");
        return false;
    }

    const auto temporary = std::filesystem::path(
        path_.wstring() + L".tmp." +
        std::filesystem::path(NewOpaqueId()).wstring());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            impl_->last_write_ok = false;
            IncrementDiagnostic(impl_->document, "write_failures");
            return false;
        }
    }
    if (!AtomicReplace(temporary, path_)) {
        std::filesystem::remove(temporary, error);
        impl_->last_write_ok = false;
        IncrementDiagnostic(impl_->document, "write_failures");
        return false;
    }
    impl_->last_write_ok = true;
    return true;
}

void StabilityLedger::RebuildSummaryLocked() {
    StabilitySummary summary;
    summary.ledger_availability = impl_->last_write_ok ? "VALID" : "INVALID";
    summary.ledger_reason = impl_->last_write_ok
        ? "atomic_bounded_ledger_valid" : "atomic_ledger_write_failed";
    summary.confirmed_crash_availability = impl_->evidence_provider_configured
        ? "VALID" : "UNSUPPORTED";
    summary.confirmed_crash_reason = impl_->evidence_provider_configured
        ? "external_crash_evidence_checked"
        : "crash_evidence_provider_not_configured";

    const auto& diagnostics = impl_->document["diagnostics"];
    summary.corrupt_inputs = UnsignedValue(diagnostics, "corrupt_inputs");
    summary.write_failures = UnsignedValue(diagnostics, "write_failures");
    summary.duplicate_terminals = UnsignedValue(
        diagnostics, "duplicate_terminals");

    for (const auto& record : impl_->document["records"]) {
        if (!record.is_object()) continue;
        const auto kind = record.value("kind", std::string{});
        const auto status = record.value("status", std::string{});
        if (kind == "process") {
            ++summary.process_runs_started;
            if (status == "STARTED") continue;
            ++summary.process_runs_terminal;
            if (status == "CLEAN_EXIT") ++summary.clean_process_exits;
            else if (status == "UNKNOWN_TERMINATION") {
                ++summary.unknown_process_terminations;
            } else if (status == "CONFIRMED_CRASH") {
                ++summary.confirmed_process_crashes;
            }
            if (record.value("crash_evidence_checked", false)) {
                ++summary.crash_evidence_covered_runs;
            }
        } else if (kind == "session") {
            ++summary.sessions_started;
            if (status != "STARTED") ++summary.sessions_terminal;
            if (status == "UNKNOWN_TERMINATION") {
                ++summary.unknown_session_terminations;
            }
        }
    }
    if (summary.process_runs_terminal > 0) {
        summary.unknown_termination_availability = "VALID";
        summary.unknown_termination_reason = "durable_run_ledger_valid";
        summary.unknown_process_termination_ratio =
            static_cast<double>(summary.unknown_process_terminations) /
            static_cast<double>(summary.process_runs_terminal);
        if (impl_->evidence_provider_configured) {
            summary.confirmed_process_crash_ratio =
                static_cast<double>(summary.confirmed_process_crashes) /
                static_cast<double>(summary.process_runs_terminal);
        }
    }
    impl_->summary = summary;
    std::atomic_store_explicit(
        &summary_cache_,
        std::make_shared<const StabilitySummary>(std::move(summary)),
        std::memory_order_release);
}

std::string StabilityLedger::process_run_id() const {
    std::lock_guard lock(mutex_);
    return impl_->process_run_id;
}

StabilitySummary StabilityLedger::Summary() const {
    const auto summary = std::atomic_load_explicit(
        &summary_cache_, std::memory_order_acquire);
    return summary ? *summary : StabilitySummary{};
}

ScopedProcessRun::ScopedProcessRun(
    std::shared_ptr<StabilityLedger> ledger,
    StabilityRecoveryEvidence evidence)
    : ledger_(std::move(ledger)) {
    started_ = ledger_ && ledger_->BeginProcessRun(evidence);
}

ScopedProcessRun::~ScopedProcessRun() {
    if (started_ && ledger_) ledger_->FinishProcessRunClean();
}

void InstallStabilityLedger(std::shared_ptr<StabilityLedger> ledger) {
    std::lock_guard lock(g_installed_mutex);
    g_installed_ledger = std::move(ledger);
}

std::shared_ptr<StabilityLedger> InstalledStabilityLedger() {
    std::lock_guard lock(g_installed_mutex);
    return g_installed_ledger.lock();
}

} // namespace livekit::telemetry
