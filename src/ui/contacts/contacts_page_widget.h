#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

class QAction;
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace OpenMeeting {
class ContactCatalogController;
class PresenceWatchController;
class SessionManager;
}

namespace MeetingUI {

class ContactItemDelegate;
class ContactListModel;
class ContactRequestItemDelegate;
class ContactRequestListModel;
class ContactsTreeDelegate;
class ContactsTreeModel;
class OrgDirectoryTreeModel;
class OrgHistoryItemDelegate;
class OrgHistoryListModel;

class ContactsPageWidget final : public QWidget {
    Q_OBJECT
public:
    ContactsPageWidget(
        OpenMeeting::ContactCatalogController &catalog,
        OpenMeeting::PresenceWatchController &presence,
        OpenMeeting::SessionManager &session,
        QWidget *parent = nullptr);

    void retranslateUi();
    void refreshTheme();
    void openRequestsTab();

signals:
    void voiceCallRequested(const QString &userId);
    void videoCallRequested(const QString &userId);
    void scheduleMeetingWithContactRequested(const QString &userId);

private:
    enum class SidebarTab {
        Contacts = 0,
        Organization = 1,
    };

    void initUi();
    void setSidebarTab(SidebarTab tab);
    void updateOrgTabVisibility();
    void updateLeftViewMode();
    void rebuildContactsTree();
    void rebuildOrgTree();
    void applyDeptMembersToTree(const QString &deptId);
    void rebuildSearchList();
    void rebuildRequestsView();
    void showContactDetail(const QString &userId, bool refreshFromServer = true);
    void updateDetailPresence();
    void updateRequestsBadge();
    void syncViewportWatch();

    void handleOpenSelfProfileDialog();
    void handlePlusMenu();
    void handleContactsTreeContextMenu(const QPoint &pos);
    void handleCreateGroup();
    void handleRenameGroup(const OpenMeeting::ContactGroup &group);
    void handleDeleteGroup(const OpenMeeting::ContactGroup &group);
    void handleToggleFavorite();
    void handleSaveRemark();
    void handleAddOrRequestContact();
    void handleMoreActionsMenu();
    void handleQuickAddSearch();

    OpenMeeting::ContactCatalogController &_catalog;
    OpenMeeting::PresenceWatchController &_presence;
    OpenMeeting::SessionManager &_session;

    SidebarTab _sidebarTab = SidebarTab::Contacts;
    bool _showingRequests = false;
    bool _contactsExpandInitialized = false;
    QSet<QString> _expandedGroupIds;
    QSet<QString> _expandedDeptIds;
    QString _selectedUserId;
    QString _searchKeyword;
    QTimer _searchDebounce;

    // Left pane controls (Figure 2 clean collapsible-tree style)
    QLineEdit *_searchEdit = nullptr;
    QAction *_searchIconAction = nullptr;
    QPushButton *_plusMenuBtn = nullptr;
    QPushButton *_requestsEntryBtn = nullptr;
    QLabel *_requestsEntryTitle = nullptr;
    QLabel *_requestsBadgeLabel = nullptr;
    QLabel *_requestsChevronLabel = nullptr;
    QFrame *_topDivider = nullptr;
    QFrame *_segTrack = nullptr;
    QPushButton *_segContactsBtn = nullptr;
    QPushButton *_segOrgBtn = nullptr;
    QStackedWidget *_leftStack = nullptr;

    QTreeView *_contactsTreeView = nullptr;
    ContactsTreeModel *_contactsTreeModel = nullptr;
    ContactsTreeDelegate *_contactsTreeDelegate = nullptr;

    QTreeView *_orgTreeView = nullptr;
    OrgDirectoryTreeModel *_orgDirModel = nullptr;
    ContactsTreeDelegate *_orgTreeDelegate = nullptr;

    QWidget *_searchPage = nullptr;
    QLabel *_listStatusLabel = nullptr;
    QListView *_searchListView = nullptr;
    ContactListModel *_searchModel = nullptr;
    ContactItemDelegate *_searchDelegate = nullptr;

    // Right pane stack
    QStackedWidget *_rightStack = nullptr;
    QLabel *_emptyPlaceholder = nullptr;

    // Detail page controls
    QWidget *_detailPage = nullptr;
    QLabel *_detailAvatarLabel = nullptr;
    QLabel *_detailNameLabel = nullptr;
    QLabel *_detailPresenceLabel = nullptr;
    QLabel *_detailSubtitleLabel = nullptr;
    QLabel *_detailBlockedBanner = nullptr;
    QPushButton *_detailStarBtn = nullptr;
    QPushButton *_detailMoreBtn = nullptr;
    QPushButton *_detailVoiceCallBtn = nullptr;
    QPushButton *_detailVideoCallBtn = nullptr;
    QPushButton *_detailScheduleBtn = nullptr;
    QPushButton *_detailAddContactBtn = nullptr;
    QPushButton *_detailUnblockBtn = nullptr;
    QPushButton *_detailEditProfileBtn = nullptr;
    QFormLayout *_detailInfoForm = nullptr;
    QLabel *_detailAccountValue = nullptr;
    QLabel *_detailEmployeeNoValue = nullptr;
    QLabel *_detailDeptValue = nullptr;
    QLabel *_detailJobValue = nullptr;
    QLabel *_detailEmailValue = nullptr;
    QLabel *_detailPhoneValue = nullptr;
    QLabel *_detailSignatureValue = nullptr;
    QWidget *_remarkRow = nullptr;
    QLineEdit *_remarkEdit = nullptr;
    QPushButton *_saveRemarkBtn = nullptr;
    QWidget *_detailSelfHistorySection = nullptr;
    QLabel *_detailSelfHistoryTitle = nullptr;
    QListView *_detailSelfHistoryView = nullptr;
    OrgHistoryListModel *_detailSelfHistoryModel = nullptr;
    OrgHistoryItemDelegate *_detailSelfHistoryDelegate = nullptr;
    QLabel *_detailFeedbackLabel = nullptr;

    // Requests page controls
    QWidget *_requestsPage = nullptr;
    QLineEdit *_quickAddAccountEdit = nullptr;
    QLineEdit *_quickAddGreetingEdit = nullptr;
    QPushButton *_quickAddSendBtn = nullptr;
    QLabel *_quickAddStatusLabel = nullptr;
    QLabel *_requestsEmptyLabel = nullptr;
    QListView *_requestsListView = nullptr;
    ContactRequestListModel *_requestModel = nullptr;
    ContactRequestItemDelegate *_requestDelegate = nullptr;
};

} // namespace MeetingUI
