#include "src/ui/contacts/initials_avatar_painter.h"

#include "src/ui/app_theme.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QFont>
#include <QtGui/QPixmap>

#include <algorithm>
#include <array>
#include <cstdint>

namespace MeetingUI {
namespace {

// Curated 8-color semantic palette with strong white-text contrast in both Light and Dark themes.
constexpr std::array<uint32_t, 8> kAvatarPalette = {
    0x2563EB, // Cobalt Blue
    0x0D9488, // Teal
    0x7C3AED, // Violet
    0xD97706, // Amber
    0xDB2777, // Rose
    0x059669, // Emerald
    0x4F46E5, // Indigo
    0xEA580C, // Tangerine
};

uint32_t fnv1aHash(const QByteArray &bytes) {
    uint32_t hash = 2166136261u;
    for (const char ch : bytes) {
        hash ^= static_cast<uint8_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

QColor InitialsAvatarPainter::colorForUserId(const QString &userId) {
    const QByteArray bytes = userId.trimmed().toUtf8();
    if (bytes.isEmpty()) {
        return AppTheme::color(AppTheme::ColorRole::Accent);
    }
    const size_t idx = fnv1aHash(bytes) % kAvatarPalette.size();
    return QColor::fromRgb(kAvatarPalette[idx]);
}

QString InitialsAvatarPainter::extractInitial(const QString &displayName) {
    const QString trimmed = displayName.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("?");
    // Safely decode full Unicode code points (never splitting surrogate pairs).
    const QVector<uint> codePoints = trimmed.toUcs4();
    if (codePoints.isEmpty()) return QStringLiteral("?");
    const uint first = codePoints.first();
    if (first >= 0x4E00 && first <= 0x9FFF) {
        // For CJK names, use the last character when 2..4 chars (familiar enterprise directory style), else first.
        const uint chosen = (codePoints.size() >= 2 && codePoints.size() <= 4)
            ? codePoints.last() : first;
        return QString::fromUcs4(&chosen, 1);
    }
    return QString::fromUcs4(&first, 1).toUpper();
}

QColor InitialsAvatarPainter::presenceColor(OpenMeeting::PresenceStatus status) {
    switch (status) {
    case OpenMeeting::PresenceStatus::Online:
        return AppTheme::color(AppTheme::ColorRole::Success);
    case OpenMeeting::PresenceStatus::InMeeting:
        return AppTheme::color(AppTheme::ColorRole::Danger);
    case OpenMeeting::PresenceStatus::Away:
        return AppTheme::color(AppTheme::ColorRole::Warning);
    case OpenMeeting::PresenceStatus::Offline:
    default:
        return AppTheme::color(AppTheme::ColorRole::TextMuted);
    }
}

QString InitialsAvatarPainter::presenceLabel(OpenMeeting::PresenceStatus status) {
    switch (status) {
    case OpenMeeting::PresenceStatus::Online:
        return QCoreApplication::translate("MeetingUI", "Online");
    case OpenMeeting::PresenceStatus::InMeeting:
        return QCoreApplication::translate("MeetingUI", "In Meeting");
    case OpenMeeting::PresenceStatus::Away:
        return QCoreApplication::translate("MeetingUI", "Away");
    case OpenMeeting::PresenceStatus::Offline:
    default:
        return QCoreApplication::translate("MeetingUI", "Offline");
    }
}

void InitialsAvatarPainter::paintAvatar(
        QPainter &painter,
        const QRectF &rect,
        const QString &userId,
        const QString &displayName,
        std::optional<OpenMeeting::PresenceStatus> presence,
        const QColor &dotBorderColor) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    painter.setPen(Qt::NoPen);
    painter.setBrush(colorForUserId(userId));
    painter.drawEllipse(rect);

    QFont font = painter.font();
    font.setPixelSize(std::max(10, static_cast<int>(rect.height() * 0.44)));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(rect, Qt::AlignCenter, extractInitial(displayName));

    if (presence) {
        const qreal dotDiameter = std::clamp(rect.height() * 0.30, 7.0, 14.0);
        const QRectF dotRect(
            rect.right() - dotDiameter + 1.0,
            rect.bottom() - dotDiameter + 1.0,
            dotDiameter,
            dotDiameter);
        const QColor border = dotBorderColor.isValid()
            ? dotBorderColor
            : AppTheme::color(AppTheme::ColorRole::Surface);
        painter.setBrush(presenceColor(*presence));
        painter.setPen(QPen(border, std::max<qreal>(1.5, dotDiameter * 0.16)));
        painter.drawEllipse(dotRect);
    }

    painter.restore();
}

QIcon InitialsAvatarPainter::buildAvatarIcon(
        const QString &userId,
        const QString &displayName,
        int logicalSize,
        qreal devicePixelRatio,
        std::optional<OpenMeeting::PresenceStatus> presence) {
    const qreal scale = std::max<qreal>(1.0, devicePixelRatio);
    const int pixelSize = std::max(1, static_cast<int>(logicalSize * scale));
    QPixmap pix(pixelSize, pixelSize);
    pix.setDevicePixelRatio(scale);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    paintAvatar(painter, QRectF(0.5, 0.5, logicalSize - 1.0, logicalSize - 1.0),
                userId, displayName, presence);
    return QIcon(pix);
}

} // namespace MeetingUI
