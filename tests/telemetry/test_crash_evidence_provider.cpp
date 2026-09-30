#include "src/telemetry/crash_evidence_provider.h"
#include "src/telemetry/build_identity.h"
#include "tests/support/test_check.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <crtdbg.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using livekit::telemetry::CrashEvidenceProvider;
using livekit::telemetry::StabilityLedger;

const std::string& BuildId() {
    return livekit::telemetry::CurrentExecutableBuildId();
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("cohavora-crash-provider-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

DWORD Child(const std::filesystem::path& root, const wchar_t* mode,
            const wchar_t* run_id) {
    wchar_t executable[MAX_PATH]{};
    TEST_CHECK(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
    const std::wstring command = std::wstring(L"\"") + executable +
        L"\" --child \"" + root.wstring() + L"\" " + mode + L" " + run_id;
    auto mutable_command = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    TEST_CHECK(CreateProcessW(executable, mutable_command.data(), nullptr,
        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    if (std::wstring_view(mode) == L"kill") {
        bool ready = false;
        for (int i = 0; i != 500; ++i) {
            if (std::filesystem::exists(root / "ready")) {
                ready = true;
                break;
            }
            Sleep(10);
        }
        TEST_CHECK(ready);
        TEST_CHECK(TerminateProcess(process.hProcess, 0xdead));
    }
    TEST_CHECK(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    TEST_CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

__declspec(noinline) void AccessViolationProbe() {
    *reinterpret_cast<volatile int*>(0) = 1;
}

std::string SymbolizeExactBuild(
    const livekit::telemetry::CrashEvidenceMetadata& evidence,
    std::string_view expected_build_id,
    std::string_view expected_symbol_identity) {
    if (!evidence.main_module_address ||
        evidence.build_id != expected_build_id ||
        expected_build_id != BuildId() ||
        expected_symbol_identity !=
            livekit::telemetry::CurrentExecutablePdbIdentity()) return {};
    const auto process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
        SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(process, nullptr, TRUE)) return {};
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    const auto address = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr)) +
        evidence.main_module_rva;
    const bool found = SymFromAddr(process, address, &displacement, symbol);
    const std::string name = found ? symbol->Name : "";
    SymCleanup(process);
    return name;
}

int ChildMain(const std::filesystem::path& root, std::wstring_view mode,
              std::wstring_view run_id) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (mode == L"missing") {
        RaiseException(0xe000c001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 5;
    }
    std::string narrow_run_id;
    for (const auto ch : run_id) {
        TEST_CHECK(ch >= L'0' && ch <= L'f');
        narrow_run_id.push_back(static_cast<char>(ch));
    }
    auto provider = CrashEvidenceProvider::Install(
        root, narrow_run_id, BuildId());
    TEST_CHECK(provider->installed());
    if (mode == L"clean") return 0;
    if (mode == L"shutdown") {
        provider.reset();
        RaiseException(0xe000c001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 5;
    }
    if (mode == L"kill") {
        std::ofstream(root / "ready") << "ready";
        Sleep(15000);
        return 5;
    }
    if (mode == L"abort") std::abort();
    if (mode == L"terminate") std::terminate();
    AccessViolationProbe();
    return 5;
}

void FaultMatrix() {
#if COHAVORA_ENABLE_MINIDUMP
    TEST_CHECK(livekit::telemetry::CurrentExecutablePdbIdentity() != "unknown");
#endif
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "evidence";
    const std::wstring access_id(32, L'a');
    const std::wstring abort_id(32, L'b');
    const std::wstring terminate_id(32, L'c');
    const std::wstring clean_id(32, L'd');
    const std::wstring killed_id(32, L'e');
    const std::wstring missing_id(32, L'f');
    const std::wstring shutdown_id(32, L'1');
    TEST_CHECK(Child(root, L"access", access_id.c_str()) != 0);
    TEST_CHECK(Child(root, L"abort", abort_id.c_str()) != 0);
    TEST_CHECK(Child(root, L"terminate", terminate_id.c_str()) != 0);
    TEST_CHECK(Child(root, L"clean", clean_id.c_str()) == 0);
    TEST_CHECK(Child(root, L"kill", killed_id.c_str()) == 0xdead);
    TEST_CHECK(Child(root, L"missing", missing_id.c_str()) != 0);
    TEST_CHECK(Child(root, L"shutdown", shutdown_id.c_str()) != 0);

    const auto scan = CrashEvidenceProvider::Scan(root);
    TEST_CHECK(scan.recovery.provider_configured);
    TEST_CHECK(scan.records.size() == 3);
    TEST_CHECK(scan.rejected_files == 1);
    for (const auto& item : scan.records) {
        TEST_CHECK(item.build_id == BuildId());
        TEST_CHECK(item.code != 0);
        if (item.process_run_id == std::string(32, 'a')) {
            TEST_CHECK(item.code == EXCEPTION_ACCESS_VIOLATION);
            TEST_CHECK(item.main_module_address);
            TEST_CHECK(item.main_module_rva != 0);
#if COHAVORA_ENABLE_MINIDUMP
            const auto symbol = SymbolizeExactBuild(item, BuildId(),
                livekit::telemetry::CurrentExecutablePdbIdentity());
            TEST_CHECK(symbol.find("AccessViolationProbe") != std::string::npos);
            TEST_CHECK(SymbolizeExactBuild(item, BuildId(),
                "00000000-0000-0000-0000000000000000-1").empty());
            TEST_CHECK(SymbolizeExactBuild(item, std::string(64, '0'),
                livekit::telemetry::CurrentExecutablePdbIdentity()).empty());
#endif
        }
    }
    TEST_CHECK(!std::filesystem::exists(root / "crash-v1-dddddddddddddddddddddddddddddddd.bin"));
    std::size_t dumps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".dmp") continue;
        ++dumps;
        std::ifstream input(entry.path(), std::ios::binary);
        MINIDUMP_HEADER header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        TEST_CHECK(input.good());
        TEST_CHECK(header.Signature == MINIDUMP_SIGNATURE);
        TEST_CHECK(header.NumberOfStreams > 0);
    }
#if COHAVORA_ENABLE_MINIDUMP
    TEST_CHECK(dumps == 3);
    for (const char id : {'a', 'b', 'c'})
        TEST_CHECK(std::filesystem::exists(root /
            ("crash-v1-" + std::string(32, id) + ".dmp")));
#else
    TEST_CHECK(dumps == 0);
#endif

    const auto ledger_path = temporary.path() / "ledger.json";
    {
        StabilityLedger crashed(ledger_path);
        TEST_CHECK(crashed.BeginProcessRun({}, std::string(32, 'a'), BuildId()));
    }
    StabilityLedger recovered(ledger_path);
    TEST_CHECK(recovered.BeginProcessRun(scan.recovery));
    TEST_CHECK(recovered.Summary().confirmed_process_crashes == 1);
    TEST_CHECK(recovered.FinishProcessRunClean());
    {
        StabilityLedger killed(ledger_path);
        TEST_CHECK(killed.BeginProcessRun({}, std::string(32, 'e'), BuildId()));
    }
    StabilityLedger killed_recovery(ledger_path);
    TEST_CHECK(killed_recovery.BeginProcessRun(scan.recovery));
    TEST_CHECK(killed_recovery.Summary().unknown_process_terminations == 1);
    TEST_CHECK(killed_recovery.FinishProcessRunClean());

    const auto corrupted = root / "crash-v1-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bin";
    std::fstream file(corrupted, std::ios::in | std::ios::out | std::ios::binary);
    TEST_CHECK(file.is_open());
    file.put('x');
    file.close();
    TEST_CHECK(CrashEvidenceProvider::Scan(root).records.size() == 2);
}

void BuildMismatchAndUnavailableDirectoryStayUnknown() {
    TemporaryDirectory temporary;
    const auto blocked = temporary.path() / "blocked";
    std::ofstream(blocked) << "not a directory";
    auto provider = CrashEvidenceProvider::Install(
        blocked, std::string(32, 'a'), BuildId());
    TEST_CHECK(!provider->installed());
    TEST_CHECK(!CrashEvidenceProvider::Scan(blocked).recovery.provider_configured);

    const auto ledger_path = temporary.path() / "ledger.json";
    {
        StabilityLedger interrupted(ledger_path);
        TEST_CHECK(interrupted.BeginProcessRun({}, std::string(32, 'a'),
            std::string(64, 'c')));
    }
    livekit::telemetry::StabilityRecoveryEvidence evidence;
    evidence.provider_configured = true;
    evidence.confirmed_crashes.push_back({std::string(32, 'a'),
        "windows_seh_metadata", BuildId()});
    StabilityLedger recovered(ledger_path);
    TEST_CHECK(recovered.BeginProcessRun(evidence));
    TEST_CHECK(recovered.Summary().confirmed_process_crashes == 0);
    TEST_CHECK(recovered.Summary().unknown_process_terminations == 1);
    TEST_CHECK(recovered.FinishProcessRunClean());
}

void EvidenceQuotaOnlyPrunesOwnedStaleSlots() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "evidence";
    std::filesystem::create_directory(root);
    const auto unknown = root / "user-owned.bin";
    std::ofstream(unknown) << "keep";
    for (int i = 0; i != 130; ++i) {
        std::ostringstream id;
        id << std::hex << std::setfill('0') << std::setw(32) << i;
        const auto path = root / ("crash-v1-" + id.str() + ".bin");
        std::ofstream(path).close();
        std::filesystem::last_write_time(path,
            std::filesystem::file_time_type::clock::now() -
                std::chrono::hours(24 * 8));
    }
    {
        auto provider = CrashEvidenceProvider::Install(
            root, std::string(32, 'f'), BuildId());
        TEST_CHECK(provider->installed());
    }
    TEST_CHECK(std::filesystem::exists(unknown));
    TEST_CHECK(CrashEvidenceProvider::Scan(root).rejected_files == 0);
    std::size_t owned = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().filename().string().starts_with("crash-v1-")) ++owned;
    TEST_CHECK(owned == 0);
}

void CollectionPolicyIsReadBeforeApplicationStartup() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "evidence";
    TEST_CHECK(CrashEvidenceProvider::CollectionEnabled(root));
    TEST_CHECK(CrashEvidenceProvider::SetCollectionEnabled(root, false));
    TEST_CHECK(!CrashEvidenceProvider::CollectionEnabled(root));
    auto disabled = CrashEvidenceProvider::Install(
        root, std::string(32, 'a'), BuildId(),
        CrashEvidenceProvider::CollectionEnabled(root));
    TEST_CHECK(!disabled->installed());
    TEST_CHECK(disabled->reason() == "disabled_by_user");
    TEST_CHECK(CrashEvidenceProvider::SetCollectionEnabled(root, true));
    TEST_CHECK(CrashEvidenceProvider::CollectionEnabled(root));
    std::ofstream(root / "policy-v1.json", std::ios::trunc) << "{broken";
    TEST_CHECK(!CrashEvidenceProvider::CollectionEnabled(root));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--child")
        return ChildMain(argv[2], argv[3], argv[4]);
    FaultMatrix();
    BuildMismatchAndUnavailableDirectoryStayUnknown();
    EvidenceQuotaOnlyPrunesOwnedStaleSlots();
    CollectionPolicyIsReadBeforeApplicationStartup();
    return 0;
}
