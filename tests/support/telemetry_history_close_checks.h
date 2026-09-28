#pragma once
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/telemetry_checkpoint.h"
#include "tests/support/test_check.h"
#include <windows.h>
#include <condition_variable>
#include <fstream>
#include <iostream>

namespace livekit::telemetry {
struct TelemetryHistoryStoreTestAccess {
    static std::weak_ptr<void> Context(TelemetryHistoryStore& s) { return s.context_; }
    static HANDLE Thread(TelemetryHistoryStore& s) {
        HANDLE h = nullptr;
        TEST_CHECK(DuplicateHandle(GetCurrentProcess(), s.worker_.native_handle(),
            GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0)); return h;
    }
    static void Boundary(TelemetryHistoryStore& s, bool after, std::function<void()> hook) {
        if (after) s.control_->after_manifest_ = std::move(hook);
        else s.control_->before_manifest_ = std::move(hook);
    }
};
}
namespace history_close_checks {
using namespace livekit::telemetry;
using namespace std::chrono_literals;
struct Block {
    std::mutex mutex; std::condition_variable cv; bool entered=false, release=false;
    void Wait() { std::unique_lock l(mutex); entered=true; cv.notify_all(); cv.wait(l,[&]{return release;}); }
    void Entered() { std::unique_lock l(mutex); TEST_CHECK(cv.wait_for(l,5s,[&]{return entered;})); }
    void Release() { { std::lock_guard l(mutex); release=true; } cv.notify_all(); }
};
inline void Exit(HANDLE thread, std::weak_ptr<void> context) {
    TEST_CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
    TEST_CHECK(context.expired());
}
inline void BoundaryTimeout(const std::filesystem::path& root,
    SafeTelemetryRecordPtr first, SafeTelemetryRecordPtr last, bool after) {
    const std::string run(32,'b');
    auto baseline = std::make_shared<SafeTelemetryRecord>(*first);
    baseline->source_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    first = baseline;
    TEST_CHECK(AppendTelemetryCheckpoint(root,first->anonymous_session_id,
        {MakeTelemetryCheckpointRecord(*first)},64ull*1024*1024,run).success);
    auto s=std::make_unique<TelemetryHistoryStore>(root,run);
    auto weak=TelemetryHistoryStoreTestAccess::Context(*s);
    auto thread=TelemetryHistoryStoreTestAccess::Thread(*s);
    auto block=std::make_shared<Block>();
    TelemetryHistoryStoreTestAccess::Boundary(*s,after,[block]{block->Wait();});
    TEST_CHECK(s->SubmitSnapshot(std::make_shared<Snapshot>(last->snapshot),{},last->anonymous_session_id));
    block->Entered();
    const auto start=std::chrono::steady_clock::now();
    const auto result=s->Close(200ms);
    const auto elapsed = std::chrono::steady_clock::now()-start;
    TEST_CHECK(elapsed < 2s);
    std::cout << "history_close boundary=" << (after ? "after" : "before")
        << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << std::endl;
    TEST_CHECK(result.state==TelemetryCloseState::TimedOut && result.disk_outcome_unknown);
    TEST_CHECK(result.status && result.status->pending_records==1);
    TEST_CHECK(s->Close().status==result.status);
    TEST_CHECK(!s->SubmitSnapshot(std::make_shared<Snapshot>(last->snapshot),{},last->anonymous_session_id));
    s.reset(); TEST_CHECK(!weak.expired());
    block->Release(); Exit(thread,weak);
    const auto disk=InspectTelemetryCheckpoint(root/("cohavora-telemetry-v2-"+first->anonymous_session_id));
    TEST_CHECK(disk.valid && disk.last_committed_revision==(after?last->snapshot.revision:first->snapshot.revision));
    TEST_CHECK(disk.session_complete==after);
    TEST_CHECK(result.state==TelemetryCloseState::TimedOut && result.status->pending_records==1);
    // Retrying the same revision is idempotent on both sides of the commit point.
    auto retry=AppendTelemetryCheckpoint(root,last->anonymous_session_id,
        {MakeTelemetryCheckpointRecord(*last)},64ull*1024*1024,run);
    TEST_CHECK(retry.success && retry.status.last_committed_revision==last->snapshot.revision);
    TEST_CHECK(retry.status.record_count==2);
}
inline void FinalFailure(const std::filesystem::path& root, SafeTelemetryRecordPtr record) {
    { std::ofstream blocked(root); blocked<<"not a directory"; }
    TelemetryHistoryStore s(root);
    TEST_CHECK(s.SubmitSnapshot(std::make_shared<Snapshot>(record->snapshot),{},record->anonymous_session_id));
    const auto r=s.Close();
    TEST_CHECK(r.state==TelemetryCloseState::Failed && !r.disk_outcome_unknown);
    TEST_CHECK(r.status->pending_records==1 && r.status->write_failures>0);
    TEST_CHECK(r.status->confirmed_commits.empty());
    TEST_CHECK(s.Close().status==r.status);
}
inline void FinalManifestFailure(const std::filesystem::path& root, SafeTelemetryRecordPtr first,
    SafeTelemetryRecordPtr last) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto baseline = std::make_shared<SafeTelemetryRecord>(*first);
    baseline->source_utc_ms = now;
    const auto initial = AppendTelemetryCheckpoint(root, {baseline});
    TEST_CHECK(initial.success);
    const auto manifest = initial.report_directory / "manifest.json";
    HANDLE reader = CreateFileW(manifest.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK(reader != INVALID_HANDLE_VALUE);
    TelemetryHistoryStore s(root);
    TEST_CHECK(s.SubmitSnapshot(std::make_shared<Snapshot>(last->snapshot),{},last->anonymous_session_id));
    const auto result = s.Close();
    TEST_CHECK(result.state == TelemetryCloseState::Failed);
    TEST_CHECK(result.status->pending_records == 1 && result.status->write_failures > 0);
    TEST_CHECK(result.status->confirmed_commits.empty());
    CloseHandle(reader);
    const auto disk = InspectTelemetryCheckpoint(initial.report_directory);
    TEST_CHECK(disk.valid && disk.last_committed_revision == first->snapshot.revision);
    TEST_CHECK(AppendTelemetryCheckpoint(root, {last}).success);
    TEST_CHECK(s.Close().status == result.status);
    std::cout << "history_close final_manifest_failure=PASS old_revision_preserved retry=PASS" << std::endl;
}
inline void UnconfirmedCommit(const std::filesystem::path& root, SafeTelemetryRecordPtr record) {
    TelemetryHistoryStore s(root);
    TelemetryHistoryStoreTestAccess::Boundary(s, true, [] { throw std::runtime_error("after replacement"); });
    TEST_CHECK(s.SubmitSnapshot(std::make_shared<Snapshot>(record->snapshot),{},record->anonymous_session_id));
    const auto result = s.Close();
    TEST_CHECK(result.state == TelemetryCloseState::Failed && result.disk_outcome_unknown);
    TEST_CHECK(result.status->pending_records == 1 && result.status->confirmed_commits.empty());
    const auto disk = InspectTelemetryCheckpoint(root/("cohavora-telemetry-v2-"+record->anonymous_session_id));
    TEST_CHECK(disk.valid && disk.last_committed_revision == record->snapshot.revision);
    std::cout << "history_close post_replace_failure=PASS disk_commit_preserved" << std::endl;
}
inline void Completed(const std::filesystem::path& root, SafeTelemetryRecordPtr record) {
    TelemetryHistoryStore s(root);
    TEST_CHECK(s.SubmitSnapshot(std::make_shared<Snapshot>(record->snapshot),{},record->anonymous_session_id));
    const auto result = s.Close();
    TEST_CHECK(result.state == TelemetryCloseState::Completed && !result.disk_outcome_unknown);
    TEST_CHECK(result.status->pending_records == 0 && result.status->confirmed_commits.size() == 1);
    TEST_CHECK(result.status->confirmed_commits.front().revision == record->snapshot.revision);
}
inline void LateCallback(const std::filesystem::path& root) {
    auto block=std::make_shared<Block>();
    auto s=std::make_unique<TelemetryHistoryStore>(root);
    auto weak=TelemetryHistoryStoreTestAccess::Context(*s);
    auto thread=TelemetryHistoryStoreTestAccess::Thread(*s);
    TEST_CHECK(s->ExportCurrent(root/"export",[block](auto){block->Wait();}));
    block->Entered();
    const auto begin=std::chrono::steady_clock::now();
    TEST_CHECK(s->Close().state==TelemetryCloseState::TimedOut);
    const auto elapsed=std::chrono::steady_clock::now()-begin;
    TEST_CHECK(elapsed>=4900ms && elapsed<7s);
    std::cout << "history_close callback_elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << std::endl;
    s.reset(); TEST_CHECK(!weak.expired());
    block->Release(); Exit(thread,weak);
}
}
