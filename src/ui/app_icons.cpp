#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"

#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <algorithm>
#include <cmath>
#include <optional>

namespace MeetingUI::AppTheme {
namespace {
class ThemeIconEngine final : public QIconEngine {
public:
    explicit ThemeIconEngine(Icon kind, std::optional<ColorRole> role = std::nullopt)
        : _kind(kind), _role(role) {}
    QIconEngine *clone() const override { return new ThemeIconEngine(_kind, _role); }
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
            : _role.has_value() ? AppTheme::color(mode == QIcon::Disabled ? ColorRole::TextMuted : *_role)
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
        case Icon::VideoOff: {
            painter->drawRoundedRect(QRectF(3, 6, 12, 12), 2, 2);
            QPainterPath path;
            path.moveTo(15, 10); path.lineTo(21, 7); path.lineTo(21, 17); path.lineTo(15, 14);
            painter->drawPath(path);
            painter->drawLine(QPointF(3.5, 4.5), QPointF(20.5, 19.5));
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
        case Icon::ChevronRight:
            painter->drawLine(QPointF(9, 6), QPointF(15, 12));
            painter->drawLine(QPointF(15, 12), QPointF(9, 18));
            break;
        case Icon::Plus:
            painter->drawLine(QPointF(12, 5), QPointF(12, 19));
            painter->drawLine(QPointF(5, 12), QPointF(19, 12));
            break;
        case Icon::MoreHorizontal:
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            for (const auto x : {6., 12., 18.})
                painter->drawEllipse(QPointF(x, 12.), 1.6, 1.6);
            break;
        case Icon::Back:
            painter->drawLine(QPointF(19, 12), QPointF(5, 12));
            painter->drawLine(QPointF(11, 6), QPointF(5, 12));
            painter->drawLine(QPointF(5, 12), QPointF(11, 18));
            break;
        case Icon::History:
            painter->drawArc(QRectF(4, 4, 16, 16), 135 * 16, -295 * 16);
            painter->drawLine(QPointF(4, 6), QPointF(4, 10));
            painter->drawLine(QPointF(4, 10), QPointF(8, 10));
            painter->drawLine(QPointF(12, 7.5), QPointF(12, 12.5));
            painter->drawLine(QPointF(12, 12.5), QPointF(15.5, 14.5));
            break;
        case Icon::SidebarRight:
            painter->drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
            painter->drawLine(QPointF(15, 4), QPointF(15, 20));
            break;
        case Icon::Expand:
            painter->drawLine(QPointF(4, 9), QPointF(4, 4));
            painter->drawLine(QPointF(4, 4), QPointF(9, 4));
            painter->drawLine(QPointF(15, 4), QPointF(20, 4));
            painter->drawLine(QPointF(20, 4), QPointF(20, 9));
            painter->drawLine(QPointF(20, 15), QPointF(20, 20));
            painter->drawLine(QPointF(20, 20), QPointF(15, 20));
            painter->drawLine(QPointF(9, 20), QPointF(4, 20));
            painter->drawLine(QPointF(4, 20), QPointF(4, 15));
            break;
        case Icon::Collapse:
            painter->drawLine(QPointF(9, 4), QPointF(9, 9));
            painter->drawLine(QPointF(9, 9), QPointF(4, 9));
            painter->drawLine(QPointF(15, 4), QPointF(15, 9));
            painter->drawLine(QPointF(15, 9), QPointF(20, 9));
            painter->drawLine(QPointF(20, 15), QPointF(15, 15));
            painter->drawLine(QPointF(15, 15), QPointF(15, 20));
            painter->drawLine(QPointF(4, 15), QPointF(9, 15));
            painter->drawLine(QPointF(9, 15), QPointF(9, 20));
            break;
        case Icon::Settings: {
            painter->drawEllipse(QPointF(12, 12), 3.2, 3.2);
            painter->drawEllipse(QPointF(12, 12), 7.2, 7.2);
            for (int deg = 0; deg < 360; deg += 45) {
                const qreal rad = deg * 3.14159265358979323846 / 180.0;
                const qreal c = std::cos(rad);
                const qreal s = std::sin(rad);
                painter->drawLine(QPointF(12 + c * 7.5, 12 + s * 7.5), QPointF(12 + c * 9.8, 12 + s * 9.8));
            }
            break;
        }
        case Icon::Copy:
            painter->drawRoundedRect(QRectF(9, 9, 11, 11), 2, 2);
            painter->drawLine(QPointF(6, 15), QPointF(5, 15));
            painter->drawArc(QRectF(4, 12, 3, 3), 180 * 16, 90 * 16);
            painter->drawLine(QPointF(4, 13.5), QPointF(4, 5.5));
            painter->drawArc(QRectF(4, 4, 3, 3), 90 * 16, 90 * 16);
            painter->drawLine(QPointF(5.5, 4), QPointF(13.5, 4));
            painter->drawArc(QRectF(12, 4, 3, 3), 0, 90 * 16);
            painter->drawLine(QPointF(15, 5.5), QPointF(15, 6.5));
            break;
        case Icon::Check:
            painter->drawLine(QPointF(5, 12.5), QPointF(9.5, 17));
            painter->drawLine(QPointF(9.5, 17), QPointF(19, 7.5));
            break;
        case Icon::Refresh:
            painter->drawArc(QRectF(4, 4, 16, 16), 45 * 16, 285 * 16);
            painter->drawLine(QPointF(19.5, 4.5), QPointF(19.5, 9));
            painter->drawLine(QPointF(19.5, 9), QPointF(15, 9));
            break;
        case Icon::Edit: {
            QPainterPath path;
            path.moveTo(4, 20);
            path.lineTo(5, 15.5);
            path.lineTo(15.5, 5);
            path.lineTo(19, 8.5);
            path.lineTo(8.5, 19);
            path.closeSubpath();
            painter->drawPath(path);
            painter->drawLine(QPointF(13.5, 7), QPointF(17, 10.5));
            break;
        }
        case Icon::ErrorCircle:
            painter->drawEllipse(QRectF(3.5, 3.5, 17, 17));
            painter->drawLine(QPointF(9, 9), QPointF(15, 15));
            painter->drawLine(QPointF(15, 9), QPointF(9, 15));
            break;
        case Icon::Microphone:
            painter->drawRoundedRect(QRectF(9, 3.5, 6, 10.5), 3, 3);
            painter->drawArc(QRectF(5.5, 8, 13, 9.5), 180 * 16, 180 * 16);
            painter->drawLine(QPointF(12, 17.5), QPointF(12, 20.5));
            painter->drawLine(QPointF(8.5, 20.5), QPointF(15.5, 20.5));
            break;
        case Icon::MicrophoneOff:
            painter->drawRoundedRect(QRectF(9, 3.5, 6, 10.5), 3, 3);
            painter->drawArc(QRectF(5.5, 8, 13, 9.5), 180 * 16, 180 * 16);
            painter->drawLine(QPointF(12, 17.5), QPointF(12, 20.5));
            painter->drawLine(QPointF(8.5, 20.5), QPointF(15.5, 20.5));
            painter->drawLine(QPointF(4, 4), QPointF(20, 20));
            break;
        case Icon::Send:
            painter->drawLine(QPointF(12, 18.5), QPointF(12, 5.5));
            painter->drawLine(QPointF(6.5, 11), QPointF(12, 5.5));
            painter->drawLine(QPointF(12, 5.5), QPointF(17.5, 11));
            break;
        case Icon::Stop:
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawRoundedRect(QRectF(7, 7, 10, 10), 2, 2);
            break;
        case Icon::Sparkle: {
            QPainterPath path;
            path.moveTo(12, 3);
            path.quadTo(12, 12, 21, 12);
            path.quadTo(12, 12, 12, 21);
            path.quadTo(12, 12, 3, 12);
            path.quadTo(12, 12, 12, 3);
            painter->drawPath(path);
            break;
        }
        case Icon::ChatMode: {
            QPainterPath path;
            path.moveTo(5, 5);
            path.lineTo(19, 5);
            path.quadTo(20.5, 5, 20.5, 6.5);
            path.lineTo(20.5, 15.5);
            path.quadTo(20.5, 17, 19, 17);
            path.lineTo(9, 17);
            path.lineTo(4.5, 20.5);
            path.lineTo(4.5, 17);
            path.quadTo(3.5, 17, 3.5, 15.5);
            path.lineTo(3.5, 6.5);
            path.quadTo(3.5, 5, 5, 5);
            painter->drawPath(path);
            break;
        }
        case Icon::Antenna:
            painter->drawLine(QPointF(12, 11), QPointF(12, 20));
            painter->drawLine(QPointF(8.5, 20), QPointF(15.5, 20));
            painter->drawLine(QPointF(12, 11), QPointF(9, 20));
            painter->drawLine(QPointF(12, 11), QPointF(15, 20));
            painter->drawArc(QRectF(8, 5, 8, 8), 35 * 16, 110 * 16);
            painter->drawArc(QRectF(5, 2.5, 14, 13), 35 * 16, 110 * 16);
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawEllipse(QPointF(12, 9.5), 1.6, 1.6);
            break;
        case Icon::ArrowDown:
            painter->drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
            painter->drawLine(QPointF(6.5, 13), QPointF(12, 18.5));
            painter->drawLine(QPointF(12, 18.5), QPointF(17.5, 13));
            break;
        case Icon::Trash:
            painter->drawLine(QPointF(4, 7), QPointF(20, 7));
            painter->drawLine(QPointF(9.5, 7), QPointF(9.5, 4.5));
            painter->drawLine(QPointF(9.5, 4.5), QPointF(14.5, 4.5));
            painter->drawLine(QPointF(14.5, 4.5), QPointF(14.5, 7));
            painter->drawRoundedRect(QRectF(6, 7, 12, 13), 1.5, 1.5);
            painter->drawLine(QPointF(10, 10.5), QPointF(10, 16.5));
            painter->drawLine(QPointF(14, 10.5), QPointF(14, 16.5));
            break;
        case Icon::Calendar:
            painter->drawRoundedRect(QRectF(3.5, 5, 17, 15.5), 2.5, 2.5);
            painter->drawLine(QPointF(8, 3), QPointF(8, 7));
            painter->drawLine(QPointF(16, 3), QPointF(16, 7));
            painter->drawLine(QPointF(3.5, 10), QPointF(20.5, 10));
            break;
        case Icon::ScreenShare:
            painter->drawRoundedRect(QRectF(3, 4.5, 18, 12.5), 2, 2);
            painter->drawLine(QPointF(8.5, 20), QPointF(15.5, 20));
            painter->drawLine(QPointF(12, 17), QPointF(12, 20));
            painter->drawLine(QPointF(12, 14), QPointF(12, 8.2));
            painter->drawLine(QPointF(9.2, 11), QPointF(12, 8.2));
            painter->drawLine(QPointF(14.8, 11), QPointF(12, 8.2));
            break;
        case Icon::Shield: {
            QPainterPath path;
            path.moveTo(12, 3.2);
            path.lineTo(19, 6.2);
            path.lineTo(19, 11.8);
            path.cubicTo(19, 16.3, 15.9, 19.3, 12, 20.8);
            path.cubicTo(8.1, 19.3, 5, 16.3, 5, 11.8);
            path.lineTo(5, 6.2);
            path.closeSubpath();
            painter->drawPath(path);
            break;
        }
        case Icon::Eye: {
            QPainterPath path;
            path.moveTo(2.5, 12);
            path.quadTo(12, 3.5, 21.5, 12);
            path.quadTo(12, 20.5, 2.5, 12);
            painter->drawPath(path);
            painter->drawEllipse(QPointF(12, 12), 2.8, 2.8);
            break;
        }
        case Icon::EyeOff: {
            QPainterPath path;
            path.moveTo(2.5, 12);
            path.quadTo(12, 3.5, 21.5, 12);
            path.quadTo(12, 20.5, 2.5, 12);
            painter->drawPath(path);
            painter->drawEllipse(QPointF(12, 12), 2.5, 2.5);
            painter->drawLine(QPointF(4, 4), QPointF(20, 20));
            break;
        }
        case Icon::Server:
            painter->drawRoundedRect(QRectF(3.5, 4, 17, 6.5), 2, 2);
            painter->drawRoundedRect(QRectF(3.5, 13.5, 17, 6.5), 2, 2);
            painter->drawPoint(QPointF(7, 7.25));
            painter->drawPoint(QPointF(7, 16.75));
            painter->drawLine(QPointF(11, 7.25), QPointF(17, 7.25));
            painter->drawLine(QPointF(11, 16.75), QPointF(17, 16.75));
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
    std::optional<ColorRole> _role;
};
} // namespace

QIcon icon(Icon kind) { return QIcon(new ThemeIconEngine(kind)); }
QIcon icon(Icon kind, ColorRole role) { return QIcon(new ThemeIconEngine(kind, role)); }
} // namespace MeetingUI::AppTheme
