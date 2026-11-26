#pragma once

#include "e2ee/meeting_encryption.h"
#include "room.h"
#include "session_telemetry.h"
#include "telemetry/diagnostic_pipeline.h"
#include "rtc/video_format_diagnostics.h"
#include "rtc/h264_level_constraints.h"
#include "remote_track_publication.h"
#include "log_redaction.h"
#include "api/video_codecs/sdp_video_format.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <strings.h>
#endif

namespace livekit {

class SubscriptionTelemetryOperation final {
public:
    SubscriptionTelemetryOperation(
        std::shared_ptr<telemetry::SessionTelemetry> telemetry,
        telemetry::OperationKind kind)
        : telemetry_(std::move(telemetry)), kind_(kind) {
        if (telemetry_) {
            id_ = telemetry_->StartOperation(kind_);
        }
    }

    ~SubscriptionTelemetryOperation() {
        Finish(telemetry::OperationOutcome::Cancelled);
    }

    void Finish(telemetry::OperationOutcome outcome) {
        bool expected = false;
        if (!terminal_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (telemetry_ && !id_.empty()) {
            telemetry_->FinishOperation(id_, kind_, outcome);
        }
    }

private:
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_;
    telemetry::OperationKind kind_ = telemetry::OperationKind::Unknown;
    std::string id_;
    std::atomic<bool> terminal_{false};
};

inline int CompareCodecName(const char* left, const char* right) {
#if defined(_WIN32)
    return _stricmp(left, right);
#else
    return strcasecmp(left, right);
#endif
}

inline telemetry::OperationOutcome TelemetryOutcome(std::exception_ptr error) {
    if (!error) return telemetry::OperationOutcome::Failure;
    try {
        std::rethrow_exception(std::move(error));
    } catch (const OperationError& operation_error) {
        switch (operation_error.code()) {
        case OperationErrorCode::Cancelled:
        case OperationErrorCode::SessionInvalid:
            return telemetry::OperationOutcome::Cancelled;
        case OperationErrorCode::JoinTimeout:
        case OperationErrorCode::PeerConnectionTimeout:
        case OperationErrorCode::TrackPublishTimeout:
        case OperationErrorCode::TrackUnpublishTimeout:
            return telemetry::OperationOutcome::Timeout;
        default:
            return telemetry::OperationOutcome::Failure;
        }
    } catch (...) {
        return telemetry::OperationOutcome::Failure;
    }
}

inline diagnostic::Outcome DiagnosticOutcome(telemetry::OperationOutcome outcome) noexcept {
    using Source = telemetry::OperationOutcome;
    using Target = diagnostic::Outcome;
    switch (outcome) {
    case Source::Success: return Target::Success;
    case Source::DegradedSuccess: return Target::DegradedSuccess;
    case Source::Timeout: return Target::Timeout;
    case Source::Cancelled: return Target::Cancelled;
    default: return Target::Failure;
    }
}

inline void ProjectOperationError(std::exception_ptr error,
                                  diagnostic::Event& event) noexcept {
    event.error_layer = diagnostic::ErrorLayer::Native;
    event.error_code = diagnostic::ErrorCode::Unknown;
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const OperationError& native) {
        using N = OperationErrorCode;
        using D = diagnostic::ErrorCode;
        switch (native.code()) {
        case N::InvalidState: event.error_code = D::InvalidState; break;
        case N::CodecUnsupported: event.error_code = D::CodecUnsupported; break;
        case N::Cancelled: event.error_code = D::Cancelled; break;
        case N::SignalConnectFailed: event.error_code = D::SignalConnectFailed; break;
        case N::JoinTimeout: event.error_code = D::JoinTimeout; break;
        case N::JoinRejected: event.error_code = D::JoinRejected; break;
        case N::PeerConnectionCreateFailed: event.error_code = D::PeerConnectionCreateFailed; break;
        case N::NegotiationFailed: event.error_code = D::NegotiationFailed; break;
        case N::PeerConnectionTimeout: event.error_code = D::PeerConnectionTimeout; break;
        case N::PermissionDenied: event.error_code = D::PermissionDenied; break;
        case N::TrackPublishTimeout: event.error_code = D::TrackPublishTimeout; break;
        case N::TrackPublishRejected: event.error_code = D::TrackPublishRejected; break;
        case N::TrackUnpublishTimeout: event.error_code = D::TrackUnpublishTimeout; break;
        case N::ReconnectExhausted: event.error_code = D::ReconnectExhausted; break;
        case N::SessionClosed: event.error_code = D::SessionClosed; break;
        case N::StateUncertain: event.error_code = D::StateUncertain; break;
        case N::SessionInvalid: event.error_code = D::SessionInvalid; break;
        default: break;
        }
        event.retryable = native.retryable();
    } catch (...) {}
}

class TelemetryOperationSpan final {
public:
    TelemetryOperationSpan() = default;
    TelemetryOperationSpan(
        std::shared_ptr<telemetry::SessionTelemetry> telemetry,
        telemetry::OperationKind kind,
        std::string prefix = {})
        : telemetry_(std::move(telemetry)), kind_(kind) {
        if (telemetry_) {
            id_ = telemetry_->StartOperation(kind_, std::move(prefix));
        }
    }
    TelemetryOperationSpan(
        std::shared_ptr<telemetry::SessionTelemetry> telemetry,
        telemetry::OperationKind kind,
        std::string operation_id,
        bool adopt_existing)
        : telemetry_(std::move(telemetry)),
          id_(adopt_existing ? std::move(operation_id) : std::string{}),
          kind_(kind) {
        if (telemetry_ && id_.empty() && !adopt_existing) {
            id_ = telemetry_->StartOperation(kind_);
        }
    }

    ~TelemetryOperationSpan() { Finish(telemetry::OperationOutcome::Cancelled); }
    TelemetryOperationSpan(const TelemetryOperationSpan&) = delete;
    TelemetryOperationSpan& operator=(const TelemetryOperationSpan&) = delete;
    TelemetryOperationSpan(TelemetryOperationSpan&& other) noexcept
        : telemetry_(std::move(other.telemetry_)),
          id_(std::move(other.id_)), kind_(other.kind_),
          on_terminal_(std::move(other.on_terminal_)), finished_(other.finished_) {
        other.finished_ = true;
    }
    TelemetryOperationSpan& operator=(TelemetryOperationSpan&& other) noexcept {
        if (this == &other) return *this;
        Finish(telemetry::OperationOutcome::Cancelled);
        telemetry_ = std::move(other.telemetry_);
        id_ = std::move(other.id_);
        kind_ = other.kind_;
        on_terminal_ = std::move(other.on_terminal_);
        finished_ = other.finished_;
        other.finished_ = true;
        return *this;
    }

    void Finish(telemetry::OperationOutcome outcome) {
        if (finished_) return;
        finished_ = true;
        if (telemetry_ && !id_.empty()) {
            telemetry_->FinishOperation(id_, kind_, outcome);
        }
        try {
            if (on_terminal_) on_terminal_(outcome);
        } catch (...) {}
    }

    const std::string& id() const noexcept { return id_; }
    void OnTerminal(std::function<void(telemetry::OperationOutcome)> callback) {
        on_terminal_ = std::move(callback);
    }

private:
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_;
    std::string id_;
    telemetry::OperationKind kind_ = telemetry::OperationKind::Unknown;
    std::function<void(telemetry::OperationOutcome)> on_terminal_;
    bool finished_ = false;
};

inline std::optional<diagnostic::VideoFormatDiagnosticRow> VideoFormatRow(
    std::string_view name, const webrtc::CodecParameterMap& parameters,
    diagnostic::VideoFormatSource source, int payload_type = -1,
    std::size_t media_section_index = 65535) {
    const auto projection = detail::ProjectH264Format(name, parameters);
    if (!projection) return std::nullopt;
    diagnostic::VideoFormatDiagnosticRow row;
    row.source = source;
    row.media_section_index = static_cast<std::uint16_t>(std::min<std::size_t>(media_section_index, 65535));
    row.payload_type = payload_type >= 0 && payload_type <= 127 ? static_cast<std::int16_t>(payload_type) : -1;
    row.profile_level_id = projection->profile_level_id;
    row.profile = projection->profile;
    row.level = projection->level;
    row.packetization_mode = projection->packetization_mode;
    row.level_asymmetry = projection->level_asymmetry;
    row.valid = projection->valid;
    row.profile_level_defaulted = projection->profile_level_defaulted;
    return row;
}

inline void LogSdpNegotiationDetails(Room& room,
                                     const char* tag,
                                     std::string_view kind,
                                     std::string_view sdp) {
    for (const auto& detail : secure_log::SdpNegotiationDetails(kind, sdp)) {
        room.Log("SIGNAL", tag, detail);
    }
}

inline std::string RemoteVideoTelemetryKey(
    std::string_view participant_sid,
    std::string_view track_sid) {
    return "remote_video/" + std::string(participant_sid) + "/" +
        std::string(track_sid);
}

inline std::string RemoteAudioTelemetryKey(
    std::string_view participant_sid,
    std::string_view track_sid) {
    return "remote_audio/" + std::string(participant_sid) + "/" +
        std::string(track_sid);
}

inline std::string RemoteVideoRenderTelemetryKey(
    std::string_view participant_sid,
    std::string_view track_sid) {
    return "remote_render/" + std::string(participant_sid) + "/" +
        std::string(track_sid);
}

inline std::string RemoteMediaRecoveryIdentity(
    const Participant& participant,
    const TrackPublication& publication) {
    const auto identity = participant.identity();
    const auto target = publication.SnapshotState();
    if (identity.empty() || target.name.empty() ||
        target.kind == TrackKind::Unknown || target.source == TrackSource::Unknown) {
        return {};
    }
    // Use the whole advertised roster, not the subset whose sinks have arrived.
    // Multiple same-name/source publications cannot be paired across new SIDs.
    std::size_t matches = 0;
    for (const auto& [_, candidate] : participant.tracks()) {
        if (!candidate) continue;
        const auto snapshot = candidate->SnapshotState();
        if (snapshot.kind == target.kind && snapshot.source == target.source &&
            snapshot.name == target.name) ++matches;
    }
    if (matches != 1) return {};
    // Internal identity only. Length prefixes keep arbitrary names unambiguous.
    return std::to_string(identity.size()) + ":" + identity + ":" +
        std::to_string(static_cast<int>(target.kind)) + ":" +
        std::to_string(static_cast<int>(target.source)) + ":" +
        std::to_string(target.name.size()) + ":" + target.name;
}

inline bool IsRemotePublicationSubscribed(
    const std::shared_ptr<TrackPublication>& publication) {
    const auto remote =
        std::dynamic_pointer_cast<RemoteTrackPublication>(publication);
    return !remote || remote->is_subscribed();
}

inline bool IsRemoteMediaExpected(const TrackPublication::StateSnapshot& snapshot,
                                  bool subscribed = true) {
    if (!subscribed || !snapshot.track || snapshot.muted ||
        snapshot.stream_state != TrackPublication::StreamState::Active ||
        !snapshot.subscription_allowed ||
        snapshot.subscription_error != TrackPublication::SubscriptionError::None) {
        return false;
    }
    return snapshot.kind != TrackKind::Audio ||
        (!snapshot.track->playout_muted() && snapshot.track->volume() > 0.001);
}

inline void SetRemoteMediaExpected(
    const std::shared_ptr<telemetry::SessionTelemetry>& telemetry_owner,
    TrackKind kind,
    std::string_view participant_sid,
    std::string_view track_sid,
    bool expected,
    telemetry::MediaExpectationReason reason) {
    if (!telemetry_owner) return;
    if (kind == TrackKind::Video) {
        telemetry_owner->SetRemoteVideoExpected(
            RemoteVideoTelemetryKey(participant_sid, track_sid), expected, reason);
    } else if (kind == TrackKind::Audio) {
        telemetry_owner->SetRemoteAudioExpected(
            RemoteAudioTelemetryKey(participant_sid, track_sid), expected, reason);
    }
}

} // namespace livekit
