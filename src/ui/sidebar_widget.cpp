#include "src/ui/sidebar_widget.h"
#include <QtGui/QMouseEvent>
#include <QtGui/QFont>

namespace MeetingUI {

SidebarWidget::SidebarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void SidebarWidget::setActiveNav(NavItemType type) {
	if (_activeNav != type) {
		_activeNav = type;
		_navChanges.fire_copy(_activeNav);
		update();
	}
}

void SidebarWidget::setCornerRadius(int radius) {
	if (_cornerRadius != radius) {
		_cornerRadius = radius;
		update();
	}
}

void SidebarWidget::updateLayout() {
	const int w = width();
	const int h = height();

	// 顶部头像位置
	const int avatarSize = 40;
	_avatarRect = QRect((w - avatarSize) / 2, 24, avatarSize, avatarSize);

	// 中间导航项
	_navItems.clear();
	const int navItemWidth = 56;
	const int navItemHeight = 56;
	const int navStartY = 84;
	const int navSpacing = 12;

	NavItem meetingNav;
	meetingNav.type = NavItemType::Meeting;
	meetingNav.text = QString::fromUtf8("会议");
	meetingNav.rect = QRect((w - navItemWidth) / 2, navStartY, navItemWidth, navItemHeight);
	_navItems.push_back(meetingNav);

	NavItem contactsNav;
	contactsNav.type = NavItemType::Contacts;
	contactsNav.text = QString::fromUtf8("通讯录");
	contactsNav.rect = QRect((w - navItemWidth) / 2, navStartY + (navItemHeight + navSpacing), navItemWidth, navItemHeight);
	_navItems.push_back(contactsNav);

	NavItem recNav;
	recNav.type = NavItemType::Recordings;
	recNav.text = QString::fromUtf8("录制");
	recNav.rect = QRect((w - navItemWidth) / 2, navStartY + (navItemHeight + navSpacing) * 2, navItemWidth, navItemHeight);
	_navItems.push_back(recNav);

	// 底部辅助项
	_bottomItems.clear();
	const int bottomItemSize = 36;
	const int bottomSpacing = 8;
	int bottomY = h - 20 - bottomItemSize;

	BottomItem userItem;
	userItem.type = BottomItemType::User;
	userItem.tooltip = QString::fromUtf8("用户中心");
	userItem.rect = QRect((w - bottomItemSize) / 2, bottomY, bottomItemSize, bottomItemSize);
	_bottomItems.push_back(userItem);

	bottomY -= (bottomItemSize + bottomSpacing);
	BottomItem settingsItem;
	settingsItem.type = BottomItemType::Settings;
	settingsItem.tooltip = QString::fromUtf8("设置");
	settingsItem.rect = QRect((w - bottomItemSize) / 2, bottomY, bottomItemSize, bottomItemSize);
	settingsItem.hasRedDot = true;
	_bottomItems.push_back(settingsItem);

	bottomY -= (bottomItemSize + bottomSpacing);
	BottomItem mailItem;
	mailItem.type = BottomItemType::Mail;
	mailItem.tooltip = QString::fromUtf8("消息与邮件");
	mailItem.rect = QRect((w - bottomItemSize) / 2, bottomY, bottomItemSize, bottomItemSize);
	_bottomItems.push_back(mailItem);
}

void SidebarWidget::paintEvent(QPaintEvent *e) {
	updateLayout();

	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	const int w = width();
	const int h = height();

	// 背景色：左上和左下为平滑圆角，右上和右下为直角以贴合右侧主操作区
	if (_cornerRadius > 0) {
		QPainterPath bgPath;
		bgPath.moveTo(w, 0);
		bgPath.lineTo(_cornerRadius, 0);
		bgPath.arcTo(0, 0, _cornerRadius * 2, _cornerRadius * 2, 90, 90);
		bgPath.lineTo(0, h - _cornerRadius);
		bgPath.arcTo(0, h - _cornerRadius * 2, _cornerRadius * 2, _cornerRadius * 2, 180, 90);
		bgPath.lineTo(w, h);
		bgPath.closeSubpath();
		p.fillPath(bgPath, QColor(0xf6, 0xf8, 0xfa));
	} else {
		p.fillRect(rect(), QColor(0xf6, 0xf8, 0xfa));
	}

	// 右侧分割线
	p.setPen(QColor(0xe5, 0xe8, 0xec));
	p.drawLine(w - 1, 0, w - 1, h);

	// 绘制头像
	drawAvatar(p, _avatarRect);

	// 绘制导航项
	for (const auto &item : _navItems) {
		const bool isActive = (item.type == _activeNav);
		const bool isHovered = (_hoveredNav && *_hoveredNav == item.type);

		// 如果选中，绘制高亮背景或胶囊
		if (isActive) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xe6, 0xf0, 0xff));
			p.drawRoundedRect(item.rect, 8, 8);
		} else if (isHovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xec, 0xef, 0xf4));
			p.drawRoundedRect(item.rect, 8, 8);
		}

		// 图标区域
		const QRect iconRect(item.rect.x() + (item.rect.width() - 24) / 2, item.rect.y() + 6, 24, 24);
		drawNavIcon(p, item.type, iconRect, isActive, isHovered);

		// 文本
		QFont font = p.font();
		font.setFamily("Microsoft YaHei");
		font.setPixelSize(11);
		font.setBold(isActive);
		p.setFont(font);

		p.setPen(isActive ? QColor(0x16, 0x77, 0xff) : (isHovered ? QColor(0x1f, 0x23, 0x29) : QColor(0x60, 0x62, 0x66)));
		p.drawText(QRect(item.rect.x(), item.rect.y() + 32, item.rect.width(), 20), Qt::AlignCenter, item.text);
	}

	// 绘制底部项
	for (const auto &bItem : _bottomItems) {
		const bool isHovered = (_hoveredBottom && *_hoveredBottom == bItem.type);
		if (isHovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xe5, 0xe8, 0xef));
			p.drawRoundedRect(bItem.rect, 6, 6);
		}
		drawBottomIcon(p, bItem.type, bItem.rect, isHovered, bItem.hasRedDot);
	}
}

void SidebarWidget::drawAvatar(QPainter &p, const QRect &r) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	// 悬浮放大微光
	QRect avRect = r;
	if (_avatarHovered) {
		p.setPen(QPen(QColor(0x16, 0x77, 0xff, 60), 2));
	} else {
		p.setPen(QPen(QColor(0xdf, 0xe3, 0xe8), 1));
	}

	// 头像外圆
	p.setBrush(QColor(0xfa, 0xfb, 0xfc));
	p.drawEllipse(avRect);

	// 头像内部渐变圆形
	QRect innerRect = avRect.adjusted(3, 3, -3, -3);
	QLinearGradient grad(innerRect.topLeft(), innerRect.bottomRight());
	grad.setColorAt(0.0, QColor(0xe8, 0xf1, 0xff));
	grad.setColorAt(1.0, QColor(0xd0, 0xe2, 0xff));
	p.setPen(Qt::NoPen);
	p.setBrush(grad);
	p.drawEllipse(innerRect);

	// 用户轮廓图标
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x33, 0x77, 0xdd));
	// 头部
	p.drawEllipse(innerRect.center().x() - 4, innerRect.top() + 7, 8, 8);
	// 身体
	QPainterPath bodyPath;
	bodyPath.moveTo(innerRect.center().x() - 9, innerRect.bottom() - 3);
	bodyPath.cubicTo(innerRect.center().x() - 8, innerRect.center().y() + 4,
					 innerRect.center().x() + 8, innerRect.center().y() + 4,
					 innerRect.center().x() + 9, innerRect.bottom() - 3);
	bodyPath.closeSubpath();
	p.drawPath(bodyPath);

	// 在线状态小蓝/绿点徽标（右下角）
	const int badgeRadius = 5;
	const QPoint badgeCenter(avRect.right() - 3, avRect.bottom() - 3);
	// 白外圈
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawEllipse(badgeCenter, badgeRadius + 1, badgeRadius + 1);
	// 蓝色状态点
	p.setBrush(QColor(0x40, 0x96, 0xff));
	p.drawEllipse(badgeCenter, badgeRadius, badgeRadius);

	p.restore();
}

void SidebarWidget::drawNavIcon(QPainter &p, NavItemType type, const QRect &r, bool active, bool hovered) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	const QColor color = active ? QColor(0x16, 0x77, 0xff) : (hovered ? QColor(0x1f, 0x23, 0x29) : QColor(0x60, 0x62, 0x66));
	p.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);

	const int cx = r.center().x();
	const int cy = r.center().y();

	if (type == NavItemType::Meeting) {
		// 摄像机图标
		QRect camBody(cx - 9, cy - 6, 12, 12);
		p.drawRoundedRect(camBody, 2, 2);
		if (active) {
			p.setBrush(color);
			p.drawRoundedRect(camBody, 2, 2);
			p.setBrush(Qt::NoBrush);
		}
		QPainterPath camHead;
		camHead.moveTo(camBody.right(), cy - 2);
		camHead.lineTo(cx + 8, cy - 5);
		camHead.lineTo(cx + 8, cy + 5);
		camHead.lineTo(camBody.right(), cy + 2);
		camHead.closeSubpath();
		if (active) {
			p.fillPath(camHead, color);
		} else {
			p.drawPath(camHead);
		}
	} else if (type == NavItemType::Contacts) {
		// 通讯录书本图标
		QRect bookRect(cx - 7, cy - 8, 14, 16);
		p.drawRoundedRect(bookRect, 2, 2);
		p.drawLine(cx - 4, cy - 4, cx + 4, cy - 4);
		p.drawLine(cx - 4, cy, cx + 4, cy);
		p.drawLine(cx - 4, cy + 4, cx + 1, cy + 4);
	} else if (type == NavItemType::Recordings) {
		// 录制圆环与同心圆图标
		p.drawEllipse(QPoint(cx, cy), 8, 8);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QPoint(cx, cy), 4, 4);
	}

	p.restore();
}

void SidebarWidget::drawBottomIcon(QPainter &p, BottomItemType type, const QRect &r, bool hovered, bool hasRedDot) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);

	const QColor color = hovered ? QColor(0x1f, 0x23, 0x29) : QColor(0x8a, 0x8f, 0x99);
	p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);

	const int cx = r.center().x();
	const int cy = r.center().y();

	if (type == BottomItemType::Mail) {
		// 信封图标
		QRect mailRect(cx - 8, cy - 6, 16, 12);
		p.drawRoundedRect(mailRect, 1.5, 1.5);
		p.drawLine(mailRect.left() + 1, mailRect.top() + 1, cx, cy);
		p.drawLine(cx, cy, mailRect.right() - 1, mailRect.top() + 1);
	} else if (type == BottomItemType::Settings) {
		// 齿轮设置图标
		p.drawEllipse(QPoint(cx, cy), 4, 4);
		for (int i = 0; i < 6; ++i) {
			const double rad = i * 3.14159265 / 3.0;
			const int x1 = cx + int(std::cos(rad) * 6);
			const int y1 = cy + int(std::sin(rad) * 6);
			const int x2 = cx + int(std::cos(rad) * 8);
			const int y2 = cy + int(std::sin(rad) * 8);
			p.drawLine(x1, y1, x2, y2);
		}
	} else if (type == BottomItemType::User) {
		// 个人+图标
		p.drawEllipse(cx - 3, cy - 6, 6, 6);
		QPainterPath bodyPath;
		bodyPath.moveTo(cx - 7, cy + 6);
		bodyPath.cubicTo(cx - 6, cy + 1, cx + 1, cy + 1, cx + 2, cy + 6);
		p.drawPath(bodyPath);
		// 加号
		p.drawLine(cx + 4, cy - 1, cx + 8, cy - 1);
		p.drawLine(cx + 6, cy - 3, cx + 6, cy + 1);
	}

	// 小红点提醒
	if (hasRedDot) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xf5, 0x3f, 0x3f));
		p.drawEllipse(r.right() - 8, r.top() + 4, 6, 6);
	}

	p.restore();
}

void SidebarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();

	const bool avHover = _avatarRect.contains(pos);
	if (avHover != _avatarHovered) {
		_avatarHovered = avHover;
		update();
	}

	std::optional<NavItemType> newHoveredNav;
	for (const auto &item : _navItems) {
		if (item.rect.contains(pos)) {
			newHoveredNav = item.type;
			break;
		}
	}
	if (newHoveredNav != _hoveredNav) {
		_hoveredNav = newHoveredNav;
		update();
	}

	std::optional<BottomItemType> newHoveredBottom;
	for (const auto &item : _bottomItems) {
		if (item.rect.contains(pos)) {
			newHoveredBottom = item.type;
			break;
		}
	}
	if (newHoveredBottom != _hoveredBottom) {
		_hoveredBottom = newHoveredBottom;
		update();
	}
}

void SidebarWidget::mousePressEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	if (_avatarRect.contains(pos)) {
		_avatarClicks.fire({});
		return;
	}
	for (const auto &item : _navItems) {
		if (item.rect.contains(pos)) {
			setActiveNav(item.type);
			return;
		}
	}
	for (const auto &item : _bottomItems) {
		if (item.rect.contains(pos)) {
			_bottomItemClicks.fire_copy(item.type);
			return;
		}
	}
}

void SidebarWidget::mouseReleaseEvent(QMouseEvent *e) {
}

void SidebarWidget::leaveEventHook(QEvent *e) {
	_avatarHovered = false;
	_hoveredNav = std::nullopt;
	_hoveredBottom = std::nullopt;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

} // namespace MeetingUI
