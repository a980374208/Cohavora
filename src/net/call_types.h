#pragma once

#include "src/net/http_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QString>

#include <optional>

namespace OpenMeeting {

constexpr int kCallBusyCode = 310001;
constexpr int kCallNotFoundCode = 310002;
constexpr int kCallAlreadyEndedCode = 310003;
constexpr int kCallPermissionDeniedCode = 310004;

enum class CallMode {
    Direct = 0,
    MeetingInvite = 1,
};

enum class CallMediaType {
    Audio = 0,
    Video = 1,
};

enum class CallSessionState {
    Ringing = 0,
    Accepted = 1,
    Rejected = 2,
    Busy = 3,
    Cancelled = 4,
    Timeout = 5,
};

struct CallSession {
    QString callId;
    CallMode mode = CallMode::Direct;
    CallMediaType mediaType = CallMediaType::Video;
    QString callerUserId;
    QString callerNickname;
    QString calleeUserId;
    QString calleeNickname;
    QString meetingId;
    QString meetingTitle;
    int participantCount = 0;
    bool e2eeRequired = false;
    CallSessionState state = CallSessionState::Ringing;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
};

struct StartCallRequest {
    QString calleeUserId;
    CallMode mode = CallMode::Direct;
    CallMediaType mediaType = CallMediaType::Video;
    QString meetingId;
    bool e2eeRequired = false;
};

struct AcceptCallResponse {
    CallSession session;
    LiveKitAuthInfo liveKit;
};

struct CallAcceptedEvent {
    CallSession session;
    std::optional<LiveKitAuthInfo> liveKit;
};

QString callModeToWire(CallMode mode);
CallMode callModeFromWire(const QString &wire);
QString callMediaTypeToWire(CallMediaType type);
CallMediaType callMediaTypeFromWire(const QString &wire);
CallSessionState callSessionStateFromWire(const QString &wire);

CallSession parseCallSession(const QJsonObject &obj);
AcceptCallResponse parseAcceptCallResponse(const QJsonObject &obj);
CallAcceptedEvent parseCallAcceptedEvent(const QJsonObject &obj);

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::CallSession)
Q_DECLARE_METATYPE(OpenMeeting::AcceptCallResponse)
Q_DECLARE_METATYPE(OpenMeeting::CallAcceptedEvent)
