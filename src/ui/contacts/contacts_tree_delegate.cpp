#include "src/ui/contacts/contacts_tree_delegate.h"

#include "src/ui/app_theme.h"
#include "src/ui/contacts/contacts_tree_model.h"
#include "src/ui/contacts/initials_avatar_painter.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QTreeView>

#include <algorithm>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {
constexpr int kHeaderRowHeight = 36;
constexpr int kMemberRowHeight = 54;
constexpr int kNoticeRowHeight = 36;
constexpr int kDepthIndentPx = 14;

void paintChevron(QPainter &painter, const QRectF &box, bool expanded, const QColor &color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QPointF c = box.center();
    QPainterPath path;
    if (expanded) {
        path.moveTo(c.x() - 3.6, c.y() - 1.8);
        path.lineTo(c.x(), c.y() + 2.2);
        path.lineTo(c.x() + 3.6, c.y() - 1.8);
    } else {
        path.moveTo(c.x() - 1.8, c.y() - 3.6);
        path.lineTo(c.x() + 2.2, c.y());
        path.lineTo(c.x() - 1.8, c.y() + 3.6);
    }
    painter.drawPath(path);
    painter.restore();
}
} // namespace

ContactsTreeDelegate::ContactsTreeDelegate(QTreeView *view, QObject *parent)
    : QStyledItemDelegate(parent)
    , _view(view) {}

QSize ContactsTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
    switch (kind) {
    case ContactsTreeNodeKind::Group:
    case ContactsTreeNodeKind::Department:
        return QSize(option.rect.width(), kHeaderRowHeight);
    case ContactsTreeNodeKind::Notice:
        return QSize(option.rect.width(), kNoticeRowHeight);
    case ContactsTreeNodeKind::Member:
    default:
        return QSize(option.rect.width(), kMemberRowHeight);
    }
}

void ContactsTreeDelegate::paint(
        QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const auto kind = static_cast<ContactsTreeNodeKind>(index.data(TreeNodeKindRole).toInt());
    const int depth = std::max(0, index.data(TreeDepthRole).toInt());
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool selected = option.state.testFlag(QStyle::State_Selected);

    if (kind == ContactsTreeNodeKind::Group || kind == ContactsTreeNodeKind::Department) {
        const QRect rowRect = option.rect.adjusted(6, 1, -6, -1);
        if (hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::ColorRole::Hover));
            painter->drawRoundedRect(rowRect, 7, 7);
        }

        const int indent = depth * kDepthIndentPx;
        const bool expanded = _view && _view->isExpanded(index);
        const QRectF chevRect(rowRect.left() + 6 + indent, rowRect.top() + (rowRect.height() - 14) / 2.0, 14, 14);
        paintChevron(*painter, chevRect, expanded, Theme::color(Theme::ColorRole::TextMuted));

        const QString rightText = index.data(TreeRightTextRole).toString();
        const bool restricted = index.data(TreeIsRestrictedRole).toBool();
        int rightReserved = 8;
        if (!rightText.isEmpty()) {
            QFont countFont = option.font;
            countFont.setPixelSize(restricted ? 11 : 12);
            painter->setFont(countFont);
            const QFontMetrics cfm(countFont);
            const int rw = cfm.horizontalAdvance(rightText);
            const QRect rightRect(rowRect.right() - 10 - rw, rowRect.top(), rw + 2, rowRect.height());
            painter->setPen(Theme::color(Theme::ColorRole::TextMuted));
            painter->drawText(rightRect, Qt::AlignRight | Qt::AlignVCenter, rightText);
            rightReserved = rw + 18;
        }

        const QString title = index.data(TreeTitleRole).toString();
        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        titleFont.setBold(false);
        painter->setFont(titleFont);
        painter->setPen(Theme::color(Theme::ColorRole::Text));
        const int textLeft = static_cast<int>(chevRect.right()) + 6;
        const int textWidth = std::max(24, rowRect.right() - rightReserved - textLeft);
        const QRect titleRect(textLeft, rowRect.top(), textWidth, rowRect.height());
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(title, Qt::ElideRight, textWidth));
        painter->restore();
        return;
    }

    if (kind == ContactsTreeNodeKind::Notice) {
        const QRect rowRect = option.rect.adjusted(6, 1, -6, -1);
        const int textLeft = rowRect.left() + 18 + std::max(0, depth - 1) * kDepthIndentPx;
        const int textWidth = std::max(40, rowRect.right() - 10 - textLeft);
        QFont noticeFont = option.font;
        noticeFont.setPixelSize(11);
        painter->setFont(noticeFont);
        painter->setPen(Theme::color(Theme::ColorRole::TextMuted));
        const QString text = index.data(TreeTitleRole).toString();
        painter->drawText(QRect(textLeft, rowRect.top(), textWidth, rowRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(text, Qt::ElideRight, textWidth));
        painter->restore();
        return;
    }

    // Member row
    const QRect card = option.rect.adjusted(6, 2, -6, -2);
    QColor bg = Theme::color(Theme::ColorRole::Surface);
    if (selected) {
        bg = Theme::color(Theme::ColorRole::AccentSoft);
    } else if (hovered) {
        bg = Theme::color(Theme::ColorRole::Hover);
    }
    if (selected || hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(card, 8, 8);
    }

    const QString userId = index.data(TreeNodeIdRole).toString();
    const QString name = index.data(TreeTitleRole).toString();
    const QString subtitle = index.data(TreeSubtitleRole).toString();
    const bool isLeader = index.data(TreeIsLeaderRole).toBool();
    const bool isSelf = index.data(TreeIsSelfRole).toBool();
    const bool isBlocked = index.data(TreeIsBlockedRole).toBool();
    const auto presence = static_cast<OpenMeeting::PresenceStatus>(
        index.data(TreePresenceRole).toInt());

    const int leftIndent = 14 + std::max(0, depth - 1) * kDepthIndentPx;
    const QRectF avatarRect(card.left() + leftIndent, card.top() + (card.height() - 34) / 2.0, 34, 34);
    InitialsAvatarPainter::paintAvatar(*painter, avatarRect, userId, name, presence, bg);

    const int textLeft = static_cast<int>(avatarRect.right()) + 10;
    const int textRight = card.right() - 10;

    QString badgeText;
    if (isLeader) {
        badgeText = QCoreApplication::translate("MeetingUI", "Lead");
    } else if (isBlocked) {
        badgeText = QCoreApplication::translate("MeetingUI", "Blocked");
    }

    int badgeReserved = 0;
    if (!badgeText.isEmpty()) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        const QFontMetrics bfm(badgeFont);
        const int bw = bfm.horizontalAdvance(badgeText) + 12;
        const QRect badgeRect(textRight - bw, card.top() + 8, bw, 17);
        painter->save();
        painter->setFont(badgeFont);
        painter->setPen(Qt::NoPen);
        if (isBlocked) {
            QColor bgErr = Theme::color(Theme::ColorRole::Danger);
            bgErr.setAlpha(28);
            painter->setBrush(bgErr);
            painter->drawRoundedRect(badgeRect, 4, 4);
            painter->setPen(Theme::color(Theme::ColorRole::Danger));
        } else {
            painter->setBrush(selected ? Theme::color(Theme::ColorRole::Surface) : Theme::color(Theme::ColorRole::AccentSoft));
            painter->drawRoundedRect(badgeRect, 4, 4);
            painter->setPen(Theme::color(Theme::ColorRole::Accent));
        }
        painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
        painter->restore();
        badgeReserved = bw + 6;
    }

    QFont nameFont = option.font;
    nameFont.setPixelSize(13);
    nameFont.setBold(selected);
    painter->setFont(nameFont);
    painter->setPen(selected ? Theme::color(Theme::ColorRole::Accent) : Theme::color(Theme::ColorRole::Text));
    const QString displayTitle = isSelf
        ? QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(name)
        : name;
    const QRect nameRect(textLeft, card.top() + 6, std::max(20, textRight - textLeft - badgeReserved), 20);
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
    const QRect subRect(textLeft, card.top() + 27, std::max(20, textRight - textLeft), 17);
    painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(subLine, Qt::ElideRight, subRect.width()));

    painter->restore();
}

} // namespace MeetingUI
