#pragma once

#include "src/media/ringtone_player.h"
#include "src/net/call_types.h"
#include "src/net/http_types.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace OpenMeeting {

class SessionManager;
class UserEventStreamClient;

enum class CallControllerState {
    Idle = 0,
    OutgoingStarting = 1,
    OutgoingRinging = 2,
    Cancelling = 3,
    IncomingRinging = 4,
    Accepting = 5,
    HandingOffToMeeting = 6,
};

enum class CallEndReason {
    Dismissed,
    Busy,
    Rejected,
    Cancelled,
    Timeout,
};

struct CallJoinHandoff {
    CallSession session;
    LiveKitAuthInfo liveKit;
    std::unique_ptr<QObject> entryReservation;
    bool audioOnly = false;
};

// Coordinates 1v1 direct calls and in-meeting invitations with MeetingEntryGuard
// and active meeting state, ensuring zero orphan rooms or double-entry races.
class CallInviteController final : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultRingTimeoutMs = 45000;

    struct Backend {
        std::function<void(const StartCallRequest &, ResultCallback<CallSession>)> startCall;
        std::function<void(const QString &, ResultCallback<AcceptCallResponse>)> acceptCall;
        std::function<void(const QString &, const QString &, ResultCallback<QString>)> rejectCall;
        std::function<void(const QString &, const QString &, ResultCallback<QString>)> cancelCall;
        // Entry guard & meeting state arbitration hooks injected by MeetingMainWindow.
        std::function<std::unique_ptr<QObject>()> tryAcquireMeetingEntry;
        std::function<bool()> isLocalInMeeting;
        std::function<QString()> activeMeetingId;
        std::function<bool()> isActiveMeetingE2ee;
        std::function<void(CallJoinHandoff &&)> joinCallMeeting;
    };

    CallInviteController(SessionManager &session,
                         UserEventStreamClient &eventStream,
                         QObject *parent = nullptr);
    CallInviteController(SessionManager &session,
                         UserEventStreamClient &eventStream,
                         Backend backend,
                         bool enableAudioRingtone = true,
                         QObject *parent = nullptr);
    ~CallInviteController() override;

    void setEntryArbitrationHooks(
        std::function<std::unique_ptr<QObject>()> tryAcquireMeetingEntry,
        std::function<bool()> isLocalInMeeting,
        std::function<QString()> activeMeetingId,
        std::function<bool()> isActiveMeetingE2ee,
        std::function<void(CallJoinHandoff &&)> joinCallMeeting);

    CallControllerState state() const { return _state; }
    std::optional<CallSession> activeCall() const { return _activeCall; }
    int remainingSeconds() const;

    // Actions
    bool startDirectCall(const QString &calleeUserId, CallMediaType mediaType);
    bool inviteToActiveMeeting(const QString &calleeUserId, ResultCallback<CallSession> callback = {});
    bool acceptIncomingCall();
    bool rejectIncomingCall(const QString &reason = QStringLiteral("rejected"));
    bool cancelOutgoingCall();

signals:
    void stateChanged(OpenMeeting::CallControllerState state, const std::optional<OpenMeeting::CallSession> &session);
    void incomingCallRinging(const OpenMeeting::CallSession &session);
    void outgoingCallRinging(const OpenMeeting::CallSession &session);
    void callEnded(const QString &callId, OpenMeeting::CallEndReason reason);
    void silentMissedCallWhileInMeeting(const OpenMeeting::CallSession &session);
    void countdownTick(int remainingSeconds);
    void errorMessage(const QString &message);

private:
    using TerminalEvent = std::variant<std::monostate, CallEndReason, CallAcceptedEvent>;

    static Backend defaultBackend(SessionManager &session);
    void handleIncomingCallEvent(const CallSession &session);
    void handleCallAcceptedEvent(const CallAcceptedEvent &event);
    void handleCallRejectedEvent(const CallSession &session);
    void handleCallCancelledEvent(const CallSession &session);
    void handleCallTimeoutEvent(const CallSession &session);
    void rememberEarlyTerminalEvent(const CallSession &session, TerminalEvent event);
    void handleCallEndedEvent(const CallSession &session, CallEndReason reason);
    void handleCountdownTimer();
    void resetForAuthChange();
    void transitionToIdle(const QString &endedCallId, CallEndReason reason = CallEndReason::Dismissed);
    void startCountdown(qint64 expiresAtMs);
    void stopCountdown();

    SessionManager &_session;
    UserEventStreamClient &_eventStream;
    Backend _backend;
    RingtonePlayer _ringtone;
    bool _enableAudioRingtone = true;

    CallControllerState _state = CallControllerState::Idle;
    std::optional<CallSession> _activeCall;
    QHash<QString, TerminalEvent> _earlyTerminalEvents;
    std::unique_ptr<QObject> _entryReservation;
    quint64 _callGeneration = 0;
    qint64 _countdownDeadlineMs = 0;
    QTimer _countdownTimer;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::CallControllerState)
Q_DECLARE_METATYPE(OpenMeeting::CallEndReason)
