#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QPainter>

#include <optional>

namespace MeetingUI {

class InitialsAvatarPainter final {
public:
    static QColor colorForUserId(const QString &userId);
    static QString extractInitial(const QString &displayName);
    static QColor presenceColor(OpenMeeting::PresenceStatus status);
    static QString presenceLabel(OpenMeeting::PresenceStatus status);

    static void paintAvatar(
        QPainter &painter,
        const QRectF &rect,
        const QString &userId,
        const QString &displayName,
        std::optional<OpenMeeting::PresenceStatus> presence = std::nullopt,
        const QColor &dotBorderColor = {});

    static QIcon buildAvatarIcon(
        const QString &userId,
        const QString &displayName,
        int logicalSize,
        qreal devicePixelRatio,
        std::optional<OpenMeeting::PresenceStatus> presence = std::nullopt);
};

} // namespace MeetingUI
