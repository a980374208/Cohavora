#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/sdp_negotiation_trace.h"
#include <map>
#include "src/telemetry/diagnostic_file_sink.h"
#include "src/telemetry/diagnostic_spdlog_bridge.h"
#include "src/telemetry/telemetry_operation_timeline.h"
#include "tests/support/test_check.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <spdlog/spdlog.h>
#include "tests/support/diagnostic_sink_fault_checks.h"
#include "tests/support/diagnostic_detach_checks.h"

namespace {

using namespace std::chrono_literals;
using namespace livekit::diagnostic;

struct TemporaryDirectory final {
    TemporaryDirectory() {
        static std::atomic<unsigned> serial{0};
        path = std::filesystem::temp_directory_path() /
            ("cohavora-diagnostic-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(serial.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& root) {
    std::vector<nlohmann::json> result;
    for (const auto& run : std::filesystem::directory_iterator(root)) {
        if (!run.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(run.path())) {
            if (file.path().extension() != ".jsonl") continue;
            std::ifstream input(file.path(), std::ios::binary);
            std::string line;
            while (std::getline(input, line)) result.push_back(nlohmann::json::parse(line));
        }
    }
    return result;
}

void BoundedConcurrentAdmission() {
    DiagnosticPipeline pipeline;
    constexpr int threads = 8;
    constexpr int events_per_thread = 125000;
    std::vector<std::thread> producers;
    for (int i = 0; i < threads; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < events_per_thread; ++j)
                pipeline.TryEmit(Event::Received(ChatKind::Text, 17));
        });
    }
    for (auto& thread : producers) thread.join();
    const auto after_ordinary = pipeline.GetStatus();
    TEST_CHECK(after_ordinary.accepted == kOrdinaryEvents);
    TEST_CHECK(after_ordinary.dropped_ordinary == threads * events_per_thread - kOrdinaryEvents);
    TEST_CHECK(after_ordinary.pending == kOrdinaryEvents);
    for (std::size_t i = 0; i < kCriticalEvents; ++i)
        TEST_CHECK(pipeline.TryEmit(Event::Started("test-build")));
    TEST_CHECK(!pipeline.TryEmit(Event::Started("test-build")));
    const auto full = pipeline.GetStatus();
    TEST_CHECK(full.pending == kQueueEvents);
    TEST_CHECK(full.dropped_critical == 1);
    TEST_CHECK(full.queue_high_water == kQueueEvents);
    TEST_CHECK(pipeline.Close() == DrainResult::Failed);
}

void WritesTypedJsonAndRecovers() {
    TemporaryDirectory directory;
    auto root = directory.path / "logs";
    std::ofstream(root) << "blocked";
    DiagnosticPipeline pipeline;
    TEST_CHECK(pipeline.TryEmit(Event::Started("test-build",
        "01234567-89ab-cdef-0123456789abcdef-1")));
    TEST_CHECK(pipeline.StartWriter(root));
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (pipeline.GetStatus().sink_failures > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(pipeline.GetStatus().sink_failures > 0);
    TEST_CHECK(!pipeline.GetStatus().sink_available);
    TEST_CHECK(std::filesystem::remove(root));
    std::filesystem::create_directory(root);
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (pipeline.GetStatus().written > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(pipeline.GetStatus().written > 0);
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);

    const auto records = ReadEvents(root);
    std::set<std::uint64_t> sequences;
    bool started = false, failed = false, recovered = false, terminal = false;
    for (const auto& record : records) {
        TEST_CHECK(record.at("schema_version") == 1);
        TEST_CHECK(record.at("process_run_id").get<std::string>().size() == 32);
        TEST_CHECK(record.at("event_sequence").get<std::uint64_t>() > 0);
        TEST_CHECK(sequences.insert(record.at("event_sequence").get<std::uint64_t>()).second);
        const auto name = record.at("event_name").get<std::string>();
        if (name == "process.started") {
            started = true;
            TEST_CHECK(record.at("attributes").at("symbol_identity") ==
                "01234567-89ab-cdef-0123456789abcdef-1");
        }
        failed |= name == "diagnostics.sink.failed";
        recovered |= name == "diagnostics.sink.recovered";
        terminal |= name == "process.terminal";
    }
    TEST_CHECK(started && failed && recovered && terminal);
}

void MakeSegment(const std::filesystem::path& path,
                 std::uint64_t bytes = DiagnosticFileSink::kSegmentBytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.seekp(static_cast<std::streamoff>(bytes - 1));
    output.put('\n');
    TEST_CHECK(output.good());
}

void BatchWritesPreserveOrderAndCommitBoundary() {
    TemporaryDirectory directory;
    DiagnosticFileSink sink(directory.path, std::string(32, '1'));
    std::vector<Event> batch;
    for (std::uint64_t i = 1; i <= 64; ++i) {
        auto event = Event::Received(ChatKind::Text, i);
        event.event_sequence = i;
        batch.push_back(event);
    }
    TEST_CHECK(sink.WriteBatch(batch) == batch.size());
    TEST_CHECK(sink.last_committed_sequence() == 64);
    const auto rows = ReadEvents(directory.path);
    TEST_CHECK(rows.size() == 64);
    for (std::size_t i = 0; i < rows.size(); ++i)
        TEST_CHECK(rows[i]["event_sequence"] == i + 1);
    batch.push_back(batch.back());
    TEST_CHECK(sink.WriteBatch(batch) == 0); // Bounded batch, no partial admission.
    TEST_CHECK(sink.last_committed_sequence() == 64);
}

void RotatesAndReclaimsOwnedSegments() {
    TemporaryDirectory directory;
    const auto root = directory.path / "logs";
    const std::string run_id(32, 'a');
    const auto run = root / ("run-" + run_id);
    MakeSegment(run / "segment-000000.jsonl",
                DiagnosticFileSink::kSegmentBytes - 32);
    DiagnosticFileSink sink(root, run_id);
    TEST_CHECK(sink.Write(Event::Started("test-build")));
    TEST_CHECK(std::filesystem::file_size(run / "segment-000000.jsonl") <
               DiagnosticFileSink::kSegmentBytes);
    TEST_CHECK(std::filesystem::exists(run / "segment-000001.jsonl"));
    sink.Close();

    const auto quota_root = directory.path / "quota";
    const auto old_run = quota_root / ("run-" + std::string(32, 'b'));
    for (int i = 0; i < 10; ++i) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "segment-%06d.jsonl", i);
        MakeSegment(old_run / name);
    }
    const auto unrelated = quota_root / "user-file.txt";
    std::ofstream(unrelated) << "preserve";
    DiagnosticFileSink next(quota_root, std::string(32, 'c'));
    TEST_CHECK(next.Write(Event::Started("test-build")));
    TEST_CHECK(!std::filesystem::exists(old_run / "segment-000000.jsonl"));
    TEST_CHECK(std::filesystem::exists(unrelated));
}

void ActiveRunCannotBeReclaimed() {
    TemporaryDirectory directory;
    const auto root = directory.path / "logs";
    const auto active_run = root / ("run-" + std::string(32, 'd'));
    for (int i = 0; i < 10; ++i) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "segment-%06d.jsonl", i);
        MakeSegment(active_run / name);
    }
    const auto lease_path = active_run / L"active.lock";
    const HANDLE lease = CreateFileW(lease_path.c_str(), GENERIC_READ, 0,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
    TEST_CHECK(lease != INVALID_HANDLE_VALUE);
    DiagnosticFileSink sink(root, std::string(32, 'e'));
    TEST_CHECK(!sink.Write(Event::Started("test-build")));
    TEST_CHECK(sink.failure_reason() == FailureReason::QuotaExceeded);
    TEST_CHECK(std::filesystem::exists(active_run / "segment-000000.jsonl"));
    std::vector<Event> batch(8, Event::Started("test-build"));
    TEST_CHECK(sink.WriteBatch(batch) == 0);
    TEST_CHECK(sink.failure_reason() == FailureReason::QuotaExceeded);
    CloseHandle(lease);
    TEST_CHECK(sink.WriteBatch(batch) == batch.size()); // Failed batch released the quota lock.

    DiagnosticFileSink invalid(root, "../invalid");
    TEST_CHECK(!invalid.Write(Event::Started("test-build")));
    TEST_CHECK(!std::filesystem::exists(root / "invalid"));
}

void RestrictsUnregisteredSpdlogOutput() {
    auto pipeline = std::make_shared<DiagnosticPipeline>();
    TEST_CHECK(InstallSafeSpdlogAdapter(pipeline));
    spdlog::error("private spdlog canary 4831");
    TEST_CHECK(pipeline->GetStatus().suppressed == 1);
}

void QuotaDefersLiveGrowthUntilRotationAndRetriesFailedAdmission() {
    TemporaryDirectory directory;
    const auto root = directory.path / "logs";
    const auto active_run = root / ("run-" + std::string(32, '6'));
    const auto segment = active_run / "segment-000000.jsonl";
    MakeSegment(segment, DiagnosticFileSink::kTotalBytes -
        DiagnosticFileSink::kSegmentBytes - 1024 * 1024);
    const auto own_run = root / ("run-" + std::string(32, '7'));
    MakeSegment(own_run / "segment-000000.jsonl",
        DiagnosticFileSink::kSegmentBytes - 4096);
    const auto lease = CreateFileW((active_run / L"active.lock").c_str(),
        GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(lease != INVALID_HANDLE_VALUE);
    const auto file = CreateFileW(segment.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    DiagnosticFileSink sink(root, std::string(32, '7'));
    auto event = Event::Received(ChatKind::Text, 1);
    event.event_sequence = 1;
    TEST_CHECK(sink.Write(event));
    LARGE_INTEGER size{};
    size.QuadPart = DiagnosticFileSink::kTotalBytes;
    TEST_CHECK(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
    TEST_CHECK(SetEndOfFile(file));
    event.event_sequence = 2;
    TEST_CHECK(sink.Write(event)); // Same segment deliberately permits excess.
    TEST_CHECK(sink.last_committed_sequence() == 2);
    // Only a few KiB remain. Rotation must inspect live sizes and refuse a
    // new segment while the foreign active run alone consumes the quota.
    bool blocked = false;
    for (int i = 0; i < 64; ++i) {
        ++event.event_sequence;
        if (!sink.Write(event)) { blocked = true; break; }
    }
    TEST_CHECK(blocked);
    TEST_CHECK(std::filesystem::exists(own_run / "segment-000001.jsonl"));
    TEST_CHECK(sink.failure_reason() == FailureReason::QuotaExceeded);
    TEST_CHECK(sink.last_committed_sequence() == event.event_sequence - 1);
    TEST_CHECK(std::filesystem::exists(segment));
    size.QuadPart -= 1024 * 1024;
    TEST_CHECK(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
    TEST_CHECK(SetEndOfFile(file));
    TEST_CHECK(sink.Write(event));
    TEST_CHECK(sink.last_committed_sequence() == event.event_sequence);
    CloseHandle(file);
    CloseHandle(lease);
}

void QuotaReclaimsExpiredHistoryOnOpenAndRotation() {
    TemporaryDirectory directory;
    const auto old_run = directory.path / ("run-" + std::string(32, '8'));
    const auto expired = old_run / "segment-000000.jsonl";
    const auto recent = old_run / "segment-000001.jsonl";
    MakeSegment(expired, 16);
    MakeSegment(recent, 16);
    std::filesystem::last_write_time(expired,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    const auto unrelated = old_run / "segment-other.jsonl";
    std::ofstream(unrelated) << "preserve";
    const auto own_run = directory.path / ("run-" + std::string(32, '9'));
    MakeSegment(own_run / "segment-000000.jsonl",
        DiagnosticFileSink::kSegmentBytes - 4096);
    DiagnosticFileSink sink(directory.path, std::string(32, '9'));
    TEST_CHECK(sink.Write(Event::Received(ChatKind::Text, 1)));
    TEST_CHECK(!std::filesystem::exists(expired));
    TEST_CHECK(std::filesystem::exists(recent));
    TEST_CHECK(std::filesystem::exists(unrelated));
    // Becoming expired does not trigger an in-segment directory scan.
    std::filesystem::last_write_time(recent,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    TEST_CHECK(sink.Write(Event::Received(ChatKind::Text, 2)));
    TEST_CHECK(std::filesystem::exists(recent));
    for (int i = 0; i < 64 &&
         !std::filesystem::exists(own_run / "segment-000001.jsonl"); ++i)
        TEST_CHECK(sink.Write(Event::Received(ChatKind::Text, 3)));
    TEST_CHECK(std::filesystem::exists(own_run / "segment-000001.jsonl"));
    TEST_CHECK(!std::filesystem::exists(recent));
    TEST_CHECK(std::filesystem::exists(unrelated));
}

void ReopeningTheSameSegmentRechecksQuota() {
    TemporaryDirectory directory;
    const auto old_run = directory.path / ("run-" + std::string(32, 'b'));
    const auto history = old_run / "segment-000000.jsonl";
    MakeSegment(history, 16);
    DiagnosticFileSink sink(directory.path, std::string(32, 'c'));
    TEST_CHECK(sink.Write(Event::Received(ChatKind::Text, 1)));
    sink.Close();
    // Closing/reopening must invalidate the successful admission even if the
    // segment index stays unchanged (also used by failure recovery paths).
    std::filesystem::last_write_time(history,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    TEST_CHECK(sink.Write(Event::Received(ChatKind::Text, 2)));
    TEST_CHECK(!std::filesystem::exists(history));
}

void RejectsUnprojectedEventFields() {
    TemporaryDirectory directory;
    DiagnosticPipeline pipeline;
    auto unsafe = Event::Started("test-build");
    std::memcpy(unsafe.build_id.data(), "private build name", 19);
    TEST_CHECK(!pipeline.TryEmit(unsafe));
    TEST_CHECK(pipeline.GetStatus().suppressed == 1);
    DiagnosticFileSink sink(directory.path / "logs", std::string(32, 'f'));
    TEST_CHECK(!sink.Write(unsafe));
    TEST_CHECK(!std::filesystem::exists(sink.run_directory()));
}

void BusinessCatalogKeepsOnlyTypedFields() {
    TemporaryDirectory directory;
    DiagnosticPipeline pipeline;
    const auto root = directory.path / "business";
    TEST_CHECK(pipeline.StartWriter(root));
    Event http;
    http.kind = EventKind::HttpRequestCompleted;
    http.route = Route::JoinMeeting;
    http.outcome = Outcome::Failure;
    http.error_layer = ErrorLayer::Business;
    http.http_status = 403;
    http.business_code = 4107;
    TEST_CHECK(http.context.request_id.Assign("request_123"));
    TEST_CHECK(http.context.parent_operation_id.Assign("admission_456"));
    TEST_CHECK(http.context.legacy_operation_id.Assign("1790409600123"));
    TEST_CHECK(pipeline.TryEmit(http));

    Event reconnect;
    reconnect.kind = EventKind::ReconnectAttemptTerminal;
    reconnect.stage = Stage::FullRestart;
    reconnect.attempt = 2;
    reconnect.outcome = Outcome::Timeout;
    reconnect.error_code = ErrorCode::JoinTimeout;
    reconnect.context.room_generation = 3;
    reconnect.context.has_room_generation = true;
    TEST_CHECK(pipeline.TryEmit(reconnect));

    Event rtc;
    rtc.kind = EventKind::RtcSdpFailed;
    rtc.stage = Stage::SetRemoteDescription;
    rtc.error_layer = ErrorLayer::Rtc;
    rtc.rtc_error_type = 7;
    TEST_CHECK(pipeline.TryEmit(rtc));
    pipeline.OpenDiagnosticWindow(5ms);
    TEST_CHECK(pipeline.DiagnosticWindowActive());
    std::this_thread::sleep_for(10ms);
    TEST_CHECK(!pipeline.DiagnosticWindowActive());
    Event lateWindowSample;
    lateWindowSample.kind = EventKind::SignalMessageSummary;
    lateWindowSample.window_sample = true;
    TEST_CHECK(!pipeline.TryEmit(lateWindowSample));
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);

    bool sawHttp = false, sawReconnect = false, sawRtc = false;
    bool sawDiagnosticMode = false;
    for (const auto& record : ReadEvents(root)) {
        const auto name = record.at("event_name").get<std::string>();
        if (name == "http.request.completed") {
            sawHttp = true;
            TEST_CHECK(record.at("component") == "net");
            TEST_CHECK(record.at("request_id") == "request_123");
            TEST_CHECK(record.at("parent_operation_id") == "admission_456");
            TEST_CHECK(record.at("legacy_operation_id") == "1790409600123");
            TEST_CHECK(record.at("attributes").at("route") == "join_meeting");
            TEST_CHECK(record.at("error_layer") == "business");
            TEST_CHECK(record.at("attributes").at("business_error") == "unknown");
            TEST_CHECK(!record.at("attributes").contains("business_code"));
        } else if (name == "reconnect.attempt.terminal") {
            sawReconnect = true;
            TEST_CHECK(record.at("component") == "room");
            TEST_CHECK(record.at("stage") == "full_restart");
            TEST_CHECK(record.at("attributes").at("attempt") == 2);
            TEST_CHECK(record.at("attributes").at("mode") == "full_restart");
            TEST_CHECK(record.at("room_generation") == 3);
            TEST_CHECK(record.at("error_code") == "join_timeout");
        } else if (name == "rtc.sdp.failed") {
            sawRtc = true;
            TEST_CHECK(record.at("attributes").at("rtc_error_type") == 7);
            TEST_CHECK(record.at("stage") == "set_remote_description");
        } else if (name == "diagnostics.mode.changed" &&
                   record.at("attributes").at("mode") == "diagnostic") {
            sawDiagnosticMode = true;
            TEST_CHECK(record.at("attributes").at("expires_at_utc_ms")
                .get<std::int64_t>() >= record.at("occurred_at_utc_ms")
                .get<std::int64_t>());
        }
        TEST_CHECK(record.dump().find("private payload canary") == std::string::npos);
    }
    TEST_CHECK(sawHttp && sawReconnect && sawRtc && sawDiagnosticMode);
}

void CloseRejectsConcurrentProducers() {
    TemporaryDirectory directory;
    DiagnosticPipeline pipeline;
    pipeline.SetRetentionEnabled(false);
    TEST_CHECK(pipeline.StartWriter(directory.path / "logs"));
    std::atomic<bool> running{true};
    std::thread producer([&] {
        while (running.load(std::memory_order_relaxed))
            pipeline.TryEmit(Event::Received(ChatKind::Text, 12));
    });
    std::thread closer([&] { pipeline.Close(); });
    const auto result = pipeline.Close();
    closer.join();
    running.store(false, std::memory_order_relaxed);
    producer.join();
    TEST_CHECK(result == DrainResult::Completed);
    TEST_CHECK(!pipeline.TryEmit(Event::Started("late")));
    TEST_CHECK(pipeline.GetStatus().pending == 0);
}

void OperationTimelineKeepsSafeBoundedCorrelation() {
    TemporaryDirectory directory;
    const auto root = directory.path / "timeline";
    const std::string session(32, 'a');
    const std::string other_session(32, 'b');
    DiagnosticPipeline pipeline;
    TEST_CHECK(pipeline.StartWriter(root));
    Event started;
    started.kind = EventKind::AdmissionStarted;
    TEST_CHECK(started.context.anonymous_session_id.Assign(session));
    TEST_CHECK(started.context.operation_id.Assign("admission:42:1"));
    TEST_CHECK(pipeline.TryEmit(started));
    Event terminal = started;
    terminal.kind = EventKind::AdmissionTerminal;
    terminal.outcome = Outcome::Success;
    TEST_CHECK(pipeline.TryEmit(terminal));
    Event other = started;
    TEST_CHECK(other.context.anonymous_session_id.Assign(other_session));
    TEST_CHECK(pipeline.TryEmit(other));
    TEST_CHECK(!other.context.operation_id.Assign("unsafe:private:canary"));
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);

    const auto complete = livekit::telemetry::ReadOperationTimeline(root, session);
    TEST_CHECK(complete.source_available);
    TEST_CHECK(complete.entries.size() == 2);
    TEST_CHECK(complete.entries[0].operation_id == "admission:42:1");
    TEST_CHECK(complete.entries[1].outcome == "success");
    const auto limited = livekit::telemetry::ReadOperationTimeline(root, session, 1);
    TEST_CHECK(limited.entries.size() == 1 && limited.omitted == 1);
    TEST_CHECK(limited.entries.front().event_name == "admission.terminal");

    const auto run = root / ("run-" + std::string(pipeline.run_id()));
    const auto segment = run / "segment-000000.jsonl";
    auto forged = ReadEvents(root).front();
    forged["operation_id"] = "privatecanary";
    auto wrong_run = ReadEvents(root).front();
    wrong_run["process_run_id"] = std::string(32, 'f');
    {
        std::ofstream output(segment, std::ios::binary | std::ios::app);
        output << forged.dump() << '\n';
        output << wrong_run.dump() << '\n';
        output << std::string(17 * 1024, 'x') << '\n';
        output << "{\"event_name\":\"admission.terminal\"";
    }
    const auto damaged = livekit::telemetry::ReadOperationTimeline(root, session);
    TEST_CHECK(damaged.entries.size() == 2);
    TEST_CHECK(damaged.invalid_lines == 4);
    TEST_CHECK(damaged.entries[0].operation_id.find("canary") ==
               std::string::npos);
}

void RetentionAndClearRemainIndependent() {
    TemporaryDirectory directory;
    const auto root = directory.path / "diagnostics";
    const auto telemetry = directory.path / "telemetry" / "reports";
    std::filesystem::create_directories(telemetry);
    std::ofstream(telemetry / "sentinel") << "keep";
    std::string old_run;
    {
        DiagnosticPipeline previous;
        old_run = std::string(previous.run_id());
        TEST_CHECK(previous.StartWriter(root));
        TEST_CHECK(previous.TryEmit(Event::Started("test-build")));
        TEST_CHECK(previous.Close() == DrainResult::Completed);
    }
    DiagnosticPipeline current;
    TEST_CHECK(current.StartWriter(root));
    TEST_CHECK(current.TryEmit(Event::Started("test-build")));
    current.SetRetentionEnabled(false);
    TEST_CHECK(!current.GetStatus().retention_enabled);
    TEST_CHECK(current.TryEmit(Event::Received(ChatKind::Text, 17)));
    for (int attempt = 0; attempt != 200; ++attempt) {
        if (current.GetStatus().pending == 0) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(current.GetStatus().pending == 0);
    current.SetRetentionEnabled(true);
    TEST_CHECK(current.GetStatus().retention_enabled);
    const auto current_segment = root /
        ("run-" + std::string(current.run_id())) /
        "segment-000000.jsonl";
    for (int attempt = 0; attempt != 200; ++attempt) {
        if (std::filesystem::exists(current_segment)) break;
        std::this_thread::sleep_for(10ms);
    }
    TEST_CHECK(std::filesystem::exists(current_segment));
    const auto cleaned = DiagnosticFileSink::ClearInactiveHistory(root);
    TEST_CHECK(cleaned.success && cleaned.removed_segments >= 1);
    TEST_CHECK(cleaned.active_runs_skipped >= 1);
    TEST_CHECK(!std::filesystem::exists(
        root / ("run-" + old_run) / "segment-000000.jsonl"));
    TEST_CHECK(std::filesystem::exists(current_segment));
    TEST_CHECK(std::filesystem::exists(telemetry / "sentinel"));
    TEST_CHECK(current.Close() == DrainResult::Completed);
    const auto events = ReadEvents(root);
    std::size_t changes = 0;
    for (const auto& event : events) {
        changes += event.at("event_name") == "diagnostics.retention.changed";
        TEST_CHECK(event.at("event_name") != "chat.received");
    }
    TEST_CHECK(changes == 2);
    TEST_CHECK(DiagnosticFileSink::ClearInactiveHistory(root).success);
    TEST_CHECK(std::filesystem::exists(telemetry / "sentinel"));
}

void DisabledEventsDoNotBackfillAfterReenable() {
    TemporaryDirectory directory;
    const auto root = directory.path / "diagnostics";
    DiagnosticPipeline pipeline;
    std::atomic<bool> mirror_entered{false};
    std::atomic<bool> release_mirror{false};
    pipeline.SetMirror([&](const Event&) {
        if (!mirror_entered.exchange(true)) {
            while (!release_mirror.load()) std::this_thread::yield();
        }
    });
    TEST_CHECK(pipeline.StartWriter(root));
    TEST_CHECK(pipeline.TryEmit(Event::Started("test-build")));
    for (int attempt = 0; attempt != 200 && !mirror_entered.load(); ++attempt)
        std::this_thread::sleep_for(10ms);
    TEST_CHECK(mirror_entered.load());
    pipeline.SetRetentionEnabled(false);
    TEST_CHECK(pipeline.TryEmit(Event::Received(ChatKind::Text, 918273)));
    pipeline.SetRetentionEnabled(true);
    release_mirror.store(true);
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);
    bool saw_disabled_event = false;
    for (const auto& event : ReadEvents(root)) {
        if (event.at("event_name") == "chat.received" &&
            event.at("attributes").at("bytes") == 918273)
            saw_disabled_event = true;
    }
    TEST_CHECK(!saw_disabled_event);
}

void FrozenCatalogTerminalEventsStayTyped() {
    TEST_CHECK(BusinessErrorName(100010) == "token_expired");
    TEST_CHECK(BusinessErrorName(100002) == "token_invalid");
    TEST_CHECK(BusinessErrorName(4107) == "unknown");
    TEST_CHECK(ComponentName(EventKind::SessionStopped) == "session_runtime");
    TEST_CHECK(SeverityName(Severity::Trace) == "trace");
    TEST_CHECK(EventSeverity(EventKind::ChatReceived) == Severity::Debug);
    TEST_CHECK(EventSeverity(EventKind::ReconnectEpisodeStarted) == Severity::Warning);
    TEST_CHECK(EventSeverity(EventKind::MediaRecoveryTimeout) == Severity::Warning);
    TEST_CHECK(EventSeverity(EventKind::MeetingBackendNotificationCompleted) ==
        Severity::Warning);
    TEST_CHECK(EventSeverity(Event::Issue(IssueCode::QtFatal)) == Severity::Fatal);
    TEST_CHECK(!IsCritical(EventKind::HttpRequestStarted));
    TEST_CHECK(!IsCritical(EventKind::AdmissionStageChanged));
    TEST_CHECK(!IsCritical(EventKind::MediaPublishStarted));
    TEST_CHECK(IsCritical(EventKind::ReconnectAttemptTerminal));
    TEST_CHECK(IsCritical(EventKind::MediaRecoveryTimeout));
    TemporaryDirectory directory;
    DiagnosticPipeline pipeline;
    TEST_CHECK(pipeline.StartWriter(directory.path / "events"));
    Event chat;
    chat.kind = EventKind::ChatSendTerminal;
    chat.chat_kind = ChatKind::Text;
    chat.outcome = Outcome::Success;
    chat.bytes = 17;
    TEST_CHECK(pipeline.TryEmit(chat));
    Event transfer;
    transfer.kind = EventKind::TransferTerminal;
    transfer.transfer_kind = TransferKind::Image;
    transfer.transfer_direction = TransferDirection::Receive;
    transfer.outcome = Outcome::Success;
    transfer.bytes = 2048;
    TEST_CHECK(pipeline.TryEmit(transfer));
    transfer.transfer_kind = TransferKind::Unknown;
    transfer.outcome = Outcome::Cancelled;
    transfer.bytes = 0;
    TEST_CHECK(pipeline.TryEmit(transfer));
    Event subscription;
    subscription.kind = EventKind::MediaSubscriptionChanged;
    subscription.media_kind = MediaKind::Video;
    subscription.subscription_state = SubscriptionState::Blocked;
    subscription.error_code = ErrorCode::CodecUnsupported;
    TEST_CHECK(pipeline.TryEmit(subscription));
    Event render;
    render.kind = EventKind::RenderBackendChanged;
    render.from_render_backend = RenderBackend::Gpu;
    render.to_render_backend = RenderBackend::QtCpu;
    render.render_reason = RenderReason::DeviceFailure;
    TEST_CHECK(pipeline.TryEmit(render));
    Event mode;
    mode.kind = EventKind::ReconnectModeChanged;
    mode.stage = Stage::FullRestart;
    mode.error_code = ErrorCode::JoinTimeout;
    TEST_CHECK(pipeline.TryEmit(mode));
    Event recovery;
    recovery.kind = EventKind::MediaRecoveryMilestone;
    recovery.context.operation_id.Assign("reconnect:42:1");
    recovery.context.recovery_epoch = 3;
    recovery.context.has_recovery_epoch = true;
    recovery.media_kind = MediaKind::Video;
    recovery.recovery_measurement = RecoveryMeasurement::VisibleRenderStable;
    recovery.duration_ms = 450;
    TEST_CHECK(pipeline.TryEmit(recovery));
    recovery.kind = EventKind::MediaRecoveryTimeout;
    recovery.outcome = Outcome::Timeout;
    TEST_CHECK(pipeline.TryEmit(recovery));
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);

    std::set<std::string> names;
    for (const auto& record : ReadEvents(directory.path / "events")) {
        const auto name = record.at("event_name").get<std::string>();
        names.insert(name);
        if (name == "chat.send.terminal") {
            TEST_CHECK(record.at("component") == "meeting_ui");
            TEST_CHECK(record.at("attributes").at("chat_kind") == "text");
            TEST_CHECK(record.at("attributes").at("bytes") == 17);
        } else if (name == "transfer.terminal") {
            TEST_CHECK(record.at("component") == "meeting_ui");
            TEST_CHECK(record.at("attributes").at("direction") == "receive");
            if (record.at("attributes").at("bytes") == 2048)
                TEST_CHECK(record.at("attributes").at("transfer_kind") == "image");
            else
                TEST_CHECK(!record.at("attributes").contains("transfer_kind"));
        } else if (name == "media.subscription.changed") {
            TEST_CHECK(record.at("component") == "room");
            TEST_CHECK(record.at("attributes").at("subscription_state") == "blocked");
            TEST_CHECK(record.at("attributes").at("reason_code") == "codec_unsupported");
            TEST_CHECK(record.at("error_code") == "codec_unsupported");
        } else if (name == "render.backend.changed") {
            TEST_CHECK(record.at("component") == "render");
            TEST_CHECK(record.at("attributes").at("from_backend") == "gpu");
            TEST_CHECK(record.at("attributes").at("to_backend") == "qt_cpu");
            TEST_CHECK(record.at("attributes").at("reason_code") == "device_failure");
            TEST_CHECK(!record.at("attributes").contains("backend"));
        } else if (name == "reconnect.mode_changed") {
            TEST_CHECK(record.at("attributes").at("from_mode") == "resume");
            TEST_CHECK(record.at("attributes").at("to_mode") == "full_restart");
            TEST_CHECK(record.at("attributes").at("reason_code") == "join_timeout");
        } else if (name == "media.recovery.milestone" ||
                   name == "media.recovery.timeout") {
            TEST_CHECK(record.at("component") == "session_telemetry");
            TEST_CHECK(record.at("operation_id") == "reconnect:42:1");
            TEST_CHECK(record.at("recovery_epoch") == 3);
            TEST_CHECK(record.at("attributes").at("measurement_point") ==
                "visible_render_stably_recovered");
        }
        TEST_CHECK(record.dump().find("chat-body-canary") == std::string::npos);
    }
    TEST_CHECK(names.contains("chat.send.terminal"));
    TEST_CHECK(names.contains("transfer.terminal"));
    TEST_CHECK(names.contains("media.subscription.changed"));
    TEST_CHECK(names.contains("render.backend.changed"));
    TEST_CHECK(names.contains("reconnect.mode_changed"));
    TEST_CHECK(names.contains("media.recovery.milestone"));
    TEST_CHECK(names.contains("media.recovery.timeout"));
}

void BenchmarkPauseIsBoundedAndDoesNotCreateSequenceGaps() {
    TemporaryDirectory directory;
    DiagnosticPipeline pipeline;
    TEST_CHECK(pipeline.StartWriter(directory.path));
    TEST_CHECK(pipeline.TryEmit(Event::Started("test-build")));
    pipeline.PauseProductionForBenchmark(20ms);
    TEST_CHECK(pipeline.ProductionPausedForBenchmark());
    TEST_CHECK(!pipeline.TryEmit(Event::Received(ChatKind::Text, 17)));
    TEST_CHECK(pipeline.BenchmarkSuppressed() == 1);
    std::this_thread::sleep_for(30ms);
    TEST_CHECK(!pipeline.ProductionPausedForBenchmark());
    TEST_CHECK(pipeline.TryEmit(Event::Received(ChatKind::Text, 17)));
    pipeline.PauseProductionForBenchmark(60000ms);
    pipeline.PauseProductionForBenchmark(0ms);
    TEST_CHECK(!pipeline.ProductionPausedForBenchmark());
    TEST_CHECK(pipeline.Close() == DrainResult::Completed);
    auto events = ReadEvents(directory.path);
    TEST_CHECK(events.size() == 4); // started, resumed event, stopping, terminal
    for (std::size_t i = 0; i < events.size(); ++i)
        TEST_CHECK(events[i]["event_sequence"] == i + 1);
    TEST_CHECK(pipeline.GetStatus().dropped_ordinary == 0);
}

} // namespace

void SdpRoundsPersistOrderedTypedEvidence() {
    TemporaryDirectory directory;
    auto pipeline = std::make_shared<DiagnosticPipeline>();
    InstallBusinessPipeline(pipeline);
    TEST_CHECK(pipeline->StartWriter(directory.path));
    Context first;
    first.operation_id.Assign("sdp_101");
    first.parent_operation_id.Assign("join_1");
    first.room_generation = 42;
    first.has_room_generation = true;
    Context second = first;
    second.operation_id.Assign("sdp_102");
    {
        SdpNegotiationTrace a(first, SdpRole::Publisher, true);
        SdpNegotiationTrace b(second, SdpRole::Subscriber);
        a.Record(SdpAction::SetLocal, SdpPhase::Completed, SdpState::Stable,
            SdpState::HaveLocalOffer, SdpReason::None, 0, ThreadRole::Rtc, SdpDescription::Offer);
        a.Finish(Outcome::Cancelled, SdpReason::Superseded);
        a.Record(SdpAction::ReceiveAnswer, SdpPhase::Completed);
        a.Finish(Outcome::Success);
        b.Record(SdpAction::SetRemote, SdpPhase::Failed, SdpState::Stable,
            SdpState::Stable, SdpReason::ParseError, 0, ThreadRole::Rtc, SdpDescription::Offer);
        b.Finish(Outcome::Failure, SdpReason::ParseError);
    }
    InstallBusinessPipeline({});
    TEST_CHECK(pipeline->Close() == DrainResult::Completed);
    std::map<std::string, std::uint64_t> sequences;
    std::map<std::string, int> terminals;
    bool late = false, applied = false, failed = false;
    for (const auto& event : ReadEvents(directory.path)) {
        if (event["event_name"] != "rtc.sdp.step") continue;
        const auto id = event["operation_id"].get<std::string>();
        const auto& attrs = event["attributes"];
        TEST_CHECK(attrs["round_sequence"] == ++sequences[id]);
        TEST_CHECK(event["room_generation"] == 42);
        TEST_CHECK(event["parent_operation_id"] == "join_1");
        if (attrs["action"] == "round" && attrs["phase"] != "started") ++terminals[id];
        if (attrs["action"] == "set_local") {
            applied = true;
            TEST_CHECK(event["stage"] == "set_local_description");
            TEST_CHECK(attrs["description_type"] == "offer");
            TEST_CHECK(attrs["signaling_before"] == "stable");
            TEST_CHECK(attrs["signaling_after"] == "have_local_offer");
        }
        if (attrs["action"] == "receive_answer") {
            late = true;
            TEST_CHECK(attrs["after_terminal"] == true);
        }
        if (attrs["action"] == "set_remote") {
            failed = true;
            TEST_CHECK(event["stage"] == "set_remote_description");
            TEST_CHECK(event["error_layer"] == "parse");
        }
        TEST_CHECK(!attrs.contains("sdp"));
    }
    TEST_CHECK(applied && failed && late);
    TEST_CHECK(terminals["sdp_101"] == 1 && terminals["sdp_102"] == 1);
    Event invalid;
    invalid.kind = EventKind::RtcSdpStep;
    TEST_CHECK(!IsValidEvent(invalid));
    invalid.context = first;
    invalid.sdp_sequence = 1;
    TEST_CHECK(IsValidEvent(invalid));
    invalid.sdp_description = static_cast<SdpDescription>(255);
    TEST_CHECK(!IsValidEvent(invalid));
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--quota-child")
        return diagnostic_sink_checks::ChildMain(argv[2], argv[3]);
    {
        TemporaryDirectory directory;
        diagnostic_detach_checks::BlockedMirror(directory.path / "mirror");
        diagnostic_detach_checks::BlockedFileOpen(directory.path / "file");
    }
    SdpRoundsPersistOrderedTypedEvidence();
    BoundedConcurrentAdmission();
    WritesTypedJsonAndRecovers();
    BatchWritesPreserveOrderAndCommitBoundary();
    RotatesAndReclaimsOwnedSegments();
    ActiveRunCannotBeReclaimed();
    QuotaDefersLiveGrowthUntilRotationAndRetriesFailedAdmission();
    QuotaReclaimsExpiredHistoryOnOpenAndRotation();
    ReopeningTheSameSegmentRechecksQuota();
    {
        TemporaryDirectory directory;
        diagnostic_sink_checks::Run(directory.path);
    }
    RestrictsUnregisteredSpdlogOutput();
    RejectsUnprojectedEventFields();
    BusinessCatalogKeepsOnlyTypedFields();
    CloseRejectsConcurrentProducers();
    OperationTimelineKeepsSafeBoundedCorrelation();
    RetentionAndClearRemainIndependent();
    DisabledEventsDoNotBackfillAfterReenable();
    FrozenCatalogTerminalEventsStayTyped();
    BenchmarkPauseIsBoundedAndDoesNotCreateSequenceGaps();
    return 0;
}
