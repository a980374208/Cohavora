#pragma once

#include <QtCore/QRect>
#include <QtWidgets/QStyledItemDelegate>

namespace MeetingUI {

class ContactRequestItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ContactRequestItemDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override;

    static QRect cardRectForItem(const QRect &itemRect);
    static QRect acceptButtonRect(const QRect &cardRect);
    static QRect declineButtonRect(const QRect &cardRect);

signals:
    void acceptRequested(const QString &requestId);
    void declineRequested(const QString &requestId);

private:
    int _hoveredRow = -1;
    int _hoveredButton = 0; // 0 = none, 1 = accept, 2 = decline
};

} // namespace MeetingUI
