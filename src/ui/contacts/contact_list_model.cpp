#include "src/ui/contacts/contact_list_model.h"

namespace MeetingUI {

ContactListModel::ContactListModel(QObject *parent)
    : QAbstractListModel(parent) {}

int ContactListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(_entries.size());
}

QVariant ContactListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto &e = _entries[static_cast<size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case ContactDisplayNameRole: return e.displayName;
    case ContactUserIdRole: return e.userId;
    case ContactSubtitleRole: return e.subtitle;
    case ContactBadgeRole: return e.badge;
    case ContactIsFavoriteRole: return e.isFavorite;
    case ContactIsBlockedRole: return e.isBlocked;
    case ContactIsLeaderRole: return e.isLeader;
    case ContactPresenceRole: return static_cast<int>(e.presence);
    case ContactCheckedRole: return e.checked;
    case Qt::CheckStateRole:
        if (_checkable) return e.checked ? Qt::Checked : Qt::Unchecked;
        return {};
    default: return {};
    }
}

bool ContactListModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!_checkable || !index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return false;
    }
    if (role != Qt::CheckStateRole && role != ContactCheckedRole) {
        return false;
    }
    const bool nextChecked = (role == Qt::CheckStateRole)
        ? (static_cast<Qt::CheckState>(value.toInt()) == Qt::Checked)
        : value.toBool();
    auto &entry = _entries[static_cast<size_t>(index.row())];
    if (entry.checked == nextChecked) {
        return true;
    }
    entry.checked = nextChecked;
    emit dataChanged(index, index, {Qt::CheckStateRole, ContactCheckedRole});
    emit checkStateChanged(entry.userId, nextChecked);
    return true;
}

Qt::ItemFlags ContactListModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (_checkable) {
        f |= Qt::ItemIsUserCheckable;
    }
    return f;
}

void ContactListModel::setCheckable(bool checkable) {
    if (_checkable == checkable) return;
    _checkable = checkable;
    if (!_entries.empty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::CheckStateRole, ContactCheckedRole});
    }
}

void ContactListModel::setCheckedUserIds(const QSet<QString> &checkedIds) {
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        auto &entry = _entries[static_cast<size_t>(i)];
        const bool next = checkedIds.contains(entry.userId);
        if (entry.checked != next) {
            entry.checked = next;
            const QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx, {Qt::CheckStateRole, ContactCheckedRole});
        }
    }
}

bool ContactListModel::toggleCheckAt(int row) {
    if (!_checkable || row < 0 || row >= rowCount()) {
        return false;
    }
    const QModelIndex idx = index(row, 0);
    return setData(idx, !_entries[static_cast<size_t>(row)].checked, ContactCheckedRole);
}

void ContactListModel::setEntries(std::vector<ContactListEntry> entries) {
    beginResetModel();
    _entries = std::move(entries);
    _rowIndexByUserId.clear();
    _rowIndexByUserId.reserve(static_cast<int>(_entries.size()));
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        _rowIndexByUserId.insert(_entries[static_cast<size_t>(i)].userId, i);
    }
    endResetModel();
}

void ContactListModel::updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &statuses) {
    for (auto it = statuses.constBegin(); it != statuses.constEnd(); ++it) {
        const auto rowIt = _rowIndexByUserId.constFind(it.key());
        if (rowIt == _rowIndexByUserId.constEnd()) continue;
        const int row = *rowIt;
        auto &entry = _entries[static_cast<size_t>(row)];
        if (entry.presence != it.value()) {
            entry.presence = it.value();
            const QModelIndex idx = index(row, 0);
            emit dataChanged(idx, idx, {ContactPresenceRole});
        }
    }
}

const ContactListEntry *ContactListModel::entryAt(int row) const {
    if (row < 0 || row >= rowCount()) return nullptr;
    return &_entries[static_cast<size_t>(row)];
}

int ContactListModel::rowForUserId(const QString &userId) const {
    return _rowIndexByUserId.value(userId, -1);
}

QStringList ContactListModel::allUserIds() const {
    QStringList ids;
    ids.reserve(static_cast<int>(_entries.size()));
    for (const auto &e : _entries) ids.push_back(e.userId);
    return ids;
}

} // namespace MeetingUI
