#pragma once

#include "base/basic_types.h"
#include "src/core/meeting_catalog_controller.h"
#include "ui/rp_widget.h"

class QFrame;
class QLabel;
class QListView;
class QPushButton;
class QWidget;

namespace MeetingUI {

class MeetingListModel;

class ScheduleWidget : public Ui::RpWidget {
public:
	explicit ScheduleWidget(QWidget *parent = nullptr);
	~ScheduleWidget() override = default;

	rpl::producer<> allMeetingsClicked() const {
		return _allMeetingsClicks.events();
	}
	rpl::producer<> addScheduleClicked() const {
		return _addScheduleClicks.events();
	}
	rpl::producer<> refreshClicked() const {
		return _refreshClicks.events();
	}
	rpl::producer<QString> meetingClicked() const {
		return _meetingClicks.events();
	}

	void setMeetingState(const OpenMeeting::MeetingListViewState &state);
	void setCurrentUserId(const QString &userId);
	void setShowAllMeetingsAction(bool show);

protected:
	void changeEvent(QEvent *event) override;

private:
	void refreshThemeIcons();
	void retranslateUi();

	QFrame *_cardFrame = nullptr;
	QLabel *_titleLabel = nullptr;
	QLabel *_countBadge = nullptr;
	QPushButton *_refreshButton = nullptr;
	QPushButton *_allMeetingsButton = nullptr;
	QLabel *_bannerLabel = nullptr;
	QListView *_listView = nullptr;
	MeetingListModel *_model = nullptr;

	QWidget *_emptyPanel = nullptr;
	QLabel *_emptyIconLabel = nullptr;
	QLabel *_emptyTitleLabel = nullptr;
	QLabel *_emptySubtitleLabel = nullptr;
	QPushButton *_emptyScheduleButton = nullptr;

	QWidget *_statePanel = nullptr;
	QLabel *_stateMessageLabel = nullptr;
	QPushButton *_stateRetryButton = nullptr;

	rpl::event_stream<> _allMeetingsClicks;
	rpl::event_stream<> _addScheduleClicks;
	rpl::event_stream<> _refreshClicks;
	rpl::event_stream<QString> _meetingClicks;
};

} // namespace MeetingUI
