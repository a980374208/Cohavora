#pragma once

#include "src/core/whiteboard/whiteboard_document.h"
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>

class QPainter;

namespace MeetingUI::WhiteboardRendering {
struct Item {
    std::string id;
    livekit::whiteboard::ObjectKind kind;
    QPainterPath path;
    QRectF bounds;
    QRectF textRect;
    QColor color;
    double width = 4, fontSize = 28;
    QString text;
};
Item compile(const livekit::whiteboard::Object &object);
void draw(QPainter &painter, const Item &item);
bool hit(const Item &item, QPointF point, double tolerance);
QImage renderPage(const livekit::whiteboard::Page &page,
                  const QImage &background = {}, bool transparent = false);
} // namespace MeetingUI::WhiteboardRendering
