#include "src/ui/contacts/org_tree_model.h"

#include <QtCore/QSet>

#include <algorithm>

namespace MeetingUI {

OrgTreeModel::OrgTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , _root(std::make_unique<Node>()) {}

OrgTreeModel::~OrgTreeModel() = default;

const OrgTreeModel::Node *OrgTreeModel::nodeFromIndex(const QModelIndex &index) const {
    if (!index.isValid()) {
        return _root.get();
    }
    return static_cast<const Node *>(index.internalPointer());
}

QModelIndex OrgTreeModel::indexForNode(const Node *node) const {
    if (!node || node == _root.get() || !node->parent) {
        return {};
    }
    return createIndex(node->rowInParent, 0, const_cast<Node *>(node));
}

QModelIndex OrgTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (column != 0 || row < 0) {
        return {};
    }
    const Node *parentNode = nodeFromIndex(parent);
    if (!parentNode || row >= static_cast<int>(parentNode->children.size())) {
        return {};
    }
    Node *child = parentNode->children[static_cast<size_t>(row)].get();
    return createIndex(row, 0, child);
}

QModelIndex OrgTreeModel::parent(const QModelIndex &child) const {
    if (!child.isValid()) {
        return {};
    }
    const Node *childNode = static_cast<const Node *>(child.internalPointer());
    if (!childNode || !childNode->parent || childNode->parent == _root.get()) {
        return {};
    }
    Node *parentNode = childNode->parent;
    return createIndex(parentNode->rowInParent, 0, parentNode);
}

int OrgTreeModel::rowCount(const QModelIndex &parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    const Node *parentNode = nodeFromIndex(parent);
    return parentNode ? static_cast<int>(parentNode->children.size()) : 0;
}

int OrgTreeModel::columnCount(const QModelIndex &) const {
    return 1;
}

QVariant OrgTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.column() != 0) {
        return {};
    }
    const Node *node = static_cast<const Node *>(index.internalPointer());
    if (!node) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return node->name;
    case OrgDeptIdRole:
        return node->deptId;
    case OrgNodeKindRole:
        return static_cast<int>(node->kind);
    case OrgDeptAncestorPathRole:
        return node->ancestorPath;
    default:
        return {};
    }
}

Qt::ItemFlags OrgTreeModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void OrgTreeModel::setDepartments(
        const std::vector<OpenMeeting::OrgDepartment> &departments,
        bool includeVirtualRoots,
        const QString &myContactsLabel,
        const QString &orgRootLabel) {
    beginResetModel();

    _root = std::make_unique<Node>();
    _deptNodeMap.clear();
    _myContactsNode = nullptr;
    _orgRootNode = nullptr;
    _departmentCount = 0;

    auto appendChild = [](Node *parentNode, std::unique_ptr<Node> child) -> Node * {
        child->parent = parentNode;
        child->rowInParent = static_cast<int>(parentNode->children.size());
        Node *raw = child.get();
        parentNode->children.push_back(std::move(child));
        return raw;
    };

    if (includeVirtualRoots) {
        auto contacts = std::make_unique<Node>();
        contacts->kind = OrgTreeNodeKind::MyContacts;
        contacts->name = myContactsLabel;
        _myContactsNode = appendChild(_root.get(), std::move(contacts));
    }

    std::vector<OpenMeeting::OrgDepartment> sorted;
    sorted.reserve(departments.size());
    QSet<QString> seenIds;
    for (const auto &d : departments) {
        const QString id = d.deptId.trimmed();
        if (id.isEmpty() || seenIds.contains(id)) {
            continue;
        }
        seenIds.insert(id);
        sorted.push_back(d);
    }

    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    Node *deptContainer = _root.get();
    if (includeVirtualRoots && !sorted.empty()) {
        auto orgRoot = std::make_unique<Node>();
        orgRoot->kind = OrgTreeNodeKind::OrgRoot;
        orgRoot->deptId = QStringLiteral("__org_root__");
        orgRoot->name = orgRootLabel;
        _orgRootNode = appendChild(_root.get(), std::move(orgRoot));
        deptContainer = _orgRootNode;
    }

    // Build parent lookup and detect cycles before attaching nodes.
    QHash<QString, QString> parentIdByDept;
    QHash<QString, const OpenMeeting::OrgDepartment *> deptById;
    for (const auto &d : sorted) {
        parentIdByDept.insert(d.deptId, d.parentId);
        deptById.insert(d.deptId, &d);
    }

    auto hasCycleToRoot = [&](const QString &startId) {
        QSet<QString> visited;
        QString cur = startId;
        while (!cur.isEmpty() && deptById.contains(cur)) {
            if (visited.contains(cur)) {
                return true;
            }
            visited.insert(cur);
            cur = parentIdByDept.value(cur);
        }
        return false;
    };

    QHash<QString, QStringList> childrenIdsByParent;
    QStringList rootDeptIds;
    for (const auto &d : sorted) {
        const bool validParent = !d.parentId.isEmpty() &&
            d.parentId != d.deptId &&
            deptById.contains(d.parentId) &&
            !hasCycleToRoot(d.deptId);
        if (validParent) {
            childrenIdsByParent[d.parentId].push_back(d.deptId);
        } else {
            rootDeptIds.push_back(d.deptId);
        }
    }

    auto buildSubtree = [&](auto &&self, const QString &deptId, Node *parentNode) -> void {
        const auto *dept = deptById.value(deptId, nullptr);
        if (!dept || _deptNodeMap.contains(deptId)) {
            return;
        }
        auto node = std::make_unique<Node>();
        node->kind = OrgTreeNodeKind::Department;
        node->deptId = dept->deptId;
        node->name = dept->name;
        node->ancestorPath = dept->ancestorPath;
        node->sortOrder = dept->sortOrder;
        Node *attached = appendChild(parentNode, std::move(node));
        _deptNodeMap.insert(dept->deptId, attached);
        ++_departmentCount;

        for (const auto &childId : childrenIdsByParent.value(deptId)) {
            self(self, childId, attached);
        }
    };

    for (const auto &rootId : rootDeptIds) {
        buildSubtree(buildSubtree, rootId, deptContainer);
    }
    // Attach any remaining disconnected cycle members at root so no department is dropped.
    for (const auto &d : sorted) {
        if (!_deptNodeMap.contains(d.deptId)) {
            buildSubtree(buildSubtree, d.deptId, deptContainer);
        }
    }

    endResetModel();
}

void OrgTreeModel::retranslateVirtualNodes(const QString &myContactsLabel, const QString &orgRootLabel) {
    if (_myContactsNode && _myContactsNode->name != myContactsLabel) {
        _myContactsNode->name = myContactsLabel;
        const QModelIndex idx = indexForNode(_myContactsNode);
        emit dataChanged(idx, idx, {Qt::DisplayRole});
    }
    if (_orgRootNode && _orgRootNode->name != orgRootLabel) {
        _orgRootNode->name = orgRootLabel;
        const QModelIndex idx = indexForNode(_orgRootNode);
        emit dataChanged(idx, idx, {Qt::DisplayRole});
    }
}

QModelIndex OrgTreeModel::indexForDeptId(const QString &deptId) const {
    if (deptId.isEmpty()) {
        return _myContactsNode ? indexForNode(_myContactsNode) : QModelIndex{};
    }
    if (deptId == QStringLiteral("__org_root__") && _orgRootNode) {
        return indexForNode(_orgRootNode);
    }
    return indexForNode(_deptNodeMap.value(deptId, nullptr));
}

QModelIndex OrgTreeModel::indexForVirtualNode(OrgTreeNodeKind kind) const {
    if (kind == OrgTreeNodeKind::MyContacts) {
        return indexForNode(_myContactsNode);
    }
    if (kind == OrgTreeNodeKind::OrgRoot) {
        return indexForNode(_orgRootNode);
    }
    return {};
}

QModelIndex OrgTreeModel::firstSelectableIndex() const {
    if (!_root || _root->children.empty()) {
        return {};
    }
    return indexForNode(_root->children.front().get());
}

} // namespace MeetingUI
