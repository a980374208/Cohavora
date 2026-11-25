#pragma once

#include "src/net/call_types.h"
#include "src/net/contact_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace OpenMeeting {

class SessionManager;

enum class EventStreamState {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Reconnecting = 3,
};

struct EventStreamHello {
    qint64 serverTimeMs = 0;
    qint64 latestSeq = 0;
    bool syncResetRequired = false;
    std::vector<QJsonObject> missedEvents;
    std::optional<CallSession> activeIncomingCall;
    std::optional<UserPresence> selfPresence;
};

struct ParsedEventFrame {
    QString type;
    qint64 seq = 0;
    qint64 timestampMs = 0;
    QJsonObject payload;
};

// Pure parser exposed for deterministic unit tests.
std::optional<ParsedEventFrame> parseEventStreamFrame(const QString &jsonText);
EventStreamHello parseEventStreamHelloPayload(const QJsonObject &payload);
QString buildUserEventsWebSocketUrl(const QString &httpBaseUrl, qint64 sinceSeq);

enum class EventStreamAuthFailureKind {
    None = 0,
    DuplicatedLogin = 1,
    TokenInvalid = 2,
};
EventStreamAuthFailureKind classifyEventStreamAuthFailure(
    unsigned int httpStatus, const std::string &handshakeDetail);

// Owns a dedicated single-thread ASIO io_context + strand wrapping livekit::WebSocketClient.
// Strictly bound to SessionManager authentication generations; never shares MeetingCoordinator's io_context.
class UserEventStreamClient final : public QObject {
    Q_OBJECT
public:
    explicit UserEventStreamClient(SessionManager &session, QObject *parent = nullptr);
    ~UserEventStreamClient() override;

    EventStreamState state() const { return _state; }
    qint64 lastSeq() const { return _lastSeq; }
    void setInitialSinceSeq(qint64 seq);
    void setManualPresenceStatus(PresenceStatus status); // Online or Away

    void start();
    void stop();

    void sendPresenceWatch(const std::optional<QStringList> &fixedUserIds,
                           const std::optional<QStringList> &viewportUserIds);

    // Exposed for deterministic testing without live sockets.
    void ingestRawTextFrameForTesting(const QString &text, quint64 expectedAuthGeneration);

signals:
    void stateChanged(OpenMeeting::EventStreamState state);
    void helloReceived(const OpenMeeting::EventStreamHello &hello);
    void syncSeqAdvanced(qint64 seq);
    void syncResetRequired();
    void sessionKicked(const QString &reason);
    void sessionAuthRejected(const QString &reason);
    void contactRelationUpdated(const QJsonObject &payload);
    void contactRequestReceived(const QJsonObject &payload);
    void contactRequestHandled(const QJsonObject &payload);
    void orgTreeUpdated(qint64 treeVersion);
    void meetingInvited(const QJsonObject &payload);
    void presenceBatchReceived(const std::vector<OpenMeeting::UserPresence> &items);
    void callIncoming(const OpenMeeting::CallSession &session);
    void callAccepted(const OpenMeeting::CallAcceptedEvent &event);
    void callRejected(const OpenMeeting::CallSession &session);
    void callCancelled(const OpenMeeting::CallSession &session);
    void callTimeout(const OpenMeeting::CallSession &session);

private:
    struct TransportImpl;

    void handleSessionReset();
    void handleSessionLoggedIn();
    void setState(EventStreamState next);
    void dispatchFrameOnUiThread(const QString &text, quint64 authGen);
    void dispatchSingleEvent(const ParsedEventFrame &frame);
    void sendJsonFrame(const QJsonObject &frame);

    SessionManager &_session;
    std::shared_ptr<TransportImpl> _transport;
    EventStreamState _state = EventStreamState::Disconnected;
    qint64 _lastSeq = 0;
    PresenceStatus _manualStatus = PresenceStatus::Online;
    QStringList _cachedFixedWatch;
    QStringList _cachedViewportWatch;
    bool _hasFixedWatch = false;
    bool _hasViewportWatch = false;
    bool _started = false;
    quint64 _startedAuthGen = 0;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::EventStreamState)
Q_DECLARE_METATYPE(OpenMeeting::EventStreamHello)
Q_DECLARE_METATYPE(std::vector<OpenMeeting::UserPresence>)
