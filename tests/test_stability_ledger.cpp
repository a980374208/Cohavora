#include "src/telemetry/stability_ledger.h"
#include "tests/support/test_check.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using livekit::telemetry::ConfirmedCrashEvidence;
using livekit::telemetry::StabilityLedger;
using livekit::telemetry::StabilityRecoveryEvidence;
using livekit::telemetry::StabilitySessionTerminal;

class TemporaryLedgerDirectory final {
public:
    TemporaryLedgerDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            (std::string("cohavora-stability-ledger-") +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryLedgerDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path ledger_path() const {
        return path_ / "stability-ledger-v1.json";
    }

private:
    std::filesystem::path path_;
};

void CleanRunsAndSessionsHaveOneTerminal() {
    TemporaryLedgerDirectory directory;
    {
        auto ledger = std::make_shared<StabilityLedger>(directory.ledger_path());
        TEST_CHECK(ledger->BeginProcessRun());
        const auto session = ledger->BeginSession();
        TEST_CHECK(!session.empty());
        TEST_CHECK(ledger->FinishSession(
            session, StabilitySessionTerminal::Stopped));
        TEST_CHECK(!ledger->FinishSession(
            session, StabilitySessionTerminal::Stopped));
        TEST_CHECK(ledger->FinishProcessRunClean());
    }

    StabilityLedger next(directory.ledger_path());
    TEST_CHECK(next.BeginProcessRun());
    const auto summary = next.Summary();
    TEST_CHECK(summary.ledger_availability == "VALID");
    TEST_CHECK(summary.clean_process_exits == 1);
    TEST_CHECK(summary.unknown_process_terminations == 0);
    TEST_CHECK(summary.confirmed_process_crashes == 0);
    TEST_CHECK(summary.sessions_started == 1);
    TEST_CHECK(summary.sessions_terminal == 1);
    TEST_CHECK(summary.duplicate_terminals == 1);
    TEST_CHECK(next.FinishProcessRunClean());
}

void MissingTerminalIsUnknownUnlessEvidenceConfirmsCrash() {
    TemporaryLedgerDirectory directory;
    std::string interrupted_run;
    {
        StabilityLedger interrupted(directory.ledger_path());
        TEST_CHECK(interrupted.BeginProcessRun());
        interrupted_run = interrupted.process_run_id();
        TEST_CHECK(!interrupted.BeginSession().empty());
    }

    StabilityRecoveryEvidence evidence;
    evidence.provider_configured = true;
    evidence.confirmed_crashes.push_back(ConfirmedCrashEvidence{
        interrupted_run, "windows_wer"});
    StabilityLedger recovered(directory.ledger_path());
    TEST_CHECK(recovered.BeginProcessRun(evidence));
    auto summary = recovered.Summary();
    TEST_CHECK(summary.confirmed_crash_availability == "VALID");
    TEST_CHECK(summary.confirmed_process_crashes == 1);
    TEST_CHECK(summary.unknown_process_terminations == 0);
    TEST_CHECK(summary.unknown_session_terminations == 1);
    TEST_CHECK(summary.confirmed_process_crash_ratio == 1.0);
    TEST_CHECK(recovered.FinishProcessRunClean());

    StabilityLedger no_provider(directory.ledger_path());
    TEST_CHECK(no_provider.BeginProcessRun());
    summary = no_provider.Summary();
    TEST_CHECK(summary.confirmed_crash_availability == "UNSUPPORTED");
    TEST_CHECK(summary.confirmed_process_crashes == 1);
    TEST_CHECK(summary.confirmed_process_crash_ratio < 0.0);
    TEST_CHECK(no_provider.FinishProcessRunClean());
}

void UnconfirmedMissingTerminalAndCorruptionStaySeparate() {
    TemporaryLedgerDirectory directory;
    {
        StabilityLedger interrupted(directory.ledger_path());
        TEST_CHECK(interrupted.BeginProcessRun());
    }
    {
        StabilityLedger recovered(directory.ledger_path());
        TEST_CHECK(recovered.BeginProcessRun());
        const auto summary = recovered.Summary();
        TEST_CHECK(summary.unknown_process_terminations == 1);
        TEST_CHECK(summary.confirmed_process_crashes == 0);
        TEST_CHECK(summary.unknown_process_termination_ratio == 1.0);
        TEST_CHECK(recovered.FinishProcessRunClean());
    }

    {
        std::ofstream corrupt(directory.ledger_path(),
            std::ios::binary | std::ios::trunc);
        corrupt << "{partial";
    }
    StabilityLedger reset(directory.ledger_path());
    TEST_CHECK(reset.BeginProcessRun());
    const auto summary = reset.Summary();
    TEST_CHECK(summary.corrupt_inputs == 1);
    TEST_CHECK(summary.unknown_process_terminations == 0);
    TEST_CHECK(summary.confirmed_process_crashes == 0);
    TEST_CHECK(reset.FinishProcessRunClean());
}

void LedgerRemainsBoundedWithoutEvictingTheActiveRun() {
    TemporaryLedgerDirectory directory;
    StabilityLedger ledger(directory.ledger_path(), 8, 16 * 1024);
    TEST_CHECK(ledger.BeginProcessRun());
    for (int i = 0; i != 24; ++i) {
        const auto session = ledger.BeginSession();
        TEST_CHECK(!session.empty());
        TEST_CHECK(ledger.FinishSession(
            session, StabilitySessionTerminal::Cancelled));
    }
    TEST_CHECK(ledger.FinishProcessRunClean());
    TEST_CHECK(std::filesystem::file_size(directory.ledger_path()) <= 16 * 1024);

    StabilityLedger next(directory.ledger_path(), 8, 16 * 1024);
    TEST_CHECK(next.BeginProcessRun());
    const auto summary = next.Summary();
    TEST_CHECK(summary.clean_process_exits == 1);
    TEST_CHECK(summary.process_runs_started >= 2);
    TEST_CHECK(summary.sessions_started <= 6);
    TEST_CHECK(next.FinishProcessRunClean());
}

} // namespace

int main() {
    CleanRunsAndSessionsHaveOneTerminal();
    MissingTerminalIsUnknownUnlessEvidenceConfirmsCrash();
    UnconfirmedMissingTerminalAndCorruptionStaySeparate();
    LedgerRemainsBoundedWithoutEvictingTheActiveRun();
    return 0;
}
