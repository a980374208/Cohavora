#pragma once

#include <QtWidgets/QStyledItemDelegate>

class QTreeView;

namespace MeetingUI {

// Unified clean MVD delegate for both ContactsTreeModel and OrgDirectoryTreeModel:
// renders collapsible Group/Department header rows (with crisp vector chevron and right-aligned online/total counter),
// indented Member rows (avatar + presence dot + name + leader badge + subtitle), and inline Notice rows.
class ContactsTreeDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ContactsTreeDelegate(QTreeView *view, QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    QTreeView *_view = nullptr;
};

} // namespace MeetingUI
