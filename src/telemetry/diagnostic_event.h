#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace livekit::diagnostic {

inline constexpr std::size_t kQueueEvents = 8192;
inline constexpr std::size_t kQueueBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kOrdinaryEvents = 7168;
inline constexpr std::size_t kOrdinaryBytes = 7 * 1024 * 1024;
inline constexpr std::size_t kCriticalEvents = 1024;
inline constexpr std::size_t kCriticalBytes = 1024 * 1024;
inline constexpr std::size_t kMaximumEventBytes = 16 * 1024;

enum class EventKind : std::uint8_t {
    ProcessStarted,
    ProcessStopping,
    ProcessTerminal,
    QueueSummary,
    SinkFailed,
    SinkRecovered,
    ChatReceived,
    ChatSendTerminal,
    TransferTerminal,
    ProcessIssue,
    ParticipantIssue,
    HttpRequestStarted,
    HttpRequestCompleted,
    HttpResponseDecodeFailed,
    AdmissionStarted,
    AdmissionStageChanged,
    AdmissionTerminal,
    RoomConnectStarted,
    RoomConnectTerminal,
    StartupTerminal,
    MediaPublishStarted,
    MediaPublishTerminal,
    MediaPublishBatchStarted,
    MediaPublishBatchTerminal,
    MediaUnpublishStarted,
    MediaUnpublishTerminal,
    MediaSubscriptionChanged,
    MediaRecoveryMilestone,
    MediaRecoveryTimeout,
    MediaEndpointRecovered,
    MediaFallback,
    RenderBackendChanged,
    DeviceSwitchStarted,
    DeviceSwitchTerminal,
    DeviceCaptureTerminal,
    ReconnectEpisodeStarted,
    ReconnectEpisodeTerminal,
    ReconnectAttemptStarted,
    ReconnectAttemptTerminal,
    ReconnectModeChanged,
    MeetingLeaveRequested,
    MeetingBackendNotificationCompleted,
    SessionStopped,
    RtcSdpFailed,
    RtcLifecycle,
    SignalMessageSummary,
    SignalIssue,
    CallbackRejectedSummary,
    DiagnosticsModeChanged,
    RetentionChanged,
    RtcSdpStep,
    MediaFirstObserved,
    RenderStallInterval,
    SettingsFirstPaint,
    SettingsDeviceProbe,
};

enum class MediaObservation : std::uint8_t { FirstDecoded, FirstPcm, FirstRender };
enum class StallBoundary : std::uint8_t { Open, Recovered, Hidden, Rebound, Stopped, Inactive };
std::string_view MediaObservationName(MediaObservation value) noexcept;
std::string_view StallBoundaryName(StallBoundary value) noexcept;
bool IsTimelineEvent(EventKind kind) noexcept;

// Closed SDP metadata only: no SDP text, addresses, credentials or exception text.
enum class SdpAction : std::uint8_t {
    Round, CreateOffer, CreateAnswer, SetLocal, SetRemote,
    SendOffer, SendAnswer, ReceiveOffer, ReceiveAnswer, Callback
};
enum class SdpDescription : std::uint8_t { Unknown, Offer, Answer };
std::string_view SdpDescriptionName(SdpDescription value) noexcept;
enum class SdpPhase : std::uint8_t { Started, Completed, Failed, Cancelled, Rejected };
enum class SdpRole : std::uint8_t { Publisher, Subscriber };
enum class SdpState : std::uint8_t {
    Unknown, Stable, HaveLocalOffer, HaveLocalPranswer,
    HaveRemoteOffer, HaveRemotePranswer, Closed
};
enum class SdpReason : std::uint8_t {
    None, RtcError, ParseError, SendError, GenerationExpired,
    Superseded, Timeout, Stopped, UnexpectedMessage
};
std::string_view SdpActionName(SdpAction value) noexcept;
std::string_view SdpPhaseName(SdpPhase value) noexcept;
std::string_view SdpRoleName(SdpRole value) noexcept;
std::string_view SdpStateName(SdpState value) noexcept;
std::string_view SdpReasonName(SdpReason value) noexcept;

enum class Severity : std::uint8_t { Trace, Debug, Info, Warning, Error, Fatal };
enum class ThreadRole : std::uint8_t { Unknown, Ui, Session, Rtc, Media, Writer };
enum class Outcome : std::uint8_t {
    Unknown, Success, DegradedSuccess, Failure, Timeout, Cancelled
};
enum class DrainResult : std::uint8_t { Unknown, Completed, TimedOut, Failed };
enum class ShutdownReason : std::uint8_t { Unknown, UserExit, LoginRejected, FatalError };
enum class SinkKind : std::uint8_t { File, Ui, Telemetry, Crash };
enum class ChatKind : std::uint8_t { Text, Image, File };
enum class TransferKind : std::uint8_t { Unknown, Image, File };
enum class TransferDirection : std::uint8_t { Send, Receive };
enum class SubscriptionState : std::uint8_t { Subscribed, Unsubscribed, Blocked, Unknown };
enum class RecoveryMeasurement : std::uint8_t {
    DecodedVideoStable, PcmAudioStable, VisibleRenderStable
};
enum class IssueCode : std::uint8_t {
    InvalidSignInOptions, SignInFailed, NativeCleanupFailed,
    SessionInvalidated, QtFatal,
    FinalSnapshotNotQueued, SessionHandlerFailed, PanicTriggered,
};
enum class ParticipantIssueReason : std::uint8_t {
    PublishTrackDenied, PublishDataDenied, UpdateMetadataDenied,
    PublishDataHandlerMissing,
};
enum class FailureReason : std::uint8_t {
    Unknown, DirectoryUnavailable, OpenFailed, WriteFailed, FlushFailed,
    QuotaExceeded, LedgerUnavailable,
};
enum class Route : std::uint8_t {
    Unknown, Login, Logout, Register, CreateMeeting, JoinMeeting,
    GetMeetingToken, GetMeetings, GetMeeting, BookMeeting, UpdateMeeting,
    EndMeeting, LeaveMeeting,
};
enum class Stage : std::uint8_t {
    Unknown, Joining, Creating, FetchingToken, StartingRoom,
    ConnectingRoom, StartingMedia, InMeeting, Resume, FullRestart,
    CreateOffer, CreateAnswer, SetLocalDescription, SetRemoteDescription,
};
enum class ErrorLayer : std::uint8_t {
    None, Policy, Http, Network, Business, Parse, Native, Rtc,
};
enum class MediaKind : std::uint8_t { Unknown, Audio, Video };
enum class SignalCategory : std::uint8_t { Unknown, Control, Media, Periodic };
enum class RtcStatus : std::uint8_t {
    Starting, Ready, Stopped, SslFailed, ThreadsFailed,
    PlayoutDeviceFailed, FactoryFailed, PlayoutStartFailed,
};
enum class MediaFallbackReason : std::uint8_t {
    NoEncodings, AddTransceiverFailed, SenderParametersFailed,
};
enum class RenderBackend : std::uint8_t { QtCpu, Gpu };
enum class RenderReason : std::uint8_t {
    GpuReady, InitializationFailed, DeviceFailure,
};
enum class DeviceSwitchReason : std::uint8_t {
    Completed, AlreadyActive, InvalidConfiguration, FactoryFailed,
    InitFailed, StartFailed, ProbeTimeout, Superseded,
    CaptureStalled,
};
enum class ErrorCode : std::uint8_t {
    None, Unknown, Cancelled, InvalidState, SignalConnectFailed,
    JoinTimeout, JoinRejected, PeerConnectionCreateFailed,
    NegotiationFailed, PeerConnectionTimeout, PermissionDenied,
    TrackPublishTimeout, TrackPublishRejected, TrackUnpublishTimeout,
    ReconnectExhausted, SessionClosed, StateUncertain, SessionInvalid,
    TransportPolicy, RedirectRejected, InvalidResponse,
    CodecUnsupported, TrackNotFound,
};

struct OpaqueId final {
    std::array<char, 65> bytes{};

    bool Assign(std::string_view value) noexcept;
    std::string_view View() const noexcept;
};

bool IsSafeOperationId(std::string_view value) noexcept;

struct Context final {
    OpaqueId anonymous_session_id;
    OpaqueId operation_id;
    OpaqueId parent_operation_id;
    OpaqueId request_id;
    OpaqueId legacy_operation_id;
    std::uint64_t session_generation = 0;
    std::uint64_t room_generation = 0;
    std::uint64_t recovery_epoch = 0;
    bool has_session_generation = false;
    bool has_room_generation = false;
    bool has_recovery_epoch = false;
};

// The LG2 producers have no free-text attribute. Later catalog events extend
// this closed type rather than accepting arbitrary JSON or exception strings.
struct Event final {
    EventKind kind = EventKind::ProcessStarted;
    ThreadRole thread_role = ThreadRole::Unknown;
    SdpDescription sdp_description = SdpDescription::Unknown;
    SdpAction sdp_action = SdpAction::Round;
    SdpPhase sdp_phase = SdpPhase::Started;
    SdpRole sdp_role = SdpRole::Publisher;
    SdpState signaling_before = SdpState::Unknown;
    SdpState signaling_after = SdpState::Unknown;
    SdpReason sdp_reason = SdpReason::None;
    std::uint64_t sdp_sequence = 0;
    bool sdp_after_terminal = false;
    bool sdp_ice_restart = false;
    Context context;
    OpaqueId media_endpoint_id;
    OpaqueId previous_media_endpoint_id;
    MediaObservation media_observation = MediaObservation::FirstDecoded;
    StallBoundary stall_boundary = StallBoundary::Open;
    std::uint64_t binding_epoch = 0;
    // Absolute steady-clock times. monotonic_us below remains process-relative.
    std::uint64_t source_monotonic_us = 0;
    std::uint64_t interval_begin_us = 0;
    std::uint64_t interval_end_us = 0;
    std::uint64_t stall_threshold_us = 0;
    std::array<char, 65> build_id{};
    std::array<char, 65> symbol_identity{};
    ShutdownReason shutdown_reason = ShutdownReason::Unknown;
    Outcome outcome = Outcome::Unknown;
    DrainResult drain_result = DrainResult::Unknown;
    SinkKind sink_kind = SinkKind::File;
    ChatKind chat_kind = ChatKind::Text;
    TransferKind transfer_kind = TransferKind::Unknown;
    TransferDirection transfer_direction = TransferDirection::Send;
    SubscriptionState subscription_state = SubscriptionState::Unknown;
    RecoveryMeasurement recovery_measurement = RecoveryMeasurement::DecodedVideoStable;
    IssueCode issue_code = IssueCode::InvalidSignInOptions;
    ParticipantIssueReason participant_issue_reason = ParticipantIssueReason::PublishTrackDenied;
    FailureReason failure_reason = FailureReason::Unknown;
    Route route = Route::Unknown;
    Stage stage = Stage::Unknown;
    ErrorLayer error_layer = ErrorLayer::None;
    ErrorCode error_code = ErrorCode::None;
    MediaKind media_kind = MediaKind::Unknown;
    SignalCategory signal_category = SignalCategory::Unknown;
    RtcStatus rtc_status = RtcStatus::Starting;
    MediaFallbackReason media_fallback_reason = MediaFallbackReason::NoEncodings;
    RenderBackend from_render_backend = RenderBackend::QtCpu;
    RenderBackend to_render_backend = RenderBackend::QtCpu;
    RenderReason render_reason = RenderReason::GpuReady;
    DeviceSwitchReason device_switch_reason = DeviceSwitchReason::Completed;
    std::int32_t http_status = 0;
    std::int32_t network_error = 0;
    std::int32_t business_code = 0;
    std::int32_t rtc_error_type = 0;
    std::uint32_t attempt = 0;
    std::uint32_t batch_track_count = 0;
    std::uint32_t signal_message_count = 0;
    bool retryable = false;
    bool window_sample = false;
    bool diagnostic_window_enabled = false;
    bool retention_enabled = true;
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t queue_high_water = 0;
    std::uint64_t bytes = 0;
    std::uint64_t duration_ms = 0;
    std::uint32_t device_count = 0;
    bool cache_hit = false;
    std::uint64_t last_committed_sequence = 0;
    std::uint64_t current_generation = 0;
    std::int64_t diagnostic_expires_at_utc_ms = 0;
    std::int64_t occurred_at_utc_ms = 0;
    std::uint64_t monotonic_us = 0;
    std::uint64_t event_sequence = 0;
    std::uint32_t process_id = 0;
    std::uint32_t redaction_version = 1;
    std::array<char, 33> process_run_id{};

    static Event Started(std::string_view build_id,
                         std::string_view symbol_identity = "unknown") noexcept;
    static Event Stopping(ShutdownReason reason) noexcept;
    static Event Terminal(Outcome outcome, DrainResult drain) noexcept;
    static Event QueueHealth(std::uint64_t accepted, std::uint64_t dropped,
                             std::uint64_t high_water) noexcept;
    static Event FileFailed(FailureReason reason,
                            std::uint64_t last_committed,
                            SinkKind sink = SinkKind::File) noexcept;
    static Event FileRecovered(std::uint64_t last_committed) noexcept;
    static Event Received(ChatKind kind, std::uint64_t bytes) noexcept;
    static Event Issue(IssueCode code) noexcept;
    static Event SettingsPaint(std::uint64_t elapsed_ms) noexcept;
    static Event SettingsProbe(MediaKind media, std::uint64_t elapsed_ms,
                               Outcome result, std::uint32_t count,
                               bool cache_hit = false) noexcept;
};

std::string_view EventName(EventKind kind) noexcept;
std::string_view ComponentName(EventKind kind) noexcept;
std::string_view BusinessErrorName(std::int32_t code) noexcept;
std::string_view SeverityName(Severity severity) noexcept;
std::string_view ThreadRoleName(ThreadRole role) noexcept;
std::string_view OutcomeName(Outcome outcome) noexcept;
std::string_view DrainResultName(DrainResult result) noexcept;
std::string_view ShutdownReasonName(ShutdownReason reason) noexcept;
std::string_view SinkKindName(SinkKind kind) noexcept;
std::string_view ChatKindName(ChatKind kind) noexcept;
std::string_view TransferKindName(TransferKind kind) noexcept;
std::string_view TransferDirectionName(TransferDirection direction) noexcept;
std::string_view SubscriptionStateName(SubscriptionState state) noexcept;
std::string_view RecoveryMeasurementName(RecoveryMeasurement point) noexcept;
std::string_view IssueCodeName(IssueCode code) noexcept;
std::string_view ParticipantIssueReasonName(ParticipantIssueReason reason) noexcept;
std::string_view FailureReasonName(FailureReason reason) noexcept;
std::string_view RouteName(Route route) noexcept;
std::string_view StageName(Stage stage) noexcept;
std::string_view ErrorLayerName(ErrorLayer layer) noexcept;
std::string_view ErrorCodeName(ErrorCode code) noexcept;
std::string_view MediaKindName(MediaKind kind) noexcept;
std::string_view SignalCategoryName(SignalCategory kind) noexcept;
std::string_view RtcStatusName(RtcStatus status) noexcept;
std::string_view MediaFallbackReasonName(MediaFallbackReason reason) noexcept;
std::string_view RenderBackendName(RenderBackend backend) noexcept;
std::string_view RenderReasonName(RenderReason reason) noexcept;
std::string_view DeviceSwitchReasonName(DeviceSwitchReason reason) noexcept;
Severity EventSeverity(EventKind kind) noexcept;
Severity EventSeverity(const Event& event) noexcept;
bool IsCritical(EventKind kind) noexcept;
bool IsValidEvent(const Event& event) noexcept;

} // namespace livekit::diagnostic
