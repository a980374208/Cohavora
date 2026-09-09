#include "src/ui/participant_item_delegate.h"
#include "src/ui/participants_list_model.h"

#include <QtGui/QPainterPath>
#include <QtCore/QCryptographicHash>

namespace OpenMeeting {

static const QColor AVATAR_COLORS[] = {
    QColor("#2563EB"), // 蓝
    QColor("#059669"), // 绿
    QColor("#7C3AED"), // 紫
    QColor("#DB2777"), // 粉
    QColor("#D97706"), // 橙
    QColor("#0891B2")  // 青
};

ParticipantItemDelegate::ParticipantItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {
}

QSize ParticipantItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &/*index*/) const {
    return QSize(option.rect.width(), 48);
}

QRect ParticipantItemDelegate::getMicRect(const QRect &itemRect) const {
    int size = 22;
    int y = itemRect.top() + (itemRect.height() - size) / 2;
    int x = itemRect.right() - 76;
    return QRect(x, y, size, size);
}

QRect ParticipantItemDelegate::getCameraRect(const QRect &itemRect) const {
    int size = 22;
    int y = itemRect.top() + (itemRect.height() - size) / 2;
    int x = itemRect.right() - 50;
    return QRect(x, y, size, size);
}

QRect ParticipantItemDelegate::getMoreRect(const QRect &itemRect) const {
    int size = 22;
    int y = itemRect.top() + (itemRect.height() - size) / 2;
    int x = itemRect.right() - 26;
    return QRect(x, y, size, size);
}

void ParticipantItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QRect rect = option.rect;

    // 1. 悬浮与选中背景绘制
    if (option.state & QStyle::State_Selected) {
        painter->fillRect(rect, QColor(38, 120, 240, 50));
    } else if (option.state & QStyle::State_MouseOver) {
        painter->fillRect(rect, QColor(255, 255, 255, 14));
    }

    // 获取数据属性
    QString name = index.data(NameRole).toString();
    QString identity = index.data(IdentityRole).toString();
    bool isLocal = index.data(IsLocalRole).toBool();
    bool isHost = index.data(IsHostRole).toBool();
    bool isAudioMuted = index.data(IsAudioMutedRole).toBool();
    bool isVideoEnabled = index.data(IsVideoEnabledRole).toBool();
    bool isSpeaking = index.data(IsSpeakingRole).toBool();

    // 2. 绘制左侧圆形头像 (直径 32px)
    int avatarSize = 32;
    QRect avatarRect(rect.left() + 10, rect.top() + (rect.height() - avatarSize) / 2, avatarSize, avatarSize);
    drawAvatar(painter, avatarRect, name.isEmpty() ? identity : name, isSpeaking);

    // 3. 计算文字区域并绘制用户名与身份标签
    int textStartX = avatarRect.right() + 10;
    int rightControlsWidth = 84; // 为右侧麦克风、摄像头、更多按钮预留宽度
    int maxTextWidth = rect.width() - textStartX - rightControlsWidth - 10;

    QFont nameFont = option.font;
    nameFont.setPointSize(10);
    painter->setFont(nameFont);
    QFontMetrics fm(nameFont);

    // 拼接补充文字 (我) 与 主持人
    QString suffix = "";
    if (isLocal) {
        suffix += QString::fromUtf8(" (我)");
    }

    int suffixWidth = fm.horizontalAdvance(suffix);
    int hostTagWidth = isHost ? 48 : 0;
    int availableForName = maxTextWidth - suffixWidth - hostTagWidth;
    if (availableForName < 30) availableForName = 30;

    QString elidedName = fm.elidedText(name.isEmpty() ? identity : name, Qt::ElideRight, availableForName);

    int textY = rect.top() + (rect.height() + fm.ascent() - fm.descent()) / 2;

    // 绘制姓名 (白色)
    painter->setPen(QColor("#F3F4F6"));
    painter->drawText(textStartX, textY, elidedName);
    int currentX = textStartX + fm.horizontalAdvance(elidedName);

    // 绘制 (我)
    if (isLocal) {
        painter->setPen(QColor("#9CA3AF"));
        painter->drawText(currentX, textY, suffix);
        currentX += suffixWidth;
    }

    // 绘制主持人胶囊标签
    if (isHost) {
        currentX += 6;
        int tagH = 16;
        int tagW = 42;
        int tagY = rect.top() + (rect.height() - tagH) / 2;
        QRect tagRect(currentX, tagY, tagW, tagH);

        painter->setPen(QColor(245, 158, 11, 150));
        painter->setBrush(QColor(245, 158, 11, 40));
        painter->drawRoundedRect(tagRect, 3, 3);

        QFont tagFont = nameFont;
        tagFont.setPointSize(8);
        tagFont.setBold(true);
        painter->setFont(tagFont);
        painter->setPen(QColor("#F59E0B"));
        painter->drawText(tagRect, Qt::AlignCenter, QString::fromUtf8("主持人"));
    }

    // 4. 绘制右侧操作图标
    drawMicIcon(painter, getMicRect(rect), isAudioMuted, isSpeaking);
    drawCameraIcon(painter, getCameraRect(rect), isVideoEnabled);
    drawMoreIcon(painter, getMoreRect(rect));

    painter->restore();
}

void ParticipantItemDelegate::drawAvatar(QPainter *painter, const QRect &rect, const QString &name, bool isSpeaking) const {
    painter->save();

    // 发言发光环
    if (isSpeaking) {
        painter->setPen(QPen(QColor("#10B981"), 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(rect.adjusted(-1, -1, 1, 1));
    }

    // 背景圆底色
    uint hashVal = qHash(name);
    QColor bgColor = AVATAR_COLORS[hashVal % (sizeof(AVATAR_COLORS) / sizeof(AVATAR_COLORS[0]))];

    painter->setPen(Qt::NoPen);
    painter->setBrush(bgColor);
    painter->drawEllipse(rect);

    // 取文字缩写
    QString initials = "?";
    if (!name.isEmpty()) {
        if (name.at(0).isLetter() && name.at(0).toLatin1() != 0) {
            initials = QString(name.at(0).toUpper());
        } else {
            // 中文等非拉丁字符取最后 1-2 字
            initials = name.right(name.length() > 2 ? 2 : 1);
        }
    }

    QFont f = painter->font();
    f.setPointSize(initials.length() > 1 ? 8 : 10);
    f.setBold(true);
    painter->setFont(f);
    painter->setPen(Qt::white);
    painter->drawText(rect, Qt::AlignCenter, initials);

    painter->restore();
}

void ParticipantItemDelegate::drawMicIcon(QPainter *painter, const QRect &rect, bool isMuted, bool isSpeaking) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QColor color = isMuted ? QColor("#EF4444") : (isSpeaking ? QColor("#10B981") : QColor("#D1D5DB"));
    painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(isMuted ? Qt::NoBrush : QBrush(color));

    int cx = rect.center().x();
    int cy = rect.center().y();

    // 麦克风筒
    QRect capsuleRect(cx - 3, cy - 6, 6, 9);
    painter->drawRoundedRect(capsuleRect, 3, 3);

    // 麦克风下支架弧线与底部立柱
    painter->setBrush(Qt::NoBrush);
    painter->drawArc(cx - 5, cy - 4, 10, 8, 0, -180 * 16);
    painter->drawLine(cx, cy + 4, cx, cy + 7);
    painter->drawLine(cx - 3, cy + 7, cx + 3, cy + 7);

    // 若静音，画红色贯穿斜杠
    if (isMuted) {
        painter->setPen(QPen(QColor("#EF4444"), 1.8, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(rect.left() + 3, rect.bottom() - 3, rect.right() - 3, rect.top() + 3);
    }

    painter->restore();
}

void ParticipantItemDelegate::drawCameraIcon(QPainter *painter, const QRect &rect, bool isVideoEnabled) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QColor color = isVideoEnabled ? QColor("#D1D5DB") : QColor("#EF4444");
    painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(isVideoEnabled ? QBrush(color) : Qt::NoBrush);

    int cx = rect.center().x();
    int cy = rect.center().y();

    // 摄像机机身
    QRect bodyRect(cx - 6, cy - 4, 8, 8);
    painter->drawRoundedRect(bodyRect, 1.5, 1.5);

    // 镜头三角形
    QPainterPath lensPath;
    lensPath.moveTo(cx + 3, cy - 3);
    lensPath.lineTo(cx + 6, cy - 5);
    lensPath.lineTo(cx + 6, cy + 5);
    lensPath.lineTo(cx + 3, cy + 3);
    lensPath.closeSubpath();
    painter->drawPath(lensPath);

    // 若关摄，画红色贯穿斜杠
    if (!isVideoEnabled) {
        painter->setPen(QPen(QColor("#EF4444"), 1.8, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(rect.left() + 3, rect.bottom() - 3, rect.right() - 3, rect.top() + 3);
    }

    painter->restore();
}

void ParticipantItemDelegate::drawMoreIcon(QPainter *painter, const QRect &rect) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor("#9CA3AF"));

    int cx = rect.center().x();
    int cy = rect.center().y();

    painter->drawEllipse(QPointF(cx - 5, cy), 1.5, 1.5);
    painter->drawEllipse(QPointF(cx, cy), 1.5, 1.5);
    painter->drawEllipse(QPointF(cx + 5, cy), 1.5, 1.5);

    painter->restore();
}

bool ParticipantItemDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) {
    if (!index.isValid()) return false;

    if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            QPoint pos = mouseEvent->pos();
            QString identity = index.data(IdentityRole).toString();

            if (getMicRect(option.rect).contains(pos)) {
                emit micClicked(identity);
                return true;
            } else if (getCameraRect(option.rect).contains(pos)) {
                emit cameraClicked(identity);
                return true;
            } else if (getMoreRect(option.rect).contains(pos)) {
                emit moreClicked(identity, mouseEvent->globalPos());
                return true;
            }
        }
    }

    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace OpenMeeting
