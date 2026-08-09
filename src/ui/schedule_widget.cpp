#include "src/ui/schedule_widget.h"
#include "styles/style_widgets.h"
#include <QtGui/QMouseEvent>
#include <QtGui/QFont>
#include <QtCore/QLocale>

namespace MeetingUI {

// ----------------------------------------------------
// FloatingActionButton (右下角圆环日历添加按钮)
// ----------------------------------------------------

FloatingActionButton::FloatingActionButton(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedSize(54, 54);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	// 100% 复用 TDeskTop 内部 Ui::BoxShadow 九宫格高斯烘焙阴影
	_shadow = std::make_unique<Ui::BoxShadow>(style::BoxShadow{
		.blurRadius = 10,
		.offset = QPoint(0, 3),
		.opacity = 0.22,
	});

	_hoverShadow = std::make_unique<Ui::BoxShadow>(style::BoxShadow{
		.blurRadius = 16,
		.offset = QPoint(0, 5),
		.opacity = 0.38,
	});
}

void FloatingActionButton::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	const int w = width();
	const int h = height();
	const int cx = w / 2;
	const int cy = h / 2;
	const int r = 21;
	const QRect circleBox(cx - r, cy - r, r * 2, r * 2);

	// 1. TDeskTop 原生九宫格高斯阴影贴图
	if (_hovered && _hoverShadow) {
		_hoverShadow->paint(p, circleBox, r);
	} else if (_shadow) {
		_shadow->paint(p, circleBox, r);
	}

	// 2. 纯白背景圆底 + 微弱高光边缘
	p.save();
	p.setBrush(_pressed ? QColor(0xf0, 0xf3, 0xf8) : Qt::white);
	p.setPen(QPen(QColor(0xe0, 0xe4, 0xec), 1));
	p.drawEllipse(circleBox);
	p.restore();

	// 绘制内部日历加号图标 (📅+)
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(QColor(0x30, 0x31, 0x33), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);

	// 日历外框
	const int calX = cx - 9;
	const int calY = cy - 8;
	const int calW = 18;
	const int calH = 17;
	p.drawRoundedRect(calX, calY, calW, calH, 2, 2);

	// 日历顶部挂耳与横线
	p.drawLine(calX + 4, calY - 2, calX + 4, calY);
	p.drawLine(calX + 14, calY - 2, calX + 14, calY);
	p.drawLine(calX, calY + 5, calX + calW, calY + 5);

	// 日历内部小加号 (+)
	const int px = cx;
	const int py = calY + 11;
	p.drawLine(px - 3, py, px + 3, py);
	p.drawLine(px, py - 3, px, py + 3);

	p.restore();
}

void FloatingActionButton::mouseMoveEvent(QMouseEvent *e) {
	update();
}

void FloatingActionButton::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = true;
		update();
	}
}

void FloatingActionButton::mouseReleaseEvent(QMouseEvent *e) {
	if (_pressed) {
		_pressed = false;
		update();
		if (rect().contains(e->pos())) {
			_clicks.fire({});
		}
	}
}

void FloatingActionButton::enterEventHook(QEnterEvent *e) {
	_hovered = true;
	update();
}

void FloatingActionButton::leaveEventHook(QEvent *e) {
	_hovered = false;
	_pressed = false;
	update();
}

// ----------------------------------------------------
// ScheduleWidget 日程与暂无会议展示区
// ----------------------------------------------------

ScheduleWidget::ScheduleWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_fabButton = new FloatingActionButton(this);
}

void ScheduleWidget::resizeEvent(QResizeEvent *e) {
	const int fabMargin = 28;
	_fabButton->move(width() - _fabButton->width() - fabMargin, height() - _fabButton->height() - fabMargin);
}

QString ScheduleWidget::getFormattedDate() const {
	const QDate d = QDate::currentDate();
	return QString::fromUtf8("%1月%2日").arg(d.month()).arg(d.day());
}

QString ScheduleWidget::getFormattedSubDate() const {
	const QDate d = QDate::currentDate();
	const int dayOfWeek = d.dayOfWeek();
	static const char *weekdays[] = {
		"周一", "周二", "周三", "周四", "周五", "周六", "周日"
	};
	const QString weekStr = (dayOfWeek >= 1 && dayOfWeek <= 7) ? QString::fromUtf8(weekdays[dayOfWeek - 1]) : QString::fromUtf8("周日");
	// 农历显示示例（与实际日期对应）
	return QString::fromUtf8("%1 农历六月廿七").arg(weekStr);
}

void ScheduleWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	// 绘制顶部日期和全部会议入口
	drawHeader(p);

	// 绘制中间咖啡杯矢量插画与“暂无会议”
	const int startY = 120;
	const QRect emptyArea(0, startY, width(), height() - startY - 70);
	drawEmptyCoffeeIllustration(p, emptyArea);
}

void ScheduleWidget::drawHeader(QPainter &p) {
	p.save();
	const int left = 32;
	const int top = 32;
	const int rightMargin = 32;
	const int w = width();

	// 主日期：例如 "8月9日" (字号 28px)
	QFont bigFont;
	bigFont.setFamily("Microsoft YaHei");
	bigFont.setPixelSize(28);
	bigFont.setBold(true);
	p.setFont(bigFont);
	p.setPen(QColor(0x1f, 0x23, 0x29));

	const QString mainDate = getFormattedDate();
	p.drawText(QRect(left, top, 200, 36), Qt::AlignLeft | Qt::AlignVCenter, mainDate);

	// 副日期：例如 "周日 农历六月廿七" (字号 13px)
	QFont subFont;
	subFont.setFamily("Microsoft YaHei");
	subFont.setPixelSize(13);
	subFont.setBold(false);
	p.setFont(subFont);
	p.setPen(QColor(0x8f, 0x95, 0x9e));

	const QString subDate = getFormattedSubDate();
	p.drawText(QRect(left, top + 42, 240, 20), Qt::AlignLeft | Qt::AlignVCenter, subDate);

	// 右侧 "全部会议 >"
	const QString allMeetingsText = QString::fromUtf8("全部会议 >");
	QFontMetrics fm(subFont);
	const int allW = fm.horizontalAdvance(allMeetingsText) + 8;
	_allMeetingsRect = QRect(w - rightMargin - allW, top + 42, allW, 20);

	if (_allMeetingsHovered) {
		p.setPen(QColor(0x16, 0x77, 0xff));
	} else {
		p.setPen(QColor(0x8f, 0x95, 0x9e));
	}
	p.drawText(_allMeetingsRect, Qt::AlignRight | Qt::AlignVCenter, allMeetingsText);

	// 分割线
	p.setPen(QColor(0xeb, 0xed, 0xf0));
	p.drawLine(left, top + 74, w - rightMargin, top + 74);

	p.restore();
}

void ScheduleWidget::drawEmptyCoffeeIllustration(QPainter &p, const QRect &area) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	const int cx = area.center().x();
	const int cy = area.center().y() - 10;

	// 1. 咖啡碟投影与底座
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x16, 0x77, 0xff, 15));
	p.drawEllipse(QPoint(cx, cy + 34), 64, 18);

	// 咖啡碟主体（浅蓝渐变碟）
	QLinearGradient saucerGrad(cx - 50, cy + 20, cx + 50, cy + 36);
	saucerGrad.setColorAt(0.0, QColor(0xe5, 0xf0, 0xff));
	saucerGrad.setColorAt(0.5, QColor(0xd6, 0xe8, 0xff));
	saucerGrad.setColorAt(1.0, QColor(0xc4, 0xde, 0xff));
	p.setBrush(saucerGrad);
	p.drawEllipse(QPoint(cx, cy + 28), 56, 14);

	// 咖啡碟内凹圆弧
	p.setBrush(QColor(0xbb, 0xd8, 0xff, 80));
	p.drawEllipse(QPoint(cx, cy + 27), 40, 9);

	// 2. 咖啡杯把手 (右侧圆环半圆)
	p.setPen(QPen(QColor(0xd0, 0xe4, 0xff), 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	p.drawArc(cx + 12, cy - 14, 30, 32, -60 * 16, 180 * 16);

	// 3. 咖啡杯身（圆润梯形带底圆）
	QPainterPath cupPath;
	cupPath.moveTo(cx - 28, cy - 16);
	cupPath.lineTo(cx - 22, cy + 18);
	cupPath.quadTo(cx, cy + 26, cx + 22, cy + 18);
	cupPath.lineTo(cx + 28, cy - 16);
	cupPath.closeSubpath();

	QLinearGradient cupGrad(cx - 30, cy - 16, cx + 30, cy + 20);
	cupGrad.setColorAt(0.0, QColor(0xf6, 0xf9, 0xff));
	cupGrad.setColorAt(0.4, QColor(0xe2, 0xee, 0xff));
	cupGrad.setColorAt(1.0, QColor(0xcb, 0xe2, 0xff));
	p.setPen(Qt::NoPen);
	p.setBrush(cupGrad);
	p.drawPath(cupPath);

	// 杯身高光弧线
	p.setPen(QPen(QColor(255, 255, 255, 180), 2.5, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(cx - 20, cy - 10, cx - 16, cy + 12);

	// 4. 咖啡杯口椭圆
	QLinearGradient rimGrad(cx, cy - 20, cx, cy - 12);
	rimGrad.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
	rimGrad.setColorAt(1.0, QColor(0xd8, 0xe8, 0xff));
	p.setPen(Qt::NoPen);
	p.setBrush(rimGrad);
	p.drawEllipse(QPoint(cx, cy - 16), 28, 7);

	// 杯内热咖啡液体（深色/浅暖褐渐变）
	QLinearGradient coffeeGrad(cx, cy - 18, cx, cy - 14);
	coffeeGrad.setColorAt(0.0, QColor(0xe0, 0xec, 0xfc));
	coffeeGrad.setColorAt(1.0, QColor(0xc4, 0xdb, 0xf8));
	p.setBrush(coffeeGrad);
	p.drawEllipse(QPoint(cx, cy - 16), 25, 5);

	// 5. 方糖立方体（放置在杯底右侧）
	const int cubeX = cx + 18;
	const int cubeY = cy + 22;
	// 顶面
	QPainterPath topPath;
	topPath.moveTo(cubeX, cubeY);
	topPath.lineTo(cubeX + 7, cubeY - 4);
	topPath.lineTo(cubeX + 14, cubeY);
	topPath.lineTo(cubeX + 7, cubeY + 4);
	topPath.closeSubpath();
	p.setBrush(QColor(0xff, 0xff, 0xff));
	p.drawPath(topPath);
	// 左侧面
	QPainterPath leftPath;
	leftPath.moveTo(cubeX, cubeY);
	leftPath.lineTo(cubeX + 7, cubeY + 4);
	leftPath.lineTo(cubeX + 7, cubeY + 12);
	leftPath.lineTo(cubeX, cubeY + 8);
	leftPath.closeSubpath();
	p.setBrush(QColor(0xeb, 0xf3, 0xff));
	p.drawPath(leftPath);
	// 右侧面
	QPainterPath rightPath;
	rightPath.moveTo(cubeX + 7, cubeY + 4);
	rightPath.lineTo(cubeX + 14, cubeY);
	rightPath.lineTo(cubeX + 14, cubeY + 8);
	rightPath.lineTo(cubeX + 7, cubeY + 12);
	rightPath.closeSubpath();
	p.setBrush(QColor(0xd2, 0xe4, 0xfc));
	p.drawPath(rightPath);

	// 6. 升起的香气热气波浪曲线
	drawCoffeeSteam(p, cx, cy - 24);

	// 7. 下方“暂无会议”文本
	QFont textFont;
	textFont.setFamily("Microsoft YaHei");
	textFont.setPixelSize(14);
	textFont.setBold(false);
	p.setFont(textFont);
	p.setPen(QColor(0x8f, 0x95, 0x9e));
	p.drawText(QRect(cx - 100, cy + 62, 200, 24), Qt::AlignCenter, QString::fromUtf8("暂无会议"));

	p.restore();
}

void ScheduleWidget::drawCoffeeSteam(QPainter &p, int cx, int cy) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	// 三道柔美白雾热气曲线
	QPen steamPen(QColor(0x82, 0xb4, 0xf8, 70), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(steamPen);
	p.setBrush(Qt::NoBrush);

	// 中间热气
	QPainterPath s1;
	s1.moveTo(cx, cy);
	s1.cubicTo(cx - 6, cy - 8, cx + 6, cy - 16, cx, cy - 24);
	p.drawPath(s1);

	// 左侧热气
	QPainterPath s2;
	s2.moveTo(cx - 10, cy + 2);
	s2.cubicTo(cx - 16, cy - 6, cx - 6, cy - 14, cx - 12, cy - 20);
	p.drawPath(s2);

	// 右侧热气
	QPainterPath s3;
	s3.moveTo(cx + 10, cy + 2);
	s3.cubicTo(cx + 6, cy - 6, cx + 16, cy - 14, cx + 10, cy - 20);
	p.drawPath(s3);

	p.restore();
}

void ScheduleWidget::mouseMoveEvent(QMouseEvent *e) {
	const bool hovered = _allMeetingsRect.contains(e->pos());
	if (hovered != _allMeetingsHovered) {
		_allMeetingsHovered = hovered;
		update();
	}
}

void ScheduleWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _allMeetingsRect.contains(e->pos())) {
		_allMeetingsClicks.fire({});
	}
}

void ScheduleWidget::leaveEventHook(QEvent *e) {
	_allMeetingsHovered = false;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

} // namespace MeetingUI
