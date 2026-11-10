#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"

#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <algorithm>

namespace MeetingUI::AppTheme {
namespace {
class ThemeIconEngine final : public QIconEngine {
public:
    explicit ThemeIconEngine(Icon kind) : _kind(kind) {}
    QIconEngine *clone() const override { return new ThemeIconEngine(_kind); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        const auto side = std::min(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2., rect.y() + (rect.height() - side) / 2.);
        painter->scale(side / 24., side / 24.);
        painter->setRenderHint(QPainter::Antialiasing);
        const bool layoutIcon = _kind == Icon::LayoutGrid || _kind == Icon::LayoutRight ||
            _kind == Icon::LayoutTop || _kind == Icon::LayoutUpperL || _kind == Icon::LayoutLowerL ||
            _kind == Icon::LayoutFloating || _kind == Icon::ChevronDown;
        const auto color = mode == QIcon::Selected ? AppTheme::color(ColorRole::OnAccent)
            : layoutIcon ? AppTheme::color(mode == QIcon::Disabled ? ColorRole::TextMuted : ColorRole::Accent)
            : QApplication::palette().color(
                mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        painter->setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        switch (_kind) {
        case Icon::Close:
            painter->drawLine(QPointF(6, 6), QPointF(18, 18));
            painter->drawLine(QPointF(18, 6), QPointF(6, 18));
            break;
        case Icon::Details:
            painter->drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
            for (const auto y : {8, 12, 16}) {
                painter->drawPoint(QPointF(7, y));
                painter->drawLine(QPointF(11, y), QPointF(17, y));
            }
            break;
        case Icon::Video: {
            painter->drawRoundedRect(QRectF(3, 6, 12, 12), 2, 2);
            QPainterPath path;
            path.moveTo(15, 10); path.lineTo(21, 7); path.lineTo(21, 17); path.lineTo(15, 14);
            painter->drawPath(path);
            break;
        }
        case Icon::Audio: {
            QPainterPath path;
            path.moveTo(3, 9); path.lineTo(7, 9); path.lineTo(12, 5); path.lineTo(12, 19);
            path.lineTo(7, 15); path.lineTo(3, 15); path.closeSubpath();
            painter->drawPath(path);
            painter->drawArc(QRectF(10, 6, 9, 12), -60 * 16, 120 * 16);
            painter->drawArc(QRectF(10, 3, 13, 18), -60 * 16, 120 * 16);
            break;
        }
        case Icon::Information:
            painter->drawEllipse(QRectF(3, 3, 18, 18));
            painter->drawPoint(QPointF(12, 7));
            painter->drawLine(QPointF(12, 11), QPointF(12, 17));
            break;
        case Icon::LayoutGrid:
            for (const auto y : {4., 14.})
                for (const auto x : {4., 14.})
                    painter->drawRoundedRect(QRectF(x, y, 6, 6), 1, 1);
            break;
        case Icon::LayoutRight:
            painter->drawRoundedRect(QRectF(3, 4, 11, 16), 1, 1);
            for (const auto y : {4., 10., 16.})
                painter->drawRoundedRect(QRectF(17, y, 4, 4), .6, .6);
            break;
        case Icon::LayoutTop:
            for (const auto x : {3., 10., 17.})
                painter->drawRoundedRect(QRectF(x, 4, 4, 4), .6, .6);
            painter->drawRoundedRect(QRectF(3, 11, 18, 9), 1, 1);
            break;
        case Icon::LayoutUpperL:
        case Icon::LayoutLowerL: {
            const bool upper = _kind == Icon::LayoutUpperL;
            const auto stripY = upper ? 3. : 17.;
            const auto mainY = upper ? 10. : 3.;
            for (const auto x : {3., 10., 17.})
                painter->drawRoundedRect(QRectF(x, stripY, 4, 4), .6, .6);
            painter->drawRoundedRect(QRectF(3, mainY, 11, 11), 1, 1);
            for (const auto y : {mainY, mainY + 7.})
                painter->drawRoundedRect(QRectF(17, y, 4, 4), .6, .6);
            break;
        }
        case Icon::LayoutFloating:
            painter->drawRoundedRect(QRectF(3, 3, 18, 18), 1, 1);
            painter->drawRoundedRect(QRectF(14, 6, 5, 12), .6, .6);
            painter->drawLine(QPointF(14, 10), QPointF(19, 10));
            painter->drawLine(QPointF(14, 14), QPointF(19, 14));
            break;
        case Icon::ChevronDown:
            painter->drawLine(QPointF(6, 9), QPointF(12, 15));
            painter->drawLine(QPointF(12, 15), QPointF(18, 9));
            break;
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    Icon _kind;
};
} // namespace

QIcon icon(Icon kind) { return QIcon(new ThemeIconEngine(kind)); }
} // namespace MeetingUI::AppTheme
