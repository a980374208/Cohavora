#include "src/ui/meeting_list_model.h"

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>

#include <algorithm>

namespace MeetingUI {
namespace {

QTimeZone meetingTimeZone(const OpenMeeting::MeetingRecord &meeting) {
	const QTimeZone requested(meeting.timeZone.toUtf8());
	return requested.isValid() ? requested : QTimeZone::systemTimeZone();
}

QString displayTitle(const OpenMeeting::MeetingRecord &meeting) {
	const auto title = meeting.title.trimmed();
	return title.isEmpty() ? QString::fromUtf8("未命名会议") : title;
}

QString displayCreator(const OpenMeeting::MeetingRecord &meeting) {
	const auto nickname = meeting.creatorNickname.trimmed();
	return nickname.isEmpty() ? meeting.creatorUserId : nickname;
}

QString repeatEndSuffix(
		const OpenMeeting::MeetingRepeatRule &rule, const QString &timeZoneId) {
	if (rule.endDateSeconds <= 0) return {};
	const QTimeZone requested(timeZoneId.toUtf8());
	const auto timeZone = requested.isValid() ? requested : QTimeZone::systemTimeZone();
	const auto end = QDateTime::fromSecsSinceEpoch(rule.endDateSeconds, timeZone);
	return end.isValid()
		? QString::fromUtf8("，至 %1").arg(end.date().toString(QStringLiteral("yyyy-MM-dd")))
		: QString();
}

QString customRepeatText(const OpenMeeting::MeetingRepeatRule &rule) {
	QStringList details;
	if (rule.interval > 0 || !rule.unitType.trimmed().isEmpty()) {
		QString unit;
		if (rule.unitType == QStringLiteral("Day")) unit = QString::fromUtf8("天");
		else if (rule.unitType == QStringLiteral("Week")) unit = QString::fromUtf8("周");
		else if (rule.unitType == QStringLiteral("Month")) unit = QString::fromUtf8("月");
		else unit = rule.unitType.trimmed().isEmpty()
			? QString::fromUtf8("未知单位") : rule.unitType.trimmed();
		details.push_back(QString::fromUtf8("每 %1 %2").arg(rule.interval).arg(unit));
	}
	if (!rule.daysOfWeek.empty()) {
		static const QStringList names{
			QString::fromUtf8("周日"), QString::fromUtf8("周一"),
			QString::fromUtf8("周二"), QString::fromUtf8("周三"),
			QString::fromUtf8("周四"), QString::fromUtf8("周五"),
			QString::fromUtf8("周六"),
		};
		QStringList days;
		for (const int day : rule.daysOfWeek) {
			days.push_back(day >= 0 && day < names.size()
				? names[day] : QString::number(day));
		}
		details.push_back(days.join(QString::fromUtf8("、")));
	}
	return details.isEmpty()
		? QString::fromUtf8("自定义重复（只读）")
		: QString::fromUtf8("自定义重复（只读：%1）").arg(details.join(QString::fromUtf8("；")));
}

} // namespace

MeetingListModel::MeetingListModel(QObject *parent)
	: QAbstractListModel(parent) {
}

int MeetingListModel::rowCount(const QModelIndex &parent) const {
	return parent.isValid() ? 0 : static_cast<int>(_rows.size());
}

QVariant MeetingListModel::data(const QModelIndex &index, int role) const {
	if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
		return {};
	}
	const auto &row = _rows[static_cast<size_t>(index.row())];
	const auto &meeting = row.meeting;
	switch (role) {
	case Qt::DisplayRole:
	case MeetingTitleRole:
		return displayTitle(meeting);
	case Qt::ToolTipRole:
		return QString::fromUtf8("%1\n会议号：%2")
			.arg(displayTitle(meeting), meeting.meetingId);
	case MeetingIdRole:
		return meeting.meetingId;
	case MeetingCreatorRole:
		return displayCreator(meeting);
	case MeetingScheduledTimeRole:
		return meeting.scheduledTimeSeconds;
	case MeetingTimeDisplayRole: {
		const auto dateTime = scheduledDateTime(meeting);
		return dateTime.isValid() ? dateTime.time().toString(QStringLiteral("HH:mm"))
			: QStringLiteral("--:--");
	}
	case MeetingDurationRole:
		return meeting.meetingDurationSeconds;
	case MeetingStatusRole:
		return statusText(meeting.status);
	case MeetingRepeatRole:
		return repeatText(meeting.repeatRule, meeting.timeZone);
	case MeetingDateGroupRole:
		return row.dateGroup;
	case MeetingShowDateHeaderRole:
		return row.showDateHeader;
	default:
		return {};
	}
}

QHash<int, QByteArray> MeetingListModel::roleNames() const {
	auto result = QAbstractListModel::roleNames();
	result.insert(MeetingIdRole, "meetingId");
	result.insert(MeetingTitleRole, "meetingTitle");
	result.insert(MeetingCreatorRole, "meetingCreator");
	result.insert(MeetingScheduledTimeRole, "meetingScheduledTime");
	result.insert(MeetingTimeDisplayRole, "meetingTimeDisplay");
	result.insert(MeetingDurationRole, "meetingDuration");
	result.insert(MeetingStatusRole, "meetingStatus");
	result.insert(MeetingRepeatRole, "meetingRepeat");
	result.insert(MeetingDateGroupRole, "meetingDateGroup");
	result.insert(MeetingShowDateHeaderRole, "meetingShowDateHeader");
	return result;
}

void MeetingListModel::setMeetings(
		const OpenMeeting::MeetingList &meetings,
		bool history,
		const QString &historyCreatorUserId) {
	_source = meetings;
	_history = history;
	_historyCreatorUserId = historyCreatorUserId;
	rebuild();
}

void MeetingListModel::setSearchText(const QString &text) {
	const auto normalized = text.trimmed();
	if (_searchText == normalized) return;
	_searchText = normalized;
	rebuild();
}

const OpenMeeting::MeetingRecord *MeetingListModel::meetingAt(int row) const {
	if (row < 0 || row >= rowCount()) return nullptr;
	return &_rows[static_cast<size_t>(row)].meeting;
}

QDateTime MeetingListModel::scheduledDateTime(const OpenMeeting::MeetingRecord &meeting) {
	return QDateTime::fromSecsSinceEpoch(
		meeting.scheduledTimeSeconds,
		meetingTimeZone(meeting));
}

QString MeetingListModel::statusText(OpenMeeting::MeetingStatus status) {
	switch (status) {
	case OpenMeeting::MeetingStatus::Scheduled:
		return QString::fromUtf8("待开始");
	case OpenMeeting::MeetingStatus::InProgress:
		return QString::fromUtf8("进行中");
	case OpenMeeting::MeetingStatus::Completed:
		return QString::fromUtf8("已结束");
	case OpenMeeting::MeetingStatus::Unknown:
		return QString::fromUtf8("状态未知");
	}
	return QString::fromUtf8("状态未知");
}

QString MeetingListModel::repeatText(
		const OpenMeeting::MeetingRepeatRule &rule, const QString &timeZone) {
	QString text;
	switch (rule.type) {
	case OpenMeeting::MeetingRepeatType::None:
		return QString::fromUtf8("不重复");
	case OpenMeeting::MeetingRepeatType::Daily:
		text = QString::fromUtf8("每天重复");
		break;
	case OpenMeeting::MeetingRepeatType::Weekly:
		text = QString::fromUtf8("每周重复");
		break;
	case OpenMeeting::MeetingRepeatType::WeekDay:
		text = QString::fromUtf8("工作日重复");
		break;
	case OpenMeeting::MeetingRepeatType::Monthly:
		text = QString::fromUtf8("每月重复");
		break;
	case OpenMeeting::MeetingRepeatType::Custom:
		text = customRepeatText(rule);
		break;
	case OpenMeeting::MeetingRepeatType::Unknown:
		text = rule.rawType.trimmed().isEmpty()
			? QString::fromUtf8("未知重复规则（只读）")
			: QString::fromUtf8("重复规则：%1（只读）").arg(rule.rawType);
		break;
	}
	if (text.isEmpty()) text = QString::fromUtf8("未知重复规则（只读）");
	return text + repeatEndSuffix(rule, timeZone);
}

void MeetingListModel::rebuild() {
	beginResetModel();
	_rows.clear();

	OpenMeeting::MeetingList projected;
	projected.reserve(_source.size());
	QSet<QString> seenMeetingIds;
	for (const auto &meeting : _source) {
		if (meeting.meetingId.isEmpty() || seenMeetingIds.contains(meeting.meetingId)) continue;
		seenMeetingIds.insert(meeting.meetingId);
		if (_history && (_historyCreatorUserId.isEmpty() ||
			meeting.creatorUserId != _historyCreatorUserId)) continue;
		if (!_searchText.isEmpty() &&
			!meeting.title.contains(_searchText, Qt::CaseInsensitive) &&
			!meeting.creatorNickname.contains(_searchText, Qt::CaseInsensitive) &&
			!meeting.creatorUserId.contains(_searchText, Qt::CaseInsensitive)) continue;
		projected.push_back(meeting);
	}

	std::stable_sort(projected.begin(), projected.end(), [history = _history](
			const OpenMeeting::MeetingRecord &a,
			const OpenMeeting::MeetingRecord &b) {
		if (a.scheduledTimeSeconds != b.scheduledTimeSeconds) {
			return history
				? a.scheduledTimeSeconds > b.scheduledTimeSeconds
				: a.scheduledTimeSeconds < b.scheduledTimeSeconds;
		}
		return a.meetingId < b.meetingId;
	});

	QString previousDateGroup;
	_rows.reserve(projected.size());
	for (auto &meeting : projected) {
		const auto dateTime = scheduledDateTime(meeting);
		const auto dateGroup = dateTime.isValid()
			? dateTime.date().toString(QString::fromUtf8("yyyy年M月d日 dddd"))
			: QString::fromUtf8("时间待确认");
		_rows.push_back({std::move(meeting), dateGroup, dateGroup != previousDateGroup});
		previousDateGroup = dateGroup;
	}
	endResetModel();
}

} // namespace MeetingUI
