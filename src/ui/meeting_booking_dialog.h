#pragma once

#include "src/net/meeting_types.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace OpenMeeting {
class ContactCatalogController;
class MeetingCatalogController;
class PresenceWatchController;
class SessionManager;
}

namespace MeetingUI {

class MeetingBookingDialog final : public QDialog {
public:
	MeetingBookingDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr,
		OpenMeeting::ContactCatalogController *contacts = nullptr,
		OpenMeeting::PresenceWatchController *presence = nullptr);
	MeetingBookingDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		const OpenMeeting::MeetingCatalogDetail &detail,
		QWidget *parent = nullptr,
		OpenMeeting::ContactCatalogController *contacts = nullptr,
		OpenMeeting::PresenceWatchController *presence = nullptr);

	void setPreselectedInvitees(const QStringList &userIds);

private:
	void initUi();
	void populateCreateDefaults();
	void populateEditValues();
	void updateInviteesDisplay();
	void openInviteePicker();
	void submit();
	void handleWriteState();
	void setBusy(bool busy);
	void updateRepeatControls();
	void showError(const QString &message);
	bool collectCommon(qint64 *scheduledTimeSeconds, qint64 *durationSeconds, QString *timeZone);
	bool collectRepeatRule(
		qint64 scheduledTimeSeconds,
		const QString &timeZone,
		OpenMeeting::MeetingRepeatRule *repeatRule);
	OpenMeeting::MeetingBookingRequest createRequest(
		qint64 scheduledTimeSeconds,
		qint64 durationSeconds,
		const QString &timeZone,
		const OpenMeeting::MeetingRepeatRule &repeatRule) const;
	std::optional<OpenMeeting::MeetingUpdateRequest> updateRequest(
		qint64 scheduledTimeSeconds,
		qint64 durationSeconds,
		const QString &timeZone,
		QString *message) const;

	OpenMeeting::MeetingCatalogController &_controller;
	OpenMeeting::SessionManager &_session;
	OpenMeeting::ContactCatalogController *_contacts = nullptr;
	OpenMeeting::PresenceWatchController *_presence = nullptr;
	std::optional<OpenMeeting::MeetingCatalogDetail> _original;
	QStringList _selectedInviteeIds;
	QLineEdit *_titleEdit = nullptr;
	QDateTimeEdit *_startEdit = nullptr;
	QSpinBox *_durationEdit = nullptr;
	QComboBox *_timeZoneEdit = nullptr;
	QComboBox *_repeatTypeEdit = nullptr;
	QCheckBox *_repeatEndEnabled = nullptr;
	QDateEdit *_repeatEndEdit = nullptr;
	QLineEdit *_passwordEdit = nullptr;
	QLabel *_inviteesLabel = nullptr;
	QPushButton *_selectInviteesButton = nullptr;
	QCheckBox *_cameraOnJoin = nullptr;
	QCheckBox *_microphoneOnJoin = nullptr;
	QLabel *_statusLabel = nullptr;
	QPushButton *_submitButton = nullptr;
	QPushButton *_cancelButton = nullptr;
	QString _initialTimeZoneText;
	int _initialDurationMinutes = 0;
	bool _submitting = false;
};

} // namespace MeetingUI
