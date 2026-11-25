#pragma once

#include <QtWidgets/QStyledItemDelegate>

namespace MeetingUI {

class ContactItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ContactItemDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override;
};

} // namespace MeetingUI
