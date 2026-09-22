#include "whiteboard_renderer.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPathStroker>
#include <algorithm>
#include <cmath>

namespace MeetingUI::WhiteboardRendering {
namespace wb = livekit::whiteboard;
Item compile(const wb::Object &object) {
    Item item;
    item.id = object.id;
    item.kind = object.kind;
    item.color = QColor::fromRgb(object.color);
    item.width = object.width;
    item.fontSize = object.fontSize;
    item.text = QString::fromUtf8(object.text.data(), static_cast<int>(object.text.size()));
    if (object.points.empty()) return item;
    const QPointF first(object.points.front().x, object.points.front().y);
    const QPointF last(object.points.back().x, object.points.back().y);
    const QRectF box = QRectF(first, last).normalized();
    switch (object.kind) {
    case wb::ObjectKind::Pen:
    case wb::ObjectKind::Highlighter:
        item.path.moveTo(first);
        for (const auto &p : object.points) item.path.lineTo(p.x, p.y);
        // A tap should create a visible round dot and remain erasable.
        if (item.path.boundingRect().isEmpty() && object.points.size() == 1)
            item.path.lineTo(first + QPointF(0.01, 0));
        break;
    case wb::ObjectKind::Rectangle: item.path.addRect(box); break;
    case wb::ObjectKind::Ellipse: item.path.addEllipse(box); break;
    case wb::ObjectKind::Text: item.textRect = box; break;
    case wb::ObjectKind::Arrow: {
        item.path.moveTo(first);
        item.path.lineTo(last);
        const double length = std::hypot(last.x() - first.x(), last.y() - first.y());
        if (length > 0) {
            const auto direction = (last - first) / length;
            const auto normal = QPointF(-direction.y(), direction.x());
            const auto head = std::min(length * 0.4, std::max(16.0, object.width * 4));
            item.path.moveTo(last - direction * head + normal * head * 0.5);
            item.path.lineTo(last);
            item.path.lineTo(last - direction * head - normal * head * 0.5);
        }
        break;
    }
    case wb::ObjectKind::Line:
        item.path.moveTo(first);
        item.path.lineTo(last);
        break;
    }
    const double margin = item.width / 2 + 2;
    item.bounds = (object.kind == wb::ObjectKind::Text ? box : item.path.boundingRect())
        .adjusted(-margin, -margin, margin, margin);
    return item;
}

void draw(QPainter &painter, const Item &item) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (item.kind == wb::ObjectKind::Highlighter) painter.setOpacity(0.28);
    painter.setPen(QPen(item.color, item.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (item.kind == wb::ObjectKind::Text) {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(qRound(item.fontSize));
        painter.setFont(font);
        painter.setClipRect(item.textRect, Qt::IntersectClip);
        painter.drawText(item.textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, item.text);
    } else {
        painter.drawPath(item.path);
    }
    painter.restore();
}

bool hit(const Item &item, QPointF point, double tolerance) {
    if (!item.bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point)) return false;
    if (item.kind == wb::ObjectKind::Text) return item.textRect.contains(point);
    QPainterPathStroker stroker;
    stroker.setWidth(item.width + tolerance * 2);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(item.path).contains(point);
}

QImage renderPage(const wb::Page &page, const QImage &background, bool transparent) {
    if (!std::isfinite(page.width) || !std::isfinite(page.height) || page.width < 1 || page.height < 1 ||
        page.width > 4096 || page.height > 4096) return {};
    QImage image(QSize(qRound(page.width), qRound(page.height)), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    image.fill(transparent ? Qt::transparent : Qt::white);
    QPainter painter(&image);
    painter.setClipRect(QRectF(0, 0, page.width, page.height));
    if (!transparent && !page.backgroundAssetId.empty() && !background.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(0, 0, page.width, page.height), background);
    }
    for (const auto &object : page.objects) draw(painter, compile(object));
    return image;
}
} // namespace MeetingUI::WhiteboardRendering
