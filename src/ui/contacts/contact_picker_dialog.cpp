#include "src/ui/contacts/contact_picker_dialog.h"

#include "src/core/contact_catalog_controller.h"
#include "src/core/presence_watch_controller.h"
#include "src/ui/app_theme.h"
#include "src/ui/contacts/contact_item_delegate.h"
#include "src/ui/contacts/contact_list_model.h"
#include "src/ui/contacts/org_tree_model.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

ContactPickerDialog::ContactPickerDialog(
        OpenMeeting::ContactCatalogController &catalog,
        OpenMeeting::PresenceWatchController *presence,
        const QString &title,
        const QString &confirmText,
        const QStringList &preselectedUserIds,
        const QSet<QString> &excludedUserIds,
        QWidget *parent)
    : QDialog(parent)
    , _catalog(catalog)
    , _presence(presence)
    , _excludedIds(excludedUserIds) {
    for (const auto &id : preselectedUserIds) {
        if (!id.trimmed().isEmpty() && !_excludedIds.contains(id.trimmed())) {
            _selectedIds.insert(id.trimmed());
        }
    }

    setWindowTitle(title);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setModal(true);
    resize(680, 500);
    AppTheme::setTone(*this, AppTheme::effectiveTheme() == AppTheme::Theme::Dark
        ? AppTheme::Tone::Dark : AppTheme::Tone::Light);

    const auto surface = Theme::color(Theme::ColorRole::Surface).name();
    const auto surfaceAlt = Theme::color(Theme::ColorRole::SurfaceAlt).name();
    const auto border = Theme::color(Theme::ColorRole::Border).name();
    const auto text = Theme::color(Theme::ColorRole::Text).name();
    const auto accent = Theme::color(Theme::ColorRole::Accent).name();
    const auto accentSoft = Theme::color(Theme::ColorRole::AccentSoft).name();

    setStyleSheet(QStringLiteral(
        "QTreeView#contactPickerOrgTree { background: %1; border: 1px solid %2; border-radius: 8px; color: %3; outline: none; padding: 4px; }"
        "QTreeView#contactPickerOrgTree::item { height: 28px; border-radius: 6px; padding: 0 6px; }"
        "QTreeView#contactPickerOrgTree::item:hover { background: %4; }"
        "QTreeView#contactPickerOrgTree::item:selected { background: %5; color: %6; font-weight: 600; }"
        "QListView#contactPickerListView { background: %1; border: 1px solid %2; border-radius: 8px; outline: none; padding: 2px; }")
        .arg(surfaceAlt, border, text, surface, accentSoft, accent));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    _searchEdit = new QLineEdit(this);
    _searchEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search contacts or organization members..."));
    _searchEdit->setClearButtonEnabled(true);
    connect(_searchEdit, &QLineEdit::textChanged, this, [this](const QString &) { populateList(); });
    root->addWidget(_searchEdit);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    _orgTreeModel = new OrgTreeModel(this);
    _orgTreeView = new QTreeView(splitter);
    _orgTreeView->setObjectName(QStringLiteral("contactPickerOrgTree"));
    _orgTreeView->setModel(_orgTreeModel);
    _orgTreeView->setHeaderHidden(true);
    _orgTreeView->setUniformRowHeights(true);
    _orgTreeView->setSelectionMode(QAbstractItemView::SingleSelection);
    _orgTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _orgTreeView->setMinimumWidth(200);
    connect(_orgTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex &current, const QModelIndex &) {
            if (!current.isValid()) return;
            _selectedDeptId = current.data(OrgDeptIdRole).toString();
            if (!_selectedDeptId.isEmpty() &&
                _selectedDeptId != QStringLiteral("__org_root__")) {
                _catalog.ensureDeptMembersLoaded(_selectedDeptId);
            }
            populateList();
        });

    _contactModel = new ContactListModel(this);
    _contactModel->setCheckable(true);
    _contactDelegate = new ContactItemDelegate(this);
    connect(_contactModel, &ContactListModel::checkStateChanged, this,
        [this](const QString &uid, bool checked) {
            if (uid.isEmpty()) return;
            if (checked) _selectedIds.insert(uid);
            else _selectedIds.remove(uid);
            updateSelectionSummary();
        });

    _contactListView = new QListView(splitter);
    _contactListView->setObjectName(QStringLiteral("contactPickerListView"));
    _contactListView->setModel(_contactModel);
    _contactListView->setItemDelegate(_contactDelegate);
    _contactListView->setUniformItemSizes(true);
    _contactListView->setSelectionMode(QAbstractItemView::SingleSelection);
    _contactListView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _contactListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _contactListView->setMouseTracking(true);

    splitter->addWidget(_orgTreeView);
    splitter->addWidget(_contactListView);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    auto *footer = new QHBoxLayout();
    _summaryLabel = new QLabel(this);
    footer->addWidget(_summaryLabel, 1);

    _loadMoreBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Load more members"), this);
    _loadMoreBtn->setAutoDefault(false);
    _loadMoreBtn->hide();
    connect(_loadMoreBtn, &QPushButton::clicked, this, [this] {
        _catalog.loadMoreDeptMembers(_selectedDeptId);
    });
    footer->addWidget(_loadMoreBtn);

    auto *cancelBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    footer->addWidget(cancelBtn);

    _confirmBtn = new QPushButton(confirmText, this);
    _confirmBtn->setDefault(true);
    connect(_confirmBtn, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(_confirmBtn);
    root->addLayout(footer);

    connect(&_catalog, &OpenMeeting::ContactCatalogController::orgTreeChanged, this,
            [this]() { rebuildOrgTree(); });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::deptMembersChanged, this,
            [this](const QString &deptId) {
                if (deptId == _selectedDeptId) populateList();
            });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::contactsChanged, this,
            [this]() { populateList(); });
    if (_presence) {
        connect(_presence, &OpenMeeting::PresenceWatchController::presencesUpdated, this,
                [this](const QStringList &userIds) {
                    if (!_contactModel || !_presence) return;
                    QHash<QString, OpenMeeting::PresenceStatus> updates;
                    updates.reserve(userIds.size());
                    for (const auto &uid : userIds) {
                        updates.insert(uid, _presence->statusFor(uid));
                    }
                    _contactModel->updatePresences(updates);
                });
    }

    rebuildOrgTree();
    populateList();
    updateSelectionSummary();
}

QStringList ContactPickerDialog::selectedUserIds() const {
    return QStringList(_selectedIds.begin(), _selectedIds.end());
}

void ContactPickerDialog::rebuildOrgTree() {
    if (!_orgTreeView || !_orgTreeModel) return;
    _orgTreeModel->setDepartments(
        _catalog.departments(),
        /*includeVirtualRoots=*/true,
        QCoreApplication::translate("MeetingUI", "My Contacts"),
        QCoreApplication::translate("MeetingUI", "Organization"));
    if (const QModelIndex orgRoot = _orgTreeModel->indexForVirtualNode(OrgTreeNodeKind::OrgRoot);
        orgRoot.isValid()) {
        _orgTreeView->expand(orgRoot);
    }
    QModelIndex target = _orgTreeModel->indexForDeptId(_selectedDeptId);
    if (!target.isValid()) {
        _selectedDeptId.clear();
        target = _orgTreeModel->firstSelectableIndex();
    }
    if (target.isValid()) {
        _orgTreeView->setCurrentIndex(target);
        _selectedDeptId = target.data(OrgDeptIdRole).toString();
    }
    if (!_selectedDeptId.isEmpty() && _selectedDeptId != QStringLiteral("__org_root__")) {
        _catalog.ensureDeptMembersLoaded(_selectedDeptId);
    }
    populateList();
}

void ContactPickerDialog::populateList() {
    if (!_contactModel) return;
    const QString filter = _searchEdit->text().trimmed().toLower();
    QSet<QString> added;
    std::vector<ContactListEntry> entries;

    auto appendEntry = [&](const QString &userId, const QString &name, const QString &sub,
                           bool isFavorite, bool isLeader) {
        if (userId.isEmpty() || _excludedIds.contains(userId) || added.contains(userId)) return;
        if (!filter.isEmpty() && !name.toLower().contains(filter) && !sub.toLower().contains(filter)) {
            return;
        }
        added.insert(userId);
        ContactListEntry e;
        e.userId = userId;
        e.displayName = name;
        e.subtitle = sub;
        e.isFavorite = isFavorite;
        e.isLeader = isLeader;
        e.checked = _selectedIds.contains(userId);
        e.presence = _presence ? _presence->statusFor(userId) : OpenMeeting::PresenceStatus::Offline;
        entries.push_back(std::move(e));
    };

    if (_selectedDeptId.isEmpty() || _selectedDeptId == QStringLiteral("__org_root__")) {
        for (const auto &rel : _catalog.contacts()) {
            if (rel.isBlocked) continue;
            const QString sub = rel.profile
                ? (!rel.profile->jobTitle.isEmpty() ? rel.profile->jobTitle : rel.profile->account)
                : QString();
            appendEntry(rel.targetUserId, rel.effectiveDisplayName(), sub, rel.isFavorite, false);
        }
        if (!filter.isEmpty()) {
            for (const auto &d : _catalog.departments()) {
                for (const auto &m : _catalog.deptMembers(d.deptId)) {
                    const QString sub = !m.profile.jobTitle.isEmpty() ? m.profile.jobTitle : m.profile.account;
                    appendEntry(m.profile.userId, m.profile.displayName(), sub, false, m.isLeader);
                }
            }
        }
    } else {
        QStringList watchIds;
        for (const auto &m : _catalog.deptMembers(_selectedDeptId)) {
            watchIds.push_back(m.profile.userId);
            const QString sub = !m.profile.jobTitle.isEmpty() ? m.profile.jobTitle : m.profile.account;
            appendEntry(m.profile.userId, m.profile.displayName(), sub, false, m.isLeader);
        }
        if (_presence && !watchIds.isEmpty()) {
            _presence->setViewportWatchedUsers(watchIds);
        }
    }

    _contactModel->setEntries(std::move(entries));
    const bool loading = _catalog.isDeptMembersLoading(_selectedDeptId);
    _loadMoreBtn->setVisible(_catalog.hasMoreDeptMembers(_selectedDeptId));
    _loadMoreBtn->setEnabled(!loading);
    _loadMoreBtn->setText(loading
        ? QCoreApplication::translate("MeetingUI", "Loading members...")
        : QCoreApplication::translate("MeetingUI", "Load more members"));
}

void ContactPickerDialog::updateSelectionSummary() {
    _summaryLabel->setText(QCoreApplication::translate("MeetingUI", "Selected: %1").arg(_selectedIds.size()));
}

} // namespace MeetingUI
