#pragma once

// Opt-in --debug companion evidence. These are explicitly in-process counters;
// the external PILOT collectors independently measure OS, SFU and receiver state.
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/media/desktop_capture.h"
#include "tests/runtime/probes/product_gpu_budget_snapshot.h"
#include "tests/runtime/probes/product_gpu_queue_snapshot.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <functional>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace MeetingApp {
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
                          bool collect_gpu_budget = false)
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
                if (gpu_budget) row["gpu_budget"] = Json::parse(gpu_budget->SampleJson());
                if (gpu_queue) row["gpu_queue"] = Json::parse(gpu_queue->SampleJson());
                const auto records = history->CurrentRecords();
                if (!records.empty()) {
                    const auto& s = records.back()->snapshot;
                    // Preserve availability alongside selected values. Nulls are
                    // never replaced with a fabricated zero by the verifier.
                    auto metrics = Json::array();
                    for (const auto& metric : livekit::telemetry::BuildSafeMetricRows(*records.back())) {
                        if (metric.key.starts_with("network.rtp.") ||
                            metric.key.starts_with("video.pipeline.") ||
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
                    row["metrics"] = std::move(metrics);
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
                output << row.dump() << '\n';
                output.flush();
            };
            while (!stop.stop_requested() && output) {
                sample();
                for (int tick = 0; tick != 10 && !stop.stop_requested(); ++tick)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (output) sample();
          } catch (...) {
            // Missing/truncated probe evidence fails the external completeness
            // check; diagnostics must not terminate the product under test.
          }
        }) {}
private:
    std::jthread worker_;
};
} // namespace MeetingApp
