#include "src/ui/schedule_widget.h"

#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/ui/meeting_list_delegate.h"
#include "src/ui/meeting_list_model.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

ScheduleWidget::ScheduleWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setObjectName(QStringLiteral("scheduleCardHost"));
	Theme::setStyleVariant(*this, "schedule-card-widget");

	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	_cardFrame = new QFrame(this);
	_cardFrame->setObjectName(QStringLiteral("scheduleCardFrame"));
	rootLayout->addWidget(_cardFrame);

	auto *cardLayout = new QVBoxLayout(_cardFrame);
	cardLayout->setContentsMargins(0, 0, 0, 0);
	cardLayout->setSpacing(0);

	// Card Header
	auto *header = new QWidget(_cardFrame);
	header->setObjectName(QStringLiteral("scheduleCardHeader"));
	header->setFixedHeight(44);
	auto *headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(16, 0, 12, 0);
	headerLayout->setSpacing(8);

	_titleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Upcoming Meetings"), header);
	_titleLabel->setObjectName(QStringLiteral("scheduleCardTitle"));
	headerLayout->addWidget(_titleLabel);

	_countBadge = new QLabel(QStringLiteral("0"), header);
	_countBadge->setObjectName(QStringLiteral("scheduleCountBadge"));
	_countBadge->setAlignment(Qt::AlignCenter);
	_countBadge->setFixedHeight(20);
	_countBadge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	headerLayout->addWidget(_countBadge, 0, Qt::AlignVCenter);

	headerLayout->addStretch(1);

	_refreshButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Refresh"), header);
	_refreshButton->setObjectName(QStringLiteral("scheduleRefreshBtn"));
	_refreshButton->setCursor(Qt::PointingHandCursor);
	_refreshButton->setIconSize(QSize(13, 13));
	connect(_refreshButton, &QPushButton::clicked, this, [this] {
		_refreshClicks.fire({});
	});
	headerLayout->addWidget(_refreshButton);

	_allMeetingsButton = new QPushButton(QCoreApplication::translate("MeetingUI", "All Meetings"), header);
	_allMeetingsButton->setObjectName(QStringLiteral("scheduleAllMeetingsBtn"));
	_allMeetingsButton->setCursor(Qt::PointingHandCursor);
	_allMeetingsButton->setLayoutDirection(Qt::RightToLeft);
	_allMeetingsButton->setIconSize(QSize(13, 13));
	_allMeetingsButton->hide();
	connect(_allMeetingsButton, &QPushButton::clicked, this, [this] {
		_allMeetingsClicks.fire({});
	});
	headerLayout->addWidget(_allMeetingsButton);

	cardLayout->addWidget(header);

	// Optional status banner when list has items and is refreshing / failed
	_bannerLabel = new QLabel(_cardFrame);
	_bannerLabel->setObjectName(QStringLiteral("scheduleBannerLabel"));
	_bannerLabel->setWordWrap(true);
	_bannerLabel->setTextFormat(Qt::PlainText);
	_bannerLabel->hide();
	cardLayout->addWidget(_bannerLabel);

	// Content Host
	auto *body = new QWidget(_cardFrame);
	body->setObjectName(QStringLiteral("scheduleCardBody"));
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(10, 10, 10, 10);
	bodyLayout->setSpacing(0);

	_model = new MeetingListModel(this);
	_listView = new QListView(body);
	_listView->setObjectName(QStringLiteral("scheduleListView"));
	_listView->setModel(_model);
	_listView->setItemDelegate(new MeetingListDelegate(true, _listView));
	_listView->setSelectionMode(QAbstractItemView::SingleSelection);
	_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
	_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	_listView->setMouseTracking(true);
	_listView->setUniformItemSizes(false);
	Theme::setStyleVariant(*_listView, "schedule-widget-listview");
	_listView->hide();
	connect(_listView, &QListView::clicked, this, [this](const QModelIndex &index) {
		const auto meetingId = index.data(MeetingIdRole).toString();
		if (!meetingId.isEmpty()) _meetingClicks.fire_copy(meetingId);
	});
	bodyLayout->addWidget(_listView, 1);

	// Clean minimalist empty panel
	_emptyPanel = new QWidget(body);
	_emptyPanel->setObjectName(QStringLiteral("scheduleEmptyPanel"));
	auto *emptyLayout = new QVBoxLayout(_emptyPanel);
	emptyLayout->setContentsMargins(24, 20, 24, 20);
	emptyLayout->setSpacing(8);
	emptyLayout->addStretch(1);

	_emptyIconLabel = new QLabel(_emptyPanel);
	_emptyIconLabel->setObjectName(QStringLiteral("scheduleEmptyIcon"));
	_emptyIconLabel->setFixedSize(44, 44);
	_emptyIconLabel->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(_emptyIconLabel, 0, Qt::AlignHCenter);

	emptyLayout->addSpacing(4);

	_emptyTitleLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "No meetings scheduled today"), _emptyPanel);
	_emptyTitleLabel->setObjectName(QStringLiteral("scheduleEmptyTitle"));
	_emptyTitleLabel->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(_emptyTitleLabel);

	_emptySubtitleLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Scheduled meetings will appear here in chronological order."), _emptyPanel);
	_emptySubtitleLabel->setObjectName(QStringLiteral("scheduleEmptySubtitle"));
	_emptySubtitleLabel->setAlignment(Qt::AlignCenter);
	_emptySubtitleLabel->setWordWrap(true);
	emptyLayout->addWidget(_emptySubtitleLabel);

	emptyLayout->addSpacing(6);

	_emptyScheduleButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Schedule a Meeting"), _emptyPanel);
	_emptyScheduleButton->setObjectName(QStringLiteral("scheduleEmptyActionBtn"));
	_emptyScheduleButton->setCursor(Qt::PointingHandCursor);
	_emptyScheduleButton->setIconSize(QSize(13, 13));
	connect(_emptyScheduleButton, &QPushButton::clicked, this, [this] {
		_addScheduleClicks.fire({});
	});
	emptyLayout->addWidget(_emptyScheduleButton, 0, Qt::AlignHCenter);

	emptyLayout->addStretch(1);
	bodyLayout->addWidget(_emptyPanel, 1);

	// Loading / Error centered state panel (when list is empty)
	_statePanel = new QWidget(body);
	_statePanel->setObjectName(QStringLiteral("scheduleStatePanel"));
	auto *stateLayout = new QVBoxLayout(_statePanel);
	stateLayout->setContentsMargins(24, 20, 24, 20);
	stateLayout->setSpacing(10);
	stateLayout->addStretch(1);

	_stateMessageLabel = new QLabel(_statePanel);
	_stateMessageLabel->setObjectName(QStringLiteral("scheduleStateMessage"));
	_stateMessageLabel->setAlignment(Qt::AlignCenter);
	_stateMessageLabel->setWordWrap(true);
	_stateMessageLabel->setTextFormat(Qt::PlainText);
	stateLayout->addWidget(_stateMessageLabel);

	_stateRetryButton = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Refresh"), _statePanel);
	_stateRetryButton->setObjectName(QStringLiteral("scheduleStateRetryBtn"));
	_stateRetryButton->setCursor(Qt::PointingHandCursor);
	_stateRetryButton->setIconSize(QSize(13, 13));
	connect(_stateRetryButton, &QPushButton::clicked, this, [this] {
		_refreshClicks.fire({});
	});
	stateLayout->addWidget(_stateRetryButton, 0, Qt::AlignHCenter);
	stateLayout->addStretch(1);
	_statePanel->hide();
	bodyLayout->addWidget(_statePanel, 1);

	cardLayout->addWidget(body, 1);

	refreshThemeIcons();
	Theme::observeChanges(*this, [this] {
		refreshThemeIcons();
		if (_listView && _listView->viewport()) {
			_listView->viewport()->update();
		}
		update();
	});
}

void ScheduleWidget::setShowAllMeetingsAction(bool show) {
	if (_allMeetingsButton) {
		_allMeetingsButton->setVisible(show);
	}
}

void ScheduleWidget::changeEvent(QEvent *event) {
	Ui::RpWidget::changeEvent(event);
	if (event && event->type() == QEvent::LanguageChange) {
		retranslateUi();
	}
}

void ScheduleWidget::retranslateUi() {
	if (_titleLabel) {
		_titleLabel->setText(QCoreApplication::translate("MeetingUI", "Upcoming Meetings"));
	}
	if (_refreshButton) {
		_refreshButton->setText(QCoreApplication::translate("MeetingUI", "Refresh"));
	}
	if (_allMeetingsButton) {
		_allMeetingsButton->setText(QCoreApplication::translate("MeetingUI", "All Meetings"));
	}
	if (_emptyTitleLabel) {
		_emptyTitleLabel->setText(QCoreApplication::translate("MeetingUI", "No meetings scheduled today"));
	}
	if (_emptySubtitleLabel) {
		_emptySubtitleLabel->setText(
			QCoreApplication::translate("MeetingUI", "Scheduled meetings will appear here in chronological order."));
	}
	if (_emptyScheduleButton) {
		_emptyScheduleButton->setText(QCoreApplication::translate("MeetingUI", "Schedule a Meeting"));
	}
	if (_stateRetryButton) {
		_stateRetryButton->setText(QCoreApplication::translate("MeetingUI", "Refresh"));
	}
	if (_listView) {
		_listView->viewport()->update();
	}
}

void ScheduleWidget::refreshThemeIcons() {
	if (_refreshButton) {
		_refreshButton->setIcon(Theme::icon(Theme::Icon::Refresh, Theme::ColorRole::TextSecondary));
	}
	if (_allMeetingsButton) {
		_allMeetingsButton->setIcon(Theme::icon(Theme::Icon::ChevronRight, Theme::ColorRole::Accent));
	}
	if (_emptyIconLabel) {
		_emptyIconLabel->setPixmap(Theme::icon(Theme::Icon::Calendar, Theme::ColorRole::TextMuted).pixmap(20, 20));
	}
	if (_emptyScheduleButton) {
		_emptyScheduleButton->setIcon(Theme::icon(Theme::Icon::Plus, Theme::ColorRole::Text));
	}
	if (_stateRetryButton) {
		_stateRetryButton->setIcon(Theme::icon(Theme::Icon::Refresh, Theme::ColorRole::Text));
	}
}

void ScheduleWidget::setMeetingState(const OpenMeeting::MeetingListViewState &state) {
	_model->setMeetings(state.meetings, false);
	const int count = _model->rowCount();
	if (_countBadge) {
		_countBadge->setText(QString::number(count));
	}
	if (_refreshButton) {
		_refreshButton->setEnabled(!state.refreshing);
	}

	QString stateMessage;
	bool showEmpty = false;
	bool showRetry = false;
	if (state.refreshing && !state.hasSnapshot) {
		stateMessage = QCoreApplication::translate("MeetingUI", "Loading schedule...");
	} else if (state.state == OpenMeeting::MeetingCatalogLoadState::Error && !state.hasSnapshot) {
		stateMessage = QCoreApplication::translate("MeetingUI", "Unable to load the schedule. Click Refresh to try again.");
		showRetry = true;
	} else if (count == 0) {
		showEmpty = true;
	} else if (state.refreshing) {
		stateMessage = QCoreApplication::translate("MeetingUI", "Refreshing. Showing the last available results.");
	} else if (state.error.code != 0) {
		stateMessage = QCoreApplication::translate("MeetingUI", "Refresh failed. Showing the last available results.");
	}

	if (count > 0) {
		_listView->show();
		_emptyPanel->hide();
		_statePanel->hide();
		_bannerLabel->setText(stateMessage);
		_bannerLabel->setVisible(!stateMessage.isEmpty());
	} else if (showEmpty) {
		_listView->hide();
		_bannerLabel->hide();
		_statePanel->hide();
		_emptyPanel->show();
	} else {
		_listView->hide();
		_bannerLabel->hide();
		_emptyPanel->hide();
		_stateMessageLabel->setText(stateMessage);
		_stateRetryButton->setVisible(showRetry);
		_statePanel->show();
	}
	update();
}

} // namespace MeetingUI
