#pragma once

#include "src/net/contact_types.h"

#include <QtWidgets/QDialog>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QWidget;

namespace OpenMeeting {
class ContactCatalogController;
class SessionManager;
}

namespace MeetingUI {

class OrgHistoryItemDelegate;
class OrgHistoryListModel;

class UserProfileDialog final : public QDialog {
    Q_OBJECT
public:
    UserProfileDialog(
        OpenMeeting::ContactCatalogController &catalog,
        OpenMeeting::SessionManager &session,
        QWidget *parent = nullptr);

private:
    void populateFromCatalog(bool overwriteInputs);
    void handleSave();

    OpenMeeting::ContactCatalogController &_catalog;
    OpenMeeting::SessionManager &_session;

    QLabel *_accountValue = nullptr;
    QLabel *_employeeNoValue = nullptr;
    QLabel *_deptValue = nullptr;
    QLabel *_jobTitleValue = nullptr;

    QLineEdit *_nicknameEdit = nullptr;
    QLineEdit *_emailEdit = nullptr;
    QLineEdit *_phoneEdit = nullptr;
    QLineEdit *_signatureEdit = nullptr;
    QLabel *_statusLabel = nullptr;
    QPushButton *_saveBtn = nullptr;

    QWidget *_historySection = nullptr;
    QListView *_historyListView = nullptr;
    OrgHistoryListModel *_historyModel = nullptr;
    OrgHistoryItemDelegate *_historyDelegate = nullptr;

    bool _inputsDirty = false;
    bool _saving = false;
};

} // namespace MeetingUI
