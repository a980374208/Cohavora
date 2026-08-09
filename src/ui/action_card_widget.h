#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include "ui/widgets/shadow.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <memory>

namespace MeetingUI {

enum class ActionCardType {
	JoinMeeting,     // 加入会议 (+)
	QuickMeeting,    // 快速会议 (⚡) ∨
	ScheduleMeeting, // 预定会议 (✔) ∨
	ShareScreen      // 共享屏幕 (🗔)
};

class ActionCardWidget : public Ui::RpWidget {
public:
	ActionCardWidget(
		QWidget *parent,
		ActionCardType type,
		const QString &title,
		bool hasDropdown = false);
	~ActionCardWidget() override = default;

	[[nodiscard]] ActionCardType type() const {
		return _type;
	}

	rpl::producer<ActionCardType> clicked() const {
		return _clicks.events();
	}
	rpl::producer<ActionCardType> dropdownClicked() const {
		return _dropdownClicks.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void drawCardIcon(QPainter &p, const QRect &iconRect);

	ActionCardType _type;
	QString _title;
	bool _hasDropdown = false;

	bool _hovered = false;
	bool _pressed = false;
	float _hoverProgress = 0.0f;

	QRect _cardRect;
	QRect _dropdownRect;

	std::unique_ptr<Ui::BoxShadow> _normalShadow;
	std::unique_ptr<Ui::BoxShadow> _hoverShadow;

	rpl::event_stream<ActionCardType> _clicks;
	rpl::event_stream<ActionCardType> _dropdownClicks;
};

class ActionGridContainer : public Ui::RpWidget {
public:
	explicit ActionGridContainer(QWidget *parent = nullptr);
	~ActionGridContainer() override = default;

	rpl::producer<ActionCardType> cardClicked() const {
		return _cardClicks.events();
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	ActionCardWidget *_joinCard = nullptr;
	ActionCardWidget *_quickCard = nullptr;
	ActionCardWidget *_scheduleCard = nullptr;
	ActionCardWidget *_shareCard = nullptr;

	rpl::event_stream<ActionCardType> _cardClicks;
};

} // namespace MeetingUI
