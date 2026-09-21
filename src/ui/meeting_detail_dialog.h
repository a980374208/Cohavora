#pragma once

#include "src/net/meeting_types.h"

#include <QtWidgets/QDialog>

#include <optional>

class QLabel;
class QPushButton;

namespace OpenMeeting {
class MeetingCatalogController;
class SessionManager;
}

namespace MeetingUI {

class MeetingDetailDialog final : public QDialog {
public:
	MeetingDetailDialog(
		const QString &meetingId,
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr);
	bool joinRequested() const { return _joinRequested; }
	std::optional<OpenMeeting::MeetingCatalogDetail> detailForJoin() const;

private:
	void updateDetail();
	void handleWriteState();
	void populate(const OpenMeeting::MeetingCatalogDetail &detail, bool allowJoin);
	void editMeeting();
	void cancelMeeting();
	void copyMeetingId();
	void requestJoin();
	void setActionsEnabled(bool enabled);

	QString _meetingId;
	OpenMeeting::MeetingCatalogController &_controller;
	OpenMeeting::SessionManager &_session;
	std::optional<OpenMeeting::MeetingCatalogDetail> _detail;
	QLabel *_titleLabel = nullptr;
	QLabel *_meetingIdLabel = nullptr;
	QLabel *_creatorLabel = nullptr;
	QLabel *_timeLabel = nullptr;
	QLabel *_durationLabel = nullptr;
	QLabel *_timeZoneLabel = nullptr;
	QLabel *_meetingStatusLabel = nullptr;
	QLabel *_repeatLabel = nullptr;
	QLabel *_stateLabel = nullptr;
	QPushButton *_retryButton = nullptr;
	QPushButton *_copyButton = nullptr;
	QPushButton *_joinButton = nullptr;
	QPushButton *_editButton = nullptr;
	QPushButton *_cancelMeetingButton = nullptr;
	bool _cancelling = false;
	bool _joinRequested = false;
};

} // namespace MeetingUI
