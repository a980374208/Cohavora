#include "diagnostic_file_sink.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <memory>
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
                                       std::string run_id)
    : root_(std::move(root)),
      run_directory_(root_ / ("run-" + run_id)),
      valid_run_id_(IsRunId(run_id)) {}

DiagnosticFileSink::~DiagnosticFileSink() {
    Close();
#if defined(_WIN32)
    if (run_lease_) CloseHandle(static_cast<HANDLE>(run_lease_));
    if (quota_mutex_) CloseHandle(static_cast<HANDLE>(quota_mutex_));
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
        for (std::filesystem::directory_iterator runs(root, error), end;
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
            std::size_t segments = 0;
            for (std::filesystem::directory_iterator files(runs->path(), error);
                 !error && files != end; files.increment(error)) {
                if (!IsOwnedSegment(files->path())) continue;
                if (++segments > 1024 || files->is_symlink(error) || error ||
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
        output_.open(path, std::ios::binary | std::ios::app);
        if (!output_) {
            failure_reason_ = FailureReason::OpenFailed;
            output_.clear();
            return false;
        }
        segment_bytes_ = std::filesystem::file_size(path, error);
        if (error) segment_bytes_ = 0;
        last_flush_ = std::chrono::steady_clock::now();
        return true;
    } catch (...) {
        failure_reason_ = FailureReason::DirectoryUnavailable;
        return false;
    }
}

bool DiagnosticFileSink::CheckQuota(std::uint64_t incoming_bytes) noexcept {
    try {
        struct Segment {
            std::filesystem::path path;
            std::uint64_t bytes;
            std::filesystem::file_time_type modified;
        };
        std::vector<Segment> reclaimable;
        constexpr std::size_t kMaximumTrackedSegments = 1024;
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
                if (files->is_regular_file(error) && !files->is_symlink(error) && !error &&
                    IsOwnedSegment(files->path())) {
                    const auto bytes = files->file_size(error);
                    if (error) break;
                    total += bytes;
                    if (!active || runs->path() == run_directory_) {
                        if (files->path() != current_path) {
                            const auto modified = files->last_write_time(error);
                            if (error) break;
                            if (reclaimable.size() == kMaximumTrackedSegments) {
                                failure_reason_ = FailureReason::DirectoryUnavailable;
                                return false;
                            }
                            reclaimable.push_back({files->path(), bytes, modified});
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
        for (const auto& segment : reclaimable) {
            if (segment.modified >= expiry &&
                total <= kTotalBytes - incoming_bytes) break;
            std::filesystem::remove(segment.path, error);
            if (error) {
                failure_reason_ = FailureReason::DirectoryUnavailable;
                return false;
            }
            total -= segment.bytes;
        }
        if (total > kTotalBytes - incoming_bytes) {
            failure_reason_ = FailureReason::QuotaExceeded;
            return false;
        }
        return true;
    } catch (...) {
        failure_reason_ = FailureReason::DirectoryUnavailable;
        return false;
    }
}

bool DiagnosticFileSink::LockQuota() noexcept {
#if defined(_WIN32)
    if (!quota_mutex_) {
        const auto name = root_.lexically_normal().wstring();
        std::uint64_t hash = 14695981039346656037ULL;
        for (wchar_t ch : name) {
            hash ^= static_cast<std::uint64_t>(std::towlower(ch));
            hash *= 1099511628211ULL;
        }
        wchar_t mutex_name[64]{};
        std::swprintf(mutex_name, 64, L"Local\\CohavoraDiagnostic-%016llx", hash);
        auto handle = CreateMutexW(nullptr, FALSE, mutex_name);
        if (!handle) {
            failure_reason_ = FailureReason::DirectoryUnavailable;
            return false;
        }
        quota_mutex_ = handle;
    }
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
    try {
        if (!IsValidEvent(event)) {
            failure_reason_ = FailureReason::WriteFailed;
            return false;
        }
        auto line = Serialize(event);
        if (line.size() > kMaximumEventBytes) {
            failure_reason_ = FailureReason::WriteFailed;
            return false;
        }
        if (!output_.is_open() && !OpenSegment()) return false;
        if (segment_bytes_ + line.size() > kSegmentBytes) {
            if (!Flush()) return false;
            output_.close();
            ++segment_index_;
            if (!OpenSegment()) return false;
        }
        if (!LockQuota()) return false;
        const bool quota_ok = CheckQuota(line.size());
        if (!quota_ok) {
            UnlockQuota();
            return false;
        }
        output_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!output_) {
            UnlockQuota();
            failure_reason_ = FailureReason::WriteFailed;
            output_.close();
            ++segment_index_;
            return false;
        }
        segment_bytes_ += line.size();
        last_written_sequence_ = event.event_sequence;
        const bool flushed = Flush();
        UnlockQuota();
        if (!flushed) return false;
        failure_reason_ = FailureReason::Unknown;
        return true;
    } catch (...) {
        failure_reason_ = FailureReason::WriteFailed;
        return false;
    }
}

bool DiagnosticFileSink::Flush() noexcept {
    if (!output_.is_open()) return true;
    output_.flush();
    if (!output_) {
        failure_reason_ = FailureReason::FlushFailed;
        output_.close();
        ++segment_index_;
        return false;
    }
    last_committed_sequence_ = last_written_sequence_;
    last_flush_ = std::chrono::steady_clock::now();
    failure_reason_ = FailureReason::Unknown;
    return true;
}

void DiagnosticFileSink::Close() noexcept {
    Flush();
    if (output_.is_open()) output_.close();
}

} // namespace livekit::diagnostic
