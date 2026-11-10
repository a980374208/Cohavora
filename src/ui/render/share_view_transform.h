#pragma once
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <algorithm>
#include <cmath>

namespace MeetingUI {
// Source pixels -> logical viewport. zoom == 0 means fit; zoom == 1 maps
// one source pixel to one physical display pixel, independent of Qt DPI.
struct ShareViewTransform {
    QSize source;
    QSizeF viewport;
    int rotation = 0;
    double dpr = 1.0;
    double zoom = 0.0;
    QPointF center{0.5, 0.5};
    QSize orientedSource() const {
        return rotation == 90 || rotation == 270 ? source.transposed() : source;
    }
    QSizeF displaySize() const {
        const auto size = orientedSource();
        if (size.isEmpty() || viewport.isEmpty()) return {};
        const double ratio = std::isfinite(dpr) && dpr > 0 ? dpr : 1.0;
        const double scale = zoom > 0 ? zoom / ratio :
            std::min(viewport.width() / size.width(), viewport.height() / size.height());
        return QSizeF(size) * scale;
    }
    void clamp() {
        const auto size = displaySize();
        const auto axis = [](double c, double view, double image) {
            if (image <= view || image <= 0) return 0.5;
            const double edge = view / (2 * image);
            return std::clamp(c, edge, 1 - edge);
        };
        center = {axis(center.x(), viewport.width(), size.width()),
                  axis(center.y(), viewport.height(), size.height())};
    }
    QRectF destination() const {
        auto value = *this; value.clamp();
        const auto size = value.displaySize();
        return {QPointF(viewport.width()/2 - value.center.x()*size.width(),
                       viewport.height()/2 - value.center.y()*size.height()), size};
    }
    void setZoom(double value) {
        zoom = value > 0 ? std::clamp(value, 0.25, 4.0) : 0;
        if (!zoom) center = {0.5, 0.5};
        clamp();
    }
    void pan(QPointF delta) {
        const auto size = displaySize();
        if (size.isEmpty() || !zoom) return;
        center -= QPointF(delta.x()/size.width(), delta.y()/size.height());
        clamp();
    }
    QPointF sourceAt(QPointF logical) const {
        const auto rect = destination();
        if (rect.isEmpty()) return {};
        const double x = (logical.x()-rect.x())/rect.width();
        const double y = (logical.y()-rect.y())/rect.height();
        switch (rotation) {
        case 90: return {y*source.width(), (1-x)*source.height()};
        case 180: return {(1-x)*source.width(), (1-y)*source.height()};
        case 270: return {(1-y)*source.width(), x*source.height()};
        default: return {x*source.width(), y*source.height()};
        }
    }
};
} // namespace MeetingUI
