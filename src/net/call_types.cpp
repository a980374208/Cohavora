#include "src/net/call_types.h"

namespace OpenMeeting {
namespace {

qint64 readCallInt64(const QJsonObject &obj, const QString &key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return 0;
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : 0;
    }
    return static_cast<qint64>(v.toDouble(0.0));
}

LiveKitAuthInfo parseLiveKitCredentials(const QJsonObject &obj, const QString &fallbackMeetingId) {
    LiveKitAuthInfo info;
    info.url = obj.value(QStringLiteral("url")).toString();
    info.token = obj.value(QStringLiteral("token")).toString();
    info.meetingId = obj.value(QStringLiteral("meetingID")).toString();
    if (info.meetingId.isEmpty()) info.meetingId = fallbackMeetingId;
    return info;
}

} // namespace

QString callModeToWire(CallMode mode) {
    return mode == CallMode::MeetingInvite
        ? QStringLiteral("meeting_invite")
        : QStringLiteral("direct");
}

CallMode callModeFromWire(const QString &wire) {
    return wire.trimmed().toLower() == QStringLiteral("meeting_invite")
        ? CallMode::MeetingInvite
        : CallMode::Direct;
}

QString callMediaTypeToWire(CallMediaType type) {
    return type == CallMediaType::Audio
        ? QStringLiteral("audio")
        : QStringLiteral("video");
}

CallMediaType callMediaTypeFromWire(const QString &wire) {
    return wire.trimmed().toLower() == QStringLiteral("audio")
        ? CallMediaType::Audio
        : CallMediaType::Video;
}

CallSessionState callSessionStateFromWire(const QString &wire) {
    const QString s = wire.trimmed().toLower();
    if (s == QStringLiteral("accepted")) return CallSessionState::Accepted;
    if (s == QStringLiteral("rejected")) return CallSessionState::Rejected;
    if (s == QStringLiteral("busy")) return CallSessionState::Busy;
    if (s == QStringLiteral("cancelled")) return CallSessionState::Cancelled;
    if (s == QStringLiteral("timeout")) return CallSessionState::Timeout;
    return CallSessionState::Ringing;
}

CallSession parseCallSession(const QJsonObject &obj) {
    CallSession s;
    s.callId = obj.value(QStringLiteral("callID")).toString();
    s.mode = callModeFromWire(obj.value(QStringLiteral("mode")).toString());
    s.mediaType = callMediaTypeFromWire(obj.value(QStringLiteral("mediaType")).toString());
    s.callerUserId = obj.value(QStringLiteral("callerUserID")).toString();
    s.callerNickname = obj.value(QStringLiteral("callerNickname")).toString();
    s.calleeUserId = obj.value(QStringLiteral("calleeUserID")).toString();
    s.calleeNickname = obj.value(QStringLiteral("calleeNickname")).toString();
    s.meetingId = obj.value(QStringLiteral("meetingID")).toString();
    s.meetingTitle = obj.value(QStringLiteral("meetingTitle")).toString();
    s.participantCount = obj.value(QStringLiteral("participantCount")).toInt(0);
    s.e2eeRequired = obj.value(QStringLiteral("e2eeRequired")).toBool(false);
    s.state = callSessionStateFromWire(obj.value(QStringLiteral("state")).toString());
    s.createdAtMs = readCallInt64(obj, QStringLiteral("createdAtMs"));
    s.expiresAtMs = readCallInt64(obj, QStringLiteral("expiresAtMs"));
    return s;
}

AcceptCallResponse parseAcceptCallResponse(const QJsonObject &obj) {
    AcceptCallResponse resp;
    if (obj.value(QStringLiteral("session")).isObject()) {
        resp.session = parseCallSession(obj.value(QStringLiteral("session")).toObject());
    }
    if (obj.value(QStringLiteral("liveKit")).isObject()) {
        resp.liveKit = parseLiveKitCredentials(obj.value(QStringLiteral("liveKit")).toObject(), resp.session.meetingId);
    }
    return resp;
}

CallAcceptedEvent parseCallAcceptedEvent(const QJsonObject &obj) {
    CallAcceptedEvent evt;
    if (obj.value(QStringLiteral("session")).isObject()) {
        evt.session = parseCallSession(obj.value(QStringLiteral("session")).toObject());
    } else if (obj.contains(QStringLiteral("callID"))) {
        evt.session = parseCallSession(obj);
    }
    if (obj.value(QStringLiteral("liveKit")).isObject()) {
        evt.liveKit = parseLiveKitCredentials(obj.value(QStringLiteral("liveKit")).toObject(), evt.session.meetingId);
    }
    return evt;
}

} // namespace OpenMeeting
