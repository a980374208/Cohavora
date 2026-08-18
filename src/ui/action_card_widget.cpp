#include "src/ui/action_card_widget.h"
#include "styles/style_widgets.h"
#include <QtGui/QMouseEvent>
#include <QtGui/QFont>

namespace MeetingUI {

ActionCardWidget::ActionCardWidget(
	QWidget *parent,
	ActionCardType type,
	const QString &title,
	bool hasDropdown)
	: Ui::RpWidget(parent)
	, _type(type)
	, _title(title)
	, _hasDropdown(hasDropdown) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	// 100% 复用 TDeskTop 内部 Ui::BoxShadow 九宫格高斯模糊烘焙阴影
	_normalShadow = std::make_unique<Ui::BoxShadow>(style::BoxShadow{
		.blurRadius = 12,
		.offset = QPoint(0, 4),
		.opacity = 0.28,
	});

	_hoverShadow = std::make_unique<Ui::BoxShadow>(style::BoxShadow{
		.blurRadius = 18,
		.offset = QPoint(0, 7),
		.opacity = 0.42,
	});
}

void ActionCardWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	const int w = width();
	const int h = height();

	// 按钮卡片区域（上部方形卡片）
	const int cardSize = std::min(w, 126);
	const int cardY = (_hovered ? 2 : 5); // 悬浮上浮动效
	_cardRect = QRect((w - cardSize) / 2, cardY, cardSize, cardSize);

	// 1. 调用 TDeskTop 原生 Ui::BoxShadow 进行九宫格抗锯齿阴影贴图
	if (_hovered && _hoverShadow) {
		_hoverShadow->paint(p, _cardRect, 20);
	} else if (_normalShadow) {
		_normalShadow->paint(p, _cardRect, 20);
	}

	// 2. 绘制卡片主体（天蓝色流光渐变 + 20px 圆角）
	p.save();
	QLinearGradient grad(_cardRect.topLeft(), _cardRect.bottomLeft());
	if (_pressed) {
		grad.setColorAt(0.0, QColor(0x0e, 0x5e, 0xce));
		grad.setColorAt(1.0, QColor(0x12, 0x64, 0xd8));
	} else if (_hovered) {
		grad.setColorAt(0.0, QColor(0x35, 0x8d, 0xff));
		grad.setColorAt(1.0, QColor(0x13, 0x72, 0xfc));
	} else {
		grad.setColorAt(0.0, QColor(0x1c, 0x7c, 0xff));
		grad.setColorAt(1.0, QColor(0x10, 0x6d, 0xec));
	}

	p.setPen(QPen(QColor(255, 255, 255, _hovered ? 90 : 50), 1.0));
	p.setBrush(grad);
	p.drawRoundedRect(_cardRect, 20, 20);

	// 绘制卡片中央大图标
	drawCardIcon(p, _cardRect);
	p.restore();

	// 绘制下方标题文本
	QFont font = p.font();
	font.setFamily("Microsoft YaHei");
	font.setPixelSize(14);
	font.setBold(false);
	p.setFont(font);
	p.setPen(QColor(0x1f, 0x23, 0x29));

	const int textTop = cardY + cardSize + 12;
	QRect textRect(0, textTop, w, 22);

	if (_hasDropdown) {
		// 计算带下拉小箭头的居中位置
		QFontMetrics fm(font);
		const int textW = fm.horizontalAdvance(_title);
		const int totalW = textW + 14;
		const int startX = (w - totalW) / 2;

		p.drawText(QRect(startX, textTop, textW, 22), Qt::AlignLeft | Qt::AlignVCenter, _title);

		// 绘制小下拉箭头 ∨
		p.save();
		p.setPen(QPen(QColor(0x60, 0x62, 0x66), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		const int arrowX = startX + textW + 6;
		const int arrowY = textTop + 8;
		QPainterPath arrowPath;
		arrowPath.moveTo(arrowX, arrowY);
		arrowPath.lineTo(arrowX + 4, arrowY + 4);
		arrowPath.lineTo(arrowX + 8, arrowY);
		p.drawPath(arrowPath);
		_dropdownRect = QRect(arrowX - 4, textTop, 18, 22);
		p.restore();
	} else {
		p.drawText(textRect, Qt::AlignCenter, _title);
		_dropdownRect = QRect();
	}
}

void ActionCardWidget::drawCardIcon(QPainter &p, const QRect &r) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	const int cx = r.center().x();
	const int cy = r.center().y();

	p.setPen(QPen(Qt::white, 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);

	if (_type == ActionCardType::JoinMeeting) {
		// 粗白色大加号 (+)
		const int len = 16;
		p.drawLine(cx - len, cy, cx + len, cy);
		p.drawLine(cx, cy - len, cx, cy + len);
	} else if (_type == ActionCardType::QuickMeeting) {
		// 动感白色闪电 (⚡)
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);

		QPainterPath path;
		path.moveTo(cx + 4, cy - 22);
		path.lineTo(cx - 14, cy + 2);
		path.lineTo(cx - 1, cy + 2);
		path.lineTo(cx - 6, cy + 22);
		path.lineTo(cx + 14, cy - 2);
		path.lineTo(cx + 1, cy - 2);
		path.closeSubpath();
		p.drawPath(path);
	} else if (_type == ActionCardType::ScheduleMeeting) {
		// 圆角盾牌勾选 / 大对勾 (✔)
		p.setPen(QPen(Qt::white, 6.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);

		QPainterPath path;
		path.moveTo(cx - 14, cy);
		path.lineTo(cx - 4, cy + 10);
		path.lineTo(cx + 14, cy - 10);
		p.drawPath(path);
	} else if (_type == ActionCardType::ShareScreen) {
		// 共享屏幕双视窗 (🗔)
		p.setPen(QPen(Qt::white, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);

		// 后层屏幕
		QRect backScreen(cx - 14, cy - 14, 20, 16);
		p.drawRoundedRect(backScreen, 3, 3);

		// 前层主屏幕（填充加白边）
		p.setBrush(QColor(0x16, 0x77, 0xff));
		QRect frontScreen(cx - 6, cy - 6, 22, 18);
		p.drawRoundedRect(frontScreen, 3, 3);

		// 底座支架
		p.drawLine(cx + 5, cy + 12, cx + 5, cy + 16);
		p.drawLine(cx, cy + 16, cx + 10, cy + 16);
	} else if (_type == ActionCardType::SimulcastTest) {
		// 实验烧杯 / 性能流测试 (🧪)
		p.setPen(QPen(Qt::white, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);

		// 试管瓶身
		QPainterPath path;
		path.moveTo(cx - 6, cy - 16);
		path.lineTo(cx + 6, cy - 16);
		path.moveTo(cx - 4, cy - 16);
		path.lineTo(cx - 4, cy - 6);
		path.lineTo(cx - 14, cy + 12);
		path.arcTo(cx - 14, cy + 4, 28, 16, 180, 180);
		path.lineTo(cx + 4, cy - 6);
		path.lineTo(cx + 4, cy - 16);
		p.drawPath(path);

		// 试管内波浪液体
		p.setBrush(QColor(0x00, 0xb4, 0x2a));
		p.drawEllipse(cx - 4, cy + 4, 8, 8);
	}

	p.restore();
}

void ActionCardWidget::mouseMoveEvent(QMouseEvent *e) {
	update();
}

void ActionCardWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = true;
		update();
	}
}

void ActionCardWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (_pressed) {
		_pressed = false;
		update();
		if (_cardRect.contains(e->pos()) || rect().contains(e->pos())) {
			if (_hasDropdown && _dropdownRect.contains(e->pos())) {
				_dropdownClicks.fire_copy(_type);
			} else {
				_clicks.fire_copy(_type);
			}
		}
	}
}

void ActionCardWidget::enterEventHook(QEnterEvent *e) {
	_hovered = true;
	update();
}

void ActionCardWidget::leaveEventHook(QEvent *e) {
	_hovered = false;
	_pressed = false;
	update();
}

// ----------------------------------------------------
// ActionGridContainer 容器
// ----------------------------------------------------

ActionGridContainer::ActionGridContainer(QWidget *parent)
	: Ui::RpWidget(parent) {
	_joinCard = new ActionCardWidget(this, ActionCardType::JoinMeeting, QString::fromUtf8("加入会议"), false);
	_quickCard = new ActionCardWidget(this, ActionCardType::QuickMeeting, QString::fromUtf8("快速会议"), true);
	_scheduleCard = new ActionCardWidget(this, ActionCardType::ScheduleMeeting, QString::fromUtf8("预定会议"), true);
	_shareCard = new ActionCardWidget(this, ActionCardType::ShareScreen, QString::fromUtf8("共享屏幕"), false);
	_testCard = new ActionCardWidget(this, ActionCardType::SimulcastTest, QString::fromUtf8("百人 & Simulcast"), false);

	_joinCard->clicked() | rpl::on_next([this](ActionCardType t) { _cardClicks.fire_copy(t); }, lifetime());
	_quickCard->clicked() | rpl::on_next([this](ActionCardType t) { _cardClicks.fire_copy(t); }, lifetime());
	_scheduleCard->clicked() | rpl::on_next([this](ActionCardType t) { _cardClicks.fire_copy(t); }, lifetime());
	_shareCard->clicked() | rpl::on_next([this](ActionCardType t) { _cardClicks.fire_copy(t); }, lifetime());
	_testCard->clicked() | rpl::on_next([this](ActionCardType t) { _cardClicks.fire_copy(t); }, lifetime());
}

void ActionGridContainer::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	const int cardW = 120;
	const int cardH = 150;
	const int gapX = 24;
	const int gapY = 24;

	const int totalGridW = cardW * 3 + gapX * 2;
	const int totalGridH = cardH * 2 + gapY;

	const int startX = std::max(12, (w - totalGridW) / 2);
	const int startY = std::max(12, (h - totalGridH) / 2);

	_joinCard->setGeometry(startX, startY, cardW, cardH);
	_quickCard->setGeometry(startX + cardW + gapX, startY, cardW, cardH);
	_scheduleCard->setGeometry(startX + (cardW + gapX) * 2, startY, cardW, cardH);
	
	_shareCard->setGeometry(startX, startY + cardH + gapY, cardW, cardH);
	_testCard->setGeometry(startX + cardW + gapX, startY + cardH + gapY, cardW, cardH);
}

} // namespace MeetingUI
