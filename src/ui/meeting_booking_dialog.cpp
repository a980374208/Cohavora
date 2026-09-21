#include <QtCore/QCoreApplication>
#include "src/ui/meeting_booking_dialog.h"

#include "src/core/meeting_catalog_controller.h"
#include "src/net/session_manager.h"
#include "src/ui/app_theme.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDateEdit>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace MeetingUI {
namespace {

QString systemTimeZoneId() {
	auto id = QString::fromUtf8(QTimeZone::systemTimeZoneId());
	if (!QTimeZone(id.toUtf8()).isValid()) id = QStringLiteral("Asia/Shanghai");
	return id;
}

void addTimeZone(QComboBox *combo, const QString &id) {
	if (id.isEmpty() || !QTimeZone(id.toUtf8()).isValid() || combo->findText(id) >= 0) return;
	combo->addItem(id);
}

QString saveFailureText(const OpenMeeting::HttpError &error) {
	if (error.code == 0) return QCoreApplication::translate("MeetingUI", "Unable to save. Please try again later.");
	return QCoreApplication::translate("MeetingUI", "Unable to save. A network interruption may leave the result unknown. Refresh the meeting list before trying again.");
}

OpenMeeting::MeetingRepeatType selectedRepeatType(const QComboBox *combo) {
	return static_cast<OpenMeeting::MeetingRepeatType>(combo->currentData().toInt());
}

QString readOnlyRepeatName(const OpenMeeting::MeetingRepeatRule &rule) {
	if (rule.type == OpenMeeting::MeetingRepeatType::Custom) {
		return QCoreApplication::translate("MeetingUI", "Custom Recurrence (Read-Only)");
	}
	return rule.rawType.trimmed().isEmpty()
		? QCoreApplication::translate("MeetingUI", "Unknown Recurrence (Read-Only)")
		: QCoreApplication::translate("MeetingUI", "%1 (Read-Only)").arg(rule.rawType);
}

} // namespace

MeetingBookingDialog::MeetingBookingDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent)
	: QDialog(parent)
	, _controller(controller)
	, _session(session) {
	initUi();
	populateCreateDefaults();
	AppTheme::makeDialogAdaptive(*this, QSize(560, 620));
}

MeetingBookingDialog::MeetingBookingDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		const OpenMeeting::MeetingCatalogDetail &detail,
		QWidget *parent)
	: QDialog(parent)
	, _controller(controller)
	, _session(session)
	, _original(detail) {
	initUi();
	populateEditValues();
	AppTheme::makeDialogAdaptive(*this, QSize(560, 620));
}

void MeetingBookingDialog::initUi() {
	setWindowTitle(_original ? QCoreApplication::translate("MeetingUI", "Edit Meeting") : QCoreApplication::translate("MeetingUI", "Schedule Meeting"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setModal(true);
	setMinimumWidth(500);
	MeetingUI::AppTheme::setStyleVariant(*this, "meeting-booking-dialog-this");
	AppTheme::setTone(*this, AppTheme::Tone::Light);
	AppTheme::styleChoiceControls(*this, AppTheme::Tone::Light);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(28, 24, 28, 24);
	root->setSpacing(16);

	auto *title = new QLabel(
		_original ? QCoreApplication::translate("MeetingUI", "Edit Meeting Schedule") : QCoreApplication::translate("MeetingUI", "Schedule a Meeting"), this);
	auto titleFont = title->font();
	titleFont.setPixelSize(20);
	titleFont.setBold(true);
	title->setFont(titleFont);
	root->addWidget(title);

	auto *form = new QFormLayout();
	form->setHorizontalSpacing(18);
	form->setVerticalSpacing(12);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

	_titleEdit = new QLineEdit(this);
	_titleEdit->setMaxLength(128);
	_titleEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter a meeting title"));
	form->addRow(QCoreApplication::translate("MeetingUI", "Meeting Title"), _titleEdit);

	_startEdit = new QDateTimeEdit(this);
	_startEdit->setCalendarPopup(true);
	_startEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
	_startEdit->setMinimumDate(QDate(2000, 1, 1));
	_startEdit->setMaximumDate(QDate(2100, 12, 31));
	form->addRow(QCoreApplication::translate("MeetingUI", "Start Time"), _startEdit);

	_durationEdit = new QSpinBox(this);
	_durationEdit->setRange(1, 7 * 24 * 60);
	_durationEdit->setSuffix(QCoreApplication::translate("MeetingUI", " min"));
	form->addRow(QCoreApplication::translate("MeetingUI", "Duration"), _durationEdit);

	_timeZoneEdit = new QComboBox(this);
	_timeZoneEdit->setEditable(true);
	_timeZoneEdit->setInsertPolicy(QComboBox::NoInsert);
	addTimeZone(_timeZoneEdit, systemTimeZoneId());
	addTimeZone(_timeZoneEdit, QStringLiteral("Asia/Shanghai"));
	addTimeZone(_timeZoneEdit, QStringLiteral("UTC"));
	addTimeZone(_timeZoneEdit, QStringLiteral("America/New_York"));
	addTimeZone(_timeZoneEdit, QStringLiteral("Europe/London"));
	form->addRow(QCoreApplication::translate("MeetingUI", "Time Zone"), _timeZoneEdit);

	_repeatTypeEdit = new QComboBox(this);
	_repeatTypeEdit->addItem(QCoreApplication::translate("MeetingUI", "Does Not Repeat"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::None));
	_repeatTypeEdit->addItem(QCoreApplication::translate("MeetingUI", "Daily"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Daily));
	_repeatTypeEdit->addItem(QCoreApplication::translate("MeetingUI", "Weekly"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Weekly));
	_repeatTypeEdit->addItem(QCoreApplication::translate("MeetingUI", "Weekdays"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::WeekDay));
	_repeatTypeEdit->addItem(QCoreApplication::translate("MeetingUI", "Monthly"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Monthly));
	form->addRow(QCoreApplication::translate("MeetingUI", "Recurrence"), _repeatTypeEdit);

	auto *repeatEndRow = new QWidget(this);
	auto *repeatEndLayout = new QHBoxLayout(repeatEndRow);
	repeatEndLayout->setContentsMargins(0, 0, 0, 0);
	repeatEndLayout->setSpacing(10);
	_repeatEndEnabled = new QCheckBox(QCoreApplication::translate("MeetingUI", "Set an end date"), repeatEndRow);
	_repeatEndEdit = new QDateEdit(repeatEndRow);
	_repeatEndEdit->setCalendarPopup(true);
	_repeatEndEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
	_repeatEndEdit->setMinimumDate(QDate(2000, 1, 1));
	_repeatEndEdit->setMaximumDate(QDate(2100, 12, 31));
	repeatEndLayout->addWidget(_repeatEndEnabled);
	repeatEndLayout->addWidget(_repeatEndEdit, 1);
	form->addRow(QCoreApplication::translate("MeetingUI", "Repeat Until"), repeatEndRow);

	_passwordEdit = new QLineEdit(this);
	_passwordEdit->setEchoMode(QLineEdit::Password);
	_passwordEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Optional; clear this field to remove the password"));
	form->addRow(QCoreApplication::translate("MeetingUI", "Meeting Password"), _passwordEdit);
	root->addLayout(form);

	_cameraOnJoin = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable camera on joining"), this);
	_microphoneOnJoin = new QCheckBox(QCoreApplication::translate("MeetingUI", "Enable microphone on joining"), this);
	root->addWidget(_cameraOnJoin);
	root->addWidget(_microphoneOnJoin);

	_statusLabel = new QLabel(this);
	_statusLabel->setWordWrap(true);
	_statusLabel->setTextFormat(Qt::PlainText);
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-booking-dialog-statuslabel");
	_statusLabel->hide();
	root->addWidget(_statusLabel);

	auto *buttons = new QHBoxLayout();
	buttons->addStretch();
	_cancelButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), this);
	_cancelButton->setObjectName(QStringLiteral("secondary"));
	_submitButton = new QPushButton(
		_original ? QCoreApplication::translate("MeetingUI", "Save Changes") : QCoreApplication::translate("MeetingUI", "Create Meeting"), this);
	_submitButton->setObjectName(QStringLiteral("primary"));
	buttons->addWidget(_cancelButton);
	buttons->addWidget(_submitButton);
	root->addLayout(buttons);

	connect(_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
	connect(_submitButton, &QPushButton::clicked, this, [this] { submit(); });
	connect(_repeatTypeEdit, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this](int) { updateRepeatControls(); });
	connect(_repeatEndEnabled, &QCheckBox::toggled,
		this, [this](bool) { updateRepeatControls(); });
	connect(_startEdit, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime &value) {
		_repeatEndEdit->setMinimumDate(value.date());
		if (_repeatEndEdit->date() < value.date()) _repeatEndEdit->setDate(value.date());
	});
	connect(&_controller, &OpenMeeting::MeetingCatalogController::writeStateChanged,
		this, [this] { handleWriteState(); });
	connect(&_session, &OpenMeeting::SessionManager::authenticationReset,
		this, [this](quint64) { reject(); });
	updateRepeatControls();
}

void MeetingBookingDialog::populateCreateDefaults() {
	const auto nickname = _session.nickname().trimmed();
	_titleEdit->setText(nickname.isEmpty()
		? QCoreApplication::translate("MeetingUI", "My Meeting")
		: QCoreApplication::translate("MeetingUI", "%1's Meeting").arg(nickname));
	const auto timeZoneId = systemTimeZoneId();
	_timeZoneEdit->setCurrentText(timeZoneId);
	_initialTimeZoneText = timeZoneId;
	const QTimeZone timeZone(timeZoneId.toUtf8());
	const auto now = QDateTime::currentDateTime().toTimeZone(timeZone);
	const auto roundedSeconds = ((now.toSecsSinceEpoch() / 1800) + 1) * 1800;
	_startEdit->setDateTime(QDateTime::fromSecsSinceEpoch(roundedSeconds, timeZone));
	_repeatTypeEdit->setCurrentIndex(0);
	_repeatEndEnabled->setChecked(false);
	_repeatEndEdit->setDate(_startEdit->date().addMonths(1));
	_durationEdit->setValue(60);
	_initialDurationMinutes = _durationEdit->value();
	_cameraOnJoin->setChecked(true);
	_microphoneOnJoin->setChecked(true);
}

void MeetingBookingDialog::populateEditValues() {
	if (!_original) return;
	const auto &record = _original->record;
	_titleEdit->setText(record.title);
	auto timeZoneId = record.timeZone;
	if (!QTimeZone(timeZoneId.toUtf8()).isValid()) timeZoneId = systemTimeZoneId();
	addTimeZone(_timeZoneEdit, timeZoneId);
	_timeZoneEdit->setCurrentText(timeZoneId);
	_initialTimeZoneText = timeZoneId;
	_startEdit->setDateTime(QDateTime::fromSecsSinceEpoch(
		record.scheduledTimeSeconds, QTimeZone(timeZoneId.toUtf8())));
	const auto repeatIndex = _repeatTypeEdit->findData(static_cast<int>(record.repeatRule.type));
	if (repeatIndex >= 0) {
		_repeatTypeEdit->setCurrentIndex(repeatIndex);
	} else {
		_repeatTypeEdit->addItem(readOnlyRepeatName(record.repeatRule),
			static_cast<int>(record.repeatRule.type));
		_repeatTypeEdit->setCurrentIndex(_repeatTypeEdit->count() - 1);
	}
	_repeatEndEnabled->setChecked(record.repeatRule.endDateSeconds > 0);
	_repeatEndEdit->setDate(record.repeatRule.endDateSeconds > 0
		? QDateTime::fromSecsSinceEpoch(
			record.repeatRule.endDateSeconds, QTimeZone(timeZoneId.toUtf8())).date()
		: _startEdit->date().addMonths(1));
	_durationEdit->setValue(static_cast<int>(std::max<qint64>(
		1, (record.meetingDurationSeconds + 59) / 60)));
	_initialDurationMinutes = _durationEdit->value();
	_passwordEdit->setText(_original->password);
	_cameraOnJoin->setChecked(!record.settings.disableCameraOnJoin);
	_microphoneOnJoin->setChecked(!record.settings.disableMicrophoneOnJoin);
}

bool MeetingBookingDialog::collectCommon(
		qint64 *scheduledTimeSeconds,
		qint64 *durationSeconds,
		QString *timeZoneId) {
	const auto title = _titleEdit->text().trimmed();
	if (title.isEmpty()) {
		showError(QCoreApplication::translate("MeetingUI", "Enter a meeting title."));
		_titleEdit->setFocus();
		return false;
	}

	*timeZoneId = _timeZoneEdit->currentText().trimmed();
	const QTimeZone timeZone(timeZoneId->toUtf8());
	if (!timeZone.isValid()) {
		showError(QCoreApplication::translate("MeetingUI", "Select a valid IANA time zone."));
		_timeZoneEdit->setFocus();
		return false;
	}

	const auto requestedDate = _startEdit->date();
	const auto requestedTime = _startEdit->time();
	const QDateTime scheduled(requestedDate, requestedTime, timeZone);
	if (!scheduled.isValid() || scheduled.date() != requestedDate || scheduled.time() != requestedTime) {
		showError(QCoreApplication::translate("MeetingUI", "The selected time is invalid in this time zone. Choose another time."));
		_startEdit->setFocus();
		return false;
	}
	*scheduledTimeSeconds = scheduled.toSecsSinceEpoch();
	*durationSeconds = static_cast<qint64>(_durationEdit->value()) * 60;

	const bool startChanged = !_original ||
		*scheduledTimeSeconds != _original->record.scheduledTimeSeconds;
	if (startChanged && *scheduledTimeSeconds <= QDateTime::currentSecsSinceEpoch()) {
		showError(QCoreApplication::translate("MeetingUI", "The start time must be in the future."));
		_startEdit->setFocus();
		return false;
	}
	return true;
}

bool MeetingBookingDialog::collectRepeatRule(
		qint64 scheduledTimeSeconds,
		const QString &timeZoneId,
		OpenMeeting::MeetingRepeatRule *repeatRule) {
	if (!repeatRule) return false;
	*repeatRule = {};
	repeatRule->type = selectedRepeatType(_repeatTypeEdit);
	repeatRule->rawType = OpenMeeting::meetingRepeatTypeToWire(repeatRule->type);
	if (!OpenMeeting::isMeetingRepeatTypeSupportedForCreate(repeatRule->type)) {
		showError(QCoreApplication::translate("MeetingUI", "This recurrence is read-only and cannot be used for a new meeting."));
		_repeatTypeEdit->setFocus();
		return false;
	}
	if (repeatRule->type == OpenMeeting::MeetingRepeatType::None) return true;
	if (_repeatEndEnabled->isChecked()) {
		const QTimeZone timeZone(timeZoneId.toUtf8());
		const auto start = QDateTime::fromSecsSinceEpoch(scheduledTimeSeconds, timeZone);
		const auto requestedDate = _repeatEndEdit->date();
		const QDateTime end(requestedDate, start.time(), timeZone);
		if (!end.isValid() || end.date() != requestedDate || end.time() != start.time()) {
			showError(QCoreApplication::translate("MeetingUI", "The recurrence end time is invalid in the selected time zone. Choose another end date."));
			_repeatEndEdit->setFocus();
			return false;
		}
		if (end < start) {
			showError(QCoreApplication::translate("MeetingUI", "The recurrence end date cannot precede the first meeting."));
			_repeatEndEdit->setFocus();
			return false;
		}
		repeatRule->endDateSeconds = end.toSecsSinceEpoch();
	}
	QString validationMessage;
	if (!OpenMeeting::isMeetingRepeatRuleSupportedForWrite(*repeatRule, &validationMessage)) {
		showError(QCoreApplication::translate("MeetingUI", "Invalid recurrence. Please choose another option."));
		return false;
	}
	return true;
}

OpenMeeting::MeetingBookingRequest MeetingBookingDialog::createRequest(
		qint64 scheduledTimeSeconds,
		qint64 durationSeconds,
		const QString &timeZone,
		const OpenMeeting::MeetingRepeatRule &repeatRule) const {
	OpenMeeting::MeetingBookingRequest request;
	request.title = _titleEdit->text().trimmed();
	request.scheduledTimeSeconds = scheduledTimeSeconds;
	request.meetingDurationSeconds = durationSeconds;
	request.password = _passwordEdit->text();
	request.timeZone = timeZone;
	request.settings.disableCameraOnJoin = !_cameraOnJoin->isChecked();
	request.settings.disableMicrophoneOnJoin = !_microphoneOnJoin->isChecked();
	request.repeatRule = repeatRule;
	return request;
}

std::optional<OpenMeeting::MeetingUpdateRequest> MeetingBookingDialog::updateRequest(
		qint64 scheduledTimeSeconds,
		qint64 durationSeconds,
		const QString &timeZone,
		QString *message) const {
	if (!_original) return std::nullopt;
	const auto &record = _original->record;
	if (record.repeatRule.type != OpenMeeting::MeetingRepeatType::None) {
		if (message) *message = QCoreApplication::translate("MeetingUI", "Recurring meetings are currently read-only. Editing will be available after server rule validation is complete.");
		return std::nullopt;
	}

	OpenMeeting::MeetingUpdateRequest request;
	request.meetingId = record.meetingId;
	const auto title = _titleEdit->text().trimmed();
	if (title != record.title) request.title = title;
	if (scheduledTimeSeconds != record.scheduledTimeSeconds) {
		request.scheduledTimeSeconds = scheduledTimeSeconds;
	}
	if (_durationEdit->value() != _initialDurationMinutes) {
		request.meetingDurationSeconds = durationSeconds;
	}
	if (_passwordEdit->text() != _original->password) request.password = _passwordEdit->text();
	if (timeZone != _initialTimeZoneText) request.timeZone = timeZone;
	const bool disableCamera = !_cameraOnJoin->isChecked();
	const bool disableMicrophone = !_microphoneOnJoin->isChecked();
	if (disableCamera != record.settings.disableCameraOnJoin) {
		request.disableCameraOnJoin = disableCamera;
	}
	if (disableMicrophone != record.settings.disableMicrophoneOnJoin) {
		request.disableMicrophoneOnJoin = disableMicrophone;
	}

	QString validationMessage;
	if (!OpenMeeting::validateMeetingUpdateRequest(request, &validationMessage)) {
		if (message) *message = QCoreApplication::translate("MeetingUI", "No changes to save.");
		return std::nullopt;
	}
	return request;
}

void MeetingBookingDialog::submit() {
	if (_submitting) return;
	showError({});
	qint64 scheduledTimeSeconds = 0;
	qint64 durationSeconds = 0;
	QString timeZone;
	if (!collectCommon(&scheduledTimeSeconds, &durationSeconds, &timeZone)) return;
	OpenMeeting::MeetingRepeatRule repeatRule;
	if (!_original && !collectRepeatRule(scheduledTimeSeconds, timeZone, &repeatRule)) return;

	_submitting = true;
	setBusy(true);
	if (!_original) {
		if (!_controller.bookMeeting(createRequest(
				scheduledTimeSeconds, durationSeconds, timeZone, repeatRule))) {
			_submitting = false;
			setBusy(false);
		}
		return;
	}

	QString message;
	const auto request = updateRequest(scheduledTimeSeconds, durationSeconds, timeZone, &message);
	if (!request) {
		_submitting = false;
		setBusy(false);
		showError(message);
		return;
	}
	if (!_controller.updateMeeting(*request)) {
		_submitting = false;
		setBusy(false);
	}
}

void MeetingBookingDialog::handleWriteState() {
	if (!_submitting) return;
	const auto &result = _controller.lastWriteResult();
	const auto expectedKind = _original
		? OpenMeeting::MeetingWriteKind::Update
		: OpenMeeting::MeetingWriteKind::Book;
	if (result.kind != expectedKind) return;
	if (_original && result.meetingId != _original->record.meetingId) return;
	if (result.inFlight) {
		setBusy(true);
		return;
	}
	_submitting = false;
	setBusy(false);
	if (result.success) {
		accept();
	} else {
		showError(saveFailureText(result.error));
	}
}

void MeetingBookingDialog::setBusy(bool busy) {
	_titleEdit->setEnabled(!busy);
	_startEdit->setEnabled(!busy);
	_durationEdit->setEnabled(!busy);
	_timeZoneEdit->setEnabled(!busy);
	updateRepeatControls();
	_passwordEdit->setEnabled(!busy);
	_cameraOnJoin->setEnabled(!busy);
	_microphoneOnJoin->setEnabled(!busy);
	_submitButton->setEnabled(!busy);
	_submitButton->setText(busy
		? QCoreApplication::translate("MeetingUI", "Saving...")
		: (_original ? QCoreApplication::translate("MeetingUI", "Save Changes") : QCoreApplication::translate("MeetingUI", "Create Meeting")));
	if (busy) {
		MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-booking-dialog-statuslabel-2");
		_statusLabel->setText(QCoreApplication::translate("MeetingUI", "Saving meeting schedule..."));
		_statusLabel->show();
	}
}

void MeetingBookingDialog::updateRepeatControls() {
	if (!_repeatTypeEdit || !_repeatEndEnabled || !_repeatEndEdit) return;
	const bool editable = !_original && !_submitting;
	const bool repeating = selectedRepeatType(_repeatTypeEdit) !=
		OpenMeeting::MeetingRepeatType::None;
	_repeatTypeEdit->setEnabled(editable);
	_repeatEndEnabled->setEnabled(editable && repeating);
	_repeatEndEdit->setEnabled(editable && repeating && _repeatEndEnabled->isChecked());
}

void MeetingBookingDialog::showError(const QString &message) {
	MeetingUI::AppTheme::setStyleVariant(*_statusLabel, "meeting-booking-dialog-statuslabel-3");
	_statusLabel->setText(message);
	_statusLabel->setVisible(!message.isEmpty());
}

} // namespace MeetingUI
