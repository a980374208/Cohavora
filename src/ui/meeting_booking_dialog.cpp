#include "src/ui/meeting_booking_dialog.h"

#include "src/core/meeting_catalog_controller.h"
#include "src/net/session_manager.h"

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
	if (error.code == 0) return QString::fromUtf8("保存失败，请稍后重试。");
	return QString::fromUtf8("保存失败。网络中断时结果可能未知，请刷新会议列表确认后再操作。");
}

OpenMeeting::MeetingRepeatType selectedRepeatType(const QComboBox *combo) {
	return static_cast<OpenMeeting::MeetingRepeatType>(combo->currentData().toInt());
}

QString readOnlyRepeatName(const OpenMeeting::MeetingRepeatRule &rule) {
	if (rule.type == OpenMeeting::MeetingRepeatType::Custom) {
		return QString::fromUtf8("自定义重复（只读）");
	}
	return rule.rawType.trimmed().isEmpty()
		? QString::fromUtf8("未知重复规则（只读）")
		: QString::fromUtf8("%1（只读）").arg(rule.rawType);
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
}

void MeetingBookingDialog::initUi() {
	setWindowTitle(_original ? QString::fromUtf8("编辑会议") : QString::fromUtf8("预定会议"));
	setModal(true);
	setMinimumWidth(500);
	setStyleSheet(R"(
		QDialog { background: #ffffff; }
		QLabel { color: #303133; font-size: 13px; }
		QLineEdit, QDateEdit, QDateTimeEdit, QSpinBox, QComboBox {
			min-height: 34px; border: 1px solid #dcdfe6; border-radius: 6px;
			padding: 0 9px; background: #ffffff;
		}
		QLineEdit:focus, QDateEdit:focus, QDateTimeEdit:focus, QSpinBox:focus, QComboBox:focus {
			border-color: #1677ff;
		}
		QPushButton { min-height: 34px; padding: 0 18px; border-radius: 6px; }
		QPushButton#primary { background: #1677ff; color: white; border: none; }
		QPushButton#primary:hover { background: #4096ff; }
		QPushButton#primary:disabled { background: #b7d6ff; }
		QPushButton#secondary { background: #f2f3f5; color: #4e5969; border: none; }
	)");

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(28, 24, 28, 24);
	root->setSpacing(16);

	auto *title = new QLabel(
		_original ? QString::fromUtf8("编辑会议安排") : QString::fromUtf8("创建会议安排"), this);
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
	_titleEdit->setPlaceholderText(QString::fromUtf8("请输入会议主题"));
	form->addRow(QString::fromUtf8("会议主题"), _titleEdit);

	_startEdit = new QDateTimeEdit(this);
	_startEdit->setCalendarPopup(true);
	_startEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
	_startEdit->setMinimumDate(QDate(2000, 1, 1));
	_startEdit->setMaximumDate(QDate(2100, 12, 31));
	form->addRow(QString::fromUtf8("开始时间"), _startEdit);

	_durationEdit = new QSpinBox(this);
	_durationEdit->setRange(1, 7 * 24 * 60);
	_durationEdit->setSuffix(QString::fromUtf8(" 分钟"));
	form->addRow(QString::fromUtf8("计划时长"), _durationEdit);

	_timeZoneEdit = new QComboBox(this);
	_timeZoneEdit->setEditable(true);
	_timeZoneEdit->setInsertPolicy(QComboBox::NoInsert);
	addTimeZone(_timeZoneEdit, systemTimeZoneId());
	addTimeZone(_timeZoneEdit, QStringLiteral("Asia/Shanghai"));
	addTimeZone(_timeZoneEdit, QStringLiteral("UTC"));
	addTimeZone(_timeZoneEdit, QStringLiteral("America/New_York"));
	addTimeZone(_timeZoneEdit, QStringLiteral("Europe/London"));
	form->addRow(QString::fromUtf8("预约时区"), _timeZoneEdit);

	_repeatTypeEdit = new QComboBox(this);
	_repeatTypeEdit->addItem(QString::fromUtf8("不重复"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::None));
	_repeatTypeEdit->addItem(QString::fromUtf8("每天"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Daily));
	_repeatTypeEdit->addItem(QString::fromUtf8("每周"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Weekly));
	_repeatTypeEdit->addItem(QString::fromUtf8("工作日"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::WeekDay));
	_repeatTypeEdit->addItem(QString::fromUtf8("每月"),
		static_cast<int>(OpenMeeting::MeetingRepeatType::Monthly));
	form->addRow(QString::fromUtf8("重复规则"), _repeatTypeEdit);

	auto *repeatEndRow = new QWidget(this);
	auto *repeatEndLayout = new QHBoxLayout(repeatEndRow);
	repeatEndLayout->setContentsMargins(0, 0, 0, 0);
	repeatEndLayout->setSpacing(10);
	_repeatEndEnabled = new QCheckBox(QString::fromUtf8("设置结束日期"), repeatEndRow);
	_repeatEndEdit = new QDateEdit(repeatEndRow);
	_repeatEndEdit->setCalendarPopup(true);
	_repeatEndEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
	_repeatEndEdit->setMinimumDate(QDate(2000, 1, 1));
	_repeatEndEdit->setMaximumDate(QDate(2100, 12, 31));
	repeatEndLayout->addWidget(_repeatEndEnabled);
	repeatEndLayout->addWidget(_repeatEndEdit, 1);
	form->addRow(QString::fromUtf8("重复结束"), repeatEndRow);

	_passwordEdit = new QLineEdit(this);
	_passwordEdit->setEchoMode(QLineEdit::Password);
	_passwordEdit->setPlaceholderText(QString::fromUtf8("选填；清空后保存可移除密码"));
	form->addRow(QString::fromUtf8("入会密码"), _passwordEdit);
	root->addLayout(form);

	_cameraOnJoin = new QCheckBox(QString::fromUtf8("入会时默认开启摄像头"), this);
	_microphoneOnJoin = new QCheckBox(QString::fromUtf8("入会时默认开启麦克风"), this);
	root->addWidget(_cameraOnJoin);
	root->addWidget(_microphoneOnJoin);

	_statusLabel = new QLabel(this);
	_statusLabel->setWordWrap(true);
	_statusLabel->setTextFormat(Qt::PlainText);
	_statusLabel->setStyleSheet(QStringLiteral("color: #d4380d;"));
	_statusLabel->hide();
	root->addWidget(_statusLabel);

	auto *buttons = new QHBoxLayout();
	buttons->addStretch();
	_cancelButton = new QPushButton(QString::fromUtf8("取消"), this);
	_cancelButton->setObjectName(QStringLiteral("secondary"));
	_submitButton = new QPushButton(
		_original ? QString::fromUtf8("保存修改") : QString::fromUtf8("创建会议"), this);
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
		? QString::fromUtf8("我的会议")
		: QString::fromUtf8("%1的会议").arg(nickname));
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
		showError(QString::fromUtf8("请输入会议主题。"));
		_titleEdit->setFocus();
		return false;
	}

	*timeZoneId = _timeZoneEdit->currentText().trimmed();
	const QTimeZone timeZone(timeZoneId->toUtf8());
	if (!timeZone.isValid()) {
		showError(QString::fromUtf8("请选择有效的 IANA 时区。"));
		_timeZoneEdit->setFocus();
		return false;
	}

	const auto requestedDate = _startEdit->date();
	const auto requestedTime = _startEdit->time();
	const QDateTime scheduled(requestedDate, requestedTime, timeZone);
	if (!scheduled.isValid() || scheduled.date() != requestedDate || scheduled.time() != requestedTime) {
		showError(QString::fromUtf8("所选时间在该时区中无效，请重新选择。"));
		_startEdit->setFocus();
		return false;
	}
	*scheduledTimeSeconds = scheduled.toSecsSinceEpoch();
	*durationSeconds = static_cast<qint64>(_durationEdit->value()) * 60;

	const bool startChanged = !_original ||
		*scheduledTimeSeconds != _original->record.scheduledTimeSeconds;
	if (startChanged && *scheduledTimeSeconds <= QDateTime::currentSecsSinceEpoch()) {
		showError(QString::fromUtf8("开始时间必须晚于当前时间。"));
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
		showError(QString::fromUtf8("该重复规则仅支持查看，不能用于新预约。"));
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
			showError(QString::fromUtf8("重复结束时间在所选时区中无效，请更换结束日期。"));
			_repeatEndEdit->setFocus();
			return false;
		}
		if (end < start) {
			showError(QString::fromUtf8("重复结束日期不能早于首次预约日期。"));
			_repeatEndEdit->setFocus();
			return false;
		}
		repeatRule->endDateSeconds = end.toSecsSinceEpoch();
	}
	QString validationMessage;
	if (!OpenMeeting::isMeetingRepeatRuleSupportedForWrite(*repeatRule, &validationMessage)) {
		showError(QString::fromUtf8("重复规则无效，请重新选择。"));
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
		if (message) *message = QString::fromUtf8("重复会议当前仅支持查看。请等待服务端规则验证完成后再编辑。");
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
		if (message) *message = QString::fromUtf8("没有需要保存的修改。");
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
		? QString::fromUtf8("正在保存...")
		: (_original ? QString::fromUtf8("保存修改") : QString::fromUtf8("创建会议")));
	if (busy) {
		_statusLabel->setStyleSheet(QStringLiteral("color: #1677ff;"));
		_statusLabel->setText(QString::fromUtf8("正在保存会议安排..."));
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
	_statusLabel->setStyleSheet(QStringLiteral("color: #d4380d;"));
	_statusLabel->setText(message);
	_statusLabel->setVisible(!message.isEmpty());
}

} // namespace MeetingUI
