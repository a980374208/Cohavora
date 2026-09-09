#include "src/ui/participants_list_model.h"

namespace OpenMeeting {

// ----------------------------------------------------
// ParticipantListModel 实现
// ----------------------------------------------------

ParticipantListModel::ParticipantListModel(QObject *parent)
    : QAbstractListModel(parent) {
}

int ParticipantListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(_items.size());
}

QVariant ParticipantListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(_items.size())) {
        return QVariant();
    }

    const auto &item = _items[index.row()];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole:
            return item.name;
        case IdentityRole:
            return item.identity;
        case IsLocalRole:
            return item.isLocal;
        case IsHostRole:
            return item.isHost;
        case IsAudioMutedRole:
            return item.isAudioMuted;
        case IsVideoEnabledRole:
            return item.isVideoEnabled;
        case IsSpeakingRole:
            return item.isSpeaking;
        case AudioLevelRole:
            return item.audioLevel;
        default:
            break;
    }
    return QVariant();
}

void ParticipantListModel::setParticipants(const std::vector<ParticipantInfo> &participants) {
    beginResetModel();
    _items = participants;
    endResetModel();
}

ParticipantInfo ParticipantListModel::participantAt(int row) const {
    if (row >= 0 && row < static_cast<int>(_items.size())) {
        return _items[row];
    }
    return ParticipantInfo();
}

ParticipantInfo ParticipantListModel::findParticipantById(const QString &identity) const {
    for (const auto &item : _items) {
        if (item.identity == identity) {
            return item;
        }
    }
    return ParticipantInfo();
}

// ----------------------------------------------------
// ParticipantFilterProxyModel 实现
// ----------------------------------------------------

ParticipantFilterProxyModel::ParticipantFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    sort(0, Qt::AscendingOrder);
}

void ParticipantFilterProxyModel::setSearchKeyword(const QString &keyword) {
    _keyword = keyword.trimmed();
    invalidateFilter();
}

bool ParticipantFilterProxyModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const {
    if (_keyword.isEmpty()) {
        return true;
    }

    QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
    QString name = idx.data(NameRole).toString();
    QString identity = idx.data(IdentityRole).toString();

    return name.contains(_keyword, Qt::CaseInsensitive) || identity.contains(_keyword, Qt::CaseInsensitive);
}

bool ParticipantFilterProxyModel::lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const {
    bool leftLocal = source_left.data(IsLocalRole).toBool();
    bool rightLocal = source_right.data(IsLocalRole).toBool();
    if (leftLocal != rightLocal) {
        return leftLocal; // 本地用户置顶
    }

    bool leftHost = source_left.data(IsHostRole).toBool();
    bool rightHost = source_right.data(IsHostRole).toBool();
    if (leftHost != rightHost) {
        return leftHost; // 主持人次顶
    }

    bool leftSpeaking = source_left.data(IsSpeakingRole).toBool();
    bool rightSpeaking = source_right.data(IsSpeakingRole).toBool();
    if (leftSpeaking != rightSpeaking) {
        return leftSpeaking; // 正在发言优先
    }

    QString leftName = source_left.data(NameRole).toString();
    QString rightName = source_right.data(NameRole).toString();
    return leftName.localeAwareCompare(rightName) < 0;
}

} // namespace OpenMeeting
