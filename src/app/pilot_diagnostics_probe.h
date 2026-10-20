#pragma once

// Opt-in --debug companion evidence. These are explicitly in-process counters;
// the external PILOT collectors independently measure OS, SFU and receiver state.
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/media/desktop_capture.h"
#include "src/app/diagnostics/product_gpu_budget_snapshot.h"
#include "src/app/diagnostics/product_gpu_queue_snapshot.h"
#include "src/app/diagnostics/product_heap_optimize_diagnostic.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <fstream>
#include <functional>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace MeetingApp {
// Shared by the opt-in probe and its projection regression. Values already
// passed through the typed safe catalog; preserve unavailable values as null.
inline nlohmann::json BuildPilotDiagnosticMetrics(
    const livekit::telemetry::SafeTelemetryRecord& record) {
    using Json = nlohmann::json;
    auto metrics = Json::array();
    for (const auto& metric : livekit::telemetry::BuildSafeMetricRows(record)) {
        if (metric.key.starts_with("network.rtp.") ||
            metric.key.starts_with("video.pipeline.") ||
            metric.key.starts_with("camera.pipeline.") ||
            metric.key.starts_with("video.codec.decoders") ||
            metric.key.starts_with("audio.window.") ||
            metric.key.starts_with("audio.concealment.") ||
            metric.key.starts_with("resource.internal.") ||
            metric.key == "queue.depth" ||
            metric.key.starts_with("render.interval.bucket.") ||
            metric.key.starts_with("render.window.") ||
            metric.key == "render.interval.p95") {
            const auto value = std::visit([](const auto& v) -> Json {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>)
                    return nullptr;
                else return v;
            }, metric.value);
            metrics.push_back({{"key", metric.key}, {"value", value},
                {"availability", metric.availability}, {"unit", metric.unit}});
        }
    }
    return metrics;
}

// A value-only copy of the last completed default-heap maintenance attempt.
// Observation availability is separate from the platform/API outcome below.
struct HeapMaintenanceSnapshot {
    std::uint64_t attempts = 0;
    bool available = false;
    bool succeeded = false;
    std::uint32_t error = 0;
    bool callbackThrew = false;
    std::int64_t elapsedMicroseconds = 0;
};

class PilotDiagnosticsProbe final {
public:
    PilotDiagnosticsProbe(const std::filesystem::path& destination,
                          std::string run_id,
                          std::shared_ptr<livekit::telemetry::TelemetryHistoryStore> history,
                          std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline,
                          std::filesystem::path history_root,
                          std::filesystem::path diagnostic_root,
                          std::function<std::size_t()> cleanup_pending,
                          std::function<std::shared_ptr<const std::string>()> participant_fingerprint,
                          bool collect_gpu_budget = false,
                          std::uint64_t heap_optimize_generation = 0,
                          bool collect_heap_pages = false,
                          product_heap_diagnostic::Action heap_action = product_heap_diagnostic::Action::Optimize,
                          std::function<HeapMaintenanceSnapshot()> heap_maintenance_observation = {})
        : worker_([=](std::stop_token stop) {
          try {
            // The runner supplies a new, run-scoped destination.
            if (std::filesystem::exists(destination)) return;
            std::ofstream output(destination, std::ios::binary);
            if (!output) return;
            std::uint64_t sequence = 0;
            std::uint64_t terminal_generation = 0;
            auto terminal_seen = std::chrono::steady_clock::time_point{};
            nlohmann::json released_heap;
            product_heap_diagnostic::ReleaseGuard heap_optimize_guard(heap_optimize_generation);
            product_heap_diagnostic::JournalWorkspace heap_workspace(heap_optimize_generation && collect_heap_pages);
            nlohmann::json heap_optimize_result;
            std::unique_ptr<product_gpu_witness::BudgetSampler> gpu_budget;
            std::unique_ptr<product_gpu_witness::QueueSampler> gpu_queue;
            if (collect_gpu_budget) {
                gpu_budget = std::make_unique<product_gpu_witness::BudgetSampler>();
                gpu_queue = std::make_unique<product_gpu_witness::QueueSampler>();
            }
            auto sample = [&] {
                using Json = nlohmann::json;
                const auto h = history->Status();
                const auto d = pipeline->GetStatus();
                const auto capture = livekit::ObserveDesktopCapture();
                const auto participant = participant_fingerprint();
                Json row{{"schema", 1}, {"run_id", run_id},
                    {"sequence", ++sequence}, {"collector", "in_process"},
                    {"utc_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()},
                    {"process_run_id", std::string(pipeline->run_id())},
                    {"participant_sha256", participant ? Json(*participant) : Json(nullptr)},
                    {"native_cleanup_pending", cleanup_pending()},
                    {"history_root", history_root.generic_string()},
                    {"diagnostic_root", diagnostic_root.generic_string()},
                    {"history", {{"reason", h->reason}, {"queue_depth", h->queue_depth},
                        {"queue_bytes", h->queue_bytes}, {"inflight_jobs", h->inflight_jobs},
                        {"queue_peak_jobs", h->queue_peak_jobs},
                        {"queue_peak_bytes", h->queue_peak_bytes},
                        {"queue_job_limit_hits", h->queue_job_limit_hits},
                        {"queue_byte_limit_hits", h->queue_byte_limit_hits},
                        {"snapshot_max_us", h->snapshot_max_us},
                        {"checkpoint_max_us", h->checkpoint_max_us},
                        {"history_refresh_max_us", h->history_refresh_max_us},
                        {"history_refresh_last_us", h->history_refresh_last_us},
                        {"history_refresh_count", h->history_refresh_count},
                        {"queue_drops", h->queue_drops}, {"pending_bytes", h->pending_bytes},
                        {"memory_bytes", h->memory_bytes}, {"memory_records", h->memory_records},
                        {"pending_records_dropped", h->pending_records_dropped},
                        {"write_failures", h->write_failures},
                        {"checkpoint_revision", h->checkpoint_revision}}},
                    {"diagnostic", {{"accepted", d.accepted}, {"written", d.written},
                        {"benchmark_production_paused", pipeline->ProductionPausedForBenchmark()},
                        {"benchmark_suppressed", pipeline->BenchmarkSuppressed()},
                        {"suppressed", d.suppressed},
                        {"dropped_ordinary", d.dropped_ordinary},
                        {"dropped_critical", d.dropped_critical},
                        {"sink_failures", d.sink_failures}, {"pending", d.pending},
                        {"retention_enabled", d.retention_enabled},
                        {"last_committed_sequence", d.last_committed_sequence}}},
                    {"capture", {{"backend", capture.backend}, {"frames", capture.frames},
                        {"failures", capture.failures}, {"failure_reason", capture.failure_reason},
                        {"binding_failures", capture.binding_failures},
                        {"binding_failure_reason", capture.binding_failure_reason}}}};
                HeapMaintenanceSnapshot maintenance;
                bool maintenanceObserved = false;
                if (heap_maintenance_observation) {
                    try {
                        maintenance = heap_maintenance_observation();
                        maintenanceObserved = true;
                    } catch (...) {
                        // A snapshot read failure is not a maintenance callback
                        // failure and must not stop unrelated probe evidence.
                    }
                }
                row["heap_maintenance_observation_status"] =
                    maintenanceObserved ? "AVAILABLE" : "NOT_AVAILABLE";
                row["heap_maintenance_observation"] = nullptr;
                if (maintenanceObserved) {
                    row["heap_maintenance_observation"] = {
                        {"attempt", maintenance.attempts}, {"available", maintenance.available},
                        {"succeeded", maintenance.succeeded}, {"error", maintenance.error},
                        {"callback_threw", maintenance.callbackThrew},
                        {"elapsed_us", maintenance.elapsedMicroseconds}};
                }
                if (gpu_budget) row["gpu_budget"] = Json::parse(gpu_budget->SampleJson());
                if (gpu_queue) row["gpu_queue"] = Json::parse(gpu_queue->SampleJson());
                const auto records = history->CurrentRecords();
                if (records.empty())
                    heap_optimize_guard.Claim(0, false, cleanup_pending(), std::chrono::steady_clock::now());
                if (!records.empty()) {
                    const auto& s = records.back()->snapshot;
                    // Diagnostic intervention only. The diagnostic runner holds
                    // the final released phase until this receipt is present.
                    if (heap_optimize_guard.Claim(s.session_generation, s.session_complete,
                            cleanup_pending(), std::chrono::steady_clock::now())) {
                        // If explicitly requested storage could not be reserved,
                        // fail closed rather than optimize without paired pages.
                        if (collect_heap_pages && !heap_workspace.get()) {
                            heap_optimize_result = {{"generation", s.session_generation}, {"pid", CurrentProcessId()}, {"run_id", run_id},
                                {"attempted", false}, {"reason", "page_workspace_unavailable"},
                                {"action", heap_action == product_heap_diagnostic::Action::Observe ? "observe" : "optimize"},
                                {"page_journal_requested", true}, {"page_journal_available", false},
                                {"formal_credit", 0}, {"qualification_credit", 0}};
                        } else {
                            const auto intervention = product_heap_diagnostic::Run(s.session_generation, heap_action, heap_workspace.get());
                            heap_optimize_result = {{"generation", intervention.generation},
                                {"run_id", run_id},
                                {"pid", CurrentProcessId()}, {"scope", "default_process_heap_only"},
                                {"attempted", intervention.attempted}, {"succeeded", intervention.succeeded},
                                {"platform_available", intervention.platform_available},
                                {"operation", StampJson(intervention.operation)},
                                {"before", SampleJson(intervention.before)},
                                {"after", SampleJson(intervention.after)},
                                {"qualification_credit", 0}, {"formal_credit", 0}};
                            heap_optimize_result["journal_precondition_failed"] = intervention.journal_precondition_failed;
                            if (intervention.journal_precondition_failed)
                                heap_optimize_result["reason"] = "before_page_or_heap_metadata_incomplete";
                            heap_optimize_result["process_creation"] = ProcessCreationJson();
                            heap_optimize_result["action"] = heap_action == product_heap_diagnostic::Action::Observe ? "observe" : "optimize";
                            heap_optimize_result["page_journal_requested"] = collect_heap_pages;
                            heap_optimize_result["page_journal_available"] = false;
                            if (const auto* journal = heap_workspace.get()) {
                                const auto journal_path = destination.parent_path() / "heap-page-journal.json";
                                if (!std::filesystem::exists(journal_path)) {
                                    std::ofstream page_output(journal_path, std::ios::binary);
                                    const Json page_json{{"schema", 1}, {"run_id", run_id},
                                        {"pid", CurrentProcessId()}, {"generation", intervention.generation},
                                        {"action", heap_optimize_result["action"]},
                                        {"scope", "process_private_pages_no_heap_ownership_claim"},
                                        {"workspace_base", journal->workspace_base}, {"workspace_size", journal->workspace_size},
                                        {"before", PageSampleJson(journal->before)}, {"after", PageSampleJson(journal->after)},
                                        {"formal_credit", 0}, {"qualification_credit", 0}};
                                    if (page_output) { page_output << page_json.dump() << '\n'; page_output.flush(); }
                                    if (page_output) {
                                        heap_optimize_result["page_journal_available"] = true;
                                        heap_optimize_result["page_journal_file"] = "heap-page-journal.json";
                                    }
                                }
                            }
                        }
                    }
                    // Preserve availability alongside selected values. Nulls are
                    // never replaced with a fabricated zero by the verifier.
                    row["metrics"] = BuildPilotDiagnosticMetrics(*records.back());
                    row["revision"] = s.revision;
                    row["session_complete"] = s.session_complete;
                    row["anonymous_session_id"] = records.back()->anonymous_session_id;
                    row["session_generation"] = s.session_generation;
                    row["source_utc_ms"] = records.back()->source_utc_ms;
                    auto histogram = Json::object();
                    for (std::size_t i = 0; i < s.render_fine_interval_histogram.size(); ++i)
                        if (s.render_fine_interval_histogram[i])
                            histogram[std::to_string(i)] = s.render_fine_interval_histogram[i];
                    row["render_interval_histogram_ms"] = std::move(histogram);
                    row["render_expected_bindings"] = s.render_expected_bindings;
                    if (s.session_complete && cleanup_pending() == 0) {
                        if (terminal_generation != s.session_generation) {
                            terminal_generation = s.session_generation;
                            terminal_seen = std::chrono::steady_clock::now();
                            released_heap = nullptr;
                        }
                        // One read-only default-heap summary in the settled
                        // release window. No pointers or allocation contents are
                        // emitted, and no trimming changes the measured process.
                        if (released_heap.is_null() &&
                            std::chrono::steady_clock::now() - terminal_seen >= std::chrono::seconds(5)) {
#if defined(_WIN32)
                            HEAP_SUMMARY summary{};
                            summary.cb = sizeof(summary);
                            const auto began = std::chrono::steady_clock::now();
                            const bool available = HeapSummary(GetProcessHeap(), 0, &summary) != FALSE;
                            released_heap = {{"available", available},
                                {"scope", "default_process_heap_only"},
                                {"session_generation", s.session_generation},
                                {"sampled_utc_ms", row["utc_ms"]},
                                {"allocated_bytes", available ? Json(summary.cbAllocated) : Json(nullptr)},
                                {"committed_bytes", available ? Json(summary.cbCommitted) : Json(nullptr)},
                                {"reserved_bytes", available ? Json(summary.cbReserved) : Json(nullptr)},
                                {"elapsed_us", std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - began).count()}};
#else
                            released_heap = {{"available", false}, {"reason", "windows_only"}};
#endif
                        }
                        row["released_heap"] = released_heap;
                    }
                }
                if (heap_optimize_generation)
                    row["heap_optimize_diagnostic"] = heap_optimize_result;
                // Stamp the completed observation, after reading its source snapshot.
                row["utc_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                output << row.dump() << '\n';
                output.flush();
            };
            auto next_sample = std::chrono::steady_clock::now();
            constexpr auto sample_period = std::chrono::seconds(1);
            while (!stop.stop_requested() && output) {
                sample();
                next_sample += sample_period;
                const auto completed_at = std::chrono::steady_clock::now();
                // Skip missed slots; never burst to catch up after a slow sample.
                while (next_sample <= completed_at)
                    next_sample += sample_period;
                while (!stop.stop_requested()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_sample) break;
                    const auto stop_check = now + std::chrono::milliseconds(100);
                    std::this_thread::sleep_until(
                        stop_check < next_sample ? stop_check : next_sample);
                }
            }
            if (output) sample();
          } catch (...) {
            // Missing/truncated probe evidence fails the external completeness
            // check; diagnostics must not terminate the product.
          }
        }) {}
private:
    static unsigned long CurrentProcessId() noexcept {
#if defined(_WIN32)
        return GetCurrentProcessId();
#else
        return 0;
#endif
    }
    static nlohmann::json StampJson(const product_heap_diagnostic::CounterStamp& stamp) {
        return {{"available", stamp.available}, {"utc_ms", stamp.utc_ms},
            {"elapsed_us", stamp.elapsed_us}, {"win32_error", stamp.error}};
    }
    static nlohmann::json SampleJson(const product_heap_diagnostic::MemorySample& sample) {
        using Json = nlohmann::json;
        return {{"process", StampJson(sample.process)},
            {"private_usage_bytes", sample.process.available ? Json(sample.private_usage_bytes) : Json(nullptr)},
            {"heaps", StampJson(sample.heaps)},
            {"process_heap_count", sample.heaps.available ? Json(sample.process_heap_count) : Json(nullptr)},
            {"summary", StampJson(sample.summary)},
            {"allocated_bytes", sample.summary.available ? Json(sample.allocated_bytes) : Json(nullptr)},
            {"committed_bytes", sample.summary.available ? Json(sample.committed_bytes) : Json(nullptr)},
            {"reserved_bytes", sample.summary.available ? Json(sample.reserved_bytes) : Json(nullptr)},
            {"virtual_memory", StampJson(sample.virtual_memory)},
            {"private_commit_bytes", sample.virtual_memory.available ? Json(sample.private_commit_bytes) : Json(nullptr)},
            {"private_commit_regions", sample.virtual_memory.available ? Json(sample.private_commit_regions) : Json(nullptr)},
            {"virtual_query_complete", sample.virtual_query_complete},
            {"virtual_query_count", sample.virtual_query_count},
            {"virtual_query_budget_exhausted", sample.virtual_query_budget_exhausted},
            {"virtual_query_overflow", sample.virtual_query_overflow}};
    }
    static nlohmann::json ProcessCreationJson() {
        bool available = false;
        std::uint64_t ticks = 0;
        unsigned long error = 0;
#if defined(_WIN32)
        FILETIME created{}, exited{}, kernel{}, user{};
        available = ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
        if (available) ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        else error = ::GetLastError();
#endif
        return {{"available", available}, {"creation_filetime_100ns", available ? nlohmann::json(ticks) : nlohmann::json(nullptr)},
            {"win32_error", error}};
    }
    static nlohmann::json PageSampleJson(const product_heap_diagnostic::PageSample& sample) {
        using Json = nlohmann::json;
        auto rows = Json::array();
        for (std::uint32_t i = 0; i < sample.row_count; ++i) {
            const auto& r = sample.rows[i];
            rows.push_back({{"base", r.base}, {"allocation_base", r.allocation_base},
                {"size", r.size}, {"state", r.state}, {"type", r.type}, {"protect", r.protect}});
        }
        auto handles = Json::array();
        for (std::uint32_t i = 0; i < sample.heap_handles_count; ++i) handles.push_back(sample.heap_handles[i]);
        return {{"stamp", StampJson(sample.stamp)}, {"heap_stamp", StampJson(sample.heap_stamp)},
            {"default_heap", sample.default_heap}, {"heap_count", sample.heap_count},
            {"heap_handles", handles}, {"heap_handles_complete", sample.heap_handles_complete},
            {"heap_handles_truncated", sample.heap_handles_truncated},
            {"rows", rows}, {"row_count", sample.row_count}, {"query_count", sample.query_count},
            {"row_capacity", 4096}, {"query_budget", 65536}, {"heap_capacity", 64},
            {"complete", sample.complete}, {"overflow", sample.overflow},
            {"truncated", sample.truncated}, {"query_budget_exhausted", sample.query_budget_exhausted}};
    }
    std::jthread worker_;
};
} // namespace MeetingApp
