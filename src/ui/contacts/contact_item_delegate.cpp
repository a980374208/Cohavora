#include "src/ui/contacts/contact_item_delegate.h"

#include "src/ui/app_theme.h"
#include "src/ui/contacts/contact_list_model.h"
#include "src/ui/contacts/initials_avatar_painter.h"

#include <QtCore/QEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

ContactItemDelegate::ContactItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QSize ContactItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const {
    return QSize(option.rect.width(), 58);
}

void ContactItemDelegate::paint(
        QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QRect card = option.rect.adjusted(6, 3, -6, -3);
    const bool checkable = index.flags().testFlag(Qt::ItemIsUserCheckable);
    const bool checked = checkable && index.data(ContactCheckedRole).toBool();
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

    QColor bg = Theme::color(Theme::ColorRole::Surface);
    if (selected || checked) bg = Theme::color(Theme::ColorRole::AccentSoft);
    else if (hovered) bg = Theme::color(Theme::ColorRole::Hover);

    painter->setPen((selected || checked) ? QPen(Theme::color(Theme::ColorRole::Accent), 1.0) : Qt::NoPen);
    painter->setBrush(bg);
    painter->drawRoundedRect(card, 8, 8);

    int leftCursor = card.left() + 10;
    if (checkable) {
        const QRectF boxRect(leftCursor, card.top() + (card.height() - 18) / 2.0, 18, 18);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::ColorRole::Accent));
            painter->drawRoundedRect(boxRect, 5, 5);

            QPainterPath tick;
            tick.moveTo(boxRect.left() + 4.5, boxRect.top() + 9.2);
            tick.lineTo(boxRect.left() + 7.8, boxRect.top() + 12.5);
            tick.lineTo(boxRect.left() + 13.8, boxRect.top() + 5.8);
            painter->setPen(QPen(Theme::color(Theme::ColorRole::OnAccent), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(tick);
        } else {
            painter->setPen(QPen(Theme::color(Theme::ColorRole::Border), 1.4));
            painter->setBrush(Theme::color(Theme::ColorRole::Surface));
            painter->drawRoundedRect(boxRect, 5, 5);
        }
        leftCursor += 28;
    }

    const QString userId = index.data(ContactUserIdRole).toString();
    const QString name = index.data(ContactDisplayNameRole).toString();
    const QString subtitle = index.data(ContactSubtitleRole).toString();
    const QString badge = index.data(ContactBadgeRole).toString();
    const bool isFav = index.data(ContactIsFavoriteRole).toBool();
    const auto presence = static_cast<OpenMeeting::PresenceStatus>(
        index.data(ContactPresenceRole).toInt());

    const QRectF avatarRect(leftCursor, card.top() + (card.height() - 36) / 2.0, 36, 36);
    InitialsAvatarPainter::paintAvatar(*painter, avatarRect, userId, name, presence, bg);

    const int textLeft = static_cast<int>(avatarRect.right()) + 12;
    const int textRight = card.right() - 10;

    QFont nameFont = option.font;
    nameFont.setPixelSize(13);
    nameFont.setBold(true);
    painter->setFont(nameFont);
    painter->setPen(Theme::color(Theme::ColorRole::Text));

    QString displayTitle = isFav ? (QStringLiteral("★ ") + name) : name;
    int badgeWidth = 0;
    if (!badge.isEmpty()) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        const QFontMetrics bfm(badgeFont);
        badgeWidth = bfm.horizontalAdvance(badge) + 14;
        const QRect badgeRect(textRight - badgeWidth, card.top() + 8, badgeWidth, 18);
        painter->save();
        painter->setFont(badgeFont);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Theme::color(Theme::ColorRole::SurfaceAlt));
        painter->drawRoundedRect(badgeRect, 4, 4);
        painter->setPen(Theme::color(Theme::ColorRole::TextSecondary));
        painter->drawText(badgeRect, Qt::AlignCenter, badge);
        painter->restore();
        badgeWidth += 6;
    }

    const QRect nameRect(textLeft, card.top() + 7, std::max(20, textRight - textLeft - badgeWidth), 20);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(displayTitle, Qt::ElideRight, nameRect.width()));

    QFont subFont = option.font;
    subFont.setPixelSize(11);
    painter->setFont(subFont);
    const QString statusText = InitialsAvatarPainter::presenceLabel(presence);
    const QString subLine = subtitle.isEmpty() ? statusText : (statusText + QStringLiteral(" · ") + subtitle);
    painter->setPen(presence == OpenMeeting::PresenceStatus::InMeeting
        ? Theme::color(Theme::ColorRole::Danger)
        : Theme::color(Theme::ColorRole::TextMuted));
    const QRect subRect(textLeft, card.top() + 29, std::max(20, textRight - textLeft), 18);
    painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(subLine, Qt::ElideRight, subRect.width()));

    painter->restore();
}

bool ContactItemDelegate::editorEvent(
        QEvent *event,
        QAbstractItemModel *model,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) {
    if (!event || !model || !index.isValid() || !index.flags().testFlag(Qt::ItemIsUserCheckable)) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && option.rect.contains(me->pos())) {
            const bool next = !index.data(ContactCheckedRole).toBool();
            model->setData(index, next, ContactCheckedRole);
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        return true;
    } else if (event->type() == QEvent::KeyPress) {
        const auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Space || ke->key() == Qt::Key_Select) {
            const bool next = !index.data(ContactCheckedRole).toBool();
            model->setData(index, next, ContactCheckedRole);
            return true;
        }
    }
    return false;
}

} // namespace MeetingUI
