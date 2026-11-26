#include "src/ui/contacts/contact_viewport_watch.h"

#include "src/ui/contacts/contact_list_model.h"
#include "src/ui/contacts/contacts_tree_model.h"

#include <QtCore/QEvent>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTreeView>

#include <utility>

namespace MeetingUI {

ContactViewportWatch::ContactViewportWatch(QWidget &owner, OpenMeeting::PresenceWatchController &presence,
        OpenMeeting::PresenceWatchController::Viewport kind, std::function<QString()> detailUser)
    : QObject(&owner), owner_(owner), presence_(&presence), kind_(kind), detailUser_(std::move(detailUser)) {
    owner.installEventFilter(this);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ContactViewportWatch::publish);
    connect(&presence, &OpenMeeting::PresenceWatchController::viewportWatchesReset, &timer_, &QTimer::stop);
}

ContactViewportWatch::~ContactViewportWatch() {
    if (presence_) presence_->removeViewport(this);
}

void ContactViewportWatch::observe(QAbstractItemView &view) {
    views_.append(QPointer<QAbstractItemView>(&view));
    view.viewport()->installEventFilter(this);
    connect(view.verticalScrollBar(), &QScrollBar::valueChanged, this, &ContactViewportWatch::schedule);
    connect(view.model(), &QAbstractItemModel::modelReset, this, &ContactViewportWatch::schedule);
    connect(view.model(), &QAbstractItemModel::rowsInserted, this, &ContactViewportWatch::schedule);
    connect(view.model(), &QAbstractItemModel::rowsRemoved, this, &ContactViewportWatch::schedule);
    connect(view.model(), &QAbstractItemModel::layoutChanged, this, &ContactViewportWatch::schedule);
    if (auto *tree = qobject_cast<QTreeView *>(&view)) {
        connect(tree, &QTreeView::expanded, this, &ContactViewportWatch::schedule);
        connect(tree, &QTreeView::collapsed, this, &ContactViewportWatch::schedule);
    }
    schedule();
}

void ContactViewportWatch::schedule() {
    if (!timer_.isActive()) timer_.start(0);
}

bool ContactViewportWatch::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Hide && watched == &owner_) {
        timer_.stop();
        if (presence_) presence_->removeViewport(this);
    } else if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
               event->type() == QEvent::Resize) {
        schedule();
    }
    return false;
}

void ContactViewportWatch::publish() {
    if (!presence_ || !owner_.isVisible()) return;
    QStringList users;
    QSet<QString> seen;
    auto append = [&](const QString &id) {
        if (!id.isEmpty() && !seen.contains(id)) {
            seen.insert(id);
            users.append(id);
        }
    };
    if (detailUser_) append(detailUser_());
    for (const auto &view : views_) {
        if (!view || !view->isVisible() || !view->model()->rowCount()) continue;
        const auto bounds = view->viewport()->rect();
        const auto *tree = qobject_cast<QTreeView *>(view.data());
        QModelIndex index;
        for (int y = 0; y < bounds.height() && !index.isValid(); ++y)
            index = view->indexAt(QPoint(bounds.center().x(), y));
        while (index.isValid() && users.size() < OpenMeeting::PresenceWatchController::kMaxViewportWatch) {
            if (!bounds.intersects(view->visualRect(index))) break;
            if (!tree) append(index.data(ContactUserIdRole).toString());
            else if (index.data(TreeNodeKindRole).toInt() == static_cast<int>(ContactsTreeNodeKind::Member))
                append(index.data(TreeNodeIdRole).toString());
            index = tree ? tree->indexBelow(index) : index.sibling(index.row() + 1, 0);
        }
    }
    presence_->setViewportWatchedUsers(this, kind_, users);
}

} // namespace MeetingUI
