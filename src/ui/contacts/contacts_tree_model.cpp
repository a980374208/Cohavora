#include "src/ui/contacts/contacts_tree_model.h"

#include <algorithm>
#include <utility>

namespace MeetingUI {

// ============================================================================
// ContactsTreeModel
// ============================================================================

ContactsTreeModel::ContactsTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , _root(std::make_unique<Node>()) {}

ContactsTreeModel::~ContactsTreeModel() = default;

bool ContactsTreeModel::isOnlinePresence(OpenMeeting::PresenceStatus s) {
    return s != OpenMeeting::PresenceStatus::Offline;
}

const ContactsTreeModel::Node *ContactsTreeModel::nodeFromIndex(const QModelIndex &index) const {
    if (!index.isValid()) return _root.get();
    return static_cast<const Node *>(index.internalPointer());
}

QModelIndex ContactsTreeModel::indexForNode(const Node *node) const {
    if (!node || node == _root.get()) return {};
    return createIndex(node->rowInParent, 0, const_cast<Node *>(node));
}

QModelIndex ContactsTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (column != 0 || row < 0) return {};
    const Node *p = nodeFromIndex(parent);
    if (!p || row >= static_cast<int>(p->children.size())) return {};
    return createIndex(row, 0, p->children[static_cast<size_t>(row)].get());
}

QModelIndex ContactsTreeModel::parent(const QModelIndex &child) const {
    if (!child.isValid()) return {};
    const Node *n = static_cast<const Node *>(child.internalPointer());
    if (!n || !n->parent || n->parent == _root.get()) return {};
    return createIndex(n->parent->rowInParent, 0, n->parent);
}

int ContactsTreeModel::rowCount(const QModelIndex &parent) const {
    if (parent.column() > 0) return 0;
    const Node *p = nodeFromIndex(parent);
    return p ? static_cast<int>(p->children.size()) : 0;
}

int ContactsTreeModel::columnCount(const QModelIndex &) const {
    return 1;
}

QVariant ContactsTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.column() != 0) return {};
    const Node *n = static_cast<const Node *>(index.internalPointer());
    if (!n) return {};

    switch (role) {
    case Qt::DisplayRole:
    case TreeTitleRole:
        return n->title;
    case TreeNodeKindRole:
        return static_cast<int>(n->kind);
    case TreeNodeIdRole:
        return n->nodeId;
    case TreeGroupKindRole:
        return static_cast<int>(n->groupKind);
    case TreeRightTextRole:
        if (n->kind == ContactsTreeNodeKind::Group) {
            const int total = static_cast<int>(n->children.size());
            if (n->groupKind == ContactsGroupKind::Blocked) {
                return QString::number(total);
            }
            return QStringLiteral("%1/%2").arg(n->onlineCount).arg(total);
        }
        return {};
    case TreeSubtitleRole:
        return n->subtitle;
    case TreePresenceRole:
        return static_cast<int>(n->presence);
    case TreeIsLeaderRole:
        return n->isLeader;
    case TreeIsFavoriteRole:
        return n->isFavorite;
    case TreeIsBlockedRole:
        return n->isBlocked;
    case TreeIsSelfRole:
        return n->isSelf;
    case TreeIsRestrictedRole:
        return false;
    case TreeDepthRole:
        return n->depth;
    default:
        return {};
    }
}

Qt::ItemFlags ContactsTreeModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    const Node *n = static_cast<const Node *>(index.internalPointer());
    if (!n) return Qt::NoItemFlags;
    if (n->kind == ContactsTreeNodeKind::Group) {
        return Qt::ItemIsEnabled;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void ContactsTreeModel::setGroups(std::vector<ContactsTreeGroupInput> groups) {
    beginResetModel();
    _root = std::make_unique<Node>();
    _groupNodeMap.clear();
    _userNodeMap.clear();

    for (auto &g : groups) {
        auto gNode = std::make_unique<Node>();
        gNode->kind = ContactsTreeNodeKind::Group;
        gNode->groupKind = g.kind;
        gNode->nodeId = g.groupId;
        gNode->title = g.name;
        gNode->depth = 0;
        gNode->parent = _root.get();
        gNode->rowInParent = static_cast<int>(_root->children.size());

        int online = 0;
        for (const auto &m : g.members) {
            auto mNode = std::make_unique<Node>();
            mNode->kind = ContactsTreeNodeKind::Member;
            mNode->nodeId = m.userId;
            mNode->title = m.displayName;
            mNode->subtitle = m.subtitle;
            mNode->presence = m.presence;
            mNode->isLeader = m.isLeader;
            mNode->isFavorite = m.isFavorite;
            mNode->isBlocked = m.isBlocked;
            mNode->isSelf = m.isSelf;
            mNode->depth = 1;
            mNode->parent = gNode.get();
            mNode->rowInParent = static_cast<int>(gNode->children.size());
            if (isOnlinePresence(m.presence)) {
                ++online;
            }
            _userNodeMap[m.userId].push_back(mNode.get());
            gNode->children.push_back(std::move(mNode));
        }
        gNode->onlineCount = online;
        _groupNodeMap.insert(gNode->nodeId, gNode.get());
        _root->children.push_back(std::move(gNode));
    }

    endResetModel();
}

void ContactsTreeModel::updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &updates) {
    if (updates.isEmpty()) return;
    QSet<Node *> dirtyGroups;
    for (auto it = updates.constBegin(); it != updates.constEnd(); ++it) {
        const auto nodesIt = _userNodeMap.constFind(it.key());
        if (nodesIt == _userNodeMap.constEnd()) continue;
        for (Node *n : nodesIt.value()) {
            if (!n || n->presence == it.value()) continue;
            const bool wasOnline = isOnlinePresence(n->presence);
            const bool nowOnline = isOnlinePresence(it.value());
            n->presence = it.value();
            const QModelIndex idx = indexForNode(n);
            emit dataChanged(idx, idx, {TreePresenceRole});
            if (wasOnline != nowOnline && n->parent && n->parent->kind == ContactsTreeNodeKind::Group) {
                n->parent->onlineCount += (nowOnline ? 1 : -1);
                n->parent->onlineCount = std::max(0, n->parent->onlineCount);
                dirtyGroups.insert(n->parent);
            }
        }
    }
    for (Node *g : dirtyGroups) {
        const QModelIndex gIdx = indexForNode(g);
        emit dataChanged(gIdx, gIdx, {TreeRightTextRole});
    }
}

QModelIndex ContactsTreeModel::indexForGroupId(const QString &groupId) const {
    return indexForNode(_groupNodeMap.value(groupId, nullptr));
}

QModelIndex ContactsTreeModel::indexForUserId(const QString &userId) const {
    const auto it = _userNodeMap.constFind(userId);
    if (it == _userNodeMap.constEnd() || it.value().empty()) return {};
    return indexForNode(it.value().front());
}

QStringList ContactsTreeModel::allMemberUserIds() const {
    QStringList out;
    out.reserve(_userNodeMap.size());
    for (auto it = _userNodeMap.constBegin(); it != _userNodeMap.constEnd(); ++it) {
        if (!it.key().isEmpty()) out.push_back(it.key());
    }
    return out;
}

// ============================================================================
// OrgDirectoryTreeModel
// ============================================================================

OrgDirectoryTreeModel::OrgDirectoryTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , _root(std::make_unique<Node>()) {}

OrgDirectoryTreeModel::~OrgDirectoryTreeModel() = default;

bool OrgDirectoryTreeModel::isOnlinePresence(OpenMeeting::PresenceStatus s) {
    return s != OpenMeeting::PresenceStatus::Offline;
}

const OrgDirectoryTreeModel::Node *OrgDirectoryTreeModel::nodeFromIndex(const QModelIndex &index) const {
    if (!index.isValid()) return _root.get();
    return static_cast<const Node *>(index.internalPointer());
}

OrgDirectoryTreeModel::Node *OrgDirectoryTreeModel::mutableNodeFromIndex(const QModelIndex &index) {
    if (!index.isValid()) return _root.get();
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex OrgDirectoryTreeModel::indexForNode(const Node *node) const {
    if (!node || node == _root.get()) return {};
    return createIndex(node->rowInParent, 0, const_cast<Node *>(node));
}

QModelIndex OrgDirectoryTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (column != 0 || row < 0) return {};
    const Node *p = nodeFromIndex(parent);
    if (!p || row >= static_cast<int>(p->children.size())) return {};
    return createIndex(row, 0, p->children[static_cast<size_t>(row)].get());
}

QModelIndex OrgDirectoryTreeModel::parent(const QModelIndex &child) const {
    if (!child.isValid()) return {};
    const Node *n = static_cast<const Node *>(child.internalPointer());
    if (!n || !n->parent || n->parent == _root.get()) return {};
    return createIndex(n->parent->rowInParent, 0, n->parent);
}

int OrgDirectoryTreeModel::rowCount(const QModelIndex &parent) const {
    if (parent.column() > 0) return 0;
    const Node *p = nodeFromIndex(parent);
    return p ? static_cast<int>(p->children.size()) : 0;
}

int OrgDirectoryTreeModel::columnCount(const QModelIndex &) const {
    return 1;
}

bool OrgDirectoryTreeModel::hasChildren(const QModelIndex &parent) const {
    const Node *p = nodeFromIndex(parent);
    if (!p) return false;
    if (p->kind == ContactsTreeNodeKind::Department) {
        // Always allow expanding any department so clicking triggers lazy member load.
        return !p->children.empty() || !p->membersLoaded;
    }
    return !p->children.empty();
}

QVariant OrgDirectoryTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.column() != 0) return {};
    const Node *n = static_cast<const Node *>(index.internalPointer());
    if (!n) return {};

    switch (role) {
    case Qt::DisplayRole:
    case TreeTitleRole:
        return n->title;
    case TreeNodeKindRole:
        return static_cast<int>(n->kind);
    case TreeNodeIdRole:
        return n->nodeId;
    case TreeGroupKindRole:
        return static_cast<int>(ContactsGroupKind::None);
    case TreeRightTextRole:
        if (n->kind == ContactsTreeNodeKind::Department) {
            if (n->isRestricted) return _restrictedTag;
            if (n->membersLoaded) {
                return QStringLiteral("%1/%2").arg(n->onlineMemberCount).arg(n->totalMemberCount);
            }
        }
        return {};
    case TreeSubtitleRole:
        return n->subtitle;
    case TreePresenceRole:
        return static_cast<int>(n->presence);
    case TreeIsLeaderRole:
        return n->isLeader;
    case TreeIsFavoriteRole:
        return n->isFavorite;
    case TreeIsBlockedRole:
        return n->isBlocked;
    case TreeIsSelfRole:
        return n->isSelf;
    case TreeIsRestrictedRole:
        return n->isRestricted;
    case TreeDepthRole:
        return n->depth;
    default:
        return {};
    }
}

Qt::ItemFlags OrgDirectoryTreeModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    const Node *n = static_cast<const Node *>(index.internalPointer());
    if (!n) return Qt::NoItemFlags;
    if (n->kind == ContactsTreeNodeKind::Member) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
    if (n->kind == ContactsTreeNodeKind::Department) {
        return Qt::ItemIsEnabled;
    }
    return Qt::ItemIsEnabled;
}

void OrgDirectoryTreeModel::setDepartments(
        const std::vector<OpenMeeting::OrgDepartment> &departments,
        const QString &loadingText,
        const QString &emptyText,
        const QString &restrictedText,
        const QString &restrictedTag) {
    beginResetModel();
    _loadingText = loadingText;
    _emptyText = emptyText;
    _restrictedText = restrictedText;
    _restrictedTag = restrictedTag;

    _root = std::make_unique<Node>();
    _deptNodeMap.clear();
    _userNodeMap.clear();

    QHash<QString, OpenMeeting::OrgDepartment> deptById;
    for (const auto &d : departments) {
        if (!d.deptId.trimmed().isEmpty()) {
            deptById.insert(d.deptId, d);
        }
    }

    // Cycle-safe parent map
    QHash<QString, std::vector<OpenMeeting::OrgDepartment>> byParent;
    for (const auto &d : departments) {
        if (d.deptId.trimmed().isEmpty()) continue;
        QString parentId = d.parentId;
        if (!parentId.isEmpty() && !deptById.contains(parentId)) {
            parentId.clear();
        }
        // Defend against self-cycles or ancestor loops
        QSet<QString> visited{d.deptId};
        QString cursor = parentId;
        while (!cursor.isEmpty()) {
            if (visited.contains(cursor)) {
                parentId.clear();
                break;
            }
            visited.insert(cursor);
            cursor = deptById.contains(cursor) ? deptById.value(cursor).parentId : QString();
        }
        byParent[parentId].push_back(d);
    }

    for (auto it = byParent.begin(); it != byParent.end(); ++it) {
        auto &vec = it.value();
        std::sort(vec.begin(), vec.end(), [](const OpenMeeting::OrgDepartment &a, const OpenMeeting::OrgDepartment &b) {
            if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
            return a.name.localeAwareCompare(b.name) < 0;
        });
    }

    std::function<void(Node *, const QString &, int)> populate = [&](Node *parentNode, const QString &parentId, int depth) {
        const auto list = byParent.value(parentId);
        for (const auto &d : list) {
            auto node = std::make_unique<Node>();
            node->kind = ContactsTreeNodeKind::Department;
            node->nodeId = d.deptId;
            node->title = d.name;
            node->sortOrder = d.sortOrder;
            node->depth = depth;
            node->parent = parentNode;
            node->rowInParent = static_cast<int>(parentNode->children.size());
            Node *raw = node.get();
            _deptNodeMap.insert(d.deptId, raw);
            parentNode->children.push_back(std::move(node));
            populate(raw, d.deptId, depth + 1);
            raw->subDeptCount = static_cast<int>(raw->children.size());
        }
    };
    populate(_root.get(), QString(), 0);
    endResetModel();
}

void OrgDirectoryTreeModel::rebuildUserIndexForDept(Node *deptNode, bool removeOnly) {
    if (!deptNode) return;
    for (int i = deptNode->subDeptCount; i < static_cast<int>(deptNode->children.size()); ++i) {
        Node *child = deptNode->children[static_cast<size_t>(i)].get();
        if (child && child->kind == ContactsTreeNodeKind::Member) {
            auto it = _userNodeMap.find(child->nodeId);
            if (it != _userNodeMap.end()) {
                auto &vec = it.value();
                vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
                if (vec.empty()) _userNodeMap.erase(it);
            }
        }
    }
    if (removeOnly) return;
    for (int i = deptNode->subDeptCount; i < static_cast<int>(deptNode->children.size()); ++i) {
        Node *child = deptNode->children[static_cast<size_t>(i)].get();
        if (child && child->kind == ContactsTreeNodeKind::Member) {
            _userNodeMap[child->nodeId].push_back(child);
        }
    }
}

void OrgDirectoryTreeModel::replaceDeptTailRows(Node *deptNode, std::vector<std::unique_ptr<Node>> newTail) {
    if (!deptNode) return;
    const QModelIndex parentIdx = indexForNode(deptNode);
    const int oldTailStart = deptNode->subDeptCount;
    const int oldCount = static_cast<int>(deptNode->children.size());
    if (oldCount > oldTailStart) {
        rebuildUserIndexForDept(deptNode, /*removeOnly=*/true);
        beginRemoveRows(parentIdx, oldTailStart, oldCount - 1);
        deptNode->children.erase(deptNode->children.begin() + oldTailStart, deptNode->children.end());
        endRemoveRows();
    }
    if (!newTail.empty()) {
        const int firstNew = deptNode->subDeptCount;
        const int lastNew = firstNew + static_cast<int>(newTail.size()) - 1;
        beginInsertRows(parentIdx, firstNew, lastNew);
        for (auto &n : newTail) {
            n->parent = deptNode;
            n->depth = deptNode->depth + 1;
            n->rowInParent = static_cast<int>(deptNode->children.size());
            deptNode->children.push_back(std::move(n));
        }
        endInsertRows();
        rebuildUserIndexForDept(deptNode, /*removeOnly=*/false);
    }
}

void OrgDirectoryTreeModel::setDeptLoading(const QString &deptId) {
    Node *deptNode = _deptNodeMap.value(deptId, nullptr);
    if (!deptNode || deptNode->membersLoaded || deptNode->membersLoading) return;
    deptNode->membersLoading = true;
    std::vector<std::unique_ptr<Node>> tail;
    auto notice = std::make_unique<Node>();
    notice->kind = ContactsTreeNodeKind::Notice;
    notice->nodeId = QStringLiteral("__loading__:") + deptId;
    notice->title = _loadingText;
    tail.push_back(std::move(notice));
    replaceDeptTailRows(deptNode, std::move(tail));
}

void OrgDirectoryTreeModel::setDeptMembers(
        const QString &deptId,
        std::vector<ContactsTreeMemberItem> members,
        bool restricted) {
    Node *deptNode = _deptNodeMap.value(deptId, nullptr);
    if (!deptNode) return;

    deptNode->membersLoaded = true;
    deptNode->membersLoading = false;
    deptNode->isRestricted = restricted;
    deptNode->totalMemberCount = static_cast<int>(members.size());

    std::vector<std::unique_ptr<Node>> tail;
    int online = 0;
    if (restricted) {
        auto notice = std::make_unique<Node>();
        notice->kind = ContactsTreeNodeKind::Notice;
        notice->nodeId = QStringLiteral("__restricted__:") + deptId;
        notice->title = _restrictedText;
        notice->isRestricted = true;
        tail.push_back(std::move(notice));
    } else if (!members.empty()) {
        tail.reserve(members.size());
        for (const auto &m : members) {
            auto n = std::make_unique<Node>();
            n->kind = ContactsTreeNodeKind::Member;
            n->nodeId = m.userId;
            n->title = m.displayName;
            n->subtitle = m.subtitle;
            n->presence = m.presence;
            n->isLeader = m.isLeader;
            n->isFavorite = m.isFavorite;
            n->isBlocked = m.isBlocked;
            n->isSelf = m.isSelf;
            if (isOnlinePresence(m.presence)) ++online;
            tail.push_back(std::move(n));
        }
    } else if (deptNode->subDeptCount == 0) {
        auto notice = std::make_unique<Node>();
        notice->kind = ContactsTreeNodeKind::Notice;
        notice->nodeId = QStringLiteral("__empty__:") + deptId;
        notice->title = _emptyText;
        tail.push_back(std::move(notice));
    }
    deptNode->onlineMemberCount = online;

    replaceDeptTailRows(deptNode, std::move(tail));
    const QModelIndex deptIdx = indexForNode(deptNode);
    emit dataChanged(deptIdx, deptIdx, {TreeRightTextRole, TreeIsRestrictedRole});
}

void OrgDirectoryTreeModel::updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &updates) {
    if (updates.isEmpty()) return;
    QSet<Node *> dirtyDepts;
    for (auto it = updates.constBegin(); it != updates.constEnd(); ++it) {
        const auto nodesIt = _userNodeMap.constFind(it.key());
        if (nodesIt == _userNodeMap.constEnd()) continue;
        for (Node *n : nodesIt.value()) {
            if (!n || n->presence == it.value()) continue;
            const bool wasOnline = isOnlinePresence(n->presence);
            const bool nowOnline = isOnlinePresence(it.value());
            n->presence = it.value();
            const QModelIndex idx = indexForNode(n);
            emit dataChanged(idx, idx, {TreePresenceRole});
            if (wasOnline != nowOnline && n->parent && n->parent->kind == ContactsTreeNodeKind::Department) {
                n->parent->onlineMemberCount += (nowOnline ? 1 : -1);
                n->parent->onlineMemberCount = std::max(0, n->parent->onlineMemberCount);
                dirtyDepts.insert(n->parent);
            }
        }
    }
    for (Node *d : dirtyDepts) {
        const QModelIndex dIdx = indexForNode(d);
        emit dataChanged(dIdx, dIdx, {TreeRightTextRole});
    }
}

QModelIndex OrgDirectoryTreeModel::indexForDeptId(const QString &deptId) const {
    return indexForNode(_deptNodeMap.value(deptId, nullptr));
}

QModelIndex OrgDirectoryTreeModel::indexForUserId(const QString &userId) const {
    const auto it = _userNodeMap.constFind(userId);
    if (it == _userNodeMap.constEnd() || it.value().empty()) return {};
    return indexForNode(it.value().front());
}

QStringList OrgDirectoryTreeModel::ancestorDeptIds(const QString &deptId) const {
    QStringList chain;
    const Node *cur = _deptNodeMap.value(deptId, nullptr);
    while (cur && cur->kind == ContactsTreeNodeKind::Department) {
        chain.prepend(cur->nodeId);
        cur = cur->parent;
    }
    return chain;
}

QStringList OrgDirectoryTreeModel::loadedMemberUserIds() const {
    QStringList out;
    out.reserve(_userNodeMap.size());
    for (auto it = _userNodeMap.constBegin(); it != _userNodeMap.constEnd(); ++it) {
        if (!it.key().isEmpty()) out.push_back(it.key());
    }
    return out;
}

bool OrgDirectoryTreeModel::isDeptLoaded(const QString &deptId) const {
    const Node *n = _deptNodeMap.value(deptId, nullptr);
    return n && n->membersLoaded;
}

} // namespace MeetingUI
