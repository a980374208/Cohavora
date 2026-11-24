#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QVector>
#include <QtWidgets/QPushButton>

namespace MeetingUI {

enum class NavItemType {
	Meeting,
	Contacts
};

enum class BottomItemType {
	Settings,
	User,
	Copilot
};

class SidebarWidget : public Ui::RpWidget {
public:
	explicit SidebarWidget(QWidget *parent = nullptr);
	~SidebarWidget() override = default;

	[[nodiscard]] NavItemType activeNav() const {
		return _activeNav;
	}
	void setActiveNav(NavItemType type);
	void setCopilotActive(bool active);
	[[nodiscard]] bool copilotActive() const {
		return _copilotActive;
	}
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
	void resizeEvent(QResizeEvent *e) override;
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
	};

	void updateLayout();
	[[nodiscard]] QRect avatarHitRect() const;
	void drawAvatar(QPainter &p, const QRect &r);
	void drawNavIcon(QPainter &p, NavItemType type, const QRect &iconRect, bool active, bool hovered);
	void drawBottomIcon(QPainter &p, BottomItemType type, const QRect &iconRect, bool hovered);

	NavItemType _activeNav = NavItemType::Meeting;
	std::optional<NavItemType> _hoveredNav;
	std::optional<BottomItemType> _hoveredBottom;
	bool _avatarHovered = false;
	bool _avatarPressed = false;

	QRect _avatarRect;
	QPushButton *_accessibleAvatar = nullptr;
	QPushButton *_accessibleSettings = nullptr;
	QPushButton *_accessibleCopilot = nullptr;
	bool _copilotActive = false;
	QVector<NavItem> _navItems;
	QVector<BottomItem> _bottomItems;
	int _cornerRadius = 14;

	rpl::event_stream<NavItemType> _navChanges;
	rpl::event_stream<BottomItemType> _bottomItemClicks;
	rpl::event_stream<> _avatarClicks;
};

} // namespace MeetingUI
