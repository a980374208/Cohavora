#include "src/ui/contacts/contacts_page_widget.h"

#include "src/core/contact_catalog_controller.h"
#include "src/core/presence_watch_controller.h"
#include "src/net/session_manager.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/ui/contacts/contact_item_delegate.h"
#include "src/ui/contacts/contact_list_model.h"
#include "src/ui/contacts/contact_request_item_delegate.h"
#include "src/ui/contacts/contact_request_list_model.h"
#include "src/ui/contacts/contacts_tree_delegate.h"
#include "src/ui/contacts/contacts_tree_model.h"
#include "src/ui/contacts/initials_avatar_painter.h"
#include "src/ui/contacts/org_history_item_delegate.h"
#include "src/ui/contacts/org_history_list_model.h"
#include "src/ui/contacts/user_profile_dialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QAction>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {

constexpr auto kGroupIdStarred = "__sys_starred__";
constexpr auto kGroupIdMyContacts = "__sys_my_contacts__";
constexpr auto kGroupIdBlocked = "__sys_blocked__";

AppTheme::Tone currentThemeTone() {
    return AppTheme::effectiveTheme() == AppTheme::Theme::Dark
        ? AppTheme::Tone::Dark
        : AppTheme::Tone::Light;
}

QString formatPresenceBadge(OpenMeeting::PresenceStatus status) {
    return QStringLiteral("● %1").arg(InitialsAvatarPainter::presenceLabel(status));
}

QIcon makeSearchIcon(const QColor &color) {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(2.0);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(2.5, 2.5, 8.5, 8.5));
    p.drawLine(QPointF(9.9, 9.9), QPointF(13.5, 13.5));
    return QIcon(pm);
}

QString deptNamesForProfile(
    const OpenMeeting::UserProfile &profile,
    const std::vector<OpenMeeting::OrgDepartment> &departments) {
    if (!profile.departmentNames.isEmpty()) {
        return profile.departmentNames.join(QStringLiteral(" / "));
    }
    if (profile.primaryDeptIds.isEmpty()) {
        return {};
    }
    QHash<QString, QString> byId;
    for (const auto &dept : departments) {
        byId.insert(dept.deptId, dept.name);
    }
    QStringList names;
    for (const auto &id : profile.primaryDeptIds) {
        const auto name = byId.value(id);
        if (!name.isEmpty()) {
            names.push_back(name);
        }
    }
    return names.join(QStringLiteral(" / "));
}

} // namespace

ContactsPageWidget::ContactsPageWidget(
    OpenMeeting::ContactCatalogController &catalog,
    OpenMeeting::PresenceWatchController &presence,
    OpenMeeting::SessionManager &session,
    QWidget *parent)
    : QWidget(parent)
    , _catalog(catalog)
    , _presence(presence)
    , _session(session) {
    setObjectName(QStringLiteral("contactsPageWidget"));
    _searchDebounce.setSingleShot(true);
    _searchDebounce.setInterval(250);
    connect(&_searchDebounce, &QTimer::timeout, this, [this] {
        if (_searchKeyword.isEmpty()) {
            _catalog.clearSearchResults();
            updateLeftViewMode();
            return;
        }
        _catalog.searchDirectory(_searchKeyword);
    });

    initUi();

    connect(&_session, &OpenMeeting::SessionManager::authenticationReset, this, [this] {
        _quickAddAccountEdit->clear();
        _quickAddGreetingEdit->clear();
        _quickAddSendBtn->setEnabled(true);
        _quickAddStatusLabel->hide();
    });

    connect(&_catalog, &OpenMeeting::ContactCatalogController::contactsChanged, this, [this] {
        rebuildContactsTree();
        if (!_searchKeyword.isEmpty()) {
            rebuildSearchList();
        }
        if (!_selectedUserId.isEmpty() && _rightStack->currentIndex() == 1) {
            showContactDetail(_selectedUserId, false);
        }
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::groupsChanged, this, [this] {
        rebuildContactsTree();
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::orgTreeChanged, this, [this] {
        updateOrgTabVisibility();
        rebuildOrgTree();
        if (!_selectedUserId.isEmpty() && _rightStack->currentIndex() == 1) {
            showContactDetail(_selectedUserId, true);
        }
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::deptMembersChanged, this, [this](const QString &deptId) {
        applyDeptMembersToTree(deptId);
        if (!_selectedUserId.isEmpty() && _rightStack->currentIndex() == 1) {
            showContactDetail(_selectedUserId, false);
        }
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::userProfileUpdated, this, [this](const QString &userId) {
        rebuildContactsTree();
        for (const auto &deptId : _expandedDeptIds) {
            if (_orgDirModel && _orgDirModel->isDeptLoaded(deptId)) {
                applyDeptMembersToTree(deptId);
            }
        }
        if (!_searchKeyword.isEmpty()) {
            rebuildSearchList();
        }
        if (!_selectedUserId.isEmpty() && _selectedUserId == userId && _rightStack->currentIndex() == 1) {
            showContactDetail(_selectedUserId, false);
        }
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::selfProfileChanged, this, [this] {
        if (!_selectedUserId.isEmpty() && _selectedUserId == _session.userId() && _rightStack->currentIndex() == 1) {
            showContactDetail(_selectedUserId, false);
        }
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::requestsChanged, this, [this] {
        updateRequestsBadge();
        rebuildRequestsView();
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::pendingRequestCountChanged, this, [this](qint64) {
        updateRequestsBadge();
    });
    connect(&_catalog, &OpenMeeting::ContactCatalogController::searchResultsChanged, this, [this] {
        if (!_searchKeyword.isEmpty()) {
            rebuildSearchList();
        }
    });
    connect(&_presence, &OpenMeeting::PresenceWatchController::presencesUpdated, this, [this](const QStringList &userIds) {
        QHash<QString, OpenMeeting::PresenceStatus> updates;
        updates.reserve(userIds.size());
        for (const auto &uid : userIds) {
            updates.insert(uid, _presence.statusFor(uid));
        }
        if (_contactsTreeModel) _contactsTreeModel->updatePresences(updates);
        if (_orgDirModel) _orgDirModel->updatePresences(updates);
        if (_searchModel) _searchModel->updatePresences(updates);
        if (!_selectedUserId.isEmpty() && userIds.contains(_selectedUserId)) {
            updateDetailPresence();
        }
    });

    updateOrgTabVisibility();
    rebuildContactsTree();
    rebuildOrgTree();
    rebuildRequestsView();
    updateRequestsBadge();
    updateLeftViewMode();
    refreshTheme();
}

void ContactsPageWidget::initUi() {
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ------------------------------------------------------------
    // Left Navigation + Collapsible Tree Pane (Figure 2 clean layout)
    // ------------------------------------------------------------
    auto *leftPane = new QFrame(this);
    leftPane->setObjectName(QStringLiteral("contactsLeftPane"));
    leftPane->setFixedWidth(304);
    auto *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // 1. Top Search Bar + '+' Quick Action Button
    auto *topSearchWrap = new QWidget(leftPane);
    auto *searchRow = new QHBoxLayout(topSearchWrap);
    searchRow->setContentsMargins(12, 12, 12, 8);
    searchRow->setSpacing(8);

    _searchEdit = new QLineEdit(topSearchWrap);
    _searchEdit->setObjectName(QStringLiteral("contactsSearchEdit"));
    _searchEdit->setClearButtonEnabled(true);
    _searchEdit->setFixedHeight(32);
    _searchIconAction = _searchEdit->addAction(
        makeSearchIcon(Theme::color(Theme::ColorRole::TextMuted)),
        QLineEdit::LeadingPosition);
    connect(_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        _searchKeyword = text.trimmed();
        updateLeftViewMode();
        if (_searchKeyword.isEmpty()) {
            _searchDebounce.stop();
            _catalog.clearSearchResults();
            return;
        }
        _searchDebounce.start();
    });
    searchRow->addWidget(_searchEdit, 1);

    _plusMenuBtn = new QPushButton(QStringLiteral("+"), topSearchWrap);
    _plusMenuBtn->setObjectName(QStringLiteral("contactsPlusBtn"));
    _plusMenuBtn->setCursor(Qt::PointingHandCursor);
    _plusMenuBtn->setFixedSize(32, 32);
    connect(_plusMenuBtn, &QPushButton::clicked, this, [this] { handlePlusMenu(); });
    searchRow->addWidget(_plusMenuBtn);
    leftLayout->addWidget(topSearchWrap);

    // 2. Entry Row: New Requests (with badge + right chevron)
    auto *entryWrap = new QWidget(leftPane);
    auto *entryWrapLayout = new QVBoxLayout(entryWrap);
    entryWrapLayout->setContentsMargins(8, 0, 8, 8);
    entryWrapLayout->setSpacing(2);

    _requestsEntryBtn = new QPushButton(entryWrap);
    _requestsEntryBtn->setObjectName(QStringLiteral("contactsRequestsEntry"));
    _requestsEntryBtn->setCheckable(true);
    _requestsEntryBtn->setCursor(Qt::PointingHandCursor);
    _requestsEntryBtn->setFixedHeight(34);
    auto *entryRowLayout = new QHBoxLayout(_requestsEntryBtn);
    entryRowLayout->setContentsMargins(10, 0, 10, 0);
    entryRowLayout->setSpacing(6);

    _requestsEntryTitle = new QLabel(_requestsEntryBtn);
    _requestsEntryTitle->setObjectName(QStringLiteral("contactsRequestsEntryTitle"));
    _requestsEntryTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    entryRowLayout->addWidget(_requestsEntryTitle, 1);

    _requestsBadgeLabel = new QLabel(_requestsEntryBtn);
    _requestsBadgeLabel->setObjectName(QStringLiteral("contactsRequestsBadge"));
    _requestsBadgeLabel->setAlignment(Qt::AlignCenter);
    _requestsBadgeLabel->setFixedHeight(16);
    _requestsBadgeLabel->setMinimumWidth(16);
    _requestsBadgeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    _requestsBadgeLabel->hide();
    entryRowLayout->addWidget(_requestsBadgeLabel, 0, Qt::AlignVCenter);

    _requestsChevronLabel = new QLabel(QStringLiteral("›"), _requestsEntryBtn);
    _requestsChevronLabel->setObjectName(QStringLiteral("contactsRequestsChevron"));
    _requestsChevronLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    entryRowLayout->addWidget(_requestsChevronLabel);

    connect(_requestsEntryBtn, &QPushButton::clicked, this, [this] { openRequestsTab(); });
    entryWrapLayout->addWidget(_requestsEntryBtn);
    leftLayout->addWidget(entryWrap);

    // Subtle 1px divider
    _topDivider = new QFrame(leftPane);
    _topDivider->setObjectName(QStringLiteral("contactsSidebarDivider"));
    _topDivider->setFixedHeight(1);
    auto *divWrap = new QWidget(leftPane);
    auto *divLayout = new QVBoxLayout(divWrap);
    divLayout->setContentsMargins(12, 0, 12, 10);
    divLayout->setSpacing(0);
    divLayout->addWidget(_topDivider);
    leftLayout->addWidget(divWrap);

    // 3. Segmented 2-Tab Pill: Contacts | Org Tree (auto-hidden when user has no organization)
    _segTrack = new QFrame(leftPane);
    _segTrack->setObjectName(QStringLiteral("contactsSegTrack"));
    auto *segOuter = new QWidget(leftPane);
    auto *segOuterLayout = new QVBoxLayout(segOuter);
    segOuterLayout->setContentsMargins(12, 0, 12, 8);
    segOuterLayout->setSpacing(0);

    auto *segLayout = new QHBoxLayout(_segTrack);
    segLayout->setContentsMargins(3, 3, 3, 3);
    segLayout->setSpacing(3);
    auto makeSegBtn = [this](const QString &objName) {
        auto *btn = new QPushButton(_segTrack);
        btn->setObjectName(objName);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(28);
        return btn;
    };
    _segContactsBtn = makeSegBtn(QStringLiteral("contactsSegContacts"));
    _segOrgBtn = makeSegBtn(QStringLiteral("contactsSegOrg"));
    segLayout->addWidget(_segContactsBtn, 1);
    segLayout->addWidget(_segOrgBtn, 1);
    connect(_segContactsBtn, &QPushButton::clicked, this, [this] { setSidebarTab(SidebarTab::Contacts); });
    connect(_segOrgBtn, &QPushButton::clicked, this, [this] { setSidebarTab(SidebarTab::Organization); });

    segOuterLayout->addWidget(_segTrack);
    leftLayout->addWidget(segOuter);

    // 4. Unified Left Stack: Contacts Tree (0) / Org Directory Tree (1) / Search Results List (2)
    _leftStack = new QStackedWidget(leftPane);
    _leftStack->setObjectName(QStringLiteral("contactsLeftStack"));

    // Page 0: Collapsible Contacts Tree (Starred -> My Contacts -> Custom Groups -> Blocked)
    _contactsTreeModel = new ContactsTreeModel(this);
    _contactsTreeView = new QTreeView(_leftStack);
    _contactsTreeView->setObjectName(QStringLiteral("contactsTreeView"));
    _contactsTreeDelegate = new ContactsTreeDelegate(_contactsTreeView, this);
    _contactsTreeView->setModel(_contactsTreeModel);
    _contactsTreeView->setItemDelegate(_contactsTreeDelegate);
    _contactsTreeView->setHeaderHidden(true);
    _contactsTreeView->setRootIsDecorated(false);
    _contactsTreeView->setIndentation(0);
    _contactsTreeView->setAnimated(false);
    _contactsTreeView->setUniformRowHeights(false);
    _contactsTreeView->setSelectionMode(QAbstractItemView::SingleSelection);
    _contactsTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _contactsTreeView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _contactsTreeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _contactsTreeView->setFrameShape(QFrame::NoFrame);
    _contactsTreeView->setMouseTracking(true);
    _contactsTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(_contactsTreeView, &QTreeView::expanded, this, [this](const QModelIndex &idx) {
        const QString gid = idx.data(TreeNodeIdRole).toString();
        if (!gid.isEmpty()) _expandedGroupIds.insert(gid);
        _contactsTreeView->viewport()->update();
    });
    connect(_contactsTreeView, &QTreeView::collapsed, this, [this](const QModelIndex &idx) {
        const QString gid = idx.data(TreeNodeIdRole).toString();
        if (!gid.isEmpty()) _expandedGroupIds.remove(gid);
        _contactsTreeView->viewport()->update();
    });
    connect(_contactsTreeView, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
        if (kind == ContactsTreeNodeKind::Group) {
            if (_contactsTreeView->isExpanded(index)) _contactsTreeView->collapse(index);
            else _contactsTreeView->expand(index);
            return;
        }
        if (kind == ContactsTreeNodeKind::Member) {
            const QString uid = index.data(TreeNodeIdRole).toString();
            if (!uid.isEmpty()) {
                _showingRequests = false;
                if (_requestsEntryBtn) _requestsEntryBtn->setChecked(false);
                showContactDetail(uid);
            }
        }
    });
    connect(_contactsTreeView, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
        if (kind != ContactsTreeNodeKind::Member) return;
        const QString uid = index.data(TreeNodeIdRole).toString();
        const bool isBlocked = index.data(TreeIsBlockedRole).toBool();
        if (!uid.isEmpty() && !isBlocked && uid != _session.userId()) {
            emit videoCallRequested(uid);
        }
    });
    connect(_contactsTreeView, &QTreeView::customContextMenuRequested, this,
        [this](const QPoint &pos) { handleContactsTreeContextMenu(pos); });
    _leftStack->addWidget(_contactsTreeView);

    // Page 1: Unified Collapsible Org Directory Tree (Departments + inline Members / Notice)
    _orgDirModel = new OrgDirectoryTreeModel(this);
    _orgTreeView = new QTreeView(_leftStack);
    _orgTreeView->setObjectName(QStringLiteral("contactsOrgTree"));
    _orgTreeDelegate = new ContactsTreeDelegate(_orgTreeView, this);
    _orgTreeView->setModel(_orgDirModel);
    _orgTreeView->setItemDelegate(_orgTreeDelegate);
    _orgTreeView->setHeaderHidden(true);
    _orgTreeView->setRootIsDecorated(false);
    _orgTreeView->setIndentation(0);
    _orgTreeView->setAnimated(false);
    _orgTreeView->setUniformRowHeights(false);
    _orgTreeView->setSelectionMode(QAbstractItemView::SingleSelection);
    _orgTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _orgTreeView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _orgTreeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _orgTreeView->setFrameShape(QFrame::NoFrame);
    _orgTreeView->setMouseTracking(true);

    connect(_orgTreeView, &QTreeView::expanded, this, [this](const QModelIndex &idx) {
        const auto kind = static_cast<ContactsTreeNodeKind>(idx.data(TreeNodeKindRole).toInt());
        if (kind == ContactsTreeNodeKind::Department) {
            const QString deptId = idx.data(TreeNodeIdRole).toString();
            if (!deptId.isEmpty()) {
                _expandedDeptIds.insert(deptId);
                if (!_orgDirModel->isDeptLoaded(deptId)) {
                    _orgDirModel->setDeptLoading(deptId);
                    _catalog.ensureDeptMembersLoaded(deptId);
                }
            }
        }
        _orgTreeView->viewport()->update();
    });
    connect(_orgTreeView, &QTreeView::collapsed, this, [this](const QModelIndex &idx) {
        const QString deptId = idx.data(TreeNodeIdRole).toString();
        if (!deptId.isEmpty()) _expandedDeptIds.remove(deptId);
        _orgTreeView->viewport()->update();
    });
    connect(_orgTreeView, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
        if (kind == ContactsTreeNodeKind::Department) {
            if (_orgTreeView->isExpanded(index)) _orgTreeView->collapse(index);
            else _orgTreeView->expand(index);
            return;
        }
        if (kind == ContactsTreeNodeKind::LoadMore) {
            _catalog.loadMoreDeptMembers(index.data(TreeNodeIdRole).toString());
            return;
        }
        if (kind == ContactsTreeNodeKind::Member) {
            const QString uid = index.data(TreeNodeIdRole).toString();
            if (!uid.isEmpty()) {
                _showingRequests = false;
                if (_requestsEntryBtn) _requestsEntryBtn->setChecked(false);
                showContactDetail(uid);
            }
        }
    });
    connect(_orgTreeView, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
        if (kind != ContactsTreeNodeKind::Member) return;
        const QString uid = index.data(TreeNodeIdRole).toString();
        const bool isBlocked = index.data(TreeIsBlockedRole).toBool();
        if (!uid.isEmpty() && !isBlocked && uid != _session.userId()) {
            emit videoCallRequested(uid);
        }
    });
    connect(_orgTreeView, &QTreeView::activated, this, [this](const QModelIndex &index) {
        if (static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt()) == ContactsTreeNodeKind::LoadMore) {
            _catalog.loadMoreDeptMembers(index.data(TreeNodeIdRole).toString());
        }
    });
    _leftStack->addWidget(_orgTreeView);

    // Page 2: Directory Search Results List
    _searchPage = new QWidget(_leftStack);
    auto *searchPageLayout = new QVBoxLayout(_searchPage);
    searchPageLayout->setContentsMargins(6, 0, 6, 8);
    searchPageLayout->setSpacing(6);

    _listStatusLabel = new QLabel(_searchPage);
    _listStatusLabel->setObjectName(QStringLiteral("contactsListStatus"));
    _listStatusLabel->setWordWrap(true);
    _listStatusLabel->setAlignment(Qt::AlignCenter);
    _listStatusLabel->hide();
    searchPageLayout->addWidget(_listStatusLabel);

    _searchModel = new ContactListModel(this);
    _searchDelegate = new ContactItemDelegate(this);
    _searchListView = new QListView(_searchPage);
    _searchListView->setObjectName(QStringLiteral("contactsListView"));
    _searchListView->setModel(_searchModel);
    _searchListView->setItemDelegate(_searchDelegate);
    _searchListView->setUniformItemSizes(true);
    _searchListView->setSelectionMode(QAbstractItemView::SingleSelection);
    _searchListView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _searchListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _searchListView->setFrameShape(QFrame::NoFrame);
    connect(_searchListView, &QListView::clicked, this, [this](const QModelIndex &index) {
        const auto *entry = _searchModel->entryAt(index.row());
        if (entry) {
            _showingRequests = false;
            if (_requestsEntryBtn) _requestsEntryBtn->setChecked(false);
            showContactDetail(entry->userId);
        }
    });
    connect(_searchListView, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto *entry = _searchModel->entryAt(index.row());
        if (entry && !entry->isBlocked && entry->userId != _session.userId()) {
            emit videoCallRequested(entry->userId);
        }
    });
    searchPageLayout->addWidget(_searchListView, 1);
    _leftStack->addWidget(_searchPage);

    leftLayout->addWidget(_leftStack, 1);
    rootLayout->addWidget(leftPane);

    auto *divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("contactsPaneDivider"));
    divider->setFixedWidth(1);
    rootLayout->addWidget(divider);

    // ------------------------------------------------------------
    // Right Detail / Requests / Empty Pane
    // ------------------------------------------------------------
    _rightStack = new QStackedWidget(this);
    _rightStack->setObjectName(QStringLiteral("contactsRightStack"));

    // Page 0: Empty placeholder
    auto *emptyPage = new QWidget(_rightStack);
    auto *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch(1);
    _emptyPlaceholder = new QLabel(emptyPage);
    _emptyPlaceholder->setObjectName(QStringLiteral("contactsEmptyPlaceholder"));
    _emptyPlaceholder->setAlignment(Qt::AlignCenter);
    _emptyPlaceholder->setWordWrap(true);
    emptyLayout->addWidget(_emptyPlaceholder);
    emptyLayout->addStretch(1);
    _rightStack->addWidget(emptyPage);

    // Page 1: Contact Detail Card
    auto *detailScroll = new QScrollArea(_rightStack);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setWidgetResizable(true);
    _detailPage = new QWidget(detailScroll);
    _detailPage->setObjectName(QStringLiteral("contactsDetailPage"));
    auto *detailOuter = new QVBoxLayout(_detailPage);
    detailOuter->setContentsMargins(32, 28, 32, 28);
    detailOuter->setSpacing(18);

    // Header row: Avatar + Name/Presence + Star/More
    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(16);
    _detailAvatarLabel = new QLabel(_detailPage);
    _detailAvatarLabel->setFixedSize(64, 64);
    headerRow->addWidget(_detailAvatarLabel);

    auto *titleCol = new QVBoxLayout();
    titleCol->setSpacing(4);
    _detailNameLabel = new QLabel(_detailPage);
    _detailNameLabel->setObjectName(QStringLiteral("contactDetailName"));
    auto nameFont = _detailNameLabel->font();
    nameFont.setPixelSize(20);
    nameFont.setBold(true);
    _detailNameLabel->setFont(nameFont);
    _detailNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleCol->addWidget(_detailNameLabel);

    _detailPresenceLabel = new QLabel(_detailPage);
    _detailPresenceLabel->setObjectName(QStringLiteral("contactDetailPresence"));
    titleCol->addWidget(_detailPresenceLabel);

    _detailSubtitleLabel = new QLabel(_detailPage);
    _detailSubtitleLabel->setObjectName(QStringLiteral("contactDetailSubtitle"));
    _detailSubtitleLabel->setWordWrap(true);
    titleCol->addWidget(_detailSubtitleLabel);
    headerRow->addLayout(titleCol, 1);

    _detailStarBtn = new QPushButton(_detailPage);
    _detailStarBtn->setObjectName(QStringLiteral("contactDetailStarBtn"));
    _detailStarBtn->setCursor(Qt::PointingHandCursor);
    _detailStarBtn->setFixedHeight(32);
    connect(_detailStarBtn, &QPushButton::clicked, this, [this] { handleToggleFavorite(); });
    headerRow->addWidget(_detailStarBtn, 0, Qt::AlignTop);

    _detailMoreBtn = new QPushButton(_detailPage);
    _detailMoreBtn->setObjectName(QStringLiteral("contactDetailMoreBtn"));
    _detailMoreBtn->setCursor(Qt::PointingHandCursor);
    _detailMoreBtn->setFixedSize(32, 32);
    connect(_detailMoreBtn, &QPushButton::clicked, this, [this] { handleMoreActionsMenu(); });
    headerRow->addWidget(_detailMoreBtn, 0, Qt::AlignTop);
    detailOuter->addLayout(headerRow);

    // Blocked banner
    _detailBlockedBanner = new QLabel(_detailPage);
    _detailBlockedBanner->setObjectName(QStringLiteral("contactDetailBlockedBanner"));
    _detailBlockedBanner->setWordWrap(true);
    _detailBlockedBanner->hide();
    detailOuter->addWidget(_detailBlockedBanner);

    // Action Buttons Row
    auto *actionsRow = new QHBoxLayout();
    actionsRow->setSpacing(10);
    _detailVoiceCallBtn = new QPushButton(_detailPage);
    _detailVoiceCallBtn->setObjectName(QStringLiteral("contactVoiceCallBtn"));
    _detailVoiceCallBtn->setCursor(Qt::PointingHandCursor);
    _detailVoiceCallBtn->setFixedHeight(38);
    connect(_detailVoiceCallBtn, &QPushButton::clicked, this, [this] {
        if (!_selectedUserId.isEmpty()) emit voiceCallRequested(_selectedUserId);
    });
    actionsRow->addWidget(_detailVoiceCallBtn);

    _detailVideoCallBtn = new QPushButton(_detailPage);
    _detailVideoCallBtn->setObjectName(QStringLiteral("contactVideoCallBtn"));
    _detailVideoCallBtn->setCursor(Qt::PointingHandCursor);
    _detailVideoCallBtn->setFixedHeight(38);
    connect(_detailVideoCallBtn, &QPushButton::clicked, this, [this] {
        if (!_selectedUserId.isEmpty()) emit videoCallRequested(_selectedUserId);
    });
    actionsRow->addWidget(_detailVideoCallBtn);

    _detailScheduleBtn = new QPushButton(_detailPage);
    _detailScheduleBtn->setObjectName(QStringLiteral("contactScheduleBtn"));
    _detailScheduleBtn->setCursor(Qt::PointingHandCursor);
    _detailScheduleBtn->setFixedHeight(38);
    connect(_detailScheduleBtn, &QPushButton::clicked, this, [this] {
        if (!_selectedUserId.isEmpty()) emit scheduleMeetingWithContactRequested(_selectedUserId);
    });
    actionsRow->addWidget(_detailScheduleBtn);

    _detailAddContactBtn = new QPushButton(_detailPage);
    _detailAddContactBtn->setObjectName(QStringLiteral("contactAddBtn"));
    _detailAddContactBtn->setCursor(Qt::PointingHandCursor);
    _detailAddContactBtn->setFixedHeight(38);
    connect(_detailAddContactBtn, &QPushButton::clicked, this, [this] { handleAddOrRequestContact(); });
    actionsRow->addWidget(_detailAddContactBtn);

    _detailUnblockBtn = new QPushButton(_detailPage);
    _detailUnblockBtn->setObjectName(QStringLiteral("contactUnblockBtn"));
    _detailUnblockBtn->setCursor(Qt::PointingHandCursor);
    _detailUnblockBtn->setFixedHeight(38);
    connect(_detailUnblockBtn, &QPushButton::clicked, this, [this] {
        if (_selectedUserId.isEmpty()) return;
        OpenMeeting::UpsertContactRelationRequest req;
        req.targetUserId = _selectedUserId;
        req.isBlocked = false;
        _catalog.upsertRelation(req);
    });
    actionsRow->addWidget(_detailUnblockBtn);

    _detailEditProfileBtn = new QPushButton(_detailPage);
    _detailEditProfileBtn->setObjectName(QStringLiteral("contactEditProfileBtn"));
    _detailEditProfileBtn->setCursor(Qt::PointingHandCursor);
    _detailEditProfileBtn->setFixedHeight(38);
    connect(_detailEditProfileBtn, &QPushButton::clicked, this, [this] {
        handleOpenSelfProfileDialog();
    });
    actionsRow->addWidget(_detailEditProfileBtn);

    actionsRow->addStretch(1);
    detailOuter->addLayout(actionsRow);

    // Info Card
    auto *infoCard = new QFrame(_detailPage);
    infoCard->setObjectName(QStringLiteral("contactDetailInfoCard"));
    _detailInfoForm = new QFormLayout(infoCard);
    _detailInfoForm->setContentsMargins(18, 16, 18, 16);
    _detailInfoForm->setHorizontalSpacing(20);
    _detailInfoForm->setVerticalSpacing(10);
    _detailInfoForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto makeValueLabel = [infoCard]() {
        auto *lbl = new QLabel(infoCard);
        lbl->setWordWrap(true);
        lbl->setTextFormat(Qt::PlainText);
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return lbl;
    };
    _detailAccountValue = makeValueLabel();
    _detailEmployeeNoValue = makeValueLabel();
    _detailDeptValue = makeValueLabel();
    _detailJobValue = makeValueLabel();
    _detailEmailValue = makeValueLabel();
    _detailPhoneValue = makeValueLabel();
    _detailSignatureValue = makeValueLabel();

    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Account"), _detailAccountValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Employee No."), _detailEmployeeNoValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Department"), _detailDeptValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Job Title"), _detailJobValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Email"), _detailEmailValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Phone"), _detailPhoneValue);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Bio"), _detailSignatureValue);

    // Remark editor row
    _remarkRow = new QWidget(infoCard);
    auto *remarkLayout = new QHBoxLayout(_remarkRow);
    remarkLayout->setContentsMargins(0, 0, 0, 0);
    remarkLayout->setSpacing(8);
    _remarkEdit = new QLineEdit(_remarkRow);
    _remarkEdit->setMaxLength(64);
    _remarkEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Set a personal alias / remark..."));
    _saveRemarkBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save"), _remarkRow);
    _saveRemarkBtn->setCursor(Qt::PointingHandCursor);
    connect(_saveRemarkBtn, &QPushButton::clicked, this, [this] { handleSaveRemark(); });
    connect(_remarkEdit, &QLineEdit::returnPressed, this, [this] { handleSaveRemark(); });
    remarkLayout->addWidget(_remarkEdit, 1);
    remarkLayout->addWidget(_saveRemarkBtn);
    _detailInfoForm->addRow(QCoreApplication::translate("MeetingUI", "Remark"), _remarkRow);

    detailOuter->addWidget(infoCard);

    // Self-only Historical Employee Numbers & Org Removal Reasons (MVD: QListView + Model + Delegate)
    _detailSelfHistorySection = new QWidget(_detailPage);
    auto *selfHistLayout = new QVBoxLayout(_detailSelfHistorySection);
    selfHistLayout->setContentsMargins(0, 4, 0, 0);
    selfHistLayout->setSpacing(8);
    _detailSelfHistoryTitle = new QLabel(_detailSelfHistorySection);
    _detailSelfHistoryTitle->setObjectName(QStringLiteral("contactDetailHistoryTitle"));
    auto histTitleFont = _detailSelfHistoryTitle->font();
    histTitleFont.setPixelSize(13);
    histTitleFont.setBold(true);
    _detailSelfHistoryTitle->setFont(histTitleFont);
    selfHistLayout->addWidget(_detailSelfHistoryTitle);

    _detailSelfHistoryModel = new OrgHistoryListModel(this);
    _detailSelfHistoryDelegate = new OrgHistoryItemDelegate(this);
    _detailSelfHistoryView = new QListView(_detailSelfHistorySection);
    _detailSelfHistoryView->setObjectName(QStringLiteral("contactDetailHistoryView"));
    _detailSelfHistoryView->setModel(_detailSelfHistoryModel);
    _detailSelfHistoryView->setItemDelegate(_detailSelfHistoryDelegate);
    _detailSelfHistoryView->setSelectionMode(QAbstractItemView::NoSelection);
    _detailSelfHistoryView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _detailSelfHistoryView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _detailSelfHistoryView->setFrameShape(QFrame::NoFrame);
    _detailSelfHistoryView->setMinimumHeight(110);
    _detailSelfHistoryView->setMaximumHeight(240);
    selfHistLayout->addWidget(_detailSelfHistoryView);
    _detailSelfHistorySection->hide();
    detailOuter->addWidget(_detailSelfHistorySection);

    _detailFeedbackLabel = new QLabel(_detailPage);
    _detailFeedbackLabel->setWordWrap(true);
    _detailFeedbackLabel->setTextFormat(Qt::PlainText);
    _detailFeedbackLabel->hide();
    detailOuter->addWidget(_detailFeedbackLabel);
    detailOuter->addStretch(1);

    detailScroll->setWidget(_detailPage);
    _rightStack->addWidget(detailScroll);

    // Page 2: Contact Requests & Add External Contact (View + Model list)
    _requestsPage = new QWidget(_rightStack);
    _requestsPage->setObjectName(QStringLiteral("contactsRequestsPage"));
    auto *reqOuter = new QVBoxLayout(_requestsPage);
    reqOuter->setContentsMargins(28, 24, 28, 24);
    reqOuter->setSpacing(14);

    auto *addCard = new QFrame(_requestsPage);
    addCard->setObjectName(QStringLiteral("contactQuickAddCard"));
    addCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto *addLayout = new QVBoxLayout(addCard);
    addLayout->setContentsMargins(16, 14, 16, 14);
    addLayout->setSpacing(10);
    auto *addTitle = new QLabel(QCoreApplication::translate("MeetingUI", "Add External Contact"), addCard);
    auto tf = addTitle->font();
    tf.setPixelSize(15);
    tf.setBold(true);
    addTitle->setFont(tf);
    addLayout->addWidget(addTitle);

    auto *addRow = new QHBoxLayout();
    addRow->setSpacing(8);
    _quickAddAccountEdit = new QLineEdit(addCard);
    _quickAddAccountEdit->setObjectName(QStringLiteral("contactQuickAddAccount"));
    _quickAddAccountEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter exact account, email, or employee no."));
    _quickAddGreetingEdit = new QLineEdit(addCard);
    _quickAddGreetingEdit->setObjectName(QStringLiteral("contactQuickAddGreeting"));
    _quickAddGreetingEdit->setMaxLength(120);
    _quickAddGreetingEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Greeting message (optional)"));
    _quickAddSendBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Send Request"), addCard);
    _quickAddSendBtn->setObjectName(QStringLiteral("contactQuickAddSend"));
    _quickAddSendBtn->setCursor(Qt::PointingHandCursor);
    _quickAddAccountEdit->setMinimumHeight(34);
    _quickAddGreetingEdit->setMinimumHeight(34);
    _quickAddSendBtn->setMinimumHeight(34);
    connect(_quickAddSendBtn, &QPushButton::clicked, this, [this] { handleQuickAddSearch(); });
    addRow->addWidget(_quickAddAccountEdit, 2);
    addRow->addWidget(_quickAddGreetingEdit, 2);
    addRow->addWidget(_quickAddSendBtn);
    addLayout->addLayout(addRow);

    _quickAddStatusLabel = new QLabel(addCard);
    _quickAddStatusLabel->setWordWrap(true);
    _quickAddStatusLabel->setTextFormat(Qt::PlainText);
    _quickAddStatusLabel->hide();
    addLayout->addWidget(_quickAddStatusLabel);
    reqOuter->addWidget(addCard);

    auto *inboxTitle = new QLabel(QCoreApplication::translate("MeetingUI", "Contact Requests"), _requestsPage);
    inboxTitle->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    inboxTitle->setFont(tf);
    reqOuter->addWidget(inboxTitle);

    _requestsEmptyLabel = new QLabel(QCoreApplication::translate("MeetingUI", "No contact requests yet."), _requestsPage);
    _requestsEmptyLabel->setObjectName(QStringLiteral("contactsRequestsEmpty"));
    reqOuter->addWidget(_requestsEmptyLabel, 1, Qt::AlignTop);

    _requestModel = new ContactRequestListModel(this);
    _requestDelegate = new ContactRequestItemDelegate(this);
    connect(_requestDelegate, &ContactRequestItemDelegate::acceptRequested, this, [this](const QString &requestId) {
        _catalog.handleContactRequest(requestId, true);
    });
    connect(_requestDelegate, &ContactRequestItemDelegate::declineRequested, this, [this](const QString &requestId) {
        _catalog.handleContactRequest(requestId, false);
    });

    _requestsListView = new QListView(_requestsPage);
    _requestsListView->setObjectName(QStringLiteral("contactsRequestsListView"));
    _requestsListView->setModel(_requestModel);
    _requestsListView->setItemDelegate(_requestDelegate);
    _requestsListView->setUniformItemSizes(true);
    _requestsListView->setSelectionMode(QAbstractItemView::NoSelection);
    _requestsListView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _requestsListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _requestsListView->setFrameShape(QFrame::NoFrame);
    _requestsListView->setMouseTracking(true);
    reqOuter->addWidget(_requestsListView, 1);

    _rightStack->addWidget(_requestsPage);
    rootLayout->addWidget(_rightStack, 1);

    retranslateUi();
    setSidebarTab(SidebarTab::Contacts);
}

void ContactsPageWidget::retranslateUi() {
    if (_searchEdit) {
        _searchEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search"));
    }
    if (_plusMenuBtn) {
        _plusMenuBtn->setToolTip(QCoreApplication::translate("MeetingUI", "More actions"));
    }
    if (_requestsEntryTitle) {
        _requestsEntryTitle->setText(QCoreApplication::translate("MeetingUI", "Requests"));
    }
    if (_segContactsBtn) {
        _segContactsBtn->setText(QCoreApplication::translate("MeetingUI", "Contacts"));
    }
    if (_segOrgBtn) {
        _segOrgBtn->setText(QCoreApplication::translate("MeetingUI", "Org Tree"));
    }
    if (_emptyPlaceholder) {
        _emptyPlaceholder->setText(QCoreApplication::translate("MeetingUI", "Select a contact or colleague to view details, place a direct call, or schedule a meeting."));
    }
    if (_detailVoiceCallBtn) _detailVoiceCallBtn->setText(QCoreApplication::translate("MeetingUI", "Voice Call"));
    if (_detailVideoCallBtn) _detailVideoCallBtn->setText(QCoreApplication::translate("MeetingUI", "Video Call"));
    if (_detailScheduleBtn) _detailScheduleBtn->setText(QCoreApplication::translate("MeetingUI", "Schedule Meeting"));
    if (_detailUnblockBtn) _detailUnblockBtn->setText(QCoreApplication::translate("MeetingUI", "Unblock"));
    if (_detailEditProfileBtn) _detailEditProfileBtn->setText(QCoreApplication::translate("MeetingUI", "Edit Personal Profile"));
    if (_detailSelfHistoryTitle) {
        _detailSelfHistoryTitle->setText(QCoreApplication::translate("MeetingUI", "My Employee No. & Organization History (Only Visible to You)"));
    }
    if (_requestsEmptyLabel) {
        _requestsEmptyLabel->setText(QCoreApplication::translate("MeetingUI", "No contact requests yet."));
    }
    if (_requestsListView) _requestsListView->viewport()->update();
    if (_detailSelfHistoryView) _detailSelfHistoryView->viewport()->update();
    updateRequestsBadge();
    rebuildContactsTree();
    rebuildOrgTree();
    if (!_selectedUserId.isEmpty() && _rightStack->currentIndex() == 1) {
        showContactDetail(_selectedUserId, false);
    }
}

void ContactsPageWidget::refreshTheme() {
    const auto border = Theme::color(Theme::ColorRole::Border).name();
    const auto surface = Theme::color(Theme::ColorRole::Surface).name();
    const auto surfaceAlt = Theme::color(Theme::ColorRole::SurfaceAlt).name();
    const auto hover = Theme::color(Theme::ColorRole::Hover).name();
    const auto windowBg = Theme::color(Theme::ColorRole::Window).name();
    const auto text = Theme::color(Theme::ColorRole::Text).name();
    const auto textSec = Theme::color(Theme::ColorRole::TextSecondary).name();
    const auto textMuted = Theme::color(Theme::ColorRole::TextMuted).name();
    const auto accent = Theme::color(Theme::ColorRole::Accent).name();
    const auto accentSoft = Theme::color(Theme::ColorRole::AccentSoft).name();
    const auto onAccent = Theme::color(Theme::ColorRole::OnAccent).name();
    const auto danger = Theme::color(Theme::ColorRole::Danger).name();

    setStyleSheet(QStringLiteral(
        "QWidget#contactsPageWidget { background: %1; }"
        "QFrame#contactsLeftPane { background: %2; }"
        "QFrame#contactsPaneDivider, QFrame#contactsSidebarDivider { background: %3; }"
        "QLineEdit#contactsSearchEdit { background: %10; color: %4; border: 1px solid transparent; border-radius: 8px; padding: 0 8px; font-size: 12.5px; }"
        "QLineEdit#contactsSearchEdit:focus { background: %2; border-color: %6; }"
        "QPushButton#contactsPlusBtn { background: %10; color: %5; border: none; border-radius: 8px; font-size: 18px; font-weight: 400; padding-bottom: 2px; }"
        "QPushButton#contactsPlusBtn:hover { background: %11; color: %6; }"
        "QPushButton#contactsRequestsEntry { background: transparent; border: none; border-radius: 8px; text-align: left; }"
        "QPushButton#contactsRequestsEntry:hover { background: %11; }"
        "QPushButton#contactsRequestsEntry:checked { background: %7; }"
        "QLabel#contactsRequestsEntryTitle { color: %4; font-size: 13px; font-weight: 500; background: transparent; }"
        "QLabel#contactsRequestsBadge { background: %6; color: %8; border-radius: 8px; padding: 0 4px; font-size: 11px; font-weight: 600; }"
        "QLabel#contactsRequestsChevron { color: %12; font-size: 16px; background: transparent; }"
        "QFrame#contactsSegTrack { background: %10; border-radius: 8px; }"
        "QPushButton#contactsSegContacts, QPushButton#contactsSegOrg { "
        "  background: transparent; color: %5; border: none; border-radius: 6px; font-size: 13px; font-weight: 500; }"
        "QPushButton#contactsSegContacts:checked, QPushButton#contactsSegOrg:checked { "
        "  background: %2; color: %6; font-weight: 600; }"
        "QFrame#contactDetailInfoCard, QFrame#contactQuickAddCard { background: %2; border: 1px solid %3; border-radius: 10px; }"
        "QLineEdit#contactQuickAddAccount, QLineEdit#contactQuickAddGreeting { background: %2; color: %4; border: 1px solid %3; border-radius: 8px; padding: 0 10px; }"
        "QLineEdit#contactQuickAddAccount:focus, QLineEdit#contactQuickAddGreeting:focus { border-color: %6; }"
        "QPushButton#contactQuickAddSend { background: %6; color: %8; border: none; border-radius: 8px; padding: 0 14px; font-weight: 600; }"
        "QPushButton#contactQuickAddSend:disabled { background: %10; color: %12; }"
        "QLabel#contactsEmptyPlaceholder, QLabel#contactDetailSubtitle, QLabel#contactsListStatus, QLabel#contactsRequestsEmpty { color: %5; }"
        "QLabel#contactDetailName, QLabel#contactDetailHistoryTitle { color: %4; }"
        "QLabel#contactDetailBlockedBanner { background: %3; color: %9; border-radius: 8px; padding: 8px 12px; }"
        "QPushButton#contactVideoCallBtn { background: %6; color: %8; border: none; border-radius: 8px; padding: 0 16px; font-weight: 600; }"
        "QPushButton#contactVoiceCallBtn, QPushButton#contactScheduleBtn, QPushButton#contactAddBtn, QPushButton#contactUnblockBtn, QPushButton#contactEditProfileBtn { "
        "  background: %2; color: %4; border: 1px solid %3; border-radius: 8px; padding: 0 14px; font-weight: 500; }"
        "QPushButton#contactVoiceCallBtn:hover, QPushButton#contactScheduleBtn:hover, QPushButton#contactAddBtn:hover, QPushButton#contactEditProfileBtn:hover { "
        "  border-color: %6; color: %6; }"
        "QTreeView#contactsTreeView, QTreeView#contactsOrgTree, "
        "QListView#contactsListView, QListView#contactsRequestsListView, QListView#contactDetailHistoryView { "
        "  background: transparent; border: none; outline: none; }")
        .arg(windowBg, surface, border, text, textSec, accent, accentSoft, onAccent, danger, surfaceAlt, hover, textMuted));

    if (_searchIconAction) {
        _searchIconAction->setIcon(makeSearchIcon(Theme::color(Theme::ColorRole::TextMuted)));
    }
    if (_detailVoiceCallBtn) _detailVoiceCallBtn->setIcon(Theme::icon(Theme::Icon::Audio, Theme::ColorRole::Text));
    if (_detailVideoCallBtn) _detailVideoCallBtn->setIcon(Theme::icon(Theme::Icon::Video, Theme::ColorRole::OnAccent));
    if (_detailScheduleBtn) _detailScheduleBtn->setIcon(Theme::icon(Theme::Icon::Calendar, Theme::ColorRole::Text));
    if (_detailMoreBtn) _detailMoreBtn->setIcon(Theme::icon(Theme::Icon::MoreHorizontal, Theme::ColorRole::TextSecondary));

    if (_contactsTreeView) _contactsTreeView->viewport()->update();
    if (_orgTreeView) _orgTreeView->viewport()->update();
    if (_searchListView) _searchListView->viewport()->update();
    if (_requestsListView) _requestsListView->viewport()->update();
    if (_detailSelfHistoryView) _detailSelfHistoryView->viewport()->update();
    if (!_selectedUserId.isEmpty() && _rightStack->currentIndex() == 1) {
        updateDetailPresence();
    }
}

void ContactsPageWidget::openRequestsTab() {
    _showingRequests = true;
    if (_requestsEntryBtn) _requestsEntryBtn->setChecked(true);
    if (_contactsTreeView) _contactsTreeView->clearSelection();
    if (_orgTreeView) _orgTreeView->clearSelection();
    _catalog.refreshRequests();
    _rightStack->setCurrentIndex(2);
    rebuildRequestsView();
}

void ContactsPageWidget::updateOrgTabVisibility() {
    const bool hasOrg = _catalog.hasOrganization();
    if (!hasOrg && _sidebarTab == SidebarTab::Organization) {
        _sidebarTab = SidebarTab::Contacts;
    }
    updateLeftViewMode();
}

void ContactsPageWidget::setSidebarTab(SidebarTab tab) {
    if (tab == SidebarTab::Organization && !_catalog.hasOrganization()) {
        tab = SidebarTab::Contacts;
    }
    _sidebarTab = tab;
    if (_segContactsBtn) _segContactsBtn->setChecked(tab == SidebarTab::Contacts);
    if (_segOrgBtn) _segOrgBtn->setChecked(tab == SidebarTab::Organization);

    if (tab == SidebarTab::Organization) {
        if (_catalog.departments().empty()) {
            _catalog.refreshOrgTree();
        } else {
            for (const auto &deptId : _expandedDeptIds) {
                if (_orgDirModel && !_orgDirModel->isDeptLoaded(deptId)) {
                    _orgDirModel->setDeptLoading(deptId);
                    _catalog.ensureDeptMembersLoaded(deptId);
                }
            }
        }
    }
    updateLeftViewMode();
    syncViewportWatch();
}

void ContactsPageWidget::updateLeftViewMode() {
    const bool searching = !_searchKeyword.isEmpty();
    const bool showSeg = !searching && _catalog.hasOrganization();
    if (_requestsEntryBtn && _requestsEntryBtn->parentWidget()) {
        _requestsEntryBtn->parentWidget()->setVisible(!searching);
    }
    if (_topDivider && _topDivider->parentWidget()) {
        _topDivider->parentWidget()->setVisible(!searching);
    }
    if (_segTrack && _segTrack->parentWidget()) {
        _segTrack->parentWidget()->setVisible(showSeg);
    }
    if (_segContactsBtn) _segContactsBtn->setChecked(_sidebarTab == SidebarTab::Contacts);
    if (_segOrgBtn) _segOrgBtn->setChecked(_sidebarTab == SidebarTab::Organization);

    if (_leftStack) {
        if (searching) {
            _leftStack->setCurrentIndex(2);
            rebuildSearchList();
        } else if (_sidebarTab == SidebarTab::Organization && _catalog.hasOrganization()) {
            _leftStack->setCurrentIndex(1);
        } else {
            _leftStack->setCurrentIndex(0);
        }
    }
    syncViewportWatch();
}

void ContactsPageWidget::rebuildContactsTree() {
    if (!_contactsTreeModel || !_contactsTreeView) return;

    const QString selfId = _session.userId();
    auto makeMember = [&](const OpenMeeting::ContactRelation &rel) {
        ContactsTreeMemberItem item;
        item.userId = rel.targetUserId;
        item.displayName = rel.effectiveDisplayName();
        if (rel.profile) {
            item.subtitle = !rel.profile->jobTitle.isEmpty()
                ? rel.profile->jobTitle
                : (!rel.profile->account.isEmpty() ? rel.profile->account : rel.profile->email);
        }
        item.presence = _presence.statusFor(rel.targetUserId);
        item.isFavorite = rel.isFavorite;
        item.isBlocked = rel.isBlocked;
        item.isSelf = (rel.targetUserId == selfId);
        return item;
    };

    auto sortMembers = [](std::vector<ContactsTreeMemberItem> &vec) {
        std::sort(vec.begin(), vec.end(), [](const ContactsTreeMemberItem &a, const ContactsTreeMemberItem &b) {
            const bool aOnline = (a.presence != OpenMeeting::PresenceStatus::Offline);
            const bool bOnline = (b.presence != OpenMeeting::PresenceStatus::Offline);
            if (aOnline != bOnline) return aOnline > bOnline;
            return a.displayName.localeAwareCompare(b.displayName) < 0;
        });
    };

    QSet<QString> validCustomGroupIds;
    for (const auto &g : _catalog.groups()) {
        if (!g.groupId.isEmpty()) validCustomGroupIds.insert(g.groupId);
    }

    ContactsTreeGroupInput starredGroup;
    starredGroup.groupId = QString::fromLatin1(kGroupIdStarred);
    starredGroup.name = QCoreApplication::translate("MeetingUI", "Starred");
    starredGroup.kind = ContactsGroupKind::Starred;

    ContactsTreeGroupInput myContactsGroup;
    myContactsGroup.groupId = QString::fromLatin1(kGroupIdMyContacts);
    myContactsGroup.name = QCoreApplication::translate("MeetingUI", "My Contacts");
    myContactsGroup.kind = ContactsGroupKind::MyContacts;

    QHash<QString, std::vector<ContactsTreeMemberItem>> customMembers;

    ContactsTreeGroupInput blockedGroup;
    blockedGroup.groupId = QString::fromLatin1(kGroupIdBlocked);
    blockedGroup.name = QCoreApplication::translate("MeetingUI", "Blocked");
    blockedGroup.kind = ContactsGroupKind::Blocked;

    for (const auto &rel : _catalog.contacts()) {
        if (rel.isBlocked) {
            blockedGroup.members.push_back(makeMember(rel));
            continue;
        }
        if (rel.isFavorite) {
            starredGroup.members.push_back(makeMember(rel));
        }
        bool placedInCustom = false;
        for (const auto &gid : rel.groupIds) {
            if (validCustomGroupIds.contains(gid)) {
                customMembers[gid].push_back(makeMember(rel));
                placedInCustom = true;
            }
        }
        if (!placedInCustom) {
            myContactsGroup.members.push_back(makeMember(rel));
        }
    }

    sortMembers(starredGroup.members);
    sortMembers(myContactsGroup.members);
    sortMembers(blockedGroup.members);

    std::vector<ContactsTreeGroupInput> groups;
    groups.reserve(3 + _catalog.groups().size());
    groups.push_back(std::move(starredGroup));
    groups.push_back(std::move(myContactsGroup));

    auto sortedCustomDefs = _catalog.groups();
    std::sort(sortedCustomDefs.begin(), sortedCustomDefs.end(),
              [](const OpenMeeting::ContactGroup &a, const OpenMeeting::ContactGroup &b) {
                  if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
                  return a.name.localeAwareCompare(b.name) < 0;
              });
    for (const auto &g : sortedCustomDefs) {
        ContactsTreeGroupInput cg;
        cg.groupId = g.groupId;
        cg.name = g.name;
        cg.kind = ContactsGroupKind::Custom;
        cg.members = customMembers.value(g.groupId);
        sortMembers(cg.members);
        groups.push_back(std::move(cg));
    }
    // User-confirmed placement: Blocked sits at the very bottom as a collapsible group.
    groups.push_back(std::move(blockedGroup));

    if (!_contactsExpandInitialized) {
        _contactsExpandInitialized = true;
        _expandedGroupIds.insert(QString::fromLatin1(kGroupIdMyContacts));
        if (!groups.empty() && !groups[0].members.empty()) {
            _expandedGroupIds.insert(QString::fromLatin1(kGroupIdStarred));
        }
    }

    _contactsTreeModel->setGroups(std::move(groups));

    // Restore expand states smoothly
    for (const QString &gid : std::as_const(_expandedGroupIds)) {
        const QModelIndex idx = _contactsTreeModel->indexForGroupId(gid);
        if (idx.isValid()) {
            _contactsTreeView->setExpanded(idx, true);
        }
    }

    if (!_selectedUserId.isEmpty() && !_showingRequests) {
        const QModelIndex uIdx = _contactsTreeModel->indexForUserId(_selectedUserId);
        if (uIdx.isValid()) {
            _contactsTreeView->setCurrentIndex(uIdx);
        }
    }
    syncViewportWatch();
}

void ContactsPageWidget::rebuildOrgTree() {
    if (!_orgDirModel || !_orgTreeView) return;

    _orgDirModel->setDepartments(
        _catalog.departments(),
        QCoreApplication::translate("MeetingUI", "Loading members..."),
        QCoreApplication::translate("MeetingUI", "No members in this department."),
        QCoreApplication::translate("MeetingUI", "Your permission only allows viewing members of your own department."),
        QCoreApplication::translate("MeetingUI", "Own dept only"));

    if (!_orgDirModel->hasDepartments()) {
        _expandedDeptIds.clear();
        syncViewportWatch();
        return;
    }

    // Prune deleted departments from expanded state & restore valid ones
    QSet<QString> validExpanded;
    for (const QString &deptId : std::as_const(_expandedDeptIds)) {
        const QModelIndex idx = _orgDirModel->indexForDeptId(deptId);
        if (!idx.isValid()) continue;
        validExpanded.insert(deptId);
        _orgTreeView->setExpanded(idx, true);

        if (_catalog.hasDeptMembersResult(deptId)) {
            applyDeptMembersToTree(deptId);
        } else {
            _orgDirModel->setDeptLoading(deptId);
            _catalog.ensureDeptMembersLoaded(deptId);
        }
    }
    _expandedDeptIds = std::move(validExpanded);
    syncViewportWatch();
}

void ContactsPageWidget::applyDeptMembersToTree(const QString &deptId) {
    if (!_orgDirModel || deptId.isEmpty()) return;
    const QString selfId = _session.userId();
    const bool restricted = _catalog.isDeptMembersRestricted(deptId);
    const auto members = _catalog.deptMembers(deptId);

    std::vector<ContactsTreeMemberItem> items;
    items.reserve(members.size());
    for (const auto &m : members) {
        const auto &uid = m.profile.userId;
        const auto rel = _catalog.findRelation(uid);
        ContactsTreeMemberItem item;
        item.userId = uid;
        item.displayName = (rel && !rel->remark.trimmed().isEmpty())
            ? rel->remark.trimmed()
            : m.profile.displayName();
        item.subtitle = !m.profile.jobTitle.isEmpty() ? m.profile.jobTitle : m.profile.account;
        item.presence = _presence.statusFor(uid);
        item.isLeader = m.isLeader;
        item.isFavorite = rel && rel->isFavorite;
        item.isBlocked = rel && rel->isBlocked;
        item.isSelf = (uid == selfId);
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const ContactsTreeMemberItem &a, const ContactsTreeMemberItem &b) {
        if (a.isLeader != b.isLeader) return a.isLeader > b.isLeader;
        const bool aOnline = (a.presence != OpenMeeting::PresenceStatus::Offline);
        const bool bOnline = (b.presence != OpenMeeting::PresenceStatus::Offline);
        if (aOnline != bOnline) return aOnline > bOnline;
        return a.displayName.localeAwareCompare(b.displayName) < 0;
    });

    _orgDirModel->setDeptMembers(deptId, std::move(items), restricted,
        _catalog.hasMoreDeptMembers(deptId), _catalog.isDeptMembersLoading(deptId));
    syncViewportWatch();
}

void ContactsPageWidget::rebuildSearchList() {
    if (!_searchModel || !_listStatusLabel) return;
    std::vector<ContactListEntry> entries;
    for (const auto &profile : _catalog.searchResults()) {
        const auto rel = _catalog.findRelation(profile.userId);
        ContactListEntry e;
        e.userId = profile.userId;
        e.displayName = (rel && !rel->remark.trimmed().isEmpty())
            ? rel->remark.trimmed()
            : profile.displayName();
        e.subtitle = !profile.jobTitle.isEmpty() ? profile.jobTitle
            : (!profile.account.isEmpty() ? profile.account : profile.email);
        e.isFavorite = rel && rel->isFavorite;
        e.isBlocked = rel && rel->isBlocked;
        e.presence = _presence.statusFor(profile.userId);
        entries.push_back(std::move(e));
    }
    _searchModel->setEntries(std::move(entries));
    if (_searchModel->rowCount() == 0) {
        _listStatusLabel->setText(QCoreApplication::translate("MeetingUI", "No matching users found."));
        _listStatusLabel->show();
    } else {
        _listStatusLabel->hide();
    }
    syncViewportWatch();
}

void ContactsPageWidget::syncViewportWatch() {
    if (!_searchKeyword.isEmpty() && _searchModel) {
        _presence.setViewportWatchedUsers(_searchModel->allUserIds());
    } else if (_sidebarTab == SidebarTab::Organization && _orgDirModel) {
        _presence.setViewportWatchedUsers(_orgDirModel->loadedMemberUserIds());
    } else {
        _presence.setViewportWatchedUsers({});
    }
}

void ContactsPageWidget::showContactDetail(const QString &userId, bool refreshFromServer) {
    _selectedUserId = userId;
    if (userId.isEmpty()) {
        _rightStack->setCurrentIndex(0);
        return;
    }

    const auto rel = _catalog.findRelation(userId);
    auto profile = _catalog.findProfile(userId);
    if (!profile && userId == _session.userId() && _catalog.selfProfile()) {
        profile = _catalog.selfProfile();
    }
    if (!profile && rel) {
        profile = rel->profile;
    }
    OpenMeeting::UserProfile effectiveProfile = profile.value_or(OpenMeeting::UserProfile{});
    if (effectiveProfile.userId.isEmpty()) {
        effectiveProfile.userId = userId;
    }

    const QString displayName = (rel && !rel->remark.trimmed().isEmpty())
        ? rel->remark.trimmed()
        : effectiveProfile.displayName();
    const bool isSelf = (userId == _session.userId());
    const bool isBlocked = rel && rel->isBlocked;
    const bool isContact = rel.has_value();

    _detailNameLabel->setText(displayName);
    QStringList subParts;
    if (!effectiveProfile.jobTitle.isEmpty()) subParts.push_back(effectiveProfile.jobTitle);
    const auto deptText = deptNamesForProfile(effectiveProfile, _catalog.departments());
    if (!deptText.isEmpty()) subParts.push_back(deptText);
    _detailSubtitleLabel->setText(subParts.join(QStringLiteral(" · ")));
    _detailSubtitleLabel->setVisible(!subParts.isEmpty());

    _detailAccountValue->setText(effectiveProfile.account.isEmpty() ? QStringLiteral("—") : effectiveProfile.account);
    _detailEmployeeNoValue->setText(effectiveProfile.employeeNo.isEmpty() ? QStringLiteral("—") : effectiveProfile.employeeNo);
    _detailDeptValue->setText(deptText.isEmpty() ? QStringLiteral("—") : deptText);
    _detailJobValue->setText(effectiveProfile.jobTitle.isEmpty() ? QStringLiteral("—") : effectiveProfile.jobTitle);
    _detailEmailValue->setText(effectiveProfile.email.isEmpty() ? QStringLiteral("—") : effectiveProfile.email);
    _detailPhoneValue->setText(effectiveProfile.phone.isEmpty() ? QStringLiteral("—") : effectiveProfile.phone);
    _detailSignatureValue->setText(effectiveProfile.signature.isEmpty() ? QStringLiteral("—") : effectiveProfile.signature);

    if (_detailInfoForm) {
        if (QWidget *remarkLabel = _detailInfoForm->labelForField(_remarkRow)) {
            remarkLabel->setVisible(!isSelf);
        }
    }
    _remarkRow->setVisible(!isSelf);
    _remarkEdit->setText(rel ? rel->remark : QString());

    _detailBlockedBanner->setVisible(isBlocked);
    if (isBlocked) {
        _detailBlockedBanner->setText(QCoreApplication::translate("MeetingUI", "This user is blocked. Direct calls and contact requests from them are muted."));
    }

    _detailStarBtn->setVisible(!isSelf && !isBlocked);
    _detailStarBtn->setText((rel && rel->isFavorite)
        ? QCoreApplication::translate("MeetingUI", "★ Starred")
        : QCoreApplication::translate("MeetingUI", "☆ Star"));
    _detailMoreBtn->setVisible(!isSelf);

    const bool canCall = !isSelf && !isBlocked;
    _detailVoiceCallBtn->setVisible(canCall);
    _detailVideoCallBtn->setVisible(canCall);
    _detailScheduleBtn->setVisible(canCall);
    _detailAddContactBtn->setVisible(!isSelf && !isContact && !isBlocked);
    _detailAddContactBtn->setText(QCoreApplication::translate("MeetingUI", "Add Contact"));
    _detailUnblockBtn->setVisible(isBlocked);
    if (_detailEditProfileBtn) {
        _detailEditProfileBtn->setVisible(isSelf);
    }

    // Strictly show historical employee numbers & org removal reasons ONLY when viewing oneself.
    if (_detailSelfHistorySection && _detailSelfHistoryModel) {
        if (isSelf && !_catalog.selfOrgHistory().empty()) {
            _detailSelfHistoryModel->setRecords(_catalog.selfOrgHistory());
            _detailSelfHistorySection->show();
        } else {
            _detailSelfHistoryModel->setRecords({});
            _detailSelfHistorySection->hide();
        }
    }

    _detailFeedbackLabel->hide();
    updateDetailPresence();
    _rightStack->setCurrentIndex(1);

    if (refreshFromServer) {
        _catalog.fetchUserProfile(userId);
    }
}

void ContactsPageWidget::handleOpenSelfProfileDialog() {
    UserProfileDialog dlg(_catalog, _session, this);
    dlg.exec();
    if (!_selectedUserId.isEmpty() && _selectedUserId == _session.userId() && _rightStack->currentIndex() == 1) {
        showContactDetail(_selectedUserId, false);
    }
}

void ContactsPageWidget::handlePlusMenu() {
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    AppTheme::styleMenu(*menu, currentThemeTone());

    auto *addExtAct = menu->addAction(QCoreApplication::translate("MeetingUI", "Add External Contact..."));
    connect(addExtAct, &QAction::triggered, this, [this] {
        openRequestsTab();
        if (_quickAddAccountEdit) _quickAddAccountEdit->setFocus();
    });

    auto *newGrpAct = menu->addAction(QCoreApplication::translate("MeetingUI", "New Contact Group..."));
    connect(newGrpAct, &QAction::triggered, this, [this] { handleCreateGroup(); });

    menu->addSeparator();
    auto *refreshAct = menu->addAction(QCoreApplication::translate("MeetingUI", "Refresh Contacts"));
    connect(refreshAct, &QAction::triggered, this, [this] { _catalog.refreshAll(); });

    if (_plusMenuBtn) {
        menu->popup(_plusMenuBtn->mapToGlobal(QPoint(0, _plusMenuBtn->height() + 4)));
    }
}

void ContactsPageWidget::handleContactsTreeContextMenu(const QPoint &pos) {
    if (!_contactsTreeView) return;
    const QModelIndex idx = _contactsTreeView->indexAt(pos);
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    AppTheme::styleMenu(*menu, currentThemeTone());

    if (idx.isValid() &&
        static_cast<ContactsTreeNodeKind>(idx.data(TreeNodeKindRole).toInt()) == ContactsTreeNodeKind::Group &&
        static_cast<ContactsGroupKind>(idx.data(TreeGroupKindRole).toInt()) == ContactsGroupKind::Custom) {
        const QString gid = idx.data(TreeNodeIdRole).toString();
        OpenMeeting::ContactGroup targetGroup;
        for (const auto &g : _catalog.groups()) {
            if (g.groupId == gid) {
                targetGroup = g;
                break;
            }
        }
        if (!targetGroup.groupId.isEmpty()) {
            auto *renameAct = menu->addAction(QCoreApplication::translate("MeetingUI", "Rename Group..."));
            connect(renameAct, &QAction::triggered, this, [this, targetGroup] {
                handleRenameGroup(targetGroup);
            });
            auto *delAct = menu->addAction(QCoreApplication::translate("MeetingUI", "Delete Group"));
            connect(delAct, &QAction::triggered, this, [this, targetGroup] {
                handleDeleteGroup(targetGroup);
            });
            menu->addSeparator();
        }
    }

    auto *newGrpAct = menu->addAction(QCoreApplication::translate("MeetingUI", "New Contact Group..."));
    connect(newGrpAct, &QAction::triggered, this, [this] { handleCreateGroup(); });
    menu->popup(_contactsTreeView->viewport()->mapToGlobal(pos));
}

void ContactsPageWidget::updateDetailPresence() {
    if (_selectedUserId.isEmpty()) return;
    const auto status = _presence.statusFor(_selectedUserId);
    const QString name = _catalog.displayNameForUser(_selectedUserId);
    const QIcon icon = InitialsAvatarPainter::buildAvatarIcon(
        _selectedUserId, name, 64, devicePixelRatioF(), status);
    _detailAvatarLabel->setPixmap(icon.pixmap(64, 64));
    _detailPresenceLabel->setText(formatPresenceBadge(status));
    _detailPresenceLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
        .arg(InitialsAvatarPainter::presenceColor(status).name()));
}

void ContactsPageWidget::updateRequestsBadge() {
    if (!_requestsBadgeLabel) return;
    const auto count = _catalog.pendingRequestCount();
    if (count > 0) {
        _requestsBadgeLabel->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
        _requestsBadgeLabel->show();
    } else {
        _requestsBadgeLabel->hide();
    }
}

void ContactsPageWidget::rebuildRequestsView() {
    if (!_requestModel || !_requestsListView) return;
    _requestModel->setRequests(_catalog.requests(), _session.userId());
    const bool empty = (_requestModel->rowCount() == 0);
    if (_requestsEmptyLabel) {
        _requestsEmptyLabel->setVisible(empty);
    }
    _requestsListView->setVisible(!empty);
}

void ContactsPageWidget::handleCreateGroup() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        QCoreApplication::translate("MeetingUI", "New Contact Group"),
        QCoreApplication::translate("MeetingUI", "Group name:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    OpenMeeting::ManageContactGroupRequest req;
    req.action = QStringLiteral("create");
    req.name = name;
    _catalog.manageGroup(req);
}

void ContactsPageWidget::handleRenameGroup(const OpenMeeting::ContactGroup &group) {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        QCoreApplication::translate("MeetingUI", "Rename Contact Group"),
        QCoreApplication::translate("MeetingUI", "New group name:"),
        QLineEdit::Normal,
        group.name,
        &ok).trimmed();
    if (!ok || name.isEmpty() || name == group.name) return;

    OpenMeeting::ManageContactGroupRequest req;
    req.action = QStringLiteral("rename");
    req.groupId = group.groupId;
    req.name = name;
    _catalog.manageGroup(req);
}

void ContactsPageWidget::handleDeleteGroup(const OpenMeeting::ContactGroup &group) {
    const auto answer = QMessageBox::question(
        this,
        QCoreApplication::translate("MeetingUI", "Delete Group"),
        QCoreApplication::translate("MeetingUI", "Delete group \"%1\"? Contacts in this group will not be removed.").arg(group.name),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    OpenMeeting::ManageContactGroupRequest req;
    req.action = QStringLiteral("delete");
    req.groupId = group.groupId;
    _expandedGroupIds.remove(group.groupId);
    _catalog.manageGroup(req);
}

void ContactsPageWidget::handleToggleFavorite() {
    if (_selectedUserId.isEmpty()) return;
    const auto rel = _catalog.findRelation(_selectedUserId);
    OpenMeeting::UpsertContactRelationRequest req;
    req.targetUserId = _selectedUserId;
    req.isFavorite = !(rel && rel->isFavorite);
    _catalog.upsertRelation(req);
}

void ContactsPageWidget::handleSaveRemark() {
    if (_selectedUserId.isEmpty()) return;
    OpenMeeting::UpsertContactRelationRequest req;
    req.targetUserId = _selectedUserId;
    req.remark = _remarkEdit->text().trimmed();
    QPointer<ContactsPageWidget> self(this);
    _catalog.upsertRelation(req, [self](bool ok, const OpenMeeting::ContactRelation &, const OpenMeeting::HttpError &err) {
        if (!self) return;
        self->_detailFeedbackLabel->setText(ok
            ? QCoreApplication::translate("MeetingUI", "Remark saved.")
            : (err.message.isEmpty() ? QCoreApplication::translate("MeetingUI", "Failed to save remark.") : err.message));
        self->_detailFeedbackLabel->show();
    });
}

void ContactsPageWidget::handleAddOrRequestContact() {
    if (_selectedUserId.isEmpty()) return;
    bool ok = false;
    const QString greeting = QInputDialog::getText(
        this,
        QCoreApplication::translate("MeetingUI", "Add Contact"),
        QCoreApplication::translate("MeetingUI", "Optional greeting message (the recipient must accept):"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok) return;

    QPointer<ContactsPageWidget> self(this);
    _catalog.sendContactRequest(_selectedUserId, greeting,
        [self](bool success, const OpenMeeting::SendContactRequestResponse &resp, const OpenMeeting::HttpError &err) {
            if (!self) return;
            if (!success) {
                self->_detailFeedbackLabel->setText(err.message.isEmpty()
                    ? QCoreApplication::translate("MeetingUI", "Unable to send contact request.")
                    : err.message);
            } else if (resp.autoAccepted) {
                self->_detailFeedbackLabel->setText(QCoreApplication::translate("MeetingUI", "Added to your contacts."));
            } else {
                self->_detailFeedbackLabel->setText(QCoreApplication::translate("MeetingUI", "Contact request sent."));
            }
            self->_detailFeedbackLabel->show();
        });
}

void ContactsPageWidget::handleMoreActionsMenu() {
    if (_selectedUserId.isEmpty()) return;
    const auto rel = _catalog.findRelation(_selectedUserId);
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    AppTheme::styleMenu(*menu, currentThemeTone());

    if (!_catalog.groups().empty()) {
        auto *groupsSub = menu->addMenu(QCoreApplication::translate("MeetingUI", "Assign to Group"));
        AppTheme::styleMenu(*groupsSub, currentThemeTone());
        for (const auto &g : _catalog.groups()) {
            const bool inGroup = rel && rel->groupIds.contains(g.groupId);
            auto *act = groupsSub->addAction(g.name);
            act->setCheckable(true);
            act->setChecked(inGroup);
            connect(act, &QAction::triggered, this, [this, gid = g.groupId, inGroup, rel] {
                QStringList nextGroups = rel ? rel->groupIds : QStringList{};
                if (inGroup) {
                    nextGroups.removeAll(gid);
                } else if (!nextGroups.contains(gid)) {
                    nextGroups.push_back(gid);
                }
                OpenMeeting::UpsertContactRelationRequest req;
                req.targetUserId = _selectedUserId;
                req.groupIds = nextGroups;
                _catalog.upsertRelation(req);
            });
        }
        menu->addSeparator();
    }

    const bool isBlocked = rel && rel->isBlocked;
    auto *blockAct = menu->addAction(isBlocked
        ? QCoreApplication::translate("MeetingUI", "Unblock Contact")
        : QCoreApplication::translate("MeetingUI", "Block Contact"));
    connect(blockAct, &QAction::triggered, this, [this, isBlocked] {
        OpenMeeting::UpsertContactRelationRequest req;
        req.targetUserId = _selectedUserId;
        req.isBlocked = !isBlocked;
        _catalog.upsertRelation(req);
    });

    if (rel) {
        auto *removeAct = menu->addAction(QCoreApplication::translate("MeetingUI", "Remove from Contacts"));
        connect(removeAct, &QAction::triggered, this, [this] {
            _catalog.removeRelation(_selectedUserId);
        });
    }

    menu->popup(_detailMoreBtn->mapToGlobal(QPoint(0, _detailMoreBtn->height() + 4)));
}

void ContactsPageWidget::handleQuickAddSearch() {
    const QString keyword = _quickAddAccountEdit->text().trimmed();
    const QString greeting = _quickAddGreetingEdit->text().trimmed();
    if (keyword.isEmpty()) {
        _quickAddStatusLabel->setText(QCoreApplication::translate("MeetingUI", "Enter an account, email, or employee number first."));
        _quickAddStatusLabel->show();
        return;
    }

    _quickAddSendBtn->setEnabled(false);
    _quickAddStatusLabel->setText(QCoreApplication::translate("MeetingUI", "Searching directory..."));
    _quickAddStatusLabel->show();

    QPointer<ContactsPageWidget> self(this);
    const bool lookupStarted = _catalog.lookupContactRequestRecipient(keyword, [self, greeting](
            bool ok, const std::optional<OpenMeeting::UserProfile> &target, const OpenMeeting::HttpError &err) {
            if (!self) return;
            if (!ok) {
                self->_quickAddSendBtn->setEnabled(true);
                self->_quickAddStatusLabel->setText(err.message.isEmpty()
                    ? QCoreApplication::translate("MeetingUI", "Directory lookup failed.")
                    : err.message);
                return;
            }
            if (!target) {
                self->_quickAddSendBtn->setEnabled(true);
                self->_quickAddStatusLabel->setText(QCoreApplication::translate("MeetingUI", "No matching user found."));
                return;
            }
            const bool sendStarted = self->_catalog.sendContactRequest(target->userId, greeting,
                [self, targetName = target->displayName()](bool reqOk, const OpenMeeting::SendContactRequestResponse &reqResp, const OpenMeeting::HttpError &reqErr) {
                    if (!self) return;
                    self->_quickAddSendBtn->setEnabled(true);
                    if (!reqOk) {
                        self->_quickAddStatusLabel->setText(reqErr.message.isEmpty()
                            ? QCoreApplication::translate("MeetingUI", "Failed to send contact request.")
                            : reqErr.message);
                        return;
                    }
                    self->_quickAddAccountEdit->clear();
                    self->_quickAddGreetingEdit->clear();
                    self->_quickAddStatusLabel->setText(reqResp.autoAccepted
                        ? QCoreApplication::translate("MeetingUI", "%1 has been added to your contacts.").arg(targetName)
                        : QCoreApplication::translate("MeetingUI", "Contact request sent to %1.").arg(targetName));
                });
            if (!sendStarted && self) {
                self->_quickAddSendBtn->setEnabled(true);
                self->_quickAddStatusLabel->setText(QCoreApplication::translate("MeetingUI", "Failed to send contact request."));
            }
        });
    if (!lookupStarted && self) {
        self->_quickAddSendBtn->setEnabled(true);
        self->_quickAddStatusLabel->setText(QCoreApplication::translate("MeetingUI", "Directory lookup failed."));
    }
}

} // namespace MeetingUI
