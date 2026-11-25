#include "src/ui/contacts/user_profile_dialog.h"

#include "src/core/contact_catalog_controller.h"
#include "src/net/session_manager.h"
#include "src/ui/app_theme.h"
#include "src/ui/contacts/org_history_item_delegate.h"
#include "src/ui/contacts/org_history_list_model.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {

QString resolveDeptText(const OpenMeeting::UserProfile &profile, const std::vector<OpenMeeting::OrgDepartment> &depts) {
    if (!profile.departmentNames.isEmpty()) {
        return profile.departmentNames.join(QStringLiteral(" / "));
    }
    if (profile.primaryDeptIds.isEmpty()) return {};
    QStringList names;
    for (const auto &id : profile.primaryDeptIds) {
        for (const auto &d : depts) {
            if (d.deptId == id && !d.name.isEmpty()) {
                names.push_back(d.name);
                break;
            }
        }
    }
    return names.join(QStringLiteral(" / "));
}

} // namespace

UserProfileDialog::UserProfileDialog(
        OpenMeeting::ContactCatalogController &catalog,
        OpenMeeting::SessionManager &session,
        QWidget *parent)
    : QDialog(parent)
    , _catalog(catalog)
    , _session(session) {
    setWindowTitle(QCoreApplication::translate("MeetingUI", "Personal Profile"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setModal(true);
    setMinimumWidth(480);
    resize(520, 560);

    const auto tone = Theme::effectiveTheme() == Theme::Theme::Dark
        ? Theme::Tone::Dark : Theme::Tone::Light;
    Theme::setTone(*this, tone);

    const auto windowBg = Theme::color(Theme::ColorRole::Window).name();
    const auto surface = Theme::color(Theme::ColorRole::Surface).name();
    const auto surfaceAlt = Theme::color(Theme::ColorRole::SurfaceAlt).name();
    const auto border = Theme::color(Theme::ColorRole::Border).name();
    const auto text = Theme::color(Theme::ColorRole::Text).name();
    const auto textSec = Theme::color(Theme::ColorRole::TextSecondary).name();
    const auto accent = Theme::color(Theme::ColorRole::Accent).name();
    const auto onAccent = Theme::color(Theme::ColorRole::OnAccent).name();

    setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %5; }"
        "QFrame#profileOrgCard, QFrame#profileEditCard { background: %2; border: 1px solid %4; border-radius: 10px; }"
        "QLabel#profileHintLabel { color: %6; font-size: 12px; }"
        "QLabel#profileSectionTitle { color: %5; font-size: 13px; font-weight: 600; }"
        "QLineEdit { background: %3; color: %5; border: 1px solid %4; border-radius: 7px; padding: 6px 10px; }"
        "QLineEdit:focus { border-color: %7; background: %2; }"
        "QPushButton#profileSaveBtn { background: %7; color: %8; border: none; border-radius: 8px; padding: 7px 18px; font-weight: 600; }"
        "QPushButton#profileSaveBtn:disabled { opacity: 0.6; }"
        "QPushButton#profileCloseBtn { background: %2; color: %5; border: 1px solid %4; border-radius: 8px; padding: 7px 16px; }"
        "QListView#profileOrgHistoryView { background: transparent; border: none; outline: none; }")
        .arg(windowBg, surface, surfaceAlt, border, text, textSec, accent, onAccent));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(14);

    // 1. Read-only Org / Identity card
    auto *orgCard = new QFrame(this);
    orgCard->setObjectName(QStringLiteral("profileOrgCard"));
    auto *orgForm = new QFormLayout(orgCard);
    orgForm->setContentsMargins(16, 14, 16, 14);
    orgForm->setHorizontalSpacing(18);
    orgForm->setVerticalSpacing(8);

    auto makeReadOnlyLabel = [orgCard]() {
        auto *lbl = new QLabel(orgCard);
        lbl->setTextFormat(Qt::PlainText);
        lbl->setWordWrap(true);
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return lbl;
    };
    _accountValue = makeReadOnlyLabel();
    _employeeNoValue = makeReadOnlyLabel();
    _deptValue = makeReadOnlyLabel();
    _jobTitleValue = makeReadOnlyLabel();

    orgForm->addRow(QCoreApplication::translate("MeetingUI", "Account"), _accountValue);
    orgForm->addRow(QCoreApplication::translate("MeetingUI", "Employee No."), _employeeNoValue);
    orgForm->addRow(QCoreApplication::translate("MeetingUI", "Department"), _deptValue);
    orgForm->addRow(QCoreApplication::translate("MeetingUI", "Job Title"), _jobTitleValue);

    auto *orgHint = new QLabel(
        QCoreApplication::translate("MeetingUI", "Employee No. is assigned automatically upon joining an organization and cannot be changed. Job title can only be modified by the organization founder."),
        orgCard);
    orgHint->setObjectName(QStringLiteral("profileHintLabel"));
    orgHint->setWordWrap(true);
    orgForm->addRow(orgHint);
    root->addWidget(orgCard);

    // 2. Editable Personal Information card (available whether in org or not)
    auto *editCard = new QFrame(this);
    editCard->setObjectName(QStringLiteral("profileEditCard"));
    auto *editForm = new QFormLayout(editCard);
    editForm->setContentsMargins(16, 14, 16, 14);
    editForm->setHorizontalSpacing(18);
    editForm->setVerticalSpacing(10);

    _nicknameEdit = new QLineEdit(editCard);
    _nicknameEdit->setMaxLength(64);
    _nicknameEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter display name"));

    _emailEdit = new QLineEdit(editCard);
    _emailEdit->setMaxLength(128);
    _emailEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter email address (optional)"));

    _phoneEdit = new QLineEdit(editCard);
    _phoneEdit->setMaxLength(32);
    _phoneEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter phone number (optional)"));

    _signatureEdit = new QLineEdit(editCard);
    _signatureEdit->setMaxLength(128);
    _signatureEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Write a short bio (optional)"));

    auto markDirty = [this] { _inputsDirty = true; };
    connect(_nicknameEdit, &QLineEdit::textEdited, this, markDirty);
    connect(_emailEdit, &QLineEdit::textEdited, this, markDirty);
    connect(_phoneEdit, &QLineEdit::textEdited, this, markDirty);
    connect(_signatureEdit, &QLineEdit::textEdited, this, markDirty);

    editForm->addRow(QCoreApplication::translate("MeetingUI", "Nickname"), _nicknameEdit);
    editForm->addRow(QCoreApplication::translate("MeetingUI", "Email"), _emailEdit);
    editForm->addRow(QCoreApplication::translate("MeetingUI", "Phone"), _phoneEdit);
    editForm->addRow(QCoreApplication::translate("MeetingUI", "Bio"), _signatureEdit);
    root->addWidget(editCard);

    // 3. Self-only Historical Employee Numbers & Org Removal Records (View + Delegate + Model)
    _historySection = new QWidget(this);
    auto *histLayout = new QVBoxLayout(_historySection);
    histLayout->setContentsMargins(0, 0, 0, 0);
    histLayout->setSpacing(6);

    auto *histTitle = new QLabel(
        QCoreApplication::translate("MeetingUI", "My Employee No. & Organization History (Only Visible to You)"),
        _historySection);
    histTitle->setObjectName(QStringLiteral("profileSectionTitle"));
    histLayout->addWidget(histTitle);

    _historyModel = new OrgHistoryListModel(this);
    _historyDelegate = new OrgHistoryItemDelegate(this);
    _historyListView = new QListView(_historySection);
    _historyListView->setObjectName(QStringLiteral("profileOrgHistoryView"));
    _historyListView->setModel(_historyModel);
    _historyListView->setItemDelegate(_historyDelegate);
    _historyListView->setSelectionMode(QAbstractItemView::NoSelection);
    _historyListView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _historyListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _historyListView->setFrameShape(QFrame::NoFrame);
    _historyListView->setMinimumHeight(110);
    histLayout->addWidget(_historyListView, 1);

    _historySection->hide();
    root->addWidget(_historySection, 1);

    // Status + Footer Buttons
    _statusLabel = new QLabel(this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setTextFormat(Qt::PlainText);
    _statusLabel->hide();
    root->addWidget(_statusLabel);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto *closeBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Close"), this);
    closeBtn->setObjectName(QStringLiteral("profileCloseBtn"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    _saveBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save Changes"), this);
    _saveBtn->setObjectName(QStringLiteral("profileSaveBtn"));
    _saveBtn->setCursor(Qt::PointingHandCursor);
    connect(_saveBtn, &QPushButton::clicked, this, [this] { handleSave(); });

    btnRow->addWidget(closeBtn);
    btnRow->addWidget(_saveBtn);
    root->addLayout(btnRow);

    connect(&_catalog, &OpenMeeting::ContactCatalogController::selfProfileChanged, this, [this] {
        populateFromCatalog(!_inputsDirty);
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::orgTreeChanged, this, [this] {
        populateFromCatalog(false);
    });

    populateFromCatalog(true);
    _catalog.fetchUserProfile(_session.userId());
}

void UserProfileDialog::populateFromCatalog(bool overwriteInputs) {
    OpenMeeting::UserProfile prof;
    if (_catalog.selfProfile()) {
        prof = *_catalog.selfProfile();
    } else if (const auto cached = _catalog.findProfile(_session.userId())) {
        prof = *cached;
    } else {
        prof.userId = _session.userId();
        prof.nickname = _session.nickname();
        prof.account = _session.savedAccount();
    }

    _accountValue->setText(prof.account.isEmpty() ? QStringLiteral("—") : prof.account);
    _employeeNoValue->setText(prof.employeeNo.isEmpty() ? QStringLiteral("—") : prof.employeeNo);
    const QString deptText = resolveDeptText(prof, _catalog.departments());
    _deptValue->setText(deptText.isEmpty() ? QStringLiteral("—") : deptText);
    _jobTitleValue->setText(prof.jobTitle.isEmpty() ? QStringLiteral("—") : prof.jobTitle);

    if (overwriteInputs) {
        _nicknameEdit->setText(prof.nickname);
        _emailEdit->setText(prof.email);
        _phoneEdit->setText(prof.phone);
        _signatureEdit->setText(prof.signature);
        _inputsDirty = false;
    }

    const auto &history = _catalog.selfOrgHistory();
    _historyModel->setRecords(history);
    _historySection->setVisible(!history.empty());
}

void UserProfileDialog::handleSave() {
    if (_saving) return;
    const QString nickname = _nicknameEdit->text().trimmed();
    if (nickname.isEmpty()) {
        _statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::color(Theme::ColorRole::Danger).name()));
        _statusLabel->setText(QCoreApplication::translate("MeetingUI", "Nickname cannot be empty."));
        _statusLabel->show();
        return;
    }

    _saving = true;
    _saveBtn->setEnabled(false);
    _statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::color(Theme::ColorRole::TextSecondary).name()));
    _statusLabel->setText(QCoreApplication::translate("MeetingUI", "Saving profile..."));
    _statusLabel->show();

    const QPointer<UserProfileDialog> self(this);
    _catalog.updateSelfProfile(
        nickname,
        _signatureEdit->text().trimmed(),
        _emailEdit->text().trimmed(),
        _phoneEdit->text().trimmed(),
        [self](bool ok, const OpenMeeting::UserProfile &, const OpenMeeting::HttpError &err) {
            if (!self) return;
            self->_saving = false;
            self->_saveBtn->setEnabled(true);
            if (ok) {
                self->_inputsDirty = false;
                self->_statusLabel->setStyleSheet(QStringLiteral("color: %1;")
                    .arg(Theme::color(Theme::ColorRole::Accent).name()));
                self->_statusLabel->setText(QCoreApplication::translate("MeetingUI", "Personal profile saved."));
                self->_statusLabel->show();
                self->populateFromCatalog(true);
            } else {
                self->_statusLabel->setStyleSheet(QStringLiteral("color: %1;")
                    .arg(Theme::color(Theme::ColorRole::Danger).name()));
                self->_statusLabel->setText(err.message.isEmpty()
                    ? QCoreApplication::translate("MeetingUI", "Failed to save profile.")
                    : err.message);
                self->_statusLabel->show();
            }
        });
}

} // namespace MeetingUI
