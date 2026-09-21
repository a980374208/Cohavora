#pragma once

#include <QtWidgets/QDialog>

class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;
class QTabWidget;

namespace OpenMeeting {
class MeetingCatalogController;
class SessionManager;
struct MeetingListViewState;
}

namespace MeetingUI {

class MeetingListModel;

class MeetingListDialog final : public QDialog {
	Q_OBJECT
public:
	MeetingListDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr);

signals:
	void meetingActivated(const QString &meetingId);
	void joinRequested(const QString &meetingId);

private:
	void updateUpcoming();
	void updateHistory();
	void updatePage(
		const OpenMeeting::MeetingListViewState &state,
		MeetingListModel *model,
		QListView *view,
		QLabel *stateLabel,
		bool history);
	void activateIndex(QListView *view, const QModelIndex &index);
	void requestSelectedJoin();
	void updateCommandButtons();
	void refreshCurrent();

	OpenMeeting::MeetingCatalogController &_controller;
	OpenMeeting::SessionManager &_session;
	QLineEdit *_searchEdit = nullptr;
	QTabWidget *_tabs = nullptr;
	QListView *_upcomingView = nullptr;
	QListView *_historyView = nullptr;
	MeetingListModel *_upcomingModel = nullptr;
	MeetingListModel *_historyModel = nullptr;
	QLabel *_upcomingState = nullptr;
	QLabel *_historyState = nullptr;
	QPushButton *_refreshButton = nullptr;
	QPushButton *_viewButton = nullptr;
	QPushButton *_joinButton = nullptr;
};

} // namespace MeetingUI
