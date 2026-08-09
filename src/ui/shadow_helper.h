#pragma once

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtCore/QRect>
#include <QtCore/QPoint>
#include <QtGui/QColor>

namespace MeetingUI {

class ShadowHelper {
public:
	// 绘制多层环境柔和阴影 (Ambient + Key Light Blur Shadow)
	static void drawBoxShadow(
			QPainter &p,
			const QRect &box,
			int radius,
			int blur,
			const QPoint &offset,
			const QColor &color) {
		p.save();
		p.setRenderHint(QPainter::Antialiasing, true);

		// 多层扩散渲染高斯逼真感
		const int layers = qMax(4, blur / 2);
		const qreal baseAlpha = color.alphaF();

		for (int i = layers; i >= 1; --i) {
			const qreal factor = static_cast<qreal>(i) / layers;
			const int spread = static_cast<int>(blur * factor);
			const QRect layerRect = box.adjusted(
				offset.x() - spread,
				offset.y() - spread,
				offset.x() + spread,
				offset.y() + spread
			);

			QColor layerColor = color;
			// 越靠外层透明度越低，平方递减模拟光子衰减
			layerColor.setAlphaF(baseAlpha * (1.0 - factor * factor) / layers * 2.2);

			QPainterPath path;
			path.addRoundedRect(layerRect, radius + spread, radius + spread);
			p.fillPath(path, layerColor);
		}

		p.restore();
	}

	// 绘制带内发光/微弱描边的抗锯齿圆角矩形
	static void drawRoundedSurface(
			QPainter &p,
			const QRect &box,
			int radius,
			const QBrush &fillBrush,
			const QColor &borderColor = QColor(0, 0, 0, 0),
			qreal borderWidth = 1.0) {
		p.save();
		p.setRenderHint(QPainter::Antialiasing, true);

		QPainterPath path;
		path.addRoundedRect(box, radius, radius);
		p.fillPath(path, fillBrush);

		if (borderColor.alpha() > 0 && borderWidth > 0.0) {
			p.setPen(QPen(borderColor, borderWidth));
			p.setBrush(Qt::NoBrush);
			p.drawPath(path);
		}

		p.restore();
	}
};

} // namespace MeetingUI
