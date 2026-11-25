#include "src/ui/contacts/contact_request_list_model.h"

namespace MeetingUI {

ContactRequestListModel::ContactRequestListModel(QObject *parent)
    : QAbstractListModel(parent) {}

int ContactRequestListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(_entries.size());
}

QVariant ContactRequestListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto &e = _entries[static_cast<size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case RequestPeerNameRole:
        return e.peerDisplayName;
    case RequestIdRole:
        return e.requestId;
    case RequestPeerUserIdRole:
        return e.peerUserId;
    case RequestGreetingRole:
        return e.greeting;
    case RequestIncomingRole:
        return e.incoming;
    case RequestStatusRole:
        return static_cast<int>(e.status);
    default:
        return {};
    }
}

void ContactRequestListModel::setRequests(
        const std::vector<OpenMeeting::ContactRequest> &requests,
        const QString &selfUserId) {
    beginResetModel();
    _entries.clear();
    _entries.reserve(requests.size());
    for (const auto &req : requests) {
        const bool incoming = (req.toUserId == selfUserId);
        const auto &peer = incoming ? req.fromProfile : req.toProfile;
        const QString peerId = incoming ? req.fromUserId : req.toUserId;
        const QString peerName = (peer && !peer->displayName().isEmpty()) ? peer->displayName() : peerId;

        ContactRequestEntry entry;
        entry.requestId = req.requestId;
        entry.peerUserId = peerId;
        entry.peerDisplayName = peerName;
        entry.greeting = req.greeting.trimmed();
        entry.incoming = incoming;
        entry.status = req.status;
        _entries.push_back(std::move(entry));
    }
    endResetModel();
}

const ContactRequestEntry *ContactRequestListModel::entryAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return nullptr;
    }
    return &_entries[static_cast<size_t>(row)];
}

} // namespace MeetingUI
