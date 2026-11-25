#include "src/ui/contacts/contact_request_item_delegate.h"

#include "src/net/contact_types.h"
#include "src/ui/app_theme.h"
#include "src/ui/contacts/contact_request_list_model.h"
#include "src/ui/contacts/initials_avatar_painter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

#include <algorithm>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {
constexpr int kCardHeight = 76;
constexpr int kButtonWidth = 68;
constexpr int kButtonHeight = 30;
constexpr int kButtonSpacing = 8;
constexpr int kCardPaddingRight = 14;
} // namespace

ContactRequestItemDelegate::ContactRequestItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QRect ContactRequestItemDelegate::cardRectForItem(const QRect &itemRect) {
    return itemRect.adjusted(0, 4, 0, -4);
}

QRect ContactRequestItemDelegate::declineButtonRect(const QRect &cardRect) {
    const int y = cardRect.top() + (cardRect.height() - kButtonHeight) / 2;
    const int x = cardRect.right() - kCardPaddingRight - kButtonWidth + 1;
    return QRect(x, y, kButtonWidth, kButtonHeight);
}

QRect ContactRequestItemDelegate::acceptButtonRect(const QRect &cardRect) {
    const QRect decline = declineButtonRect(cardRect);
    return QRect(decline.left() - kButtonSpacing - kButtonWidth, decline.top(), kButtonWidth, kButtonHeight);
}

QSize ContactRequestItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const {
    return QSize(option.rect.width(), kCardHeight);
}

void ContactRequestItemDelegate::paint(
        QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QRect card = cardRectForItem(option.rect);
    const QColor cardBg = Theme::color(Theme::ColorRole::Surface);
    painter->setPen(QPen(Theme::color(Theme::ColorRole::Border), 1.0));
    painter->setBrush(cardBg);
    painter->drawRoundedRect(card, 10, 10);

    const QString peerId = index.data(RequestPeerUserIdRole).toString();
    const QString peerName = index.data(RequestPeerNameRole).toString();
    const QString greeting = index.data(RequestGreetingRole).toString();
    const bool incoming = index.data(RequestIncomingRole).toBool();
    const auto status = static_cast<OpenMeeting::ContactRequestStatus>(
        index.data(RequestStatusRole).toInt());

    const QRectF avatarRect(card.left() + 14, card.top() + (card.height() - 40) / 2.0, 40, 40);
    InitialsAvatarPainter::paintAvatar(
        *painter, avatarRect, peerId, peerName, OpenMeeting::PresenceStatus::Offline, cardBg);

    const bool showActionButtons = (status == OpenMeeting::ContactRequestStatus::Pending && incoming);
    const QRect acceptRect = acceptButtonRect(card);
    const QRect declineRect = declineButtonRect(card);
    const int rightAreaLeft = showActionButtons
        ? (acceptRect.left() - 12)
        : (card.right() - kCardPaddingRight - 84);

    const int textLeft = static_cast<int>(avatarRect.right()) + 12;
    const int textWidth = std::max(40, rightAreaLeft - textLeft);

    const QString titleText = incoming
        ? QCoreApplication::translate("MeetingUI", "%1 wants to add you").arg(peerName)
        : QCoreApplication::translate("MeetingUI", "Request sent to %1").arg(peerName);

    QFont titleFont = option.font;
    titleFont.setPixelSize(13);
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(Theme::color(Theme::ColorRole::Text));

    if (greeting.isEmpty()) {
        const QRect titleRect(textLeft, card.top() + (card.height() - 22) / 2, textWidth, 22);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(titleText, Qt::ElideRight, titleRect.width()));
    } else {
        const QRect titleRect(textLeft, card.top() + 13, textWidth, 20);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(titleText, Qt::ElideRight, titleRect.width()));

        QFont greetFont = option.font;
        greetFont.setPixelSize(12);
        painter->setFont(greetFont);
        painter->setPen(Theme::color(Theme::ColorRole::TextSecondary));
        const QRect greetRect(textLeft, card.top() + 36, textWidth, 18);
        painter->drawText(greetRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(greeting, Qt::ElideRight, greetRect.width()));
    }

    if (showActionButtons) {
        QFont btnFont = option.font;
        btnFont.setPixelSize(12);
        btnFont.setBold(true);
        painter->setFont(btnFont);

        const bool acceptHover = (_hoveredRow == index.row() && _hoveredButton == 1);
        painter->setPen(Qt::NoPen);
        painter->setBrush(acceptHover
            ? Theme::color(Theme::ColorRole::Accent).lighter(108)
            : Theme::color(Theme::ColorRole::Accent));
        painter->drawRoundedRect(acceptRect, 7, 7);
        painter->setPen(Theme::color(Theme::ColorRole::OnAccent));
        painter->drawText(acceptRect, Qt::AlignCenter, QCoreApplication::translate("MeetingUI", "Accept"));

        btnFont.setBold(false);
        painter->setFont(btnFont);
        const bool declineHover = (_hoveredRow == index.row() && _hoveredButton == 2);
        painter->setPen(QPen(declineHover ? Theme::color(Theme::ColorRole::Accent) : Theme::color(Theme::ColorRole::Border), 1.0));
        painter->setBrush(declineHover ? Theme::color(Theme::ColorRole::Hover) : Theme::color(Theme::ColorRole::Surface));
        painter->drawRoundedRect(declineRect, 7, 7);
        painter->setPen(Theme::color(Theme::ColorRole::Text));
        painter->drawText(declineRect, Qt::AlignCenter, QCoreApplication::translate("MeetingUI", "Decline"));
    } else {
        QString statusText;
        if (status == OpenMeeting::ContactRequestStatus::Accepted) {
            statusText = QCoreApplication::translate("MeetingUI", "Accepted");
        } else if (status == OpenMeeting::ContactRequestStatus::Rejected) {
            statusText = QCoreApplication::translate("MeetingUI", "Declined");
        } else {
            statusText = QCoreApplication::translate("MeetingUI", "Pending");
        }
        QFont statusFont = option.font;
        statusFont.setPixelSize(12);
        painter->setFont(statusFont);
        painter->setPen(Theme::color(Theme::ColorRole::TextMuted));
        const QRect statusRect(rightAreaLeft, card.top(), card.right() - kCardPaddingRight - rightAreaLeft, card.height());
        painter->drawText(statusRect, Qt::AlignRight | Qt::AlignVCenter, statusText);
    }

    painter->restore();
}

bool ContactRequestItemDelegate::editorEvent(
        QEvent *event,
        QAbstractItemModel *model,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) {
    if (!event || !index.isValid()) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    const bool incoming = index.data(RequestIncomingRole).toBool();
    const auto status = static_cast<OpenMeeting::ContactRequestStatus>(
        index.data(RequestStatusRole).toInt());
    const bool actionable = (status == OpenMeeting::ContactRequestStatus::Pending && incoming);

    if (event->type() == QEvent::MouseMove) {
        const auto *me = static_cast<QMouseEvent *>(event);
        int nextBtn = 0;
        if (actionable) {
            const QRect card = cardRectForItem(option.rect);
            if (acceptButtonRect(card).contains(me->pos())) nextBtn = 1;
            else if (declineButtonRect(card).contains(me->pos())) nextBtn = 2;
        }
        if (_hoveredRow != index.row() || _hoveredButton != nextBtn) {
            _hoveredRow = index.row();
            _hoveredButton = nextBtn;
            if (option.widget) {
                const_cast<QWidget *>(option.widget)->update(option.rect);
            }
        }
        return false;
    }

    if (actionable && event->type() == QEvent::MouseButtonRelease) {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QRect card = cardRectForItem(option.rect);
            const QString requestId = index.data(RequestIdRole).toString();
            if (!requestId.isEmpty()) {
                if (acceptButtonRect(card).contains(me->pos())) {
                    emit acceptRequested(requestId);
                    return true;
                }
                if (declineButtonRect(card).contains(me->pos())) {
                    emit declineRequested(requestId);
                    return true;
                }
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace MeetingUI
