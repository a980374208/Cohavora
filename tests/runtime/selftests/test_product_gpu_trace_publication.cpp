// Exercise the production atomic publisher; no ETW session is started.
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
namespace {
BOOL WINAPI PublicationTestMoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags);
}
#define MoveFileExW PublicationTestMoveFileExW
#define wmain product_gpu_trace_entrypoint
#include "../probes/product_gpu_trace.cpp"
#undef wmain
#undef MoveFileExW
#include <iostream>

namespace {
struct MoveFailureControl {
    HANDLE release_after_failure = INVALID_HANDLE_VALUE;
    bool report_sharing_as_access_denied = false;
    DWORD first_native_error = ERROR_SUCCESS;
    std::uint64_t failures = 0;
};
MoveFailureControl move_failure;
BOOL WINAPI PublicationTestMoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    const auto moved = ::MoveFileExW(source, destination, flags);
    if (moved) return moved;
    const auto native_error = GetLastError();
    if (!move_failure.failures++) move_failure.first_native_error = native_error;
    if (move_failure.release_after_failure != INVALID_HANDLE_VALUE) {
        // Reproduce a real denied move whose reader disappears before DELETE probing.
        CloseHandle(move_failure.release_after_failure);
        move_failure.release_after_failure = INVALID_HANDLE_VALUE;
    }
    // The captured B14 failure was error 5. Windows can expose the same sharing
    // overlap as 32, so explicitly exercise the observed error-5 contract too.
    const auto reported = move_failure.report_sharing_as_access_denied &&
        (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_SHARING_VIOLATION)
        ? ERROR_ACCESS_DENIED : native_error;
    SetLastError(reported);
    return FALSE;
}
bool Require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
std::string Contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool SourceTemporarySharing(const std::filesystem::path& root) {
    const auto path = root / "source-sharing.json";
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    if (!Replace(path, "old")) return false;
    { std::ofstream source(temporary, std::ios::binary); source << "previous temporary\n"; }
    const auto reader = CreateFileW(temporary.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return false;
    move_failure = {};
    move_failure.report_sharing_as_access_denied = true;
    std::thread release([reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CloseHandle(reader);
    });
    const auto began = std::chrono::steady_clock::now();
    DWORD error = 0;
    std::uint64_t retries = 0;
    const bool published = Replace(path, "new", &error, &retries);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    release.join();
    std::cout << "SOURCE_TEMP_SHARING: native_move_error=" << move_failure.first_native_error
              << " publisher_error=" << error << " retries=" << retries << " published=" << published << '\n';
    const bool real_sharing_failure = move_failure.failures &&
        (move_failure.first_native_error == ERROR_ACCESS_DENIED || move_failure.first_native_error == ERROR_SHARING_VIOLATION);
    const bool passed = Require(real_sharing_failure, "source fixture did not deny a real Win32 move") &
        Require(published, "a brief source temporary-file reader aborted atomic publication") &
        Require(Contents(path) == "new\n", "source sharing recovery did not publish complete contents") &
        Require(!published || elapsed >= std::chrono::milliseconds(70), "source sharing recovery bypassed the source lock");
    move_failure = {};
    std::filesystem::remove(path);
    std::filesystem::remove(temporary);
    return passed;
}
bool ReleasedTargetSharing(const std::filesystem::path& root) {
    const auto path = root / "released-target.json";
    if (!Replace(path, "old")) return false;
    const auto reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return false;
    move_failure = {};
    move_failure.release_after_failure = reader;
    move_failure.report_sharing_as_access_denied = true;
    DWORD error = 0;
    std::uint64_t retries = 0;
    const bool published = Replace(path, "new", &error, &retries);
    if (move_failure.release_after_failure != INVALID_HANDLE_VALUE) CloseHandle(move_failure.release_after_failure);
    std::cout << "RELEASED_TARGET_SHARING: native_move_error=" << move_failure.first_native_error
              << " publisher_error=" << error << " retries=" << retries << " published=" << published << '\n';
    const bool real_sharing_failure = move_failure.failures &&
        (move_failure.first_native_error == ERROR_ACCESS_DENIED || move_failure.first_native_error == ERROR_SHARING_VIOLATION);
    const bool passed = Require(real_sharing_failure, "target fixture did not deny a real Win32 move") &
        Require(published, "a target reader released between failed move and probe aborted publication") &
        Require(Contents(path) == "new\n", "released target recovery did not publish complete contents");
    move_failure = {};
    std::filesystem::remove(path);
    std::filesystem::remove(path.wstring() + L".tmp");
    return passed;
}
struct RestoreDacl {
    std::filesystem::path path;
    std::vector<BYTE> descriptor;
    explicit RestoreDacl(std::filesystem::path value) : path(std::move(value)) {
        DWORD size = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
        descriptor.resize(size);
        if (!size || !GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), size, &size)) descriptor.clear();
    }
    ~RestoreDacl() {
        if (!descriptor.empty()) SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()));
    }
    bool Apply(const wchar_t* sddl) {
        if (descriptor.empty()) return false;
        PSECURITY_DESCRIPTOR desired = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &desired, nullptr)) return false;
        const bool changed = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, desired) != FALSE;
        LocalFree(desired);
        return changed;
    }
};
bool PermanentAccessDenied(const std::filesystem::path& root) {
    const auto directory = root / "acl-denied";
    std::filesystem::create_directory(directory);
    const auto path = directory / "heartbeat.json";
    if (!Replace(path, "old")) return false;
    bool passed = false;
    {
        RestoreDacl parent(directory), target(path);
        // Deny parent DELETE_CHILD and target DELETE, while preserving writes
        // to the temporary file. A healthy-looking unlocked path is insufficient.
        if (!parent.Apply(L"D:P(D;;0x00000040;;;WD)(A;;FA;;;WD)") ||
            !target.Apply(L"D:P(D;;0x00010000;;;WD)(A;;FA;;;WD)")) return false;
        const auto began = std::chrono::steady_clock::now();
        DWORD error = 0;
        std::uint64_t retries = 0;
        const bool published = Replace(path, "must-not-publish", &error, &retries);
        const auto elapsed = std::chrono::steady_clock::now() - began;
        passed = Require(!published && error == ERROR_ACCESS_DENIED && retries == 0 &&
                         elapsed < std::chrono::milliseconds(250), "permanent ACL denial was retried or ignored") &
            Require(Contents(path) == "old\n", "ACL-denied publication damaged complete contents");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.wstring() + L".tmp");
    std::filesystem::remove(directory);
    return passed;
}
}
int main() {
    const auto root = std::filesystem::current_path() / ("publication-test-" + std::to_string(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) return 2;
    const auto path = root / "heartbeat.json";
    bool passed = Require(Replace(path, "old"), "initial publication failed");
    const auto reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return 2;
    std::thread release([reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CloseHandle(reader);
    });
    const auto began = std::chrono::steady_clock::now();
    const bool published = Replace(path, "new");
    const auto error = GetLastError();
    const auto elapsed = std::chrono::steady_clock::now() - began;
    release.join();
    if (!published) std::cerr << "atomic publication win32_error=" << error << '\n';
    passed &= Require(published, "a brief overlapping reader aborted atomic heartbeat publication");
    passed &= Require(Contents(path) == "new\n", "successful publication was not a complete replacement");
    passed &= Require(!published || elapsed >= std::chrono::milliseconds(70), "publication unexpectedly bypassed the reader lock");

    const auto blocked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (blocked == INVALID_HANDLE_VALUE) return 2;
    const auto blocked_start = std::chrono::steady_clock::now();
    const bool permanent = Replace(path, "must-not-publish");
    const auto blocked_elapsed = std::chrono::steady_clock::now() - blocked_start;
    CloseHandle(blocked);
    passed &= Require(!permanent && blocked_elapsed < std::chrono::seconds(2), "persistent reader lock did not fail within a bounded interval");
    passed &= Require(Contents(path) == (published ? "new\n" : "old\n"), "failed publication damaged the previous complete heartbeat");

    passed &= SourceTemporarySharing(root);
    passed &= ReleasedTargetSharing(root);
    passed &= PermanentAccessDenied(root);

    const auto readonly_path = root / "readonly.json";
    passed &= Require(Replace(readonly_path, "old"), "readonly fixture preparation failed");
    const auto previous_attributes = GetFileAttributesW(readonly_path.c_str());
    passed &= Require(SetFileAttributesW(readonly_path.c_str(), previous_attributes | FILE_ATTRIBUTE_READONLY) != FALSE,
                      "readonly fixture attribute setup failed");
    const auto readonly_start = std::chrono::steady_clock::now();
    const bool readonly_published = Replace(readonly_path, "must-not-publish");
    passed &= Require(!readonly_published && std::chrono::steady_clock::now() - readonly_start < std::chrono::milliseconds(250),
                      "readonly target was ignored or retried");
    passed &= Require(Contents(readonly_path) == "old\n", "readonly denial damaged complete contents");
    SetFileAttributesW(readonly_path.c_str(), previous_attributes);
    std::filesystem::remove(readonly_path);
    std::filesystem::remove(readonly_path.wstring() + L".tmp");

    const auto missing_start = std::chrono::steady_clock::now();
    passed &= Require(!Replace(root / "missing-parent" / "heartbeat.json", "must-not-publish") &&
                      std::chrono::steady_clock::now() - missing_start < std::chrono::milliseconds(250),
                      "missing parent was ignored or retried");

    const auto inaccessible = root / "existing-directory";
    std::filesystem::create_directory(inaccessible);
    const auto denied_start = std::chrono::steady_clock::now();
    const bool denied = Replace(inaccessible, "must-not-publish");
    passed &= Require(!denied && std::chrono::steady_clock::now() - denied_start < std::chrono::milliseconds(250),
                      "non-sharing publication error was ignored or retried");
    std::filesystem::remove(path);
    std::filesystem::remove(path.wstring() + L".tmp");
    std::filesystem::remove(inaccessible);
    std::filesystem::remove(inaccessible.wstring() + L".tmp");
    std::filesystem::remove(root);
    if (!passed) return 1;
    std::cout << "PASS: target/source sharing and released-lock race recovered; permanent lock, ACL, readonly and path failures remain fatal; atomic contents preserved\n";
    return 0;
}
