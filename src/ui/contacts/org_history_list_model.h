#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QAbstractListModel>

#include <vector>

namespace MeetingUI {

enum OrgHistoryRole {
    HistoryRecordIdRole = Qt::UserRole + 1,
    HistoryEmployeeNoRole,
    HistoryJobTitleRole,
    HistoryDeptNamesRole,
    HistoryStatusRole,
    HistoryJoinedAtMsRole,
    HistoryRemovedAtMsRole,
    HistoryRemovedReasonRole,
};

class OrgHistoryListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit OrgHistoryListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setRecords(const std::vector<OpenMeeting::OrgEmployeeRecord> &records);
    const OpenMeeting::OrgEmployeeRecord *recordAt(int row) const;

private:
    std::vector<OpenMeeting::OrgEmployeeRecord> _records;
};

} // namespace MeetingUI
