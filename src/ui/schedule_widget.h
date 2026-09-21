#pragma once

#include "base/basic_types.h"
#include "src/core/meeting_catalog_controller.h"
#include "ui/rp_widget.h"
#include "ui/widgets/shadow.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QDate>
#include <memory>

class QListView;

namespace MeetingUI {

class MeetingListModel;

class FloatingActionButton : public Ui::RpWidget {
public:
	explicit FloatingActionButton(QWidget *parent = nullptr);
	~FloatingActionButton() override = default;

	rpl::producer<> clicked() const {
		return _clicks.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	bool _hovered = false;
	bool _pressed = false;
	std::unique_ptr<Ui::BoxShadow> _shadow;
	std::unique_ptr<Ui::BoxShadow> _hoverShadow;
	rpl::event_stream<> _clicks;
};

class ScheduleWidget : public Ui::RpWidget {
public:
	explicit ScheduleWidget(QWidget *parent = nullptr);
	~ScheduleWidget() override = default;

	rpl::producer<> allMeetingsClicked() const {
		return _allMeetingsClicks.events();
	}
	rpl::producer<> addScheduleClicked() const {
		return _fabButton->clicked();
	}
	rpl::producer<> refreshClicked() const {
		return _refreshClicks.events();
	}
	rpl::producer<QString> meetingClicked() const {
		return _meetingClicks.events();
	}

	void setMeetingState(const OpenMeeting::MeetingListViewState &state);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void drawHeader(QPainter &p);
	void drawEmptyCoffeeIllustration(QPainter &p, const QRect &area);
	void drawCoffeeSteam(QPainter &p, int cx, int cy);
	void drawStateMessage(QPainter &p, const QRect &area);
	void layoutChildren();

	QString getFormattedDate() const;
	QString getFormattedSubDate() const;

	FloatingActionButton *_fabButton = nullptr;
	QListView *_listView = nullptr;
	MeetingListModel *_model = nullptr;
	QRect _dateRect;
	QRect _subDateRect;
	int _headerBottom = 144;
	int _stateHeight = 0;
	QRect _allMeetingsRect;
	QRect _refreshRect;
	bool _allMeetingsHovered = false;
	bool _refreshHovered = false;
	QString _stateMessage;
	bool _showEmptyIllustration = true;

	rpl::event_stream<> _allMeetingsClicks;
	rpl::event_stream<> _refreshClicks;
	rpl::event_stream<QString> _meetingClicks;
};

} // namespace MeetingUI
