#pragma once

#include <QtWidgets/QStyledItemDelegate>

namespace MeetingUI {

class OrgHistoryItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit OrgHistoryItemDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace MeetingUI
