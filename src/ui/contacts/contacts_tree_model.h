#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace MeetingUI {

enum class ContactsTreeNodeKind {
    Root = 0,
    Group = 1,
    Department = 2,
    Member = 3,
    Notice = 4,
};

enum class ContactsGroupKind {
    None = 0,
    Starred = 1,
    MyContacts = 2,
    Custom = 3,
    Blocked = 4,
};

enum ContactsTreeRole {
    TreeNodeKindRole = Qt::UserRole + 101,
    TreeNodeIdRole,
    TreeGroupKindRole,
    TreeTitleRole,
    TreeRightTextRole,
    TreeSubtitleRole,
    TreePresenceRole,
    TreeIsLeaderRole,
    TreeIsFavoriteRole,
    TreeIsBlockedRole,
    TreeIsSelfRole,
    TreeIsRestrictedRole,
    TreeDepthRole,
};

struct ContactsTreeMemberItem {
    QString userId;
    QString displayName;
    QString subtitle;
    OpenMeeting::PresenceStatus presence = OpenMeeting::PresenceStatus::Offline;
    bool isLeader = false;
    bool isFavorite = false;
    bool isBlocked = false;
    bool isSelf = false;
};

struct ContactsTreeGroupInput {
    QString groupId;
    QString name;
    ContactsGroupKind kind = ContactsGroupKind::Custom;
    std::vector<ContactsTreeMemberItem> members;
};

// Hierarchical MVD model for the Contacts tab:
// Starred -> My Contacts -> Custom Groups -> Blocked (bottom collapsible group).
class ContactsTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ContactsTreeModel(QObject *parent = nullptr);
    ~ContactsTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setGroups(std::vector<ContactsTreeGroupInput> groups);
    void updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &updates);

    QModelIndex indexForGroupId(const QString &groupId) const;
    QModelIndex indexForUserId(const QString &userId) const;
    QStringList allMemberUserIds() const;

private:
    struct Node {
        ContactsTreeNodeKind kind = ContactsTreeNodeKind::Root;
        ContactsGroupKind groupKind = ContactsGroupKind::None;
        QString nodeId;
        QString title;
        QString subtitle;
        OpenMeeting::PresenceStatus presence = OpenMeeting::PresenceStatus::Offline;
        bool isLeader = false;
        bool isFavorite = false;
        bool isBlocked = false;
        bool isSelf = false;
        int depth = 0;
        int onlineCount = 0;
        Node *parent = nullptr;
        int rowInParent = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    const Node *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexForNode(const Node *node) const;
    static bool isOnlinePresence(OpenMeeting::PresenceStatus s);

    std::unique_ptr<Node> _root;
    QHash<QString, Node *> _groupNodeMap;
    QHash<QString, std::vector<Node *>> _userNodeMap;
};

// Unified collapsible organization tree MVD model:
// Departments contain sub-departments AND inline member rows (or inline notice when restricted / loading / empty).
class OrgDirectoryTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit OrgDirectoryTreeModel(QObject *parent = nullptr);
    ~OrgDirectoryTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setDepartments(
        const std::vector<OpenMeeting::OrgDepartment> &departments,
        const QString &loadingText,
        const QString &emptyText,
        const QString &restrictedText,
        const QString &restrictedTag);

    void setDeptLoading(const QString &deptId);
    void setDeptMembers(
        const QString &deptId,
        std::vector<ContactsTreeMemberItem> members,
        bool restricted);
    void updatePresences(const QHash<QString, OpenMeeting::PresenceStatus> &updates);

    QModelIndex indexForDeptId(const QString &deptId) const;
    QModelIndex indexForUserId(const QString &userId) const;
    QStringList ancestorDeptIds(const QString &deptId) const;
    QStringList loadedMemberUserIds() const;
    bool hasDepartments() const { return !_deptNodeMap.isEmpty(); }
    bool isDeptLoaded(const QString &deptId) const;

private:
    struct Node {
        ContactsTreeNodeKind kind = ContactsTreeNodeKind::Root;
        QString nodeId;
        QString title;
        QString subtitle;
        int sortOrder = 0;
        int depth = 0;
        OpenMeeting::PresenceStatus presence = OpenMeeting::PresenceStatus::Offline;
        bool isLeader = false;
        bool isFavorite = false;
        bool isBlocked = false;
        bool isSelf = false;
        bool isRestricted = false;

        // Department-specific lazy-load state
        int subDeptCount = 0;
        bool membersLoaded = false;
        bool membersLoading = false;
        int onlineMemberCount = 0;
        int totalMemberCount = 0;

        Node *parent = nullptr;
        int rowInParent = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    const Node *nodeFromIndex(const QModelIndex &index) const;
    Node *mutableNodeFromIndex(const QModelIndex &index);
    QModelIndex indexForNode(const Node *node) const;
    void rebuildUserIndexForDept(Node *deptNode, bool removeOnly);
    void replaceDeptTailRows(Node *deptNode, std::vector<std::unique_ptr<Node>> newTail);
    static bool isOnlinePresence(OpenMeeting::PresenceStatus s);

    std::unique_ptr<Node> _root;
    QHash<QString, Node *> _deptNodeMap;
    QHash<QString, std::vector<Node *>> _userNodeMap;
    QString _loadingText;
    QString _emptyText;
    QString _restrictedText;
    QString _restrictedTag;
};

} // namespace MeetingUI
