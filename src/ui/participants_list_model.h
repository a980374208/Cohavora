#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QSortFilterProxyModel>
#include <vector>
#include "src/core/meeting_coordinator.h"

namespace OpenMeeting {

enum ParticipantItemRoles {
    IdentityRole = Qt::UserRole + 1,
    NameRole,
    IsLocalRole,
    IsHostRole,
    IsAudioMutedRole,
    IsVideoEnabledRole,
    IsSpeakingRole,
    AudioLevelRole
};

class ParticipantListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ParticipantListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setParticipants(const std::vector<ParticipantInfo> &participants);
    ParticipantInfo participantAt(int row) const;
    ParticipantInfo findParticipantById(const QString &identity) const;

private:
    std::vector<ParticipantInfo> _items;
};

class ParticipantFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ParticipantFilterProxyModel(QObject *parent = nullptr);

    void setSearchKeyword(const QString &keyword);

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
    bool lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const override;

private:
    QString _keyword;
};

} // namespace OpenMeeting
