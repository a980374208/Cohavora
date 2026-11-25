#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

#include <vector>

namespace MeetingUI {

struct ContactRequestEntry {
    QString requestId;
    QString peerUserId;
    QString peerDisplayName;
    QString greeting;
    bool incoming = false;
    OpenMeeting::ContactRequestStatus status = OpenMeeting::ContactRequestStatus::Pending;
};

enum ContactRequestRole {
    RequestIdRole = Qt::UserRole + 1,
    RequestPeerUserIdRole,
    RequestPeerNameRole,
    RequestGreetingRole,
    RequestIncomingRole,
    RequestStatusRole,
};

class ContactRequestListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ContactRequestListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setRequests(const std::vector<OpenMeeting::ContactRequest> &requests, const QString &selfUserId);
    const ContactRequestEntry *entryAt(int row) const;

private:
    std::vector<ContactRequestEntry> _entries;
};

} // namespace MeetingUI
