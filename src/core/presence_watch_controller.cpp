#include "src/core/presence_watch_controller.h"

#include "src/core/contact_catalog_controller.h"
#include "src/net/session_manager.h"
#include "src/net/user_event_stream_client.h"

#include <QtCore/QDateTime>
#include <QtCore/QSet>

namespace OpenMeeting {

PresenceWatchController::PresenceWatchController(
        SessionManager &session,
        ContactCatalogController &catalog,
        UserEventStreamClient &stream,
        QObject *parent)
    : QObject(parent)
    , _session(session)
    , _catalog(catalog)
    , _stream(stream) {
    _viewportThrottleTimer.setSingleShot(true);
    _viewportThrottleTimer.setInterval(150);
    connect(&_viewportThrottleTimer, &QTimer::timeout, this, &PresenceWatchController::flushViewportWatch);

    connect(&_catalog, &ContactCatalogController::contactsChanged, this, &PresenceWatchController::rebuildFixedWatch);
    connect(&_stream, &UserEventStreamClient::presenceBatchReceived, this, &PresenceWatchController::handlePresenceBatch);
    connect(&_session, &SessionManager::authenticationReset, this, [this](quint64) { clearAll(); });
    connect(&_session, &SessionManager::loggedOut, this, [this]() { clearAll(); });
    connect(&_session, &SessionManager::sessionInvalidated, this, [this](SessionInvalidationReason) { clearAll(); });

    rebuildFixedWatch();
}

UserPresence PresenceWatchController::presenceFor(const QString &userId) const {
    const auto it = _presences.constFind(userId);
    if (it != _presences.constEnd()) return *it;
    UserPresence fallback;
    fallback.userId = userId;
    fallback.status = PresenceStatus::Offline;
    return fallback;
}

void PresenceWatchController::setViewportWatchedUsers(const QStringList &userIds) {
    QSet<QString> seen;
    QStringList deduped;
    deduped.reserve(std::min(userIds.size(), kMaxViewportWatch));
    for (const auto &raw : userIds) {
        const QString id = raw.trimmed();
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.insert(id);
        deduped.push_back(id);
        if (deduped.size() >= kMaxViewportWatch) break;
    }
    _pendingViewportUsers = std::move(deduped);
    if (!_viewportThrottleTimer.isActive()) {
        _viewportThrottleTimer.start();
    }
}

void PresenceWatchController::setLocalInMeeting(bool inMeeting, const QString &meetingId) {
    _localInMeeting = inMeeting;
    _localMeetingId = inMeeting ? meetingId : QString();
    const QString me = _session.userId();
    if (me.isEmpty()) return;
    UserPresence rec = _presences.value(me);
    rec.userId = me;
    rec.meetingId = _localMeetingId;
    rec.status = inMeeting ? PresenceStatus::InMeeting
                           : (_stream.state() == EventStreamState::Connected ? PresenceStatus::Online : PresenceStatus::Offline);
    rec.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    _presences.insert(me, rec);
    emit presencesUpdated({me});
}

void PresenceWatchController::setManualStatus(PresenceStatus status) {
    _stream.setManualPresenceStatus(status);
}

void PresenceWatchController::rebuildFixedWatch() {
    if (!_session.isLoggedIn()) return;
    const QStringList fixed = _catalog.trackedContactUserIds(kMaxFixedWatch);
    _stream.sendPresenceWatch(fixed, std::nullopt);
}

void PresenceWatchController::flushViewportWatch() {
    if (!_session.isLoggedIn()) return;
    if (_pendingViewportUsers == _lastSentViewportUsers) return;
    _lastSentViewportUsers = _pendingViewportUsers;
    _stream.sendPresenceWatch(std::nullopt, _lastSentViewportUsers);
}

void PresenceWatchController::handlePresenceBatch(const std::vector<UserPresence> &items) {
    QStringList changed;
    changed.reserve(static_cast<int>(items.size()));
    const QString me = _session.userId();
    for (auto rec : items) {
        if (rec.userId.isEmpty()) continue;
        // Preserve authoritative local meeting state while active.
        if (rec.userId == me && _localInMeeting) {
            rec.status = PresenceStatus::InMeeting;
            rec.meetingId = _localMeetingId;
        }
        const auto prev = _presences.constFind(rec.userId);
        if (prev == _presences.constEnd() || prev->status != rec.status || prev->meetingId != rec.meetingId) {
            _presences.insert(rec.userId, rec);
            changed.push_back(rec.userId);
        }
    }
    if (!changed.isEmpty()) {
        emit presencesUpdated(changed);
    }
}

void PresenceWatchController::clearAll() {
    _viewportThrottleTimer.stop();
    _presences.clear();
    _pendingViewportUsers.clear();
    _lastSentViewportUsers.clear();
    _localInMeeting = false;
    _localMeetingId.clear();
    emit presencesUpdated({});
}

} // namespace OpenMeeting
