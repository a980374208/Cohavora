#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <vector>

namespace MeetingUI {

struct ContactListEntry {
    QString userId;
    QString displayName;
    QString subtitle;
    QString badge;
    bool isFavorite = false;
    bool isBlocked = false;
    bool isLeader = false;
    bool checked = false;
    OpenMeeting::PresenceStatus presence = OpenMeeting::PresenceStatus::Offline;
};

enum ContactListRole {
    ContactUserIdRole = Qt::UserRole + 1,
    ContactDisplayNameRole,
    ContactSubtitleRole,
    ContactBadgeRole,
    ContactIsFavoriteRole,
    ContactIsBlockedRole,
    ContactIsLeaderRole,
    ContactPresenceRole,
    ContactCheckedRole,
};

class ContactListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ContactListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setCheckable(bool checkable);
    bool isCheckable() const { return _checkable; }
    void setEntries(std::vector<ContactListEntry> entries);
    void setCheckedUserIds(const QSet<QString> &checkedIds);
    bool toggleCheckAt(int row);
    void updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &statuses);
    const ContactListEntry *entryAt(int row) const;
    int rowForUserId(const QString &userId) const;
    QStringList allUserIds() const;

signals:
    void checkStateChanged(const QString &userId, bool checked);

private:
    bool _checkable = false;
    std::vector<ContactListEntry> _entries;
    QHash<QString, int> _rowIndexByUserId;
};

} // namespace MeetingUI
