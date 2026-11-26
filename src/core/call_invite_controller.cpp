#include "src/core/call_invite_controller.h"

#include "src/net/openmeeting_http_client.h"
#include "src/net/session_manager.h"
#include "src/net/user_event_stream_client.h"
#include "src/telemetry/log_redaction.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace OpenMeeting {

CallInviteController::Backend CallInviteController::defaultBackend(SessionManager &session) {
    Backend b;
    b.startCall = [&session](const StartCallRequest &req, ResultCallback<CallSession> cb) {
        session.httpClient().startCall(req, std::move(cb));
    };
    b.acceptCall = [&session](const QString &callId, ResultCallback<AcceptCallResponse> cb) {
        session.httpClient().acceptCall(callId, std::move(cb));
    };
    b.rejectCall = [&session](const QString &callId, const QString &reason, ResultCallback<QString> cb) {
        session.httpClient().rejectCall(callId, reason, std::move(cb));
    };
    b.cancelCall = [&session](const QString &callId, const QString &reason, ResultCallback<QString> cb) {
        session.httpClient().cancelCall(callId, reason, std::move(cb));
    };
    return b;
}

CallInviteController::CallInviteController(
        SessionManager &session, UserEventStreamClient &eventStream, QObject *parent)
    : CallInviteController(session, eventStream, defaultBackend(session), true, parent) {
}

CallInviteController::CallInviteController(
        SessionManager &session, UserEventStreamClient &eventStream,
        Backend backend, bool enableAudioRingtone, QObject *parent)
    : QObject(parent)
    , _session(session)
    , _eventStream(eventStream)
    , _backend(std::move(backend))
    , _enableAudioRingtone(enableAudioRingtone) {
    qRegisterMetaType<CallControllerState>();
    qRegisterMetaType<CallEndReason>();
    _countdownTimer.setInterval(1000);
    connect(&_countdownTimer, &QTimer::timeout, this, &CallInviteController::handleCountdownTimer);

    connect(&_eventStream, &UserEventStreamClient::callIncoming, this, &CallInviteController::handleIncomingCallEvent);
    connect(&_eventStream, &UserEventStreamClient::callAccepted, this, &CallInviteController::handleCallAcceptedEvent);
    connect(&_eventStream, &UserEventStreamClient::callRejected, this, &CallInviteController::handleCallRejectedEvent);
    connect(&_eventStream, &UserEventStreamClient::callCancelled, this, &CallInviteController::handleCallCancelledEvent);
    connect(&_eventStream, &UserEventStreamClient::callTimeout, this, &CallInviteController::handleCallTimeoutEvent);

    connect(&_session, &SessionManager::authenticationReset, this, [this](quint64) { resetForAuthChange(); });
    connect(&_session, &SessionManager::loggedOut, this, [this]() { resetForAuthChange(); });
    connect(&_session, &SessionManager::sessionInvalidated, this, [this](SessionInvalidationReason) { resetForAuthChange(); });
}

CallInviteController::~CallInviteController() {
    _ringtone.stop();
    stopCountdown();
}

void CallInviteController::setEntryArbitrationHooks(
        std::function<std::unique_ptr<QObject>()> tryAcquireMeetingEntry,
        std::function<bool()> isLocalInMeeting,
        std::function<QString()> activeMeetingId,
        std::function<bool()> isActiveMeetingE2ee,
        std::function<void(CallJoinHandoff &&)> joinCallMeeting) {
    _backend.tryAcquireMeetingEntry = std::move(tryAcquireMeetingEntry);
    _backend.isLocalInMeeting = std::move(isLocalInMeeting);
    _backend.activeMeetingId = std::move(activeMeetingId);
    _backend.isActiveMeetingE2ee = std::move(isActiveMeetingE2ee);
    _backend.joinCallMeeting = std::move(joinCallMeeting);
}

int CallInviteController::remainingSeconds() const {
    if (_countdownDeadlineMs <= 0) return 0;
    const qint64 diff = _countdownDeadlineMs - QDateTime::currentMSecsSinceEpoch();
    return diff > 0 ? static_cast<int>((diff + 999) / 1000) : 0;
}

bool CallInviteController::startDirectCall(const QString &calleeUserId, CallMediaType mediaType) {
    const QString calleeId = calleeUserId.trimmed();
    if (!_session.isLoggedIn() || calleeId.isEmpty() || calleeId == _session.userId()) {
        return false;
    }
    if (_backend.isLocalInMeeting && _backend.isLocalInMeeting()) {
        // Redirect to in-meeting invitation if already in an active meeting.
        return inviteToActiveMeeting(calleeId);
    }
    if (_state != CallControllerState::Idle) {
        emit errorMessage(QCoreApplication::translate("MeetingUI", "Another call or meeting entry is already in progress."));
        return false;
    }
    if (_backend.tryAcquireMeetingEntry) {
        auto reservation = _backend.tryAcquireMeetingEntry();
        if (!reservation) {
            emit errorMessage(QCoreApplication::translate("MeetingUI", "A meeting entry is already in progress."));
            return false;
        }
        _entryReservation = std::move(reservation);
    }

    const quint64 authGen = _session.authGeneration();
    const quint64 callGen = ++_callGeneration;
    _state = CallControllerState::OutgoingStarting;
    CallSession placeholder;
    placeholder.mode = CallMode::Direct;
    placeholder.mediaType = mediaType;
    placeholder.callerUserId = _session.userId();
    placeholder.calleeUserId = calleeId;
    _activeCall = placeholder;
    const QPointer<CallInviteController> self(this);
    emit stateChanged(_state, _activeCall);
    if (!self || self->_callGeneration != callGen || self->_session.authGeneration() != authGen) return false;

    StartCallRequest req;
    req.calleeUserId = calleeId;
    req.mode = CallMode::Direct;
    req.mediaType = mediaType;

    _backend.startCall(req, [self, authGen, callGen](bool ok, const CallSession &sess, const HttpError &err) {
        if (!self || self->_callGeneration != callGen || self->_session.authGeneration() != authGen) return;
        if (!ok || sess.callId.isEmpty()) {
            QString msg = err.message;
            if (err.businessCode == kCallBusyCode || err.code == kCallBusyCode) {
                msg = QCoreApplication::translate("MeetingUI", "The contact is currently busy or in another meeting.");
            } else if (err.businessCode == kContactBlockedCode || err.code == kContactBlockedCode) {
                msg = QCoreApplication::translate("MeetingUI", "Unable to reach this user right now.");
            } else if (msg.isEmpty()) {
                msg = QCoreApplication::translate("MeetingUI", "Failed to start call.");
            }
            spdlog::warn("[CallInvite] startDirectCall failed {}",
                         livekit::secure_log::ErrorCodeSummary("call_start", err.code, "http"));
            self->transitionToIdle({});
            if (self) emit self->errorMessage(msg);
            return;
        }
        self->_activeCall = sess;
        self->_state = CallControllerState::OutgoingRinging;
        // The event stream and the start response can arrive in either order.
        auto early = self->_earlyTerminalEvents.take(sess.callId);
        self->_earlyTerminalEvents.clear();
        if (const auto *accepted = std::get_if<CallAcceptedEvent>(&early)) {
            self->handleCallAcceptedEvent(*accepted);
            return;
        }
        if (const auto *reason = std::get_if<CallEndReason>(&early)) {
            self->transitionToIdle(sess.callId, *reason);
            return;
        }
        self->startCountdown(sess.expiresAtMs > 0 ? sess.expiresAtMs : QDateTime::currentMSecsSinceEpoch() + kDefaultRingTimeoutMs);
        if (!self || self->_callGeneration != callGen || self->_state != CallControllerState::OutgoingRinging) return;
        emit self->stateChanged(self->_state, self->_activeCall);
        if (self && self->_callGeneration == callGen && self->_state == CallControllerState::OutgoingRinging) {
            emit self->outgoingCallRinging(sess);
        }
    });
    return true;
}

bool CallInviteController::inviteToActiveMeeting(const QString &calleeUserId, ResultCallback<CallSession> callback) {
    if (!_session.isLoggedIn() || calleeUserId.trimmed().isEmpty()) return false;
    const QString meetingId = _backend.activeMeetingId ? _backend.activeMeetingId() : QString();
    if (meetingId.isEmpty()) {
        if (callback) {
            HttpError err;
            err.message = QCoreApplication::translate("MeetingUI", "No active meeting to invite into.");
            callback(false, CallSession{}, err);
        }
        return false;
    }
    StartCallRequest req;
    req.calleeUserId = calleeUserId;
    req.mode = CallMode::MeetingInvite;
    req.mediaType = CallMediaType::Video;
    req.meetingId = meetingId;
    req.e2eeRequired = _backend.isActiveMeetingE2ee && _backend.isActiveMeetingE2ee();

    const quint64 authGen = _session.authGeneration();
    const QPointer<CallInviteController> self(this);
    _backend.startCall(req, [self, authGen, cb = std::move(callback)](bool ok, const CallSession &sess, const HttpError &err) {
        if (!self || self->_session.authGeneration() != authGen) return;
        if (cb) cb(ok, sess, err);
    });
    return true;
}

bool CallInviteController::acceptIncomingCall() {
    if (_state != CallControllerState::IncomingRinging || !_activeCall) return false;
    // Always stop ringtone synchronously BEFORE acquiring audio hardware for the meeting.
    _ringtone.stop();
    stopCountdown();

    if (_backend.tryAcquireMeetingEntry && !_entryReservation) {
        auto reservation = _backend.tryAcquireMeetingEntry();
        if (!reservation) {
            const QString id = _activeCall->callId;
            if (_backend.rejectCall) _backend.rejectCall(id, QStringLiteral("busy"), {});
            transitionToIdle(id);
            return false;
        }
        _entryReservation = std::move(reservation);
    }

    const QString callId = _activeCall->callId;
    const quint64 authGen = _session.authGeneration();
    const quint64 callGen = _callGeneration;
    _state = CallControllerState::Accepting;
    emit stateChanged(_state, _activeCall);

    const QPointer<CallInviteController> self(this);
    _backend.acceptCall(callId, [self, authGen, callGen, callId](bool ok, const AcceptCallResponse &resp, const HttpError &err) {
        if (!self || self->_callGeneration != callGen || self->_session.authGeneration() != authGen) return;
        if (!ok || resp.liveKit.url.isEmpty() || resp.liveKit.token.isEmpty()) {
            const QString msg = (err.businessCode == kCallAlreadyEndedCode || err.code == kCallAlreadyEndedCode)
                ? QCoreApplication::translate("MeetingUI", "The caller cancelled or the call timed out.")
                : (err.message.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unable to accept call.") : err.message);
            self->transitionToIdle(callId);
            if (self) emit self->errorMessage(msg);
            return;
        }
        self->_state = CallControllerState::HandingOffToMeeting;
        CallJoinHandoff handoff;
        handoff.session = resp.session;
        handoff.liveKit = resp.liveKit;
        handoff.entryReservation = std::move(self->_entryReservation);
        handoff.audioOnly = (resp.session.mediaType == CallMediaType::Audio);
        self->_activeCall.reset();
        self->_state = CallControllerState::Idle;
        emit self->stateChanged(self->_state, std::nullopt);
        if (self->_backend.joinCallMeeting) {
            self->_backend.joinCallMeeting(std::move(handoff));
        }
    });
    return true;
}

bool CallInviteController::rejectIncomingCall(const QString &reason) {
    if (_state != CallControllerState::IncomingRinging || !_activeCall) return false;
    const QString callId = _activeCall->callId;
    _ringtone.stop();
    stopCountdown();
    if (_backend.rejectCall) {
        _backend.rejectCall(callId, reason, {});
    }
    transitionToIdle(callId);
    return true;
}

bool CallInviteController::cancelOutgoingCall() {
    if ((_state != CallControllerState::OutgoingRinging && _state != CallControllerState::OutgoingStarting) || !_activeCall) {
        return false;
    }
    const QString callId = _activeCall->callId;
    stopCountdown();
    _state = CallControllerState::Cancelling;
    emit stateChanged(_state, _activeCall);
    if (!callId.isEmpty() && _backend.cancelCall) {
        _backend.cancelCall(callId, QStringLiteral("cancelled"), {});
    }
    transitionToIdle(callId);
    return true;
}

void CallInviteController::handleIncomingCallEvent(const CallSession &session) {
    if (!_session.isLoggedIn() || session.callId.isEmpty()) return;
    if (session.calleeUserId != _session.userId()) return;

    // Already in a meeting: ignore duplicate invites to the same meeting; otherwise auto-busy + silent banner.
    if (_backend.isLocalInMeeting && _backend.isLocalInMeeting()) {
        const QString curMeeting = _backend.activeMeetingId ? _backend.activeMeetingId() : QString();
        if (!curMeeting.isEmpty() && curMeeting == session.meetingId) {
            return;
        }
        if (_backend.rejectCall) {
            _backend.rejectCall(session.callId, QStringLiteral("busy"), {});
        }
        emit silentMissedCallWhileInMeeting(session);
        return;
    }

    // Already busy with another call or entry guard.
    if (_state != CallControllerState::Idle) {
        if (_activeCall && _activeCall->callId == session.callId) return;
        if (_backend.rejectCall) {
            _backend.rejectCall(session.callId, QStringLiteral("busy"), {});
        }
        return;
    }

    ++_callGeneration;
    _activeCall = session;
    _state = CallControllerState::IncomingRinging;
    startCountdown(session.expiresAtMs > 0 ? session.expiresAtMs : QDateTime::currentMSecsSinceEpoch() + kDefaultRingTimeoutMs);
    if (_enableAudioRingtone) {
        const auto &prefs = _session.mediaPreferences();
        _ringtone.start(prefs.speakerDeviceId, prefs.enableSpeaker);
    }
    emit stateChanged(_state, _activeCall);
    emit incomingCallRinging(session);
}

void CallInviteController::handleCallAcceptedEvent(const CallAcceptedEvent &event) {
    if (_state == CallControllerState::OutgoingStarting) {
        rememberEarlyTerminalEvent(event.session, event);
        return;
    }
    if (_state != CallControllerState::OutgoingRinging || !_activeCall) return;
    if (event.session.callId != _activeCall->callId) return;
    stopCountdown();

    if (!event.liveKit || event.liveKit->url.isEmpty() || event.liveKit->token.isEmpty()) {
        const QPointer<CallInviteController> self(this);
        transitionToIdle(event.session.callId);
        if (self) emit self->errorMessage(QCoreApplication::translate("MeetingUI", "Call accepted but credentials missing."));
        return;
    }

    _state = CallControllerState::HandingOffToMeeting;
    CallJoinHandoff handoff;
    handoff.session = event.session;
    handoff.liveKit = *event.liveKit;
    handoff.entryReservation = std::move(_entryReservation);
    handoff.audioOnly = (event.session.mediaType == CallMediaType::Audio);
    _activeCall.reset();
    _state = CallControllerState::Idle;
    const QPointer<CallInviteController> self(this);
    const quint64 generation = _callGeneration;
    emit stateChanged(_state, std::nullopt);
    if (self && self->_callGeneration == generation && self->_backend.joinCallMeeting) {
        self->_backend.joinCallMeeting(std::move(handoff));
    }
}

void CallInviteController::handleCallRejectedEvent(const CallSession &session) {
    handleCallEndedEvent(session, session.state == CallSessionState::Busy ? CallEndReason::Busy : CallEndReason::Rejected);
}

void CallInviteController::handleCallCancelledEvent(const CallSession &session) {
    handleCallEndedEvent(session, CallEndReason::Cancelled);
}

void CallInviteController::handleCallTimeoutEvent(const CallSession &session) {
    handleCallEndedEvent(session, CallEndReason::Timeout);
}

void CallInviteController::rememberEarlyTerminalEvent(const CallSession &session, TerminalEvent event) {
    if (!_activeCall || session.callId.isEmpty() || session.mode != CallMode::Direct ||
        session.callerUserId != _activeCall->callerUserId || session.calleeUserId != _activeCall->calleeUserId) return;
    constexpr qsizetype kMaxEarlyTerminalEvents = 16;
    if (_earlyTerminalEvents.size() < kMaxEarlyTerminalEvents && !_earlyTerminalEvents.contains(session.callId)) {
        _earlyTerminalEvents.insert(session.callId, std::move(event));
    }
}

void CallInviteController::handleCallEndedEvent(const CallSession &session, CallEndReason reason) {
    if (_state == CallControllerState::OutgoingStarting) {
        rememberEarlyTerminalEvent(session, reason);
        return;
    }
    if (session.callId.isEmpty() || !_activeCall || _activeCall->callId != session.callId) return;
    transitionToIdle(session.callId, _state == CallControllerState::OutgoingRinging ? reason : CallEndReason::Dismissed);
}

void CallInviteController::handleCountdownTimer() {
    const int rem = remainingSeconds();
    emit countdownTick(rem);
    if (rem <= 0 && _activeCall) {
        const QString callId = _activeCall->callId;
        const bool outgoing = _state == CallControllerState::OutgoingRinging;
        if (outgoing && _backend.cancelCall) {
            _backend.cancelCall(callId, QStringLiteral("timeout"), {});
        } else if (_state == CallControllerState::IncomingRinging && _backend.rejectCall) {
            _backend.rejectCall(callId, QStringLiteral("timeout"), {});
        }
        transitionToIdle(callId, outgoing ? CallEndReason::Timeout : CallEndReason::Dismissed);
    }
}

void CallInviteController::resetForAuthChange() {
    ++_callGeneration;
    _earlyTerminalEvents.clear();
    _ringtone.stop();
    stopCountdown();
    _entryReservation.reset();
    const QString oldId = _activeCall ? _activeCall->callId : QString();
    _activeCall.reset();
    if (_state != CallControllerState::Idle) {
        _state = CallControllerState::Idle;
        emit stateChanged(_state, std::nullopt);
        emit callEnded(oldId, CallEndReason::Dismissed);
    }
}

void CallInviteController::transitionToIdle(const QString &endedCallId, CallEndReason reason) {
    ++_callGeneration;
    _earlyTerminalEvents.clear();
    _ringtone.stop();
    stopCountdown();
    _entryReservation.reset();
    _activeCall.reset();
    _state = CallControllerState::Idle;
    emit stateChanged(_state, std::nullopt);
    emit callEnded(endedCallId, reason);
}

void CallInviteController::startCountdown(qint64 expiresAtMs) {
    _countdownDeadlineMs = expiresAtMs;
    _countdownTimer.start();
    emit countdownTick(remainingSeconds());
}

void CallInviteController::stopCountdown() {
    _countdownTimer.stop();
    _countdownDeadlineMs = 0;
}

} // namespace OpenMeeting
