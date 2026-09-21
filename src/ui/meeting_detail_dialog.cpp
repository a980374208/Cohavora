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
	if (minutes % 60 == 0) return QString::fromUtf8("%1 小时").arg(minutes / 60);
	if (minutes > 60) {
		return QString::fromUtf8("%1 小时 %2 分钟").arg(minutes / 60).arg(minutes % 60);
	}
	return QString::fromUtf8("%1 分钟").arg(minutes);
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
	setWindowTitle(QString::fromUtf8("会议详情"));
	setModal(true);
	setMinimumWidth(540);
	setStyleSheet(R"(
		QDialog { background: #ffffff; }
		QLabel { color: #303133; font-size: 13px; }
		QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; }
		QPushButton#primary { background: #1677ff; color: white; border: none; }
		QPushButton#secondary { background: #f2f3f5; color: #4e5969; border: none; }
		QPushButton#danger { background: #fff1f0; color: #cf1322; border: 1px solid #ffccc7; }
	)");

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(28, 24, 28, 24);
	root->setSpacing(15);
	_titleLabel = detailValue(this);
	auto titleFont = _titleLabel->font();
	titleFont.setPixelSize(21);
	titleFont.setBold(true);
	_titleLabel->setFont(titleFont);
	_titleLabel->setText(QString::fromUtf8("正在加载会议..."));
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
	form->addRow(QString::fromUtf8("会议号"), _meetingIdLabel);
	form->addRow(QString::fromUtf8("创建者"), _creatorLabel);
	form->addRow(QString::fromUtf8("预约时间"), _timeLabel);
	form->addRow(QString::fromUtf8("计划时长"), _durationLabel);
	form->addRow(QString::fromUtf8("预约时区"), _timeZoneLabel);
	form->addRow(QString::fromUtf8("状态"), _meetingStatusLabel);
	form->addRow(QString::fromUtf8("重复"), _repeatLabel);
	root->addLayout(form);

	_stateLabel = new QLabel(this);
	_stateLabel->setWordWrap(true);
	_stateLabel->setTextFormat(Qt::PlainText);
	_stateLabel->setStyleSheet(QStringLiteral("color: #8f959e;"));
	root->addWidget(_stateLabel);

	_retryButton = new QPushButton(QString::fromUtf8("重试"), this);
	_retryButton->setObjectName(QStringLiteral("secondary"));
	_retryButton->hide();
	root->addWidget(_retryButton, 0, Qt::AlignLeft);

	auto *buttons = new QHBoxLayout();
	_copyButton = new QPushButton(QString::fromUtf8("复制会议号"), this);
	_copyButton->setObjectName(QStringLiteral("secondary"));
	_joinButton = new QPushButton(QString::fromUtf8("加入会议"), this);
	_joinButton->setObjectName(QStringLiteral("primary"));
	_editButton = new QPushButton(QString::fromUtf8("编辑"), this);
	_editButton->setObjectName(QStringLiteral("secondary"));
	_cancelMeetingButton = new QPushButton(QString::fromUtf8("取消会议"), this);
	_cancelMeetingButton->setObjectName(QStringLiteral("danger"));
	auto *closeButton = new QPushButton(QString::fromUtf8("关闭"), this);
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
			_stateLabel->setStyleSheet(QStringLiteral("color: #1677ff;"));
			_stateLabel->setText(QString::fromUtf8("正在刷新详情..."));
		} else if (state.error.code != 0) {
			_stateLabel->setStyleSheet(QStringLiteral("color: #d4380d;"));
			_stateLabel->setText(QString::fromUtf8("刷新失败，当前显示最近一次结果。"));
		}
		return;
	}

	setActionsEnabled(false);
	if (state.state == OpenMeeting::MeetingCatalogLoadState::Loading || state.refreshing) {
		_titleLabel->setText(QString::fromUtf8("正在加载会议..."));
		_stateLabel->setStyleSheet(QStringLiteral("color: #1677ff;"));
		_stateLabel->setText(QString::fromUtf8("正在读取最新会议详情..."));
		_retryButton->hide();
	} else if (state.state == OpenMeeting::MeetingCatalogLoadState::Error) {
		_titleLabel->setText(QString::fromUtf8("无法加载会议"));
		_stateLabel->setStyleSheet(QStringLiteral("color: #d4380d;"));
		_stateLabel->setText(QString::fromUtf8("会议详情加载失败，请检查网络后重试。"));
		_retryButton->show();
	}
}

void MeetingDetailDialog::populate(
		const OpenMeeting::MeetingCatalogDetail &detail,
		bool allowJoin) {
	const auto &meeting = detail.record;
	if (!_cancelling) _stateLabel->clear();
	_titleLabel->setText(meeting.title.trimmed().isEmpty()
		? QString::fromUtf8("未命名会议") : meeting.title);
	_meetingIdLabel->setText(meeting.meetingId);
	_creatorLabel->setText(meeting.creatorNickname.trimmed().isEmpty()
		? meeting.creatorUserId : meeting.creatorNickname);
	const auto dateTime = MeetingListModel::scheduledDateTime(meeting);
	_timeLabel->setText(dateTime.isValid()
		? dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
		: QString::fromUtf8("时间待确认"));
	_durationLabel->setText(durationText(meeting.meetingDurationSeconds));
	_timeZoneLabel->setText(meeting.timeZone.trimmed().isEmpty()
		? QString::fromUtf8("系统本地时区") : meeting.timeZone);
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
		_stateLabel->setStyleSheet(QStringLiteral("color: #8f959e;"));
		_stateLabel->setText(QString::fromUtf8("重复会议当前仅支持查看。"));
	} else if (!ownsMeeting && !_cancelling) {
		_stateLabel->setStyleSheet(QStringLiteral("color: #8f959e;"));
		_stateLabel->setText(QString::fromUtf8("只有会议创建者可以编辑或取消该预约。"));
	} else if (!scheduled && !_cancelling) {
		_stateLabel->setStyleSheet(QStringLiteral("color: #8f959e;"));
		_stateLabel->setText(QString::fromUtf8("当前状态下会议详情为只读。"));
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
	_stateLabel->setStyleSheet(QStringLiteral("color: #d4380d;"));
	_stateLabel->setText(QString::fromUtf8(
		"取消失败。网络中断时结果可能未知，请刷新会议列表确认后再操作。"));
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
		QString::fromUtf8("取消会议"),
		QString::fromUtf8("确定取消“%1”？\n预约时间：%2")
			.arg(_detail->record.title.trimmed().isEmpty()
				? QString::fromUtf8("未命名会议") : _detail->record.title,
				when.isValid() ? when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
					: QString::fromUtf8("时间待确认")),
		QMessageBox::Yes | QMessageBox::No,
		this);
	confirm.setTextFormat(Qt::PlainText);
	if (confirm.exec() != QMessageBox::Yes) return;

	_cancelling = true;
	setActionsEnabled(false);
	_stateLabel->setStyleSheet(QStringLiteral("color: #1677ff;"));
	_stateLabel->setText(QString::fromUtf8("正在取消会议..."));
	if (!_controller.cancelMeeting(_meetingId)) {
		_cancelling = false;
		if (_detail) populate(*_detail, false);
	}
}

void MeetingDetailDialog::copyMeetingId() {
	if (!_detail) return;
	QApplication::clipboard()->setText(_detail->record.meetingId);
	_stateLabel->setStyleSheet(QStringLiteral("color: #089f62;"));
	_stateLabel->setText(QString::fromUtf8("会议号已复制。"));
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
