#include "src/ui/contacts/org_history_list_model.h"

namespace MeetingUI {

OrgHistoryListModel::OrgHistoryListModel(QObject *parent)
    : QAbstractListModel(parent) {}

int OrgHistoryListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(_records.size());
}

QVariant OrgHistoryListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(_records.size())) {
        return {};
    }
    const auto &rec = _records[static_cast<size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case HistoryEmployeeNoRole:
        return rec.employeeNo;
    case HistoryRecordIdRole:
        return rec.recordId;
    case HistoryJobTitleRole:
        return rec.jobTitle;
    case HistoryDeptNamesRole:
        return rec.deptNames;
    case HistoryStatusRole:
        return rec.status;
    case HistoryJoinedAtMsRole:
        return rec.joinedAtMs;
    case HistoryRemovedAtMsRole:
        return rec.removedAtMs;
    case HistoryRemovedReasonRole:
        return rec.removedReason;
    default:
        return {};
    }
}

void OrgHistoryListModel::setRecords(const std::vector<OpenMeeting::OrgEmployeeRecord> &records) {
    beginResetModel();
    _records = records;
    endResetModel();
}

const OpenMeeting::OrgEmployeeRecord *OrgHistoryListModel::recordAt(int row) const {
    if (row < 0 || row >= static_cast<int>(_records.size())) return nullptr;
    return &_records[static_cast<size_t>(row)];
}

} // namespace MeetingUI
