#pragma once

#include <QtWidgets/QStyledItemDelegate>
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>

namespace OpenMeeting {

class ParticipantItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ParticipantItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;

signals:
    void micClicked(const QString &identity);
    void cameraClicked(const QString &identity);
    void moreClicked(const QString &identity, const QPoint &globalPos);

private:
    QRect getMicRect(const QRect &itemRect) const;
    QRect getCameraRect(const QRect &itemRect) const;
    QRect getMoreRect(const QRect &itemRect) const;

    void drawAvatar(QPainter *painter, const QRect &rect, const QString &name, bool isSpeaking) const;
    void drawMicIcon(QPainter *painter, const QRect &rect, bool isMuted, bool isSpeaking) const;
    void drawCameraIcon(QPainter *painter, const QRect &rect, bool isVideoEnabled) const;
    void drawMoreIcon(QPainter *painter, const QRect &rect) const;
};

} // namespace OpenMeeting
