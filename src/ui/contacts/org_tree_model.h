#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QString>

#include <memory>
#include <vector>

namespace MeetingUI {

enum class OrgTreeNodeKind {
    Root = 0,
    MyContacts = 1,
    OrgRoot = 2,
    Department = 3,
};

enum OrgTreeRole {
    OrgDeptIdRole = Qt::UserRole + 1,
    OrgNodeKindRole,
    OrgDeptAncestorPathRole,
};

class OrgTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit OrgTreeModel(QObject *parent = nullptr);
    ~OrgTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setDepartments(
        const std::vector<OpenMeeting::OrgDepartment> &departments,
        bool includeVirtualRoots = false,
        const QString &myContactsLabel = {},
        const QString &orgRootLabel = {});

    void retranslateVirtualNodes(const QString &myContactsLabel, const QString &orgRootLabel);

    QModelIndex indexForDeptId(const QString &deptId) const;
    QModelIndex indexForVirtualNode(OrgTreeNodeKind kind) const;
    QModelIndex firstSelectableIndex() const;
    bool hasDepartments() const { return _departmentCount > 0; }

private:
    struct Node {
        OrgTreeNodeKind kind = OrgTreeNodeKind::Root;
        QString deptId;
        QString name;
        QString ancestorPath;
        int sortOrder = 0;
        Node *parent = nullptr;
        int rowInParent = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    const Node *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexForNode(const Node *node) const;

    std::unique_ptr<Node> _root;
    QHash<QString, Node *> _deptNodeMap;
    Node *_myContactsNode = nullptr;
    Node *_orgRootNode = nullptr;
    int _departmentCount = 0;
};

} // namespace MeetingUI
