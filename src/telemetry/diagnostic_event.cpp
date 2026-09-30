#include "diagnostic_event.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace livekit::diagnostic {
namespace {

template <std::size_t N>
void CopyToken(std::array<char, N> &target, std::string_view value) noexcept {
    target.fill('\0');
    if (value.empty() || value.size() >= N) return;
    if (value.find(':') != std::string_view::npos && !IsSafeOperationId(value))
        return;
    for (char ch : value) {
        const bool valid = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.' || ch == ':';
        if (!valid) return;
    }
    std::copy(value.begin(), value.end(), target.begin());
}

template <std::size_t N>
bool IsToken(const std::array<char, N>& value, bool operation = false) noexcept {
    bool terminated = false;
    for (char ch : value) {
        if (ch == '\0') {
            terminated = true;
            continue;
        }
        if (terminated) return false;
        const bool valid = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.' || (operation && ch == ':');
        if (!valid) return false;
    }
    return terminated && (!operation || IsSafeOperationId(value.data()));
}

} // namespace

bool IsSafeOperationId(std::string_view value) noexcept {
    if (value.empty()) return true;
    if (value.size() > 64) return false;
    const auto first = value.find(':');
    if (first == std::string_view::npos) {
        return std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '_' || ch == '-' || ch == '.';
        });
    }
    const auto second = value.find(':', first + 1);
    if (second == std::string_view::npos ||
        value.find(':', second + 1) != std::string_view::npos) return false;
    constexpr std::array prefixes{
        "admission", "connect", "startup", "publish_batch",
        "publish_track", "subscribe", "unsubscribe", "unpublish",
        "disconnect", "reconnect", "reconnect_episode", "reconnect_attempt",
        "camera_device_switch", "microphone_device_switch",
        "speaker_device_switch",
    };
    if (!std::any_of(prefixes.begin(), prefixes.end(),
            [&](const char* prefix) { return value.substr(0, first) == prefix; }))
        return false;
    const auto digits = [](std::string_view part) {
        return !part.empty() && std::all_of(part.begin(), part.end(),
            [](char ch) { return ch >= '0' && ch <= '9'; });
    };
    return digits(value.substr(first + 1, second - first - 1)) &&
        digits(value.substr(second + 1));
}

bool OpaqueId::Assign(std::string_view value) noexcept {
    std::array<char, 65> candidate{};
    CopyToken(candidate, value);
    if (candidate[0] == '\0') return false;
    bytes = candidate;
    return true;
}

std::string_view OpaqueId::View() const noexcept {
    return {bytes.data(), std::strlen(bytes.data())};
}

Event Event::Started(std::string_view build,
                     std::string_view symbol) noexcept {
    Event event;
    event.kind = EventKind::ProcessStarted;
    CopyToken(event.build_id, build);
    if (event.build_id[0] == '\0') CopyToken(event.build_id, "unknown");
    CopyToken(event.symbol_identity, symbol);
    if (event.symbol_identity[0] == '\0')
        CopyToken(event.symbol_identity, "unknown");
    return event;
}

Event Event::Stopping(ShutdownReason reason) noexcept {
    Event event;
    event.kind = EventKind::ProcessStopping;
    event.shutdown_reason = reason;
    return event;
}

Event Event::Terminal(Outcome result, DrainResult drain) noexcept {
    Event event;
    event.kind = EventKind::ProcessTerminal;
    event.outcome = result;
    event.drain_result = drain;
    return event;
}

Event Event::QueueHealth(std::uint64_t accepted_count,
                         std::uint64_t dropped_count,
                         std::uint64_t high_water) noexcept {
    Event event;
    event.kind = EventKind::QueueSummary;
    event.accepted = accepted_count;
    event.dropped = dropped_count;
    event.queue_high_water = high_water;
    return event;
}

Event Event::FileFailed(FailureReason reason, std::uint64_t committed,
                        SinkKind sink) noexcept {
    Event event;
    event.kind = EventKind::SinkFailed;
    event.failure_reason = reason;
    event.sink_kind = sink;
    event.last_committed_sequence = committed;
    return event;
}

Event Event::FileRecovered(std::uint64_t committed) noexcept {
    Event event;
    event.kind = EventKind::SinkRecovered;
    event.last_committed_sequence = committed;
    return event;
}

Event Event::Received(ChatKind kind, std::uint64_t size) noexcept {
    Event event;
    event.kind = EventKind::ChatReceived;
    event.chat_kind = kind;
    event.bytes = size;
    return event;
}

Event Event::Issue(IssueCode code) noexcept {
    Event event;
    event.kind = EventKind::ProcessIssue;
    event.issue_code = code;
    return event;
}

std::string_view SdpDescriptionName(SdpDescription value) noexcept {
    constexpr std::string_view names[] = {"unknown", "offer", "answer"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view SdpActionName(SdpAction value) noexcept {
    constexpr std::string_view names[] = {"round", "create_offer", "create_answer", "set_local", "set_remote", "send_offer", "send_answer", "receive_offer", "receive_answer", "callback"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view SdpPhaseName(SdpPhase value) noexcept {
    constexpr std::string_view names[] = {"started", "completed", "failed", "cancelled", "rejected"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view SdpRoleName(SdpRole value) noexcept {
    constexpr std::string_view names[] = {"publisher", "subscriber"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view SdpStateName(SdpState value) noexcept {
    constexpr std::string_view names[] = {"unknown", "stable", "have_local_offer", "have_local_pranswer", "have_remote_offer", "have_remote_pranswer", "closed"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view SdpReasonName(SdpReason value) noexcept {
    constexpr std::string_view names[] = {"none", "rtc_error", "parse_error", "send_error", "generation_expired", "superseded", "timeout", "stopped", "unexpected_message"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string_view EventName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::ProcessStarted: return "process.started";
    case EventKind::ProcessStopping: return "process.stopping";
    case EventKind::ProcessTerminal: return "process.terminal";
    case EventKind::QueueSummary: return "diagnostics.queue.summary";
    case EventKind::SinkFailed: return "diagnostics.sink.failed";
    case EventKind::SinkRecovered: return "diagnostics.sink.recovered";
    case EventKind::ChatReceived: return "chat.received";
    case EventKind::ChatSendTerminal: return "chat.send.terminal";
    case EventKind::TransferTerminal: return "transfer.terminal";
    case EventKind::ProcessIssue: return "process.issue";
    case EventKind::ParticipantIssue: return "participant.issue";
    case EventKind::HttpRequestStarted: return "http.request.started";
    case EventKind::HttpRequestCompleted: return "http.request.completed";
    case EventKind::HttpResponseDecodeFailed: return "http.response.decode_failed";
    case EventKind::AdmissionStarted: return "admission.started";
    case EventKind::AdmissionStageChanged: return "admission.stage_changed";
    case EventKind::AdmissionTerminal: return "admission.terminal";
    case EventKind::RoomConnectStarted: return "room.connect.started";
    case EventKind::RoomConnectTerminal: return "room.connect.terminal";
    case EventKind::StartupTerminal: return "startup.terminal";
    case EventKind::MediaPublishStarted: return "media.publish.started";
    case EventKind::MediaPublishTerminal: return "media.publish.terminal";
    case EventKind::MediaPublishBatchStarted: return "media.publish_batch.started";
    case EventKind::MediaPublishBatchTerminal: return "media.publish_batch.terminal";
    case EventKind::MediaUnpublishStarted: return "media.unpublish.started";
    case EventKind::MediaUnpublishTerminal: return "media.unpublish.terminal";
    case EventKind::MediaSubscriptionChanged: return "media.subscription.changed";
    case EventKind::MediaRecoveryMilestone: return "media.recovery.milestone";
    case EventKind::MediaRecoveryTimeout: return "media.recovery.timeout";
    case EventKind::MediaEndpointRecovered: return "media.endpoint.recovered";
    case EventKind::MediaFirstObserved: return "media.first_observed";
    case EventKind::RenderStallInterval: return "render.stall.interval";
    case EventKind::MediaFallback: return "media.fallback";
    case EventKind::RenderBackendChanged: return "render.backend.changed";
    case EventKind::DeviceSwitchStarted: return "device.switch.started";
    case EventKind::DeviceSwitchTerminal: return "device.switch.terminal";
    case EventKind::DeviceCaptureTerminal: return "device.capture.terminal";
    case EventKind::ReconnectEpisodeStarted: return "reconnect.episode.started";
    case EventKind::ReconnectEpisodeTerminal: return "reconnect.episode.terminal";
    case EventKind::ReconnectAttemptStarted: return "reconnect.attempt.started";
    case EventKind::ReconnectAttemptTerminal: return "reconnect.attempt.terminal";
    case EventKind::ReconnectModeChanged: return "reconnect.mode_changed";
    case EventKind::MeetingLeaveRequested: return "meeting.leave.requested";
    case EventKind::MeetingBackendNotificationCompleted: return "meeting.backend_notification.completed";
    case EventKind::SessionStopped: return "session.stopped";
    case EventKind::RtcSdpFailed: return "rtc.sdp.failed";
    case EventKind::RtcSdpStep: return "rtc.sdp.step";
    case EventKind::RtcLifecycle: return "rtc.lifecycle";
    case EventKind::SignalMessageSummary: return "signal.message.summary";
    case EventKind::SignalIssue: return "signal.issue";
    case EventKind::CallbackRejectedSummary: return "callback.rejected.summary";
    case EventKind::DiagnosticsModeChanged: return "diagnostics.mode.changed";
    case EventKind::RetentionChanged: return "diagnostics.retention.changed";
    }
    return "diagnostics.queue.summary";
}

std::string_view ComponentName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::ProcessStarted:
    case EventKind::ProcessStopping:
    case EventKind::ProcessTerminal:
    case EventKind::ProcessIssue: return "app";
    case EventKind::QueueSummary:
    case EventKind::SinkFailed:
    case EventKind::SinkRecovered:
    case EventKind::DiagnosticsModeChanged:
    case EventKind::RetentionChanged: return "diagnostic_pipeline";
    case EventKind::HttpRequestStarted:
    case EventKind::HttpRequestCompleted:
    case EventKind::HttpResponseDecodeFailed: return "net";
    case EventKind::AdmissionStarted:
    case EventKind::AdmissionStageChanged:
    case EventKind::AdmissionTerminal:
    case EventKind::StartupTerminal:
    case EventKind::MeetingLeaveRequested:
    case EventKind::MeetingBackendNotificationCompleted: return "meeting_coordinator";
    case EventKind::MediaPublishStarted:
    case EventKind::MediaPublishTerminal:
    case EventKind::MediaUnpublishTerminal:
    case EventKind::ParticipantIssue: return "participant";
    case EventKind::MediaPublishBatchStarted:
    case EventKind::MediaPublishBatchTerminal:
    case EventKind::MediaUnpublishStarted:
    case EventKind::MediaSubscriptionChanged:
    case EventKind::MediaFallback:
    case EventKind::RoomConnectStarted:
    case EventKind::RoomConnectTerminal:
    case EventKind::ReconnectEpisodeStarted:
    case EventKind::ReconnectEpisodeTerminal:
    case EventKind::ReconnectAttemptStarted:
    case EventKind::ReconnectAttemptTerminal:
    case EventKind::ReconnectModeChanged:
    case EventKind::SignalMessageSummary:
    case EventKind::SignalIssue: return "room";
    case EventKind::MediaRecoveryMilestone:
    case EventKind::MediaRecoveryTimeout:
    case EventKind::MediaFirstObserved:
    case EventKind::RenderStallInterval:
    case EventKind::MediaEndpointRecovered: return "session_telemetry";
    case EventKind::SessionStopped:
    case EventKind::CallbackRejectedSummary: return "session_runtime";
    case EventKind::DeviceSwitchStarted:
    case EventKind::DeviceSwitchTerminal:
    case EventKind::DeviceCaptureTerminal: return "media";
    case EventKind::RenderBackendChanged: return "render";
    case EventKind::ChatReceived:
    case EventKind::ChatSendTerminal:
    case EventKind::TransferTerminal: return "meeting_ui";
    case EventKind::RtcSdpStep:
    case EventKind::RtcSdpFailed:
    case EventKind::RtcLifecycle: return "rtc";
    }
    return "diagnostic_pipeline";
}

std::string_view BusinessErrorName(std::int32_t code) noexcept {
    switch (code) {
    case 100010: return "token_expired";
    case 100002: return "token_invalid";
    case 100004: return "token_not_found";
    default: return "unknown";
    }
}

Severity EventSeverity(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::ChatReceived:
    case EventKind::HttpRequestStarted:
    case EventKind::AdmissionStageChanged:
    case EventKind::MediaPublishStarted:
    case EventKind::ReconnectAttemptStarted:
    case EventKind::SignalMessageSummary:
    case EventKind::CallbackRejectedSummary: return Severity::Debug;
    case EventKind::QueueSummary:
    case EventKind::SinkFailed: return Severity::Warning;
    case EventKind::ReconnectEpisodeStarted:
    case EventKind::ReconnectAttemptTerminal:
    case EventKind::ReconnectModeChanged:
    case EventKind::MediaRecoveryTimeout:
    case EventKind::RenderBackendChanged:
    case EventKind::MeetingBackendNotificationCompleted: return Severity::Warning;
    case EventKind::ProcessIssue: return Severity::Error;
    case EventKind::ParticipantIssue: return Severity::Warning;
    case EventKind::HttpResponseDecodeFailed: return Severity::Warning;
    case EventKind::AdmissionTerminal:
    case EventKind::RoomConnectTerminal:
    case EventKind::StartupTerminal:
    case EventKind::MediaPublishTerminal:
    case EventKind::MediaPublishBatchTerminal:
    case EventKind::MediaUnpublishTerminal:
    case EventKind::ReconnectEpisodeTerminal:
        return Severity::Info;
    case EventKind::RtcSdpFailed: return Severity::Warning;
    case EventKind::MediaFallback: return Severity::Warning;
    case EventKind::DeviceSwitchStarted:
    case EventKind::DeviceSwitchTerminal: return Severity::Info;
    case EventKind::DeviceCaptureTerminal: return Severity::Info;
    case EventKind::SignalIssue: return Severity::Warning;
    case EventKind::RtcLifecycle: return Severity::Info;
    default: return Severity::Info;
    }
}

Severity EventSeverity(const Event& event) noexcept {
    if (event.kind == EventKind::RtcSdpStep) {
        if (event.sdp_phase == SdpPhase::Failed) return Severity::Error;
        if (event.sdp_phase == SdpPhase::Rejected || event.sdp_phase == SdpPhase::Cancelled)
            return Severity::Warning;
        return Severity::Info;
    }
    if (event.kind == EventKind::ProcessIssue &&
        event.issue_code == IssueCode::QtFatal) return Severity::Fatal;
    if (event.kind == EventKind::MediaRecoveryTimeout) return Severity::Warning;
    if (event.kind == EventKind::RtcLifecycle &&
        event.rtc_status != RtcStatus::Starting &&
        event.rtc_status != RtcStatus::Ready &&
        event.rtc_status != RtcStatus::Stopped)
        return Severity::Error;
    if (event.outcome == Outcome::Failure || event.outcome == Outcome::Timeout) {
        if (event.kind == EventKind::ReconnectAttemptTerminal ||
            event.kind == EventKind::HttpRequestCompleted ||
            event.kind == EventKind::MeetingBackendNotificationCompleted)
            return Severity::Warning;
        return Severity::Error;
    }
    if (event.outcome == Outcome::DegradedSuccess) return Severity::Warning;
    return EventSeverity(event.kind);
}

bool IsCritical(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::ChatReceived:
    case EventKind::ChatSendTerminal:
    case EventKind::TransferTerminal:
    case EventKind::HttpRequestStarted:
    case EventKind::AdmissionStageChanged:
    case EventKind::MediaPublishStarted:
    case EventKind::MediaPublishBatchStarted:
    case EventKind::MediaUnpublishStarted:
    case EventKind::DeviceSwitchStarted:
    case EventKind::ReconnectAttemptStarted:
    case EventKind::SignalMessageSummary:
    case EventKind::CallbackRejectedSummary:
    case EventKind::RenderStallInterval:
        return false;
    default: return true;
    }
}

bool IsValidEvent(const Event& event) noexcept {
    switch (event.kind) {
    case EventKind::ProcessStarted:
    case EventKind::ProcessStopping:
    case EventKind::ProcessTerminal:
    case EventKind::QueueSummary:
    case EventKind::SinkFailed:
    case EventKind::SinkRecovered:
    case EventKind::ChatReceived:
    case EventKind::ChatSendTerminal:
    case EventKind::TransferTerminal:
    case EventKind::ProcessIssue:
    case EventKind::ParticipantIssue:
    case EventKind::HttpRequestStarted:
    case EventKind::HttpRequestCompleted:
    case EventKind::HttpResponseDecodeFailed:
    case EventKind::AdmissionStarted:
    case EventKind::AdmissionStageChanged:
    case EventKind::AdmissionTerminal:
    case EventKind::RoomConnectStarted:
    case EventKind::RoomConnectTerminal:
    case EventKind::StartupTerminal:
    case EventKind::MediaPublishStarted:
    case EventKind::MediaPublishTerminal:
    case EventKind::MediaPublishBatchStarted:
    case EventKind::MediaPublishBatchTerminal:
    case EventKind::MediaUnpublishStarted:
    case EventKind::MediaUnpublishTerminal:
    case EventKind::MediaSubscriptionChanged:
    case EventKind::MediaRecoveryMilestone:
    case EventKind::MediaRecoveryTimeout:
    case EventKind::MediaEndpointRecovered:
    case EventKind::MediaFallback:
    case EventKind::RenderBackendChanged:
    case EventKind::DeviceSwitchStarted:
    case EventKind::DeviceSwitchTerminal:
    case EventKind::DeviceCaptureTerminal:
    case EventKind::ReconnectEpisodeStarted:
    case EventKind::ReconnectEpisodeTerminal:
    case EventKind::ReconnectAttemptStarted:
    case EventKind::ReconnectAttemptTerminal:
    case EventKind::ReconnectModeChanged:
    case EventKind::MeetingLeaveRequested:
    case EventKind::MeetingBackendNotificationCompleted:
    case EventKind::SessionStopped:
    case EventKind::RtcSdpStep:
    case EventKind::RtcSdpFailed:
    case EventKind::RtcLifecycle:
    case EventKind::SignalMessageSummary:
    case EventKind::SignalIssue:
    case EventKind::CallbackRejectedSummary:
    case EventKind::DiagnosticsModeChanged:
    case EventKind::MediaFirstObserved:
    case EventKind::RenderStallInterval:
    case EventKind::RetentionChanged:
        break;
    default: return false;
    }
    const auto hex_id = [](std::string_view value) {
        return value.size() == 32 && std::all_of(value.begin(), value.end(),
            [](char ch) { return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f'); });
    };
    if (event.kind == EventKind::MediaFirstObserved || event.kind == EventKind::RenderStallInterval) {
        if (!hex_id(event.media_endpoint_id.View()) ||
            !hex_id(event.context.anonymous_session_id.View()) ||
            !event.context.has_session_generation || !event.context.session_generation ||
            !event.context.has_room_generation || !event.context.room_generation ||
            !event.binding_epoch || !event.source_monotonic_us ||
            event.source_monotonic_us > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            event.context.operation_id.View().empty()) return false;
        if (event.kind == EventKind::MediaFirstObserved &&
            (event.media_observation > MediaObservation::FirstRender ||
             event.media_kind != (event.media_observation == MediaObservation::FirstPcm
                ? MediaKind::Audio : MediaKind::Video))) return false;
        if (event.kind == EventKind::RenderStallInterval &&
            (event.stall_boundary > StallBoundary::Inactive ||
             event.media_kind != MediaKind::Video || !event.interval_begin_us ||
             event.stall_threshold_us < 500000 ||
             event.interval_begin_us > event.source_monotonic_us ||
             event.interval_end_us > event.source_monotonic_us ||
             (event.stall_boundary == StallBoundary::Open ? event.interval_end_us != 0
                : event.interval_end_us < event.interval_begin_us))) return false;
    }
    if (event.kind == EventKind::MediaEndpointRecovered &&
        (!hex_id(event.media_endpoint_id.View()) ||
         (!event.previous_media_endpoint_id.View().empty() &&
          !hex_id(event.previous_media_endpoint_id.View())) ||
         !event.context.has_recovery_epoch ||
         !event.context.has_room_generation)) return false;
    if (event.kind == EventKind::RtcSdpStep &&
        (event.context.operation_id.View().empty() || event.sdp_sequence == 0 || event.sdp_description > SdpDescription::Answer ||
         event.sdp_action > SdpAction::Callback || event.sdp_phase > SdpPhase::Rejected ||
         event.sdp_role > SdpRole::Subscriber || event.signaling_before > SdpState::Closed ||
         event.signaling_after > SdpState::Closed || event.sdp_reason > SdpReason::UnexpectedMessage)) return false;
    return (!event.window_sample || event.kind == EventKind::SignalMessageSummary) &&
        event.http_status >= 0 && event.http_status <= 599 &&
        event.network_error >= 0 && event.network_error <= 1000 &&
        event.rtc_error_type >= 0 && event.rtc_error_type <= 100 &&
        IsToken(event.build_id) &&
        IsToken(event.symbol_identity) &&
        IsToken(event.context.anonymous_session_id.bytes) &&
        IsToken(event.context.operation_id.bytes, true) &&
        IsToken(event.context.parent_operation_id.bytes, true) &&
        IsToken(event.context.request_id.bytes) &&
        IsToken(event.context.legacy_operation_id.bytes);
}

std::string_view MediaObservationName(MediaObservation value) noexcept {
    constexpr std::string_view names[]{"first_decoded", "first_pcm", "first_render_submit"};
    const auto i = static_cast<std::size_t>(value);
    return i < std::size(names) ? names[i] : "unknown";
}

std::string_view StallBoundaryName(StallBoundary value) noexcept {
    constexpr std::string_view names[]{"open", "recovered", "hidden", "rebound", "stopped", "inactive"};
    const auto i = static_cast<std::size_t>(value);
    return i < std::size(names) ? names[i] : "unknown";
}

bool IsTimelineEvent(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::HttpRequestStarted: case EventKind::HttpRequestCompleted:
    case EventKind::HttpResponseDecodeFailed: case EventKind::AdmissionStarted:
    case EventKind::AdmissionStageChanged: case EventKind::AdmissionTerminal:
    case EventKind::RoomConnectStarted: case EventKind::RoomConnectTerminal:
    case EventKind::StartupTerminal: case EventKind::MediaPublishStarted:
    case EventKind::MediaPublishTerminal: case EventKind::MediaPublishBatchStarted:
    case EventKind::MediaPublishBatchTerminal: case EventKind::MediaUnpublishStarted:
    case EventKind::MediaUnpublishTerminal: case EventKind::MediaRecoveryMilestone:
    case EventKind::MediaRecoveryTimeout: case EventKind::MediaEndpointRecovered:
    case EventKind::ReconnectEpisodeStarted: case EventKind::ReconnectEpisodeTerminal:
    case EventKind::ReconnectAttemptStarted: case EventKind::ReconnectAttemptTerminal:
    case EventKind::ReconnectModeChanged: case EventKind::DeviceSwitchStarted:
    case EventKind::DeviceSwitchTerminal: case EventKind::MeetingLeaveRequested:
    case EventKind::MeetingBackendNotificationCompleted: case EventKind::SessionStopped:
    case EventKind::MediaFirstObserved: case EventKind::RenderStallInterval: return true;
    default: return false;
    }
}

std::string_view RouteName(Route value) noexcept {
    constexpr std::string_view names[] = {
        "unknown", "login", "logout", "register", "create_meeting",
        "join_meeting", "get_meeting_token", "get_meetings", "get_meeting",
        "book_meeting", "update_meeting", "end_meeting", "leave_meeting"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[0];
}

std::string_view StageName(Stage value) noexcept {
    constexpr std::string_view names[] = {
        "unknown", "joining", "creating", "fetching_token", "starting_room",
        "connecting_room", "starting_media", "in_meeting", "resume",
        "full_restart", "create_offer", "create_answer", "set_local_description",
        "set_remote_description"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[0];
}

std::string_view ErrorLayerName(ErrorLayer value) noexcept {
    constexpr std::string_view names[] = {
        "none", "policy", "http", "network", "business", "parse", "native", "rtc"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[0];
}

std::string_view ErrorCodeName(ErrorCode value) noexcept {
    constexpr std::string_view names[] = {
        "none", "unknown", "cancelled", "invalid_state", "signal_connect_failed",
        "join_timeout", "join_rejected", "peer_connection_create_failed",
        "negotiation_failed", "peer_connection_timeout", "permission_denied",
        "track_publish_timeout", "track_publish_rejected", "track_unpublish_timeout",
        "reconnect_exhausted", "session_closed", "state_uncertain", "session_invalid",
        "transport_policy", "redirect_rejected", "invalid_response",
        "codec_unsupported", "track_not_found"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[1];
}

std::string_view MediaKindName(MediaKind value) noexcept {
    switch (value) {
    case MediaKind::Audio: return "audio";
    case MediaKind::Video: return "video";
    default: return "unknown";
    }
}

std::string_view TransferKindName(TransferKind value) noexcept {
    switch (value) {
    case TransferKind::Image: return "image";
    case TransferKind::File: return "file";
    default: return "unknown";
    }
}

std::string_view TransferDirectionName(TransferDirection value) noexcept {
    return value == TransferDirection::Receive ? "receive" : "send";
}

std::string_view SubscriptionStateName(SubscriptionState value) noexcept {
    switch (value) {
    case SubscriptionState::Subscribed: return "subscribed";
    case SubscriptionState::Unsubscribed: return "unsubscribed";
    case SubscriptionState::Blocked: return "blocked";
    default: return "unknown";
    }
}

std::string_view RecoveryMeasurementName(RecoveryMeasurement value) noexcept {
    switch (value) {
    case RecoveryMeasurement::DecodedVideoStable: return "decoded_video_stably_recovered";
    case RecoveryMeasurement::PcmAudioStable: return "pcm_stably_recovered";
    case RecoveryMeasurement::VisibleRenderStable: return "visible_render_stably_recovered";
    }
    return "unknown";
}

std::string_view SignalCategoryName(SignalCategory value) noexcept {
    switch (value) {
    case SignalCategory::Control: return "control";
    case SignalCategory::Media: return "media";
    case SignalCategory::Periodic: return "periodic";
    default: return "unknown";
    }
}

std::string_view RtcStatusName(RtcStatus value) noexcept {
    constexpr std::string_view names[] = {
        "starting", "ready", "stopped", "ssl_failed", "threads_failed",
        "playout_device_failed", "factory_failed", "playout_start_failed"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[0];
}

std::string_view MediaFallbackReasonName(MediaFallbackReason value) noexcept {
    switch (value) {
    case MediaFallbackReason::NoEncodings: return "no_encodings";
    case MediaFallbackReason::AddTransceiverFailed: return "add_transceiver_failed";
    case MediaFallbackReason::SenderParametersFailed: return "sender_parameters_failed";
    }
    return "no_encodings";
}

std::string_view RenderBackendName(RenderBackend value) noexcept {
    return value == RenderBackend::Gpu ? "gpu" : "qt_cpu";
}

std::string_view RenderReasonName(RenderReason value) noexcept {
    switch (value) {
    case RenderReason::GpuReady: return "gpu_ready";
    case RenderReason::InitializationFailed: return "initialization_failed";
    case RenderReason::DeviceFailure: return "device_failure";
    }
    return "device_failure";
}

std::string_view DeviceSwitchReasonName(DeviceSwitchReason value) noexcept {
    constexpr std::string_view names[] = {
        "completed", "already_active", "invalid_configuration", "factory_failed",
        "init_failed", "start_failed", "probe_timeout", "superseded",
        "capture_stalled"};
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(names) ? names[index] : names[0];
}

std::string_view SeverityName(Severity value) noexcept {
    switch (value) {
    case Severity::Trace: return "trace";
    case Severity::Debug: return "debug";
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    case Severity::Fatal: return "fatal";
    }
    return "info";
}

std::string_view ThreadRoleName(ThreadRole value) noexcept {
    switch (value) {
    case ThreadRole::Ui: return "ui";
    case ThreadRole::Session: return "session";
    case ThreadRole::Rtc: return "rtc";
    case ThreadRole::Media: return "media";
    case ThreadRole::Writer: return "writer";
    default: return "unknown";
    }
}

std::string_view OutcomeName(Outcome value) noexcept {
    switch (value) {
    case Outcome::Success: return "success";
    case Outcome::DegradedSuccess: return "degraded_success";
    case Outcome::Failure: return "failure";
    case Outcome::Timeout: return "timeout";
    case Outcome::Cancelled: return "cancelled";
    default: return "unknown";
    }
}

std::string_view DrainResultName(DrainResult value) noexcept {
    switch (value) {
    case DrainResult::Completed: return "completed";
    case DrainResult::TimedOut: return "timed_out";
    case DrainResult::Failed: return "failed";
    default: return "unknown";
    }
}

std::string_view ShutdownReasonName(ShutdownReason value) noexcept {
    switch (value) {
    case ShutdownReason::UserExit: return "user_exit";
    case ShutdownReason::LoginRejected: return "login_rejected";
    case ShutdownReason::FatalError: return "fatal_error";
    default: return "unknown";
    }
}

std::string_view SinkKindName(SinkKind value) noexcept {
    switch (value) {
    case SinkKind::Ui: return "ui";
    case SinkKind::Telemetry: return "telemetry";
    case SinkKind::Crash: return "crash";
    default: return "file";
    }
}

std::string_view ChatKindName(ChatKind value) noexcept {
    switch (value) {
    case ChatKind::Image: return "image";
    case ChatKind::File: return "file";
    default: return "text";
    }
}

std::string_view IssueCodeName(IssueCode value) noexcept {
    switch (value) {
    case IssueCode::InvalidSignInOptions: return "invalid_sign_in_options";
    case IssueCode::SignInFailed: return "sign_in_failed";
    case IssueCode::NativeCleanupFailed: return "native_cleanup_failed";
    case IssueCode::SessionInvalidated: return "session_invalidated";
    case IssueCode::QtFatal: return "qt_fatal";
    case IssueCode::FinalSnapshotNotQueued: return "final_snapshot_not_queued";
    case IssueCode::SessionHandlerFailed: return "session_handler_failed";
    case IssueCode::PanicTriggered: return "panic_triggered";
    }
    return "unknown";
}

std::string_view ParticipantIssueReasonName(ParticipantIssueReason value) noexcept {
    switch (value) {
    case ParticipantIssueReason::PublishTrackDenied: return "publish_track_denied";
    case ParticipantIssueReason::PublishDataDenied: return "publish_data_denied";
    case ParticipantIssueReason::UpdateMetadataDenied: return "update_metadata_denied";
    case ParticipantIssueReason::PublishDataHandlerMissing: return "publish_data_handler_missing";
    }
    return "unknown";
}

std::string_view FailureReasonName(FailureReason value) noexcept {
    switch (value) {
    case FailureReason::DirectoryUnavailable: return "directory_unavailable";
    case FailureReason::OpenFailed: return "open_failed";
    case FailureReason::WriteFailed: return "write_failed";
    case FailureReason::FlushFailed: return "flush_failed";
    case FailureReason::QuotaExceeded: return "quota_exceeded";
    case FailureReason::LedgerUnavailable: return "ledger_unavailable";
    default: return "unknown";
    }
}

static_assert(sizeof(Event) * kQueueEvents <= kQueueBytes);

} // namespace livekit::diagnostic
