#include "crash_evidence_provider.h"
#include "../core/sensitive_memory_policy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#if COHAVORA_ENABLE_MINIDUMP
#include <DbgHelp.h>
#endif
#include <csignal>
#include <exception>
#endif

namespace livekit::telemetry {
namespace {

constexpr std::string_view kPrefix = "crash-v1-";
constexpr std::string_view kSuffix = ".bin";
constexpr std::array<char, 8> kMagic{'C', 'O', 'H', 'C', 'R', 'S', 'H', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kAbortCode = 0xe0000001;
constexpr std::uint32_t kTerminateCode = 0xe0000002;
constexpr auto kPolicyFile = "policy-v1.json";

#pragma pack(push, 1)
struct CrashRecord {
    std::array<char, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t process_id = 0;
    std::array<char, 32> run_id{};
    std::array<char, 64> build_id{};
    std::uint32_t code = 0;
    std::uint32_t main_module_address = 0;
    std::uint64_t main_module_rva = 0;
    std::uint32_t checksum = 0;
};
#pragma pack(pop)
static_assert(sizeof(CrashRecord) == 132);

bool HexId(std::string_view value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f'); });
}

std::uint32_t Checksum(const CrashRecord& record) noexcept {
    std::uint32_t hash = 2166136261u;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
    for (std::size_t i = 0; i < offsetof(CrashRecord, checksum); ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

bool OwnedName(std::string_view name) {
    return name.starts_with(kPrefix) && name.ends_with(kSuffix) &&
        HexId(name.substr(kPrefix.size(), 32), 32) &&
        name.size() == kPrefix.size() + 32 + kSuffix.size();
}

bool SafeDirectory(const std::filesystem::path& root) {
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error) || error)
        return false;
    for (auto path = root; !path.empty(); path = path.parent_path()) {
        if (std::filesystem::is_symlink(path, error) || error) return false;
        if (path == path.parent_path()) break;
    }
    return true;
}

bool ReadRecord(const std::filesystem::path& path, CrashRecord& record) {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || error ||
        !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) != sizeof(record) || error)
        return false;
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(&record), sizeof(record));
    return input.gcount() == sizeof(record) && !input.bad() &&
        record.magic == kMagic && record.version == kVersion &&
        record.process_id != 0 && record.code != 0 &&
        record.main_module_address <= 1 && record.checksum == Checksum(record) &&
        HexId(std::string_view(record.run_id.data(), record.run_id.size()), 32) &&
        HexId(std::string_view(record.build_id.data(), record.build_id.size()), 64);
}

bool PruneOwnedEvidence(const std::filesystem::path& root) {
    struct Artifact {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uint64_t bytes = 0;
        bool removable = false;
    };
    std::vector<Artifact> files;
    std::uint64_t total_bytes = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!OwnedName(name)) continue;
        if (files.size() >= 4096 || it->is_symlink(error) || error ||
            !it->is_regular_file(error) || error) return false;
        const auto bytes = it->file_size(error);
        const auto modified = it->last_write_time(error);
        if (error || bytes > (std::numeric_limits<std::uint64_t>::max)() -
                total_bytes) return false;
        CrashRecord record{};
        const bool valid = ReadRecord(it->path(), record) &&
            std::string_view(record.run_id.data(), record.run_id.size()) ==
                std::string_view(name.data() + kPrefix.size(), 32);
        files.push_back({it->path(), modified, bytes, valid || bytes == 0});
        total_bytes += bytes;
    }
    if (error) return false;
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.modified < right.modified;
    });
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24 * 7);
    std::size_t count = files.size();
    for (const auto& file : files) {
        const bool pressure = count >= 128 ||
            total_bytes > 1024 * 1024 - sizeof(CrashRecord);
        if (!pressure && file.modified >= cutoff) break;
        if (!file.removable) continue;
        std::filesystem::remove(file.path, error);
        if (error) {
            error.clear();
            continue;
        }
        --count;
        total_bytes -= file.bytes;
    }
    return count < 128 &&
        total_bytes <= 1024 * 1024 - sizeof(CrashRecord);
}

#if defined(_WIN32)
struct HandlerState {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
#if COHAVORA_ENABLE_MINIDUMP
    std::filesystem::path dump_path;
#endif
    CrashRecord record;
    std::atomic<bool> captured{false};
    std::uintptr_t main_base = 0;
    std::size_t main_size = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
    void (*previous_abort)(int) = SIG_DFL;
    std::terminate_handler previous_terminate = nullptr;
};

std::atomic<HandlerState*> g_handler{nullptr};

void Capture(HandlerState* state, std::uint32_t code,
             std::uintptr_t address, EXCEPTION_POINTERS* pointers = nullptr) noexcept {
    if (!state || state->captured.exchange(true, std::memory_order_acq_rel))
        return;
    CrashRecord record = state->record;
    record.code = code;
    if (address >= state->main_base &&
        address - state->main_base < state->main_size) {
        record.main_module_address = 1;
        record.main_module_rva = address - state->main_base;
    }
    record.checksum = Checksum(record);
    DWORD written = 0;
    WriteFile(state->file, &record, sizeof(record), &written, nullptr);
#if COHAVORA_ENABLE_MINIDUMP
    // Paths are prepared at startup. A failed dump must not lose metadata.
    if (!livekit::ApplicationMemoryDumpAllowed()) return;
    const auto dump = CreateFileW(state->dump_path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) return;
    CONTEXT context{};
    EXCEPTION_RECORD exception{};
    EXCEPTION_POINTERS synthetic{&exception, &context};
    if (!pointers) {
        RtlCaptureContext(&context);
        exception.ExceptionCode = code;
        pointers = &synthetic;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
    const bool captured = MiniDumpWriteDump(GetCurrentProcess(),
        GetCurrentProcessId(), dump, MiniDumpNormal, &info, nullptr, nullptr);
    CloseHandle(dump);
    // If another thread admitted a secret while the dump was being written,
    // discard that partial/complete file too. Dumps predating secret admission
    // and OS/admin-created dumps are outside this application's policy.
    if (!captured || !livekit::ApplicationMemoryDumpAllowed())
        DeleteFileW(state->dump_path.c_str());
#else
    (void)pointers;
#endif
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers && pointers->ExceptionRecord)
        Capture(g_handler.load(std::memory_order_acquire),
            pointers->ExceptionRecord->ExceptionCode,
            reinterpret_cast<std::uintptr_t>(
                pointers->ExceptionRecord->ExceptionAddress), pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) noexcept {
    Capture(g_handler.load(std::memory_order_acquire), kAbortCode, 0);
    TerminateProcess(GetCurrentProcess(), kAbortCode);
}

void OnTerminate() noexcept {
    Capture(g_handler.load(std::memory_order_acquire), kTerminateCode, 0);
    TerminateProcess(GetCurrentProcess(), kTerminateCode);
}
#endif

} // namespace

struct CrashEvidenceProvider::Impl {
#if defined(_WIN32)
    HandlerState state;
#endif
};

std::filesystem::path CrashEvidenceProvider::DefaultRoot() {
#if defined(_WIN32)
    PWSTR local = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
            nullptr, &local) != S_OK || !local) return {};
    const std::filesystem::path root =
        std::filesystem::path(local) / L"Cohavora" / L"crash-evidence";
    CoTaskMemFree(local);
    return root;
#else
    return {};
#endif
}

bool CrashEvidenceProvider::CollectionEnabled(
    const std::filesystem::path& root) {
    try {
        if (root.empty()) return false;
        std::error_code error;
        const auto path = root / kPolicyFile;
        if (!std::filesystem::exists(path, error) && !error) return true;
        if (error || !SafeDirectory(root) ||
            std::filesystem::is_symlink(path, error) || error ||
            !std::filesystem::is_regular_file(path, error) || error ||
            std::filesystem::file_size(path, error) > 1024 || error)
            return false;
        std::ifstream input(path, std::ios::binary);
        const auto policy = nlohmann::json::parse(input);
        return !input.bad() && policy.is_object() &&
            policy.value("schema", std::string{}) ==
                "cohavora-crash-metadata-policy" &&
            policy.value("version", 0u) == 1u &&
            policy.contains("enabled") && policy["enabled"].is_boolean() &&
            policy["enabled"].get<bool>();
    } catch (...) { return false; }
}

bool CrashEvidenceProvider::SetCollectionEnabled(
    const std::filesystem::path& root, bool enabled) {
    try {
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error || !SafeDirectory(root)) return false;
        const auto path = root / kPolicyFile;
        const auto temporary = root / "policy-v1.json.tmp";
        for (const auto& item : {path, temporary}) {
            const bool exists = std::filesystem::exists(item, error);
            if (error || (exists && std::filesystem::is_symlink(item, error)) ||
                error) return false;
        }
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << nlohmann::json{{"schema", "cohavora-crash-metadata-policy"},
                {"version", 1}, {"enabled", enabled}}.dump();
            output.flush();
            if (!output) return false;
        }
#if defined(_WIN32)
        return MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::filesystem::rename(temporary, path, error);
        return !error;
#endif
    } catch (...) { return false; }
}

CrashEvidenceScan CrashEvidenceProvider::Scan(
    const std::filesystem::path& root, bool enabled) {
    CrashEvidenceScan result;
    if (!enabled || !SafeDirectory(root)) return result;
    result.recovery.provider_configured = true;
    std::error_code error;
    std::size_t inspected = 0;
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        if (++inspected > 4096) {
            ++result.rejected_files;
            break;
        }
        const auto name = it->path().filename().string();
        if (!OwnedName(name)) continue;
        CrashRecord record{};
        if (!ReadRecord(it->path(), record) ||
            std::string_view(record.run_id.data(), record.run_id.size()) !=
                std::string_view(name.data() + kPrefix.size(), 32)) {
            ++result.rejected_files;
            continue;
        }
        CrashEvidenceMetadata metadata;
        metadata.process_run_id.assign(record.run_id.data(), record.run_id.size());
        metadata.build_id.assign(record.build_id.data(), record.build_id.size());
        metadata.code = record.code;
        metadata.main_module_address = record.main_module_address != 0;
        metadata.main_module_rva = record.main_module_rva;
        result.recovery.confirmed_crashes.push_back({
            metadata.process_run_id, "windows_seh_metadata", metadata.build_id});
        result.records.push_back(std::move(metadata));
    }
    if (error) result.recovery.provider_configured = false;
    return result;
}

std::unique_ptr<CrashEvidenceProvider> CrashEvidenceProvider::Install(
    const std::filesystem::path& root, std::string process_run_id,
    std::string build_id, bool enabled) {
    auto provider = std::unique_ptr<CrashEvidenceProvider>(
        new CrashEvidenceProvider);
    if (!enabled) {
        provider->reason_ = "disabled_by_user";
        return provider;
    }
#if defined(_WIN32)
    if (!HexId(process_run_id, 32) || !HexId(build_id, 64)) {
        provider->reason_ = "identity_invalid";
        return provider;
    }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error || !SafeDirectory(root)) {
        provider->reason_ = "directory_unavailable";
        return provider;
    }
    if (!PruneOwnedEvidence(root)) {
        provider->reason_ = "evidence_quota_unavailable";
        return provider;
    }
    auto impl = std::make_unique<Impl>();
    auto& state = impl->state;
    state.path = root / (std::string(kPrefix) + process_run_id +
        std::string(kSuffix));
#if COHAVORA_ENABLE_MINIDUMP
    state.dump_path = root / (std::string(kPrefix) + process_run_id + ".dmp");
#endif
    state.file = CreateFileW(state.path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state.file == INVALID_HANDLE_VALUE) {
        provider->reason_ = "evidence_file_unavailable";
        return provider;
    }
    state.record.magic = kMagic;
    state.record.version = kVersion;
    state.record.process_id = GetCurrentProcessId();
    std::copy(process_run_id.begin(), process_run_id.end(), state.record.run_id.begin());
    std::copy(build_id.begin(), build_id.end(), state.record.build_id.begin());
    state.main_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (state.main_base) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(state.main_base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
            const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                state.main_base + dos->e_lfanew);
            if (pe->Signature == IMAGE_NT_SIGNATURE)
                state.main_size = pe->OptionalHeader.SizeOfImage;
        }
    }
    HandlerState* expected = nullptr;
    if (!g_handler.compare_exchange_strong(expected, &state)) {
        CloseHandle(state.file);
        std::filesystem::remove(state.path, error);
        provider->reason_ = "provider_already_installed";
        return provider;
    }
    state.previous_filter = SetUnhandledExceptionFilter(&OnUnhandledException);
    state.previous_abort = std::signal(SIGABRT, &OnAbort);
    state.previous_terminate = std::set_terminate(&OnTerminate);
    provider->impl_ = std::move(impl);
#if COHAVORA_ENABLE_MINIDUMP
    provider->reason_ = "installed_metadata_and_minidump";
#else
    provider->reason_ = "installed_metadata_only";
#endif
#else
    (void)root;
    (void)process_run_id;
    (void)build_id;
#endif
    return provider;
}

CrashEvidenceProvider::~CrashEvidenceProvider() {
#if defined(_WIN32)
    if (!impl_) return;
    auto& state = impl_->state;
    g_handler.store(nullptr, std::memory_order_release);
    std::set_terminate(state.previous_terminate);
    std::signal(SIGABRT, state.previous_abort);
    SetUnhandledExceptionFilter(state.previous_filter);
    CloseHandle(state.file);
    if (!state.captured.load(std::memory_order_acquire)) {
        std::error_code error;
        std::filesystem::remove(state.path, error);
    }
#endif
}

bool CrashEvidenceProvider::installed() const noexcept {
    return impl_ != nullptr;
}

} // namespace livekit::telemetry
