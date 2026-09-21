#include <QtCore/QCoreApplication>
#include "src/ui/app_theme.h"
#include "src/ui/meeting_detail_dialog.h"

#include "src/core/meeting_catalog_controller.h"
#include "src/net/session_manager.h"
#include "src/ui/meeting_booking_dialog.h"
#include "src/ui/meeting_list_model.h"

#include <QtGui/QClipboard>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace MeetingUI {
namespace {

QLabel *detailValue(QWidget *parent) {
	auto *label = new QLabel(parent);
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	return label;
}

QString durationText(qint64 seconds) {
	const auto minutes = std::max<qint64>(1, (seconds + 59) / 60);
	if (minutes % 60 == 0) return QCoreApplication::translate("MeetingUI", "%1 hr").arg(minutes / 60);
	if (minutes > 60) {
		return QCoreApplication::translate("MeetingUI", "%1 hr %2 min").arg(minutes / 60).arg(minutes % 60);
	}
	return QCoreApplication::translate("MeetingUI", "%1 min").arg(minutes);
}

} // namespace

MeetingDetailDialog::MeetingDetailDialog(
		const QString &meetingId,
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent)
	: QDialog(parent)
	, _meetingId(meetingId)
	, _controller(controller)
	, _session(session) {
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Meeting Details"));
	setModal(true);
	setMinimumWidth(540);
	MeetingUI::AppTheme::setStyleVariant(*this, "meeting-detail-dialog-this");

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(28, 24, 28, 24);
	root->setSpacing(15);
	_titleLabel = detailValue(this);
	auto titleFont = _titleLabel->font();
	titleFont.setPixelSize(21);
	titleFont.setBold(true);
	_titleLabel->setFont(titleFont);
	_titleLabel->setText(QCoreApplication::translate("MeetingUI", "Loading meeting..."));
	root->addWidget(_titleLabel);

	auto *form = new QFormLayout();
	form->setHorizontalSpacing(18);
	form->setVerticalSpacing(11);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
	_meetingIdLabel = detailValue(this);
	_creatorLabel = detailValue(this);
	_timeLabel = detailValue(this);
	_durationLabel = detailValue(this);
	_timeZoneLabel = detailValue(this);
	_meetingStatusLabel = detailValue(this);
	_repeatLabel = detailValue(this);
	form->addRow(QCoreApplication::translate("MeetingUI", "Meeting ID"), _meetingIdLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Created By"), _creatorLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Scheduled For"), _timeLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Duration"), _durationLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Time Zone"), _timeZoneLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Status"), _meetingStatusLabel);
	form->addRow(QCoreApplication::translate("MeetingUI", "Repeat"), _repeatLabel);
	root->addLayout(form);

	_stateLabel = new QLabel(this);
	_stateLabel->setWordWrap(true);
	_stateLabel->setTextFormat(Qt::PlainText);
	MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel");
	root->addWidget(_stateLabel);

	_retryButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Retry"), this);
	_retryButton->setObjectName(QStringLiteral("secondary"));
	_retryButton->hide();
	root->addWidget(_retryButton, 0, Qt::AlignLeft);

	auto *buttons = new QHBoxLayout();
	_copyButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Copy Meeting ID"), this);
	_copyButton->setObjectName(QStringLiteral("secondary"));
	_joinButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Join Meeting"), this);
	_joinButton->setObjectName(QStringLiteral("primary"));
	_editButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Edit"), this);
	_editButton->setObjectName(QStringLiteral("secondary"));
	_cancelMeetingButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel Meeting"), this);
	_cancelMeetingButton->setObjectName(QStringLiteral("danger"));
	auto *closeButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Close"), this);
	closeButton->setObjectName(QStringLiteral("secondary"));
	buttons->addWidget(_copyButton);
	buttons->addStretch();
	buttons->addWidget(_cancelMeetingButton);
	buttons->addWidget(_editButton);
	buttons->addWidget(closeButton);
	buttons->addWidget(_joinButton);
	root->addLayout(buttons);

	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	connect(_retryButton, &QPushButton::clicked, this,
		[this] { _controller.loadMeetingDetail(_meetingId); });
	connect(_copyButton, &QPushButton::clicked, this,
		[this] { copyMeetingId(); });
	connect(_editButton, &QPushButton::clicked, this,
		[this] { editMeeting(); });
	connect(_cancelMeetingButton, &QPushButton::clicked, this,
		[this] { cancelMeeting(); });
	connect(_joinButton, &QPushButton::clicked, this,
		[this] { requestJoin(); });
	connect(&_controller, &OpenMeeting::MeetingCatalogController::detailChanged,
		this, [this] { updateDetail(); });
	connect(&_controller, &OpenMeeting::MeetingCatalogController::writeStateChanged,
		this, [this] { handleWriteState(); });
	connect(&_session, &OpenMeeting::SessionManager::authenticationReset,
		this, [this](quint64) { reject(); });

	AppTheme::makeDialogAdaptive(*this, QSize(640, 520));
	setActionsEnabled(false);
	updateDetail();
	_controller.loadMeetingDetail(_meetingId);
}

void MeetingDetailDialog::updateDetail() {
	const auto &state = _controller.detailState();
	if (state.meetingId != _meetingId) return;
	if (state.detail) {
		_detail = state.detail;
		const bool freshForJoin = !state.refreshing && state.error.code == 0 &&
			state.state == OpenMeeting::MeetingCatalogLoadState::Ready;
		populate(*state.detail, freshForJoin);
		_retryButton->hide();
		if (state.refreshing) {
			MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-2");
			_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Refreshing details..."));
		} else if (state.error.code != 0) {
			MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-3");
			_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Refresh failed. Showing the last available results."));
		}
		return;
	}

	setActionsEnabled(false);
	if (state.state == OpenMeeting::MeetingCatalogLoadState::Loading || state.refreshing) {
		_titleLabel->setText(QCoreApplication::translate("MeetingUI", "Loading meeting..."));
		MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-4");
		_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Loading the latest meeting details..."));
		_retryButton->hide();
	} else if (state.state == OpenMeeting::MeetingCatalogLoadState::Error) {
		_titleLabel->setText(QCoreApplication::translate("MeetingUI", "Unable to Load Meeting"));
		MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-5");
		_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Unable to load meeting details. Check your network and try again."));
		_retryButton->show();
	}
}

void MeetingDetailDialog::populate(
		const OpenMeeting::MeetingCatalogDetail &detail,
		bool allowJoin) {
	const auto &meeting = detail.record;
	if (!_cancelling) _stateLabel->clear();
	_titleLabel->setText(meeting.title.trimmed().isEmpty()
		? QCoreApplication::translate("MeetingUI", "Untitled Meeting") : meeting.title);
	_meetingIdLabel->setText(meeting.meetingId);
	_creatorLabel->setText(meeting.creatorNickname.trimmed().isEmpty()
		? meeting.creatorUserId : meeting.creatorNickname);
	const auto dateTime = MeetingListModel::scheduledDateTime(meeting);
	_timeLabel->setText(dateTime.isValid()
		? dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
		: QCoreApplication::translate("MeetingUI", "Time to Be Confirmed"));
	_durationLabel->setText(durationText(meeting.meetingDurationSeconds));
	_timeZoneLabel->setText(meeting.timeZone.trimmed().isEmpty()
		? QCoreApplication::translate("MeetingUI", "System Time Zone") : meeting.timeZone);
	_meetingStatusLabel->setText(MeetingListModel::statusText(meeting.status));
	_repeatLabel->setText(MeetingListModel::repeatText(meeting.repeatRule, meeting.timeZone));

	const bool ownsMeeting = !meeting.creatorUserId.isEmpty() &&
		meeting.creatorUserId == _session.userId();
	const bool scheduled = meeting.status == OpenMeeting::MeetingStatus::Scheduled;
	const bool singleMeeting = meeting.repeatRule.type == OpenMeeting::MeetingRepeatType::None;
	const bool canManage = ownsMeeting && scheduled && singleMeeting && !_cancelling;
	const bool canJoin = allowJoin && !_cancelling &&
		(meeting.status == OpenMeeting::MeetingStatus::Scheduled ||
		 meeting.status == OpenMeeting::MeetingStatus::InProgress);
	_copyButton->setEnabled(true);
	_joinButton->setEnabled(canJoin);
	_editButton->setEnabled(canManage);
	_cancelMeetingButton->setEnabled(canManage);
	if (!singleMeeting && !_cancelling) {
		MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-6");
		_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Recurring meetings are currently read-only."));
	} else if (!ownsMeeting && !_cancelling) {
		MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-7");
		_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Only the meeting creator can edit or cancel this meeting."));
	} else if (!scheduled && !_cancelling) {
		MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-8");
		_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Meeting details are read-only in the current state."));
	}
}

void MeetingDetailDialog::handleWriteState() {
	if (!_cancelling) return;
	const auto &result = _controller.lastWriteResult();
	if (result.kind != OpenMeeting::MeetingWriteKind::Cancel || result.meetingId != _meetingId) return;
	if (result.inFlight) return;
	_cancelling = false;
	if (result.success) {
		accept();
		return;
	}
	if (_detail) populate(*_detail, false);
	MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-9");
	_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Unable to cancel. A network interruption may leave the result unknown. Refresh the meeting list before trying again."));
}

void MeetingDetailDialog::editMeeting() {
	if (!_detail || !_editButton->isEnabled()) return;
	MeetingBookingDialog dialog(_controller, _session, *_detail, this);
	dialog.exec();
}

void MeetingDetailDialog::cancelMeeting() {
	if (!_detail || _cancelling || !_cancelMeetingButton->isEnabled()) return;
	const auto when = MeetingListModel::scheduledDateTime(_detail->record);
	QMessageBox confirm(QMessageBox::Question,
		QCoreApplication::translate("MeetingUI", "Cancel Meeting"),
		QCoreApplication::translate("MeetingUI", "Cancel \"%1\"?\nScheduled for: %2")
			.arg(_detail->record.title.trimmed().isEmpty()
				? QCoreApplication::translate("MeetingUI", "Untitled Meeting") : _detail->record.title,
				when.isValid() ? when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
					: QCoreApplication::translate("MeetingUI", "Time to Be Confirmed")),
		QMessageBox::Yes | QMessageBox::No,
		this);
	confirm.setTextFormat(Qt::PlainText);
	if (confirm.exec() != QMessageBox::Yes) return;

	_cancelling = true;
	setActionsEnabled(false);
	MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-10");
	_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Canceling meeting..."));
	if (!_controller.cancelMeeting(_meetingId)) {
		_cancelling = false;
		if (_detail) populate(*_detail, false);
	}
}

void MeetingDetailDialog::copyMeetingId() {
	if (!_detail) return;
	QApplication::clipboard()->setText(_detail->record.meetingId);
	MeetingUI::AppTheme::setStyleVariant(*_stateLabel, "meeting-detail-dialog-statelabel-11");
	_stateLabel->setText(QCoreApplication::translate("MeetingUI", "Meeting ID copied."));
}

void MeetingDetailDialog::requestJoin() {
	if (!_detail || !_joinButton->isEnabled()) return;
	const auto &state = _controller.detailState();
	if (state.meetingId != _meetingId || state.refreshing || state.error.code != 0 ||
		state.state != OpenMeeting::MeetingCatalogLoadState::Ready || !state.detail ||
		state.detail->record.meetingId != _meetingId) return;
	_joinRequested = true;
	accept();
}

std::optional<OpenMeeting::MeetingCatalogDetail> MeetingDetailDialog::detailForJoin() const {
	return _joinRequested ? _detail : std::nullopt;
}

void MeetingDetailDialog::setActionsEnabled(bool enabled) {
	_copyButton->setEnabled(enabled);
	_joinButton->setEnabled(enabled);
	_editButton->setEnabled(enabled);
	_cancelMeetingButton->setEnabled(enabled);
}

} // namespace MeetingUI
