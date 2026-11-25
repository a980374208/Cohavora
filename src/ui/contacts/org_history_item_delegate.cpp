#include "src/ui/contacts/org_history_item_delegate.h"

#include "src/ui/app_theme.h"
#include "src/ui/contacts/org_history_list_model.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>

#include <algorithm>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

namespace {

QString formatTimestamp(qint64 ms) {
    if (ms <= 0) return QStringLiteral("—");
    const auto dt = QDateTime::fromMSecsSinceEpoch(ms);
    if (!dt.isValid()) return QStringLiteral("—");
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString buildReasonText(bool isRemoved, const QString &reason) {
    if (!isRemoved) return {};
    const QString trimmed = reason.trimmed();
    if (trimmed.isEmpty()) {
        return QCoreApplication::translate("MeetingUI", "Removal reason: Not specified");
    }
    return QCoreApplication::translate("MeetingUI", "Removal reason: %1").arg(trimmed);
}

QString buildMetaText(const QStringList &deptNames, const QString &jobTitle) {
    QStringList parts;
    if (!deptNames.isEmpty()) {
        parts.push_back(deptNames.join(QStringLiteral(" / ")));
    }
    if (!jobTitle.trimmed().isEmpty()) {
        parts.push_back(jobTitle.trimmed());
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace

OrgHistoryItemDelegate::OrgHistoryItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QSize OrgHistoryItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const bool isRemoved = (index.data(HistoryStatusRole).toInt() == 2);
    const QString metaText = buildMetaText(
        index.data(HistoryDeptNamesRole).toStringList(),
        index.data(HistoryJobTitleRole).toString());
    const QString reasonText = buildReasonText(isRemoved, index.data(HistoryRemovedReasonRole).toString());

    const int availableWidth = std::max(220, option.rect.width() - 36);
    QFont bodyFont = option.font;
    bodyFont.setPixelSize(12);
    const QFontMetrics fm(bodyFont);

    int h = 10 + 24 + 6 + 18; // outer top + badge row + gap + time row
    if (!metaText.isEmpty()) {
        h += 4 + 18;
    }
    if (!reasonText.isEmpty()) {
        const QRect br = fm.boundingRect(QRect(0, 0, availableWidth, 200), Qt::TextWordWrap, reasonText);
        h += 6 + std::max(18, br.height());
    }
    h += 14; // bottom padding + card margin
    return QSize(option.rect.width(), h);
}

void OrgHistoryItemDelegate::paint(
        QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QRect card = option.rect.adjusted(0, 4, 0, -4);
    painter->setPen(QPen(Theme::color(Theme::ColorRole::Border), 1.0));
    painter->setBrush(Theme::color(Theme::ColorRole::Surface));
    painter->drawRoundedRect(card, 8, 8);

    const QString empNo = index.data(HistoryEmployeeNoRole).toString();
    const bool isRemoved = (index.data(HistoryStatusRole).toInt() == 2);
    const qint64 joinedAt = index.data(HistoryJoinedAtMsRole).toLongLong();
    const qint64 removedAt = index.data(HistoryRemovedAtMsRole).toLongLong();
    const QString metaText = buildMetaText(
        index.data(HistoryDeptNamesRole).toStringList(),
        index.data(HistoryJobTitleRole).toString());
    const QString reasonText = buildReasonText(isRemoved, index.data(HistoryRemovedReasonRole).toString());

    int x = card.left() + 12;
    int y = card.top() + 10;
    const int contentWidth = std::max(120, card.width() - 24);

    // Row 1: Employee No badge + Status pill
    QFont badgeFont = option.font;
    badgeFont.setPixelSize(12);
    badgeFont.setBold(true);
    painter->setFont(badgeFont);
    const QFontMetrics badgeFm(badgeFont);

    const QString empLabel = empNo.isEmpty() ? QStringLiteral("—") : empNo;
    const int empPillW = badgeFm.horizontalAdvance(empLabel) + 16;
    const QRect empRect(x, y, empPillW, 22);
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::color(Theme::ColorRole::AccentSoft));
    painter->drawRoundedRect(empRect, 6, 6);
    painter->setPen(Theme::color(Theme::ColorRole::Accent));
    painter->drawText(empRect, Qt::AlignCenter, empLabel);

    const QString statusLabel = isRemoved
        ? QCoreApplication::translate("MeetingUI", "Removed from Org")
        : QCoreApplication::translate("MeetingUI", "Active");
    const int statusPillW = badgeFm.horizontalAdvance(statusLabel) + 16;
    const QRect statusRect(empRect.right() + 8, y, statusPillW, 22);
    if (isRemoved) {
        QColor bg = Theme::color(Theme::ColorRole::Danger);
        bg.setAlpha(28);
        painter->setBrush(bg);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(statusRect, 6, 6);
        painter->setPen(Theme::color(Theme::ColorRole::Danger));
    } else {
        painter->setBrush(Theme::color(Theme::ColorRole::SurfaceAlt));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(statusRect, 6, 6);
        painter->setPen(Theme::color(Theme::ColorRole::TextSecondary));
    }
    painter->drawText(statusRect, Qt::AlignCenter, statusLabel);

    y += 28;

    // Row 2: Join / Remove timestamps
    QFont subFont = option.font;
    subFont.setPixelSize(12);
    painter->setFont(subFont);
    painter->setPen(Theme::color(Theme::ColorRole::TextSecondary));

    QString timeText = QCoreApplication::translate("MeetingUI", "Joined: %1").arg(formatTimestamp(joinedAt));
    if (isRemoved && removedAt > 0) {
        timeText += QStringLiteral("  ·  ")
            + QCoreApplication::translate("MeetingUI", "Removed: %1").arg(formatTimestamp(removedAt));
    }
    const QRect timeRect(x, y, contentWidth, 18);
    painter->drawText(timeRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(timeText, Qt::ElideRight, contentWidth));
    y += 20;

    // Row 3 (optional): Dept / Job Title snapshot
    if (!metaText.isEmpty()) {
        const QRect metaRect(x, y, contentWidth, 18);
        painter->setPen(Theme::color(Theme::ColorRole::Text));
        painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(metaText, Qt::ElideRight, contentWidth));
        y += 22;
    }

    // Row 4 (optional): Removal reason
    if (!reasonText.isEmpty()) {
        const QRect reasonRect(x, y + 2, contentWidth, std::max(18, card.bottom() - y - 8));
        painter->setPen(Theme::color(Theme::ColorRole::Danger));
        painter->drawText(reasonRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, reasonText);
    }

    painter->restore();
}

} // namespace MeetingUI
