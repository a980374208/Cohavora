#pragma once

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QTreeView;

namespace OpenMeeting {
class ContactCatalogController;
class PresenceWatchController;
}

namespace MeetingUI {

class ContactItemDelegate;
class ContactListModel;
class OrgTreeModel;

// Reusable multi-select or single-select contact picker for MeetingBookingDialog and InMeetingInviteDialog.
class ContactPickerDialog final : public QDialog {
    Q_OBJECT
public:
    ContactPickerDialog(
        OpenMeeting::ContactCatalogController &catalog,
        OpenMeeting::PresenceWatchController *presence,
        const QString &title,
        const QString &confirmText,
        const QStringList &preselectedUserIds = {},
        const QSet<QString> &excludedUserIds = {},
        QWidget *parent = nullptr);

    QStringList selectedUserIds() const;

private:
    void populateList();
    void rebuildOrgTree();
    void updateSelectionSummary();

    OpenMeeting::ContactCatalogController &_catalog;
    OpenMeeting::PresenceWatchController *_presence = nullptr;
    QSet<QString> _selectedIds;
    QSet<QString> _excludedIds;
    QString _selectedDeptId;

    QLineEdit *_searchEdit = nullptr;
    QTreeView *_orgTreeView = nullptr;
    OrgTreeModel *_orgTreeModel = nullptr;
    QListView *_contactListView = nullptr;
    ContactListModel *_contactModel = nullptr;
    ContactItemDelegate *_contactDelegate = nullptr;
    QLabel *_summaryLabel = nullptr;
    QPushButton *_loadMoreBtn = nullptr;
    QPushButton *_confirmBtn = nullptr;
};

} // namespace MeetingUI
