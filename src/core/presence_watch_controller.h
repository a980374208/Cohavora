#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include <vector>

namespace OpenMeeting {

class ContactCatalogController;
class SessionManager;
class UserEventStreamClient;

// Manages local Presence cache, fixed watchlist (up to 500 personal contacts),
// and coalesced viewport watch window (up to 200 visible rows in org tree / search).
class PresenceWatchController final : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxFixedWatch = 500;
    static constexpr int kMaxViewportWatch = 200;

    PresenceWatchController(SessionManager &session,
                            ContactCatalogController &catalog,
                            UserEventStreamClient &stream,
                            QObject *parent = nullptr);

    UserPresence presenceFor(const QString &userId) const;
    PresenceStatus statusFor(const QString &userId) const { return presenceFor(userId).status; }

    void setViewportWatchedUsers(const QStringList &userIds);
    void setLocalInMeeting(bool inMeeting, const QString &meetingId = {});
    void setManualStatus(PresenceStatus status); // Online or Away

signals:
    void presencesUpdated(const QStringList &userIds);

private:
    void rebuildFixedWatch();
    void flushViewportWatch();
    void handlePresenceBatch(const std::vector<UserPresence> &items);
    void clearAll();

    SessionManager &_session;
    ContactCatalogController &_catalog;
    UserEventStreamClient &_stream;

    QHash<QString, UserPresence> _presences;
    QStringList _pendingViewportUsers;
    QStringList _lastSentViewportUsers;
    QTimer _viewportThrottleTimer;
    bool _localInMeeting = false;
    QString _localMeetingId;
};

} // namespace OpenMeeting
