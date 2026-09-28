#pragma once

#include "diagnostic_pipeline.h"
#include <chrono>
#include <atomic>
#include <mutex>

namespace livekit::diagnostic {

// Value-only correlation shared by the Room, signaling send and RTC callbacks.
// Never owns a Room/PeerConnection and never changes negotiation decisions.
class SdpNegotiationTrace final {
public:
    SdpNegotiationTrace(Context context, SdpRole role, bool ice_restart = false)
        : context_(context), role_(role), ice_restart_(ice_restart) {
        Record(SdpAction::Round, SdpPhase::Started);
    }
    ~SdpNegotiationTrace() { Finish(Outcome::Cancelled, SdpReason::Stopped); }

    void Record(SdpAction action, SdpPhase phase,
                SdpState before = SdpState::Unknown,
                SdpState after = SdpState::Unknown,
                SdpReason reason = SdpReason::None, int rtc_error = 0,
                ThreadRole thread = ThreadRole::Session,
                SdpDescription description = SdpDescription::Unknown) noexcept {
        std::lock_guard lock(mutex_);
        if (initial_state_ == SdpState::Unknown && before != SdpState::Unknown) initial_state_ = before;
        if (after != SdpState::Unknown) last_state_ = after;
        EmitLocked(action, phase, before, after, reason, rtc_error, thread, description);
    }

    void Finish(Outcome outcome, SdpReason reason = SdpReason::None) noexcept {
        std::lock_guard lock(mutex_);
        if (terminal_) return;
        outcome_ = outcome;
        EmitLocked(SdpAction::Round, outcome == Outcome::Success ? SdpPhase::Completed
            : outcome == Outcome::Cancelled ? SdpPhase::Cancelled : SdpPhase::Failed,
            initial_state_, last_state_, reason, 0, ThreadRole::Unknown, SdpDescription::Unknown);
        terminal_ = true;
    }

private:
    void EmitLocked(SdpAction action, SdpPhase phase, SdpState before,
                    SdpState after, SdpReason reason, int rtc_error, ThreadRole thread, SdpDescription description) noexcept {
        Event event;
        event.kind = EventKind::RtcSdpStep;
        event.context = context_;
        event.thread_role = thread;
        event.sdp_role = role_;
        event.sdp_action = action;
        event.sdp_description = description;
        if (action == SdpAction::CreateOffer || action == SdpAction::SendOffer || action == SdpAction::ReceiveOffer)
            event.sdp_description = SdpDescription::Offer;
        if (action == SdpAction::CreateAnswer || action == SdpAction::SendAnswer || action == SdpAction::ReceiveAnswer)
            event.sdp_description = SdpDescription::Answer;
        event.sdp_phase = phase;
        event.sdp_sequence = ++sequence_;
        event.signaling_before = before;
        event.signaling_after = after;
        event.sdp_reason = reason;
        event.sdp_after_terminal = terminal_;
        event.sdp_ice_restart = ice_restart_;
        event.rtc_error_type = rtc_error;
        if (action == SdpAction::CreateOffer) event.stage = Stage::CreateOffer;
        if (action == SdpAction::CreateAnswer) event.stage = Stage::CreateAnswer;
        if (action == SdpAction::SetLocal) event.stage = Stage::SetLocalDescription;
        if (action == SdpAction::SetRemote) event.stage = Stage::SetRemoteDescription;
        if (phase == SdpPhase::Completed) event.outcome = Outcome::Success;
        if (phase == SdpPhase::Failed) event.outcome = Outcome::Failure;
        if (phase == SdpPhase::Cancelled || phase == SdpPhase::Rejected)
            event.outcome = Outcome::Cancelled;
        if (action == SdpAction::Round && phase != SdpPhase::Started) {
            event.outcome = outcome_;
            event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_).count();
        }
        if (reason == SdpReason::ParseError) event.error_layer = ErrorLayer::Parse;
        if (reason == SdpReason::RtcError) event.error_layer = ErrorLayer::Rtc;
        if (reason == SdpReason::SendError) event.error_layer = ErrorLayer::Network;
        EmitBusinessEvent(event);
    }
    const Context context_;
    const SdpRole role_;
    const bool ice_restart_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::mutex mutex_;
    std::uint64_t sequence_ = 0;
    SdpState initial_state_ = SdpState::Unknown;
    SdpState last_state_ = SdpState::Unknown;
    bool terminal_ = false;
    Outcome outcome_ = Outcome::Unknown;
};

// Travels with one queued WebSocket frame. Completion means the socket write
// finished, not that the remote endpoint acknowledged or applied the SDP.
class SdpSendTrace final {
public:
    SdpSendTrace(std::shared_ptr<SdpNegotiationTrace> round, bool answer)
        : round_(std::move(round)), answer_(answer) {
        round_->Record(Action(), SdpPhase::Started);
    }
    ~SdpSendTrace() { End(SdpPhase::Cancelled, SdpReason::Stopped); }
    void Complete() noexcept { End(SdpPhase::Completed, SdpReason::None); }
    void Fail() noexcept { End(SdpPhase::Failed, SdpReason::SendError); }
private:
    SdpAction Action() const { return answer_ ? SdpAction::SendAnswer : SdpAction::SendOffer; }
    void End(SdpPhase phase, SdpReason reason) noexcept {
        if (ended_.exchange(true)) return;
        round_->Record(Action(), phase, SdpState::Unknown, SdpState::Unknown, reason);
        if (phase == SdpPhase::Failed) round_->Finish(Outcome::Failure, reason);
        else if (phase == SdpPhase::Cancelled) round_->Finish(Outcome::Cancelled, reason);
        else if (answer_) round_->Finish(Outcome::Success);
    }
    const std::shared_ptr<SdpNegotiationTrace> round_;
    const bool answer_;
    std::atomic<bool> ended_{false};
};

} // namespace livekit::diagnostic
