#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QVector>

namespace MeetingUI {

enum class NavItemType {
	Meeting,
	Contacts,
	Recordings
};

enum class BottomItemType {
	Mail,
	Settings,
	User
};

class SidebarWidget : public Ui::RpWidget {
public:
	explicit SidebarWidget(QWidget *parent = nullptr);
	~SidebarWidget() override = default;

	[[nodiscard]] NavItemType activeNav() const {
		return _activeNav;
	}
	void setActiveNav(NavItemType type);
	void setCornerRadius(int radius);

	rpl::producer<NavItemType> navChanged() const {
		return _navChanges.events();
	}
	rpl::producer<BottomItemType> bottomItemClicked() const {
		return _bottomItemClicks.events();
	}
	rpl::producer<> avatarClicked() const {
		return _avatarClicks.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct NavItem {
		NavItemType type;
		QString text;
		QRect rect;
	};

	struct BottomItem {
		BottomItemType type;
		QString tooltip;
		QRect rect;
		bool hasRedDot = false;
	};

	void updateLayout();
	void drawAvatar(QPainter &p, const QRect &r);
	void drawNavIcon(QPainter &p, NavItemType type, const QRect &iconRect, bool active, bool hovered);
	void drawBottomIcon(QPainter &p, BottomItemType type, const QRect &iconRect, bool hovered, bool hasRedDot);

	NavItemType _activeNav = NavItemType::Meeting;
	std::optional<NavItemType> _hoveredNav;
	std::optional<BottomItemType> _hoveredBottom;
	bool _avatarHovered = false;

	QRect _avatarRect;
	QVector<NavItem> _navItems;
	QVector<BottomItem> _bottomItems;
	int _cornerRadius = 14;

	rpl::event_stream<NavItemType> _navChanges;
	rpl::event_stream<BottomItemType> _bottomItemClicks;
	rpl::event_stream<> _avatarClicks;
};

} // namespace MeetingUI
