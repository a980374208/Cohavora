#include "diagnostic_file_sink.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <limits>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#endif

namespace livekit::diagnostic {
namespace {

bool IsOwnedSegment(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.size() == 20 && name.starts_with("segment-") &&
        name.ends_with(".jsonl") &&
        std::all_of(name.begin() + 8, name.begin() + 14, [](char ch) {
            return ch >= '0' && ch <= '9';
        });
}

bool IsRunId(std::string_view id) noexcept {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool IsOwnedRun(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.starts_with("run-") && IsRunId(std::string_view(name).substr(4));
}

#if defined(_WIN32)
bool ReadActualFileSize(const std::filesystem::path& path,
                        std::uint64_t& bytes) noexcept {
    const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool success = GetFileSizeEx(file, &size) && size.QuadPart >= 0;
    CloseHandle(file);
    if (success) bytes = static_cast<std::uint64_t>(size.QuadPart);
    return success;
}

enum class QuotaScan { WithinLimit, ReclaimRequired, Failed };

// Called with the quota mutex held. Closed history uses enumeration metadata;
// live files require a fresh size query (NTFS directory sizes may lag appends).
// Recompute at segment admission; same-segment appends deliberately skip quota
// checks. There is no shared usage estimate or reserved space to recover.
QuotaScan ScanQuotaUsage(const std::filesystem::path& root,
                        const std::filesystem::path& current_run,
                        std::uint32_t segment_index,
                        std::uint64_t limit, const std::atomic<bool>* abandoned) {
    struct Leases {
        std::vector<HANDLE> handles;
        ~Leases() { for (auto h : handles) CloseHandle(h); }
    } leases;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](FILETIME t) {
        return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    const auto expiry = ticks(now) - 7ULL * 24 * 60 * 60 * 10000000;
    wchar_t current_name[32]{};
    std::swprintf(current_name, 32, L"segment-%06u.jsonl", segment_index);
    std::uint64_t total = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator runs(root, error), end;
         !error && runs != end; runs.increment(error)) {
        if (!runs->is_directory(error) || error || !IsOwnedRun(runs->path()) ||
            runs->is_symlink(error)) {
            error.clear();
            continue;
        }
        if (abandoned && abandoned->load(std::memory_order_acquire)) return QuotaScan::Failed;
        const bool own = runs->path() == current_run;
        bool active = own;
        if (!own) {
            auto lease = CreateFileW((runs->path() / L"active.lock").c_str(),
                GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lease == INVALID_HANDLE_VALUE) active = true;
            else {
                auto guard = std::unique_ptr<void, decltype(&CloseHandle)>(lease, &CloseHandle);
                leases.handles.push_back(lease);
                guard.release();
            }
        }
        WIN32_FIND_DATAW entry{};
        const auto find = FindFirstFileExW((runs->path() / L"*").c_str(),
            FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_FILE_NOT_FOUND
                ? QuotaScan::ReclaimRequired : QuotaScan::Failed;
        const auto close_find = std::unique_ptr<void, decltype(&FindClose)>(find, &FindClose);
        do {
            if (abandoned && abandoned->load(std::memory_order_acquire)) return QuotaScan::Failed;
            const std::wstring_view name(entry.cFileName);
            if (name.size() != 20 || !name.starts_with(L"segment-") ||
                !name.ends_with(L".jsonl") ||
                !std::all_of(name.begin() + 8, name.begin() + 14,
                    [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
            // Let the existing slow path apply its file-type policy to any
            // reparse points rather than trusting enumerated target metadata.
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                return QuotaScan::ReclaimRequired;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::uint64_t bytes = (std::uint64_t(entry.nFileSizeHigh) << 32) |
                                  entry.nFileSizeLow;
            if (active) {
                if (!ReadActualFileSize(runs->path() / entry.cFileName, bytes))
                    return QuotaScan::Failed;
            }
            if (bytes > limit - total) return QuotaScan::ReclaimRequired;
            total += bytes;
            if ((!active || own) && !(own && name == current_name)) {
                if (ticks(entry.ftLastWriteTime) < expiry)
                    return QuotaScan::ReclaimRequired;
            }
        } while (FindNextFileW(find, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) return QuotaScan::Failed;
    }
    return error ? QuotaScan::Failed : QuotaScan::WithinLimit;
}

bool IsPrivateDirectory(const std::filesystem::path& path) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) return false;
    struct DescriptorGuard {
        PSECURITY_DESCRIPTOR value;
        ~DescriptorGuard() { LocalFree(value); }
    } descriptor_guard{descriptor};
    if (!owner || !acl) return false;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    struct TokenGuard {
        HANDLE value;
        ~TokenGuard() { CloseHandle(value); }
    } token_guard{token};
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    if (length == 0) return false;
    std::vector<std::byte> buffer(length);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), length, &length))
        return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!EqualSid(owner, user->User.Sid)) return false;

    for (const auto kind : {WinWorldSid, WinAuthenticatedUserSid,
                            WinBuiltinUsersSid}) {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sid{};
        DWORD sid_length = static_cast<DWORD>(sid.size());
        if (!CreateWellKnownSid(kind, nullptr, sid.data(), &sid_length))
            return false;
        TRUSTEE_W trustee{};
        BuildTrusteeWithSidW(&trustee, sid.data());
        ACCESS_MASK rights = 0;
        if (GetEffectiveRightsFromAclW(acl, &trustee, &rights) !=
            ERROR_SUCCESS) return false;
        constexpr ACCESS_MASK unsafe = GENERIC_ALL | GENERIC_READ |
            GENERIC_WRITE | FILE_READ_DATA | FILE_WRITE_DATA |
            FILE_APPEND_DATA | FILE_DELETE_CHILD;
        if ((rights & unsafe) != 0) return false;
    }
    return true;
}
#endif

void AddContext(nlohmann::json& value, const Context& context) {
    if (!context.anonymous_session_id.View().empty())
        value["anonymous_session_id"] = context.anonymous_session_id.View();
    if (!context.operation_id.View().empty())
        value["operation_id"] = context.operation_id.View();
    if (!context.parent_operation_id.View().empty())
        value["parent_operation_id"] = context.parent_operation_id.View();
    if (!context.request_id.View().empty())
        value["request_id"] = context.request_id.View();
    if (!context.legacy_operation_id.View().empty())
        value["legacy_operation_id"] = context.legacy_operation_id.View();
    if (context.has_session_generation)
        value["session_generation"] = context.session_generation;
    if (context.has_room_generation)
        value["room_generation"] = context.room_generation;
    if (context.has_recovery_epoch)
        value["recovery_epoch"] = context.recovery_epoch;
}

} // namespace

DiagnosticFileSink::DiagnosticFileSink(std::filesystem::path root,
                                       std::string run_id,
                                       const std::atomic<bool>* abandoned)
    : abandoned_(abandoned), root_(std::filesystem::absolute(root).lexically_normal()),
      run_directory_(root_ / ("run-" + run_id)),
      valid_run_id_(IsRunId(run_id)) {}

DiagnosticFileSink::~DiagnosticFileSink() {
    Close();
#if defined(_WIN32)
    if (run_lease_) CloseHandle(static_cast<HANDLE>(run_lease_));
    if (quota_mutex_) CloseHandle(static_cast<HANDLE>(quota_mutex_));
    if (root_identity_) CloseHandle(static_cast<HANDLE>(root_identity_));
#endif
}

DiagnosticClearResult DiagnosticFileSink::ClearInactiveHistory(
    const std::filesystem::path& root) noexcept {
    DiagnosticClearResult result;
    try {
        std::error_code error;
        if (!std::filesystem::exists(root, error) && !error) {
            result.success = true;
            result.reason = "history_empty";
            return result;
        }
        if (error || std::filesystem::is_symlink(root, error) || error ||
            !std::filesystem::is_directory(root, error) || error) {
            result.reason = "history_root_invalid";
            return result;
        }
#if defined(_WIN32)
        if (!IsPrivateDirectory(root)) {
            result.reason = "history_root_invalid";
            return result;
        }
#endif
        DiagnosticFileSink gate(root, std::string(32, '0'));
        if (!gate.LockQuota()) {
            result.reason = "history_busy";
            return result;
        }
        struct Unlock {
            DiagnosticFileSink& sink;
            ~Unlock() { sink.UnlockQuota(); }
        } unlock{gate};
        std::size_t inspected = 0;
        for (std::filesystem::directory_iterator runs(gate.root_, error), end;
             !error && runs != end; runs.increment(error)) {
            if (++inspected > 4096) {
                result.reason = "history_scan_limit";
                return result;
            }
            if (!IsOwnedRun(runs->path())) continue;
            if (runs->is_symlink(error) || error ||
                !runs->is_directory(error) || error) {
                result.reason = "history_run_invalid";
                return result;
            }
#if defined(_WIN32)
            if (!IsPrivateDirectory(runs->path())) {
                result.reason = "history_run_invalid";
                return result;
            }
            const auto handle = CreateFileW(
                (runs->path() / L"active.lock").c_str(), GENERIC_READ,
                0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                ++result.active_runs_skipped;
                continue;
            }
            const auto lease = std::unique_ptr<void, decltype(&CloseHandle)>(
                handle, &CloseHandle);
#endif
            for (std::filesystem::directory_iterator files(runs->path(), error);
                 !error && files != end; files.increment(error)) {
                if (!IsOwnedSegment(files->path())) continue;
                if (files->is_symlink(error) || error ||
                    !files->is_regular_file(error) || error) {
                    result.reason = "history_segment_invalid";
                    return result;
                }
                std::filesystem::remove(files->path(), error);
                if (error) {
                    result.reason = "history_remove_failed";
                    return result;
                }
                ++result.removed_segments;
            }
            if (error) {
                result.reason = "history_scan_failed";
                return result;
            }
        }
        if (error) {
            result.reason = "history_scan_failed";
            return result;
        }
        result.success = true;
        result.reason = "history_cleared";
    } catch (...) {
        result.reason = "history_exception";
    }
    return result;
}

bool DiagnosticFileSink::OpenSegment() noexcept {
    try {
        if (Stopped()) return false;
        if (!valid_run_id_) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        std::error_code error;
        if (std::filesystem::is_symlink(root_, error) ||
            std::filesystem::is_symlink(run_directory_, error)) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        std::filesystem::create_directories(root_, error);
        if (error) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
#if defined(_WIN32)
        if (!IsPrivateDirectory(root_)) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
#endif
        if (Stopped() || !BindQuotaIdentity()) return false;
        std::filesystem::create_directories(run_directory_, error);
        if (error) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
#if defined(_WIN32)
        if (!IsPrivateDirectory(run_directory_)) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        if (Stopped()) return false;
        if (!run_lease_) {
            const auto lease_path = run_directory_ / L"active.lock";
            auto handle = CreateFileW(lease_path.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                failure_reason_ = FailureReason::DirectoryUnavailable;
                return false;
            }
            run_lease_ = handle;
        }
#endif
        char name[32]{};
        std::snprintf(name, sizeof(name), "segment-%06u.jsonl", segment_index_);
        const auto path = run_directory_ / name;
        if (Stopped()) return false;
#if defined(_WIN32)
        auto file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            failure_reason_ = FailureReason::OpenFailed;
            return false;
        }
        file_ = file;
        quota_checked_segment_ = false;
        if (Stopped()) { CloseSegment(); return false; }
        std::uint64_t size = 0;
        if (!read_size_(file_, size)) {
            CloseSegment();
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        segment_bytes_ = size;
        if (Stopped()) { CloseSegment(); return false; }
#else
        output_.open(path, std::ios::binary | std::ios::app);
        if (!output_) {
            failure_reason_ = FailureReason::OpenFailed;
            output_.clear();
            return false;
        }
        quota_checked_segment_ = false;
        // Query the opened stream itself, not a separately resolved pathname.
        // app only guarantees seeking before writes, so explicitly seek here.
        output_.seekp(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(output_.tellp());
        if (!output_ || size < 0) {
            // An open stream is not an admitted segment. Never let a retry
            // bypass this failed size query by finding output_ still open.
            output_.close();
            output_.clear();
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        segment_bytes_ = static_cast<std::uint64_t>(size);
#endif
        return true;
    } catch (...) {
        failure_reason_ = FailureReason::DirectoryUnavailable;
        return false;
    }
}

bool DiagnosticFileSink::CheckQuota(std::uint64_t incoming_bytes) noexcept {
    try {
      for (;;) {
        if (Stopped()) return false;
        if (incoming_bytes > kTotalBytes) {
            failure_reason_ = FailureReason::QuotaExceeded;
            return false;
        }
#if defined(_WIN32)
        const auto scan = ScanQuotaUsage(root_, run_directory_, segment_index_,
                                        kTotalBytes - incoming_bytes, abandoned_);
        if (Stopped()) return false;
        if (scan == QuotaScan::WithinLimit) return true;
        if (scan == QuotaScan::Failed) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
#endif
        struct Segment {
            std::filesystem::path path;
            std::uint64_t bytes;
            std::filesystem::file_time_type modified;
        };
        std::vector<Segment> reclaimable;
        constexpr std::size_t kMaximumTrackedSegments = 1024;
        bool truncated = false;
        const auto older = [](const Segment& a, const Segment& b) {
            return a.modified < b.modified;
        };
#if defined(_WIN32)
        struct HeldLeases {
            ~HeldLeases() {
                for (auto handle : handles) CloseHandle(handle);
            }
            std::vector<HANDLE> handles;
        } held_leases;
#endif
        std::uint64_t total = 0;
        std::error_code error;
        char current_name[32]{};
        std::snprintf(current_name, sizeof(current_name), "segment-%06u.jsonl",
                      segment_index_);
        const auto current_path = run_directory_ / current_name;
        if (!std::filesystem::exists(root_, error)) return true;
        for (std::filesystem::directory_iterator runs(root_, error), end;
             !error && runs != end; runs.increment(error)) {
            if (!runs->is_directory(error) || error || !IsOwnedRun(runs->path()) ||
                runs->is_symlink(error)) {
                error.clear();
                continue;
            }
            if (Stopped()) return false;
            bool active = runs->path() == run_directory_;
#if defined(_WIN32)
            if (!active) {
                const auto lease_path = runs->path() / L"active.lock";
                auto lease = CreateFileW(lease_path.c_str(), GENERIC_READ, 0, nullptr,
                                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (lease == INVALID_HANDLE_VALUE) active = true;
                else held_leases.handles.push_back(lease);
            }
#endif
            for (std::filesystem::directory_iterator files(runs->path(), error);
                 !error && files != end; files.increment(error)) {
                if (Stopped()) return false;
                if (files->is_regular_file(error) && !files->is_symlink(error) && !error &&
                    IsOwnedSegment(files->path())) {
                    std::uint64_t bytes = 0;
#if defined(_WIN32)
                    if (active) {
                        if (!ReadActualFileSize(files->path(), bytes)) {
                            failure_reason_ = FailureReason::DirectoryUnavailable;
                            return false;
                        }
                    } else
#endif
                    {
                        bytes = files->file_size(error);
                        if (error) break;
                    }
                    if (bytes > (std::numeric_limits<std::uint64_t>::max)() - total) {
                        failure_reason_ = FailureReason::DirectoryUnavailable;
                        return false;
                    }
                    total += bytes;
                    if (!active || runs->path() == run_directory_) {
                        if (files->path() != current_path) {
                            const auto modified = files->last_write_time(error);
                            if (error) break;
                            Segment candidate{files->path(), bytes, modified};
                            if (reclaimable.size() < kMaximumTrackedSegments) {
                                reclaimable.push_back(std::move(candidate));
                                std::push_heap(reclaimable.begin(), reclaimable.end(), older);
                            } else {
                                truncated = true;
                                if (older(candidate, reclaimable.front())) {
                                    std::pop_heap(reclaimable.begin(), reclaimable.end(), older);
                                    reclaimable.back() = std::move(candidate);
                                    std::push_heap(reclaimable.begin(), reclaimable.end(), older);
                                }
                            }
                        }
                    }
                }
                error.clear();
            }
            if (error) break;
        }
        if (error) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        std::sort(reclaimable.begin(), reclaimable.end(),
                  [](const Segment& a, const Segment& b) {
                      return a.modified < b.modified;
                  });
        const auto expiry = std::filesystem::file_time_type::clock::now() -
                            std::chrono::hours(24 * 7);
        std::size_t removed = 0;
        for (const auto& segment : reclaimable) {
            if (segment.modified >= expiry &&
                total <= kTotalBytes - incoming_bytes) break;
            if (Stopped()) return false;
            std::filesystem::remove(segment.path, error);
            if (error) {
                failure_reason_ = FailureReason::DirectoryUnavailable;
                return false;
            }
            total -= segment.bytes;
            ++removed;
        }
        // Re-scan after a bounded oldest-first batch. This also handles more
        // than 1024 expired tiny files without retaining an unbounded list.
        if (truncated && removed != 0) continue;
        if (total > kTotalBytes - incoming_bytes) {
            failure_reason_ = FailureReason::QuotaExceeded;
            return false;
        }
        return true;
      }
    } catch (...) {
        failure_reason_ = FailureReason::DirectoryUnavailable;
        return false;
    }
}

bool DiagnosticFileSink::BindQuotaIdentity() noexcept {
    try {
#if defined(_WIN32)
    if (!quota_mutex_) {
        // Bind all aliases to the directory object, not its spelling. Keep the
        // handle without delete sharing so this identity cannot be replaced
        // while the sink is using its mutex. Final symlink policy is unchanged.
        auto directory = CreateFileW(root_.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        auto guard = std::unique_ptr<void, decltype(&CloseHandle)>(directory, &CloseHandle);
        FILE_ID_INFO identity{};
        if (!GetFileInformationByHandleEx(directory, FileIdInfo, &identity, sizeof(identity))) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        // Freeze the resolved path too: future segment opens must not follow
        // a junction alias that has been redirected after admission.
        const auto length = GetFinalPathNameByHandleW(directory, nullptr, 0, FILE_NAME_NORMALIZED);
        if (!length) { failure_reason_ = FailureReason::DirectoryUnavailable; return false; }
        std::wstring resolved(length, L'\0');
        const auto written = GetFinalPathNameByHandleW(directory, resolved.data(), length, FILE_NAME_NORMALIZED);
        if (!written || written >= length) { failure_reason_ = FailureReason::DirectoryUnavailable; return false; }
        resolved.resize(written);
        wchar_t prefix[64]{};
        std::swprintf(prefix, 64, L"Local\\CohavoraDiagnostic-v2-%016llx-", identity.VolumeSerialNumber);
        std::wstring mutex_name(prefix);
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto byte : identity.FileId.Identifier) {
            mutex_name += hex[byte >> 4]; mutex_name += hex[byte & 15];
        }
        const auto run_name = run_directory_.filename();
        root_ = std::filesystem::path(resolved);
        run_directory_ = root_ / run_name;
        auto handle = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
        if (!handle) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        quota_mutex_ = handle;
        root_identity_ = guard.release();
    }
#endif
        return true;
    } catch (...) { failure_reason_ = FailureReason::DirectoryUnavailable; return false; }
}

bool DiagnosticFileSink::LockQuota() noexcept {
#if defined(_WIN32)
    if (!BindQuotaIdentity()) return false;
    const auto result = WaitForSingleObject(static_cast<HANDLE>(quota_mutex_), 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        failure_reason_ = FailureReason::DirectoryUnavailable;
        return false;
    }
#endif
    return true;
}

void DiagnosticFileSink::UnlockQuota() noexcept {
#if defined(_WIN32)
    ReleaseMutex(static_cast<HANDLE>(quota_mutex_));
#endif
}

std::string DiagnosticFileSink::Serialize(const Event& event) const {
    using nlohmann::json;
    json value = {
        {"schema_version", 1},
        {"event_name", EventName(event.kind)},
        {"severity", SeverityName(EventSeverity(event))},
        {"component", ComponentName(event.kind)},
        {"occurred_at_utc_ms", event.occurred_at_utc_ms},
        {"monotonic_us", event.monotonic_us},
        {"event_sequence", event.event_sequence},
        {"process_run_id", event.process_run_id.data()},
        {"pid", event.process_id},
        {"thread_role", ThreadRoleName(event.thread_role)},
        {"redaction_version", event.redaction_version},
    };
    AddContext(value, event.context);
    if (event.outcome != Outcome::Unknown &&
        event.kind != EventKind::ProcessTerminal)
        value["outcome"] = OutcomeName(event.outcome);
    if (event.stage != Stage::Unknown)
        value["stage"] = StageName(event.stage);
    if (event.error_code != ErrorCode::None)
        value["error_code"] = ErrorCodeName(event.error_code);
    if (event.error_layer != ErrorLayer::None)
        value["error_layer"] = ErrorLayerName(event.error_layer);
    if (event.duration_ms != 0)
        value["duration_ms"] = event.duration_ms;
    if (event.retryable)
        value["retryable"] = true;
    json attributes = json::object();
    switch (event.kind) {
    case EventKind::ProcessStarted:
        attributes["build_id"] = event.build_id.data();
        attributes["symbol_identity"] = event.symbol_identity.data();
        break;
    case EventKind::ProcessStopping:
        attributes["shutdown_reason"] = ShutdownReasonName(event.shutdown_reason); break;
    case EventKind::ProcessTerminal:
        attributes["outcome"] = OutcomeName(event.outcome);
        attributes["drain_result"] = DrainResultName(event.drain_result);
        break;
    case EventKind::QueueSummary:
        attributes["accepted"] = event.accepted;
        attributes["dropped"] = event.dropped;
        attributes["queue_high_water"] = event.queue_high_water;
        break;
    case EventKind::SinkFailed:
        attributes["sink_kind"] = SinkKindName(event.sink_kind);
        attributes["reason_code"] = FailureReasonName(event.failure_reason);
        attributes["last_committed_sequence"] = event.last_committed_sequence;
        break;
    case EventKind::SinkRecovered:
        attributes["sink_kind"] = SinkKindName(event.sink_kind);
        attributes["last_committed_sequence"] = event.last_committed_sequence;
        break;
    case EventKind::ChatReceived:
    case EventKind::ChatSendTerminal:
        attributes["chat_kind"] = ChatKindName(event.chat_kind);
        attributes["bytes"] = event.bytes;
        break;
    case EventKind::TransferTerminal:
        if (event.transfer_kind != TransferKind::Unknown)
            attributes["transfer_kind"] = TransferKindName(event.transfer_kind);
        attributes["direction"] = TransferDirectionName(event.transfer_direction);
        attributes["bytes"] = event.bytes;
        break;
    case EventKind::ProcessIssue:
        attributes["reason_code"] = IssueCodeName(event.issue_code);
        break;
    case EventKind::ParticipantIssue:
        attributes["reason_code"] = ParticipantIssueReasonName(event.participant_issue_reason);
        break;
    case EventKind::HttpRequestStarted:
    case EventKind::HttpRequestCompleted:
    case EventKind::HttpResponseDecodeFailed:
        attributes["route"] = RouteName(event.route);
        if (event.kind == EventKind::HttpRequestCompleted) {
            if (event.http_status >= 100)
                attributes["http_status"] = event.http_status;
            attributes["network_error"] = event.network_error;
            if (event.business_code != 0)
                attributes["business_error"] = BusinessErrorName(event.business_code);
        }
        break;
    case EventKind::ReconnectAttemptStarted:
    case EventKind::ReconnectAttemptTerminal:
        attributes["attempt"] = event.attempt;
        attributes["mode"] = event.stage == Stage::Resume ? "resume" :
            event.stage == Stage::FullRestart ? "full_restart" : "unknown";
        break;
    case EventKind::MediaPublishStarted:
    case EventKind::MediaPublishTerminal:
    case EventKind::MediaUnpublishStarted:
    case EventKind::MediaUnpublishTerminal:
        attributes["media_kind"] = MediaKindName(event.media_kind);
        break;
    case EventKind::MediaPublishBatchStarted:
    case EventKind::MediaPublishBatchTerminal:
        attributes["track_count"] = event.batch_track_count;
        break;
    case EventKind::MediaSubscriptionChanged:
        attributes["media_kind"] = MediaKindName(event.media_kind);
        attributes["subscription_state"] = SubscriptionStateName(event.subscription_state);
        attributes["reason_code"] = event.error_code == ErrorCode::None
            ? "unknown" : ErrorCodeName(event.error_code);
        break;
    case EventKind::MediaRecoveryMilestone:
    case EventKind::MediaRecoveryTimeout:
        attributes["media_kind"] = MediaKindName(event.media_kind);
        attributes["measurement_point"] = RecoveryMeasurementName(event.recovery_measurement);
        if (event.kind == EventKind::MediaRecoveryMilestone)
            attributes["expected_endpoints"] = event.batch_track_count;
        break;
    case EventKind::MediaEndpointRecovered:
        attributes["media_kind"] = MediaKindName(event.media_kind);
        attributes["measurement_point"] = RecoveryMeasurementName(event.recovery_measurement);
        attributes["endpoint_id"] = event.media_endpoint_id.View();
        if (!event.previous_media_endpoint_id.View().empty())
            attributes["previous_endpoint_id"] = event.previous_media_endpoint_id.View();
        break;
    case EventKind::MediaFallback:
        attributes["media_kind"] = MediaKindName(event.media_kind);
        attributes["reason"] = MediaFallbackReasonName(event.media_fallback_reason);
        break;
    case EventKind::RenderBackendChanged:
        attributes["from_backend"] = RenderBackendName(event.from_render_backend);
        attributes["to_backend"] = RenderBackendName(event.to_render_backend);
        attributes["reason_code"] = RenderReasonName(event.render_reason);
        break;
    case EventKind::DeviceSwitchStarted:
    case EventKind::DeviceSwitchTerminal:
    case EventKind::DeviceCaptureTerminal:
        if (event.kind != EventKind::DeviceSwitchStarted)
            attributes["reason"] = DeviceSwitchReasonName(event.device_switch_reason);
        if (event.kind == EventKind::DeviceCaptureTerminal)
            attributes["media_kind"] = MediaKindName(event.media_kind);
        break;
    case EventKind::RtcSdpStep:
        attributes["description_type"] = SdpDescriptionName(event.sdp_description);
        attributes["action"] = SdpActionName(event.sdp_action);
        attributes["phase"] = SdpPhaseName(event.sdp_phase);
        attributes["pc_role"] = SdpRoleName(event.sdp_role);
        attributes["round_sequence"] = event.sdp_sequence;
        attributes["signaling_before"] = SdpStateName(event.signaling_before);
        attributes["signaling_after"] = SdpStateName(event.signaling_after);
        attributes["reason"] = SdpReasonName(event.sdp_reason);
        attributes["after_terminal"] = event.sdp_after_terminal;
        attributes["ice_restart"] = event.sdp_ice_restart;
        attributes["rtc_error_type"] = event.rtc_error_type;
        break;
    case EventKind::RtcSdpFailed:
        attributes["rtc_error_type"] = event.rtc_error_type;
        break;
    case EventKind::RtcLifecycle:
        attributes["status"] = RtcStatusName(event.rtc_status);
        break;
    case EventKind::SignalMessageSummary:
        attributes["count"] = event.signal_message_count;
        attributes["category"] = SignalCategoryName(event.signal_category);
        if (event.window_sample) attributes["window_sample"] = true;
        break;
    case EventKind::SignalIssue:
        break;
    case EventKind::CallbackRejectedSummary:
        attributes["count"] = event.signal_message_count;
        attributes["source"] = "meeting_coordinator";
        attributes["reason_code"] = "stale_generation";
        if (event.context.has_session_generation)
            attributes["old_generation"] = event.context.session_generation;
        attributes["current_generation"] = event.current_generation;
        break;
    case EventKind::DiagnosticsModeChanged:
        attributes["mode"] = event.diagnostic_window_enabled
            ? "diagnostic" : "normal";
        attributes["expires_at_utc_ms"] = event.diagnostic_expires_at_utc_ms;
        break;
    case EventKind::RetentionChanged:
        attributes["enabled"] = event.retention_enabled;
        break;
    case EventKind::MeetingLeaveRequested:
        attributes["leave_reason"] = event.route == Route::EndMeeting
            ? "end_for_all" : "leave";
        break;
    case EventKind::MeetingBackendNotificationCompleted:
        if (event.http_status >= 100)
            attributes["http_status"] = event.http_status;
        break;
    case EventKind::AdmissionStarted:
    case EventKind::AdmissionStageChanged:
    case EventKind::AdmissionTerminal:
    case EventKind::RoomConnectStarted:
    case EventKind::RoomConnectTerminal:
    case EventKind::StartupTerminal:
    case EventKind::ReconnectEpisodeStarted:
    case EventKind::ReconnectEpisodeTerminal:
    case EventKind::ReconnectModeChanged:
    case EventKind::SessionStopped:
        break;
    }
    if (event.kind == EventKind::ReconnectEpisodeStarted)
        attributes["reason_code"] = event.error_code == ErrorCode::None
            ? "unknown" : ErrorCodeName(event.error_code);
    if (event.kind == EventKind::ReconnectModeChanged) {
        attributes["from_mode"] = "resume";
        attributes["to_mode"] = event.stage == Stage::FullRestart
            ? "full_restart" : "unknown";
        attributes["reason_code"] = event.error_code == ErrorCode::None
            ? "unknown" : ErrorCodeName(event.error_code);
    }
    value["attributes"] = std::move(attributes);
    return value.dump() + '\n';
}

bool DiagnosticFileSink::Write(const Event& event) noexcept {
    return WriteBatch(std::span<const Event>(&event, 1)) == 1;
}

std::size_t DiagnosticFileSink::WriteBatch(std::span<const Event> events) noexcept {
    std::size_t committed = 0;
    try {
        if (events.size() > 64) {
            failure_reason_ = FailureReason::WriteFailed;
            return 0;
        }
        std::vector<std::string> lines;
        lines.reserve(events.size());
        for (const auto& event : events) {
            if (!IsValidEvent(event)) {
                failure_reason_ = FailureReason::WriteFailed;
                return 0;
            }
            auto line = Serialize(event);
            if (line.size() > kMaximumEventBytes) {
                failure_reason_ = FailureReason::WriteFailed;
                return 0;
            }
            lines.push_back(std::move(line));
        }
        while (committed < events.size()) {
            if (Stopped()) return committed;
            if (!IsOpen() && !OpenSegment()) return committed;
            if (Stopped()) return committed;
            if (segment_bytes_ + lines[committed].size() > kSegmentBytes) {
                if (!Flush()) return committed;
                CloseSegment();
                ++segment_index_;
                if (!OpenSegment()) return committed;
            }
            std::string payload;
            auto end = committed;
            while (end < lines.size() &&
                   segment_bytes_ + payload.size() + lines[end].size() <= kSegmentBytes)
                payload += lines[end++];
            if (!quota_checked_segment_) {
                if (Stopped() || !LockQuota()) return committed;
                struct Unlock {
                    DiagnosticFileSink& sink;
                    ~Unlock() { sink.UnlockQuota(); }
                } unlock{*this};
                if (Stopped() || !CheckQuota(payload.size()) || Stopped()) return committed;
                quota_checked_segment_ = true;
            } // Never hold the root mutex across data writes or flushes.
            if (Stopped()) return committed;
#if defined(_WIN32)
            std::size_t offset = 0;
            while (offset < payload.size()) {
                if (Stopped()) return committed;
                std::uint32_t written = 0;
                const auto remaining = static_cast<std::uint32_t>(payload.size() - offset);
                if (!write_(file_, payload.data() + offset, remaining, written) ||
                    written == 0 || written > remaining) {
                    failure_reason_ = FailureReason::WriteFailed;
                    CloseSegment();
                    ++segment_index_;
                    return committed;
                }
                offset += written;
            }
#else
            output_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            if (!output_) {
                failure_reason_ = FailureReason::WriteFailed;
                CloseSegment();
                ++segment_index_;
                return committed;
            }
#endif
            if (Stopped()) return committed;
            segment_bytes_ += payload.size();
            last_written_sequence_ = events[end - 1].event_sequence;
            if (!Flush()) return committed;
            committed = end;
        }
        failure_reason_ = FailureReason::Unknown;
        return committed;
    } catch (...) {
        failure_reason_ = FailureReason::WriteFailed;
        return committed;
    }
}

bool DiagnosticFileSink::Stopped() const noexcept {
    return abandoned_ && abandoned_->load(std::memory_order_acquire);
}

bool DiagnosticFileSink::IsOpen() const noexcept {
#if defined(_WIN32)
    return file_ != nullptr;
#else
    return output_.is_open();
#endif
}

void DiagnosticFileSink::CloseSegment() noexcept {
#if defined(_WIN32)
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;
#else
    if (output_.is_open()) output_.close();
    output_.clear();
#endif
    quota_checked_segment_ = false;
}

#if defined(_WIN32)
bool DiagnosticFileSink::NativeSize(void* file, std::uint64_t& bytes) noexcept {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(file), &size) || size.QuadPart < 0) return false;
    bytes = static_cast<std::uint64_t>(size.QuadPart);
    return true;
}

bool DiagnosticFileSink::NativeWrite(void* file, const char* data,
        std::uint32_t size, std::uint32_t& written) noexcept {
    DWORD count = 0;
    const bool ok = WriteFile(static_cast<HANDLE>(file), data, size, &count, nullptr) != FALSE;
    written = count;
    return ok;
}
#endif

bool DiagnosticFileSink::Flush() noexcept {
    if (Stopped()) return false;
    if (!IsOpen()) return true;
#if !defined(_WIN32)
    output_.flush();
    if (!output_) {
        failure_reason_ = FailureReason::FlushFailed;
        CloseSegment();
        ++segment_index_;
        return false;
    }
#endif
    // Windows WriteFile already drained our user-space buffer. Do not add
    // FlushFileBuffers: previous ostream::flush did not promise disk durability.
    last_committed_sequence_ = last_written_sequence_;
    failure_reason_ = FailureReason::Unknown;
    return true;
}

void DiagnosticFileSink::Close() noexcept {
#if !defined(_WIN32)
    if (!Stopped()) Flush();
#endif
    CloseSegment();
}

} // namespace livekit::diagnostic
